/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"

#include <limits.h>

/*
 * Plan - Buildings
 *
 * Paint 1-block-high filled footprints on the active layer.  Each of four
 * colours maps to a floor count (1–4).  Generate replaces those footprints
 * with inward-walled buildings: base slab, clear-height storeys with
 * perimeter walls, intermediate slabs, and a flat roof.
 */

#define BUILDING_FLOOR_COUNTS 4
#define BUILDING_NOISE_INTENSITY 5
#define BUILDING_NOISE_SATURATION 5

typedef struct {
    filter_t filter;
    int floor_height;      /* clear height between slabs */
    int floor_thickness;   /* slab / roof thickness */
    int wall_thickness;    /* inward wall bands */
    uint8_t floor_colors[BUILDING_FLOOR_COUNTS][4];
} filter_buildings_t;

typedef struct {
    int x, y;
} building_xy_t;

typedef struct {
    int floors;
    int base_z;
    building_xy_t *cells;
    int ncells;
    int cells_cap;
    int xmin, ymin, xmax, ymax;
    uint8_t *occ; /* (xmax-xmin+1) * (ymax-ymin+1) */
} building_group_t;

static const uint8_t k_default_colors[BUILDING_FLOOR_COUNTS][4] = {
    {220, 70, 70, 255},
    {70, 180, 90, 255},
    {70, 110, 220, 255},
    {220, 190, 60, 255},
};

static const uint8_t k_base_grey[4] = {128, 128, 128, 255};
static const uint8_t k_empty[4] = {0, 0, 0, 0};

static void reset_defaults(filter_buildings_t *filter)
{
    int i;

    filter->floor_height = 4;
    filter->floor_thickness = 1;
    filter->wall_thickness = 1;
    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++)
        memcpy(filter->floor_colors[i], k_default_colors[i], 4);
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int floor_count_for_color(const filter_buildings_t *filter,
                                 const uint8_t color[4])
{
    int i;

    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++) {
        if (memcmp(filter->floor_colors[i], color, 4) == 0)
            return i + 1;
    }
    return 0;
}

static void make_building_color(int x, int y, int z, uint8_t out[4])
{
    int rgb[3];

    memcpy(out, k_base_grey, 4);
    rgb[0] = out[0];
    rgb[1] = out[1];
    rgb[2] = out[2];
    blend_with_noise_alpha(rgb,
                           uniform_noise((float)x, (float)y, (float)z),
                           (float)BUILDING_NOISE_INTENSITY,
                           (float)BUILDING_NOISE_SATURATION,
                           rgb);
    out[0] = (uint8_t)clampi(rgb[0], 0, 255);
    out[1] = (uint8_t)clampi(rgb[1], 0, 255);
    out[2] = (uint8_t)clampi(rgb[2], 0, 255);
    out[3] = 255;
}

static int occ_index(const building_group_t *g, int x, int y)
{
    return (y - g->ymin) * (g->xmax - g->xmin + 1) + (x - g->xmin);
}

static bool occ_get(const building_group_t *g, int x, int y)
{
    if (x < g->xmin || x > g->xmax || y < g->ymin || y > g->ymax)
        return false;
    return g->occ[occ_index(g, x, y)] != 0;
}

/* True if (x,y) is within `wall_t` orthogonal steps of a non-footprint cell. */
static bool is_wall_cell(const building_group_t *g, int x, int y, int wall_t)
{
    int d, dx, dy, ax, ay;

    for (d = 1; d <= wall_t; d++) {
        for (dx = -d; dx <= d; dx++) {
            ax = abs(dx);
            dy = d - ax;
            ay = y + dy;
            if (!occ_get(g, x + dx, ay))
                return true;
            if (dy != 0 && !occ_get(g, x + dx, y - dy))
                return true;
        }
    }
    return false;
}

static void paint_voxel(volume_t *vol, int x, int y, int z)
{
    int pos[3] = {x, y, z};
    uint8_t c[4];

    make_building_color(x, y, z, c);
    volume_set_at(vol, NULL, pos, c);
}

static void clear_voxel(volume_t *vol, int x, int y, int z)
{
    int pos[3] = {x, y, z};
    volume_set_at(vol, NULL, pos, k_empty);
}

static void paint_slab(volume_t *vol, const building_group_t *g,
                       int z0, int thickness)
{
    int i, t, z;

    for (i = 0; i < g->ncells; i++) {
        for (t = 0; t < thickness; t++) {
            z = z0 + t;
            paint_voxel(vol, g->cells[i].x, g->cells[i].y, z);
        }
    }
}

static void paint_walls(volume_t *vol, const building_group_t *g,
                        int z0, int height, int wall_t)
{
    int i, t, z, x, y;

    for (i = 0; i < g->ncells; i++) {
        x = g->cells[i].x;
        y = g->cells[i].y;
        if (!is_wall_cell(g, x, y, wall_t))
            continue;
        for (t = 0; t < height; t++) {
            z = z0 + t;
            paint_voxel(vol, x, y, z);
        }
    }
}

static void clear_building_column_range(volume_t *vol, const building_group_t *g,
                                        int z0, int height)
{
    int i, t;

    for (i = 0; i < g->ncells; i++) {
        for (t = 0; t < height; t++)
            clear_voxel(vol, g->cells[i].x, g->cells[i].y, z0 + t);
    }
}

static void generate_group(volume_t *vol, const building_group_t *g,
                           int floor_h, int floor_t, int wall_t)
{
    int i;
    int total_h;
    int z;

    if (g->ncells <= 0)
        return;

    total_h = g->floors * floor_h + (g->floors + 1) * floor_t;
    clear_building_column_range(vol, g, g->base_z, total_h);

    /* Base slab. */
    paint_slab(vol, g, g->base_z, floor_t);
    z = g->base_z + floor_t;

    for (i = 0; i < g->floors; i++) {
        paint_walls(vol, g, z, floor_h, wall_t);
        z += floor_h;
        /* Intermediate floor, or flat roof after the last storey. */
        paint_slab(vol, g, z, floor_t);
        z += floor_t;
    }
}

static bool group_key_equal(const building_group_t *g, int floors, int base_z)
{
    return g->floors == floors && g->base_z == base_z;
}

static building_group_t *find_or_add_group(building_group_t **groups, int *ngroups,
                                           int *cap, int floors, int base_z)
{
    int i;
    building_group_t *g;
    building_group_t *ng;

    for (i = 0; i < *ngroups; i++) {
        if (group_key_equal(&(*groups)[i], floors, base_z))
            return &(*groups)[i];
    }

    if (*ngroups >= *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        ng = realloc(*groups, (size_t)ncap * sizeof(*ng));
        if (!ng)
            return NULL;
        *groups = ng;
        *cap = ncap;
    }

    g = &(*groups)[*ngroups];
    memset(g, 0, sizeof(*g));
    g->floors = floors;
    g->base_z = base_z;
    g->xmin = INT_MAX;
    g->ymin = INT_MAX;
    g->xmax = INT_MIN;
    g->ymax = INT_MIN;
    (*ngroups)++;
    return g;
}

static bool group_add_cell(building_group_t *g, int x, int y)
{
    building_xy_t *nc;

    if (g->ncells >= g->cells_cap) {
        int new_cap = g->cells_cap ? g->cells_cap * 2 : 64;
        nc = realloc(g->cells, (size_t)new_cap * sizeof(*nc));
        if (!nc)
            return false;
        g->cells = nc;
        g->cells_cap = new_cap;
    }

    g->cells[g->ncells].x = x;
    g->cells[g->ncells].y = y;
    g->ncells++;
    if (x < g->xmin) g->xmin = x;
    if (y < g->ymin) g->ymin = y;
    if (x > g->xmax) g->xmax = x;
    if (y > g->ymax) g->ymax = y;
    return true;
}

static bool group_build_occupancy(building_group_t *g)
{
    int w, h, i, n;

    if (g->ncells <= 0)
        return true;

    w = g->xmax - g->xmin + 1;
    h = g->ymax - g->ymin + 1;
    n = w * h;
    g->occ = calloc((size_t)n, 1);
    if (!g->occ)
        return false;

    for (i = 0; i < g->ncells; i++)
        g->occ[occ_index(g, g->cells[i].x, g->cells[i].y)] = 1;
    return true;
}

static void free_groups(building_group_t *groups, int ngroups)
{
    int i;

    if (!groups)
        return;
    for (i = 0; i < ngroups; i++) {
        free(groups[i].cells);
        free(groups[i].occ);
    }
    free(groups);
}

static bool collect_groups(const volume_t *src, const filter_buildings_t *filter,
                           building_group_t **out_groups, int *out_ngroups)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t color[4];
    int floors;
    building_group_t *groups = NULL;
    int ngroups = 0;
    int cap = 0;
    int i;

    *out_groups = NULL;
    *out_ngroups = 0;

    iter = volume_get_iterator(src, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        building_group_t *g;

        volume_get_at(src, &iter, pos, color);
        if (!color[3])
            continue;
        floors = floor_count_for_color(filter, color);
        if (!floors)
            continue;

        g = find_or_add_group(&groups, &ngroups, &cap, floors, pos[2]);
        if (!g) {
            free_groups(groups, ngroups);
            return false;
        }
        if (!group_add_cell(g, pos[0], pos[1])) {
            free_groups(groups, ngroups);
            return false;
        }
    }

    for (i = 0; i < ngroups; i++) {
        if (!group_build_occupancy(&groups[i])) {
            free_groups(groups, ngroups);
            return false;
        }
    }

    *out_groups = groups;
    *out_ngroups = ngroups;
    return true;
}

static layer_t *prepare_target_layer(layer_t *plan_layer)
{
    layer_t *target;
    const char *suffix = " Buildings";
    int max_base;

    target = image_add_layer(goxel.image, NULL);
    if (!target)
        return NULL;

    target->visible = true;
    max_base = (int)sizeof(target->name) - 1 - (int)strlen(suffix);
    if (max_base < 0)
        max_base = 0;
    snprintf(target->name, sizeof(target->name), "%.*s%s",
             max_base, plan_layer->name, suffix);
    plan_layer->visible = false;
    return target;
}

static void apply_buildings(filter_buildings_t *filter, layer_t *layer)
{
    volume_t *src = NULL;
    layer_t *target_layer;
    building_group_t *groups = NULL;
    int ngroups = 0;
    int i;
    int floor_h, floor_t, wall_t;

    if (!layer || !layer->volume || volume_is_empty(layer->volume)) {
        gui_alert("Plan - Buildings", "Active layer has no voxels.");
        return;
    }

    floor_h = clampi(filter->floor_height, 1, 256);
    floor_t = clampi(filter->floor_thickness, 1, 64);
    wall_t = clampi(filter->wall_thickness, 1, 64);
    filter->floor_height = floor_h;
    filter->floor_thickness = floor_t;
    filter->wall_thickness = wall_t;

    src = volume_copy(layer->volume);
    if (!src) {
        gui_alert("Plan - Buildings", "Could not copy the active layer.");
        return;
    }

    if (!collect_groups(src, filter, &groups, &ngroups)) {
        gui_alert("Plan - Buildings", "Out of memory while scanning footprints.");
        volume_delete(src);
        return;
    }

    if (ngroups == 0) {
        gui_alert("Plan - Buildings",
                  "No footprint blocks match the configured floor colours.");
        free_groups(groups, ngroups);
        volume_delete(src);
        return;
    }

    image_history_push(goxel.image);
    target_layer = prepare_target_layer(layer);
    if (!target_layer) {
        gui_alert("Plan - Buildings", "Could not create the buildings layer.");
        free_groups(groups, ngroups);
        volume_delete(src);
        return;
    }

    for (i = 0; i < ngroups; i++)
        generate_group(target_layer->volume, &groups[i],
                       floor_h, floor_t, wall_t);

    free_groups(groups, ngroups);
    volume_delete(src);
}

static int gui(filter_t *filter_)
{
    filter_buildings_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    int i;
    char color_id[32];
    char switch_id[32];
    char set_id[32];
    char label[8];

    const char *help_text =
        "Paint 1-block-high filled shapes on the active layer using the floor "
        "colours below.  Generate builds walls along the inward edges, slabs "
        "at the base and between storeys, and a flat roof on a new Buildings "
        "layer.  The plan layer is preserved and hidden.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_input_int("Height of each floor", &filter->floor_height, 1, 256);
    gui_tooltip_if_hovered(
        "Clear height between floor slabs (wall / interior height). Default 4.");
    gui_input_int("Floor thickness", &filter->floor_thickness, 1, 64);
    gui_tooltip_if_hovered("Thickness of each floor slab and the roof. Default 1.");
    gui_input_int("Wall thickness", &filter->wall_thickness, 1, 64);
    gui_tooltip_if_hovered(
        "How many blocks inward from the footprint edge become walls. Default 1.");

    gui_separator();
    gui_text("Number of floors:");

    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++) {
        snprintf(label, sizeof(label), "%d", i + 1);
        snprintf(color_id, sizeof(color_id), "##floor_color_%d", i + 1);
        snprintf(switch_id, sizeof(switch_id), "Switch to##floor_%d", i + 1);
        snprintf(set_id, sizeof(set_id), "Set##floor_%d", i + 1);

        gui_text("%s", label);
        gui_same_line();
        gui_color_small_no_label(color_id, filter->floor_colors[i]);
        gui_same_line();
        if (gui_button(switch_id, 0, 0))
            memcpy(goxel.painter.color, filter->floor_colors[i], 4);
        gui_tooltip_if_hovered("Switch the current brush colour to this colour.");
        gui_same_line();
        if (gui_button(set_id, 0, 0))
            memcpy(filter->floor_colors[i], goxel.painter.color, 4);
        gui_tooltip_if_hovered("Set this floor marker to the current brush colour.");
    }

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    if (gui_button("Generate", -1, 0))
        apply_buildings(filter, layer);

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_buildings_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(buildings, filter_buildings_t,
                .name = "Plan - Buildings",
                .on_open = on_open,
                .panel_width = 280,
                .gui_fn = gui, )
