/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "utils/noise.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Paint roads from a block layout on the active layer.
 *
 * 1. Collect every plan block on the active layer.
 * 2. Use both z-1 and z around each plan block as copy/paint surfaces.
 * 3. Copy terrain from another layer around those anchors
 *    (radius = ceil(thickness/2 + anti-alias + dithering)).
 * 4. Paint the road colour onto that surface using thickness / AA / dither /
 *    noise.  Output goes on a new "<plan> Roads" layer; the plan layer is
 *    hidden and left unchanged.
 */

typedef struct {
    int x, y, z; /* original plan voxel */
} road_vox_t;

typedef struct {
    filter_t filter;
    int thickness;
    int anti_alias;
    float dithering;
    uint8_t color[4];
    int noise_intensity;
    int noise_saturation;
    /* Layer id, not a pointer: undo/redo swaps the image for a snapshot whose
     * layers are fresh allocations, so a stored pointer would dangle. */
    int source_layer_id;
} filter_roads_t;

static const uint8_t k_default_color[4] = {61, 61, 61, 255};

static void reset_defaults(filter_roads_t *filter)
{
    filter->thickness = 6;
    filter->anti_alias = 1;
    filter->dithering = 0.5f;
    filter->noise_intensity = 5;
    filter->noise_saturation = 10;
    filter->source_layer_id = 0;
    memcpy(filter->color, k_default_color, 4);
}

static layer_t *find_layer_by_id(int id)
{
    layer_t *layer;

    if (!goxel.image || id <= 0)
        return NULL;
    DL_FOREACH(goxel.image->layers, layer) {
        if (layer->id == id)
            return layer;
    }
    return NULL;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool collect_plan_voxels(const volume_t *vol, road_vox_t **out, int *nout)
{
    volume_iterator_t iter;
    int pos[3], n = 0, cap = 0;
    road_vox_t *voxels = NULL;

    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        road_vox_t *nv;
        if (!volume_get_alpha_at(vol, &iter, pos))
            continue;
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 256;
            nv = realloc(voxels, (size_t)ncap * sizeof(*voxels));
            if (!nv) {
                free(voxels);
                return false;
            }
            voxels = nv;
            cap = ncap;
        }
        voxels[n].x = pos[0];
        voxels[n].y = pos[1];
        voxels[n].z = pos[2];
        n++;
    }

    *out = voxels;
    *nout = n;
    return true;
}

/*
 * Collapse the plan to one seed per XY column and project each seed onto the
 * free voxel directly above the highest occupied voxel of the selected layer,
 * matching a plan drawn on top of that terrain. Columns without terrain are
 * omitted.
 */
static void project_plan_voxels(road_vox_t *plan, int *nplan,
                                const volume_t *surface)
{
    int bbox[2][3];
    int i, j, n = 0;
    uint8_t c[4];

    if (!volume_get_bbox(surface, bbox, true)) {
        *nplan = 0;
        return;
    }

    for (i = 0; i < *nplan; i++) {
        int pos[3] = {plan[i].x, plan[i].y, 0};
        bool duplicate = false;

        for (j = 0; j < n; j++) {
            if (plan[j].x == plan[i].x && plan[j].y == plan[i].y) {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        for (pos[2] = bbox[1][2] - 1; pos[2] >= bbox[0][2]; pos[2]--) {
            volume_get_at(surface, NULL, pos, c);
            if (c[3])
                break;
        }
        if (pos[2] < bbox[0][2])
            continue;

        plan[n].x = pos[0];
        plan[n].y = pos[1];
        plan[n].z = pos[2] + 1;
        n++;
    }
    *nplan = n;
}

/* Nearest plan in XY; returns that plan voxel's Z. */
static float min_dist_xy(int x, int y, const road_vox_t *plan, int nplan,
                         int *out_plan_z)
{
    float best = FLT_MAX;
    int best_z = 0;
    int i;

    for (i = 0; i < nplan; i++) {
        float dx = (float)(x - plan[i].x);
        float dy = (float)(y - plan[i].y);
        float d = sqrtf(dx * dx + dy * dy);
        if (d < best) {
            best = d;
            best_z = plan[i].z;
        }
    }
    if (out_plan_z)
        *out_plan_z = best_z;
    return best;
}

static float road_coverage(float dist, int thickness, float aa, float dithering,
                           int x, int y)
{
    float half = (float)thickness * 0.5f;
    float k = half - dist;

    if (dithering > 0.f) {
        float n = uniform_noise((float)x, (float)y, 0.f);
        k += (n * 2.f - 1.f) * dithering;
    }
    if (aa > 0.f)
        return clamp(k / aa, -1.f, 1.f) / 2.f + 0.5f;
    return (k >= 0.f) ? 1.f : 0.f;
}

/* Copy source voxels on z-1 and z for one XY position. */
static void copy_two_levels(volume_t *dest, const volume_t *src,
                            int x, int y, int z_lo, int z_hi, int plan_z)
{
    int pos[3] = {x, y, plan_z - 1};
    uint8_t sc[4];
    int zi;
    const int levels[2] = {plan_z - 1, plan_z};

    for (zi = 0; zi < 2; zi++) {
        pos[2] = levels[zi];
        if (pos[2] < z_lo || pos[2] > z_hi)
            continue;
        volume_get_at(src, NULL, pos, sc);
        if (!sc[3])
            continue;
        volume_set_at(dest, NULL, pos, sc);
    }
}

static void make_road_color(const filter_roads_t *filter, int x, int y, int z,
                            uint8_t out[4])
{
    int rgb[3];

    memcpy(out, filter->color, 4);
    if (filter->noise_intensity <= 0)
        return;

    rgb[0] = out[0];
    rgb[1] = out[1];
    rgb[2] = out[2];
    blend_with_noise_alpha(rgb,
                           uniform_noise((float)x, (float)y, (float)z),
                           (float)filter->noise_intensity,
                           (float)filter->noise_saturation,
                           rgb);
    out[0] = (uint8_t)clampi(rgb[0], 0, 255);
    out[1] = (uint8_t)clampi(rgb[1], 0, 255);
    out[2] = (uint8_t)clampi(rgb[2], 0, 255);
}

static void paint_road_voxel(volume_t *dest, const filter_roads_t *filter,
                             int x, int y, int z, float cov)
{
    uint8_t existing[4], paint[4], out[4];
    int pos[3] = {x, y, z};

    if (cov <= 0.f)
        return;

    volume_get_at(dest, NULL, pos, existing);
    if (!existing[3])
        return;

    make_road_color(filter, x, y, z, paint);
    paint[3] = (uint8_t)clampi((int)(cov * 255.f + 0.5f), 0, 255);
    if (!paint[3])
        return;

    voxel_combine(existing, paint, MODE_PAINT, out);
    volume_set_at(dest, NULL, pos, out);
}

static layer_t *prepare_target_layer(layer_t *plan_layer)
{
    layer_t *target;
    const char *suffix = " Roads";
    int max_base;

    if (layer_has_children(goxel.image, plan_layer))
        target = image_add_child_layer(goxel.image, plan_layer);
    else
        target = image_add_layer(goxel.image, NULL);
    if (!target)
        return NULL;
    target->visible = true;
    max_base = (int)sizeof(target->name) - 1 - (int)strlen(suffix);
    if (max_base < 0) max_base = 0;
    snprintf(target->name, sizeof(target->name), "%.*s%s",
             max_base, plan_layer->name, suffix);
    plan_layer->visible = false;
    plan_layer->collapsed = false;
    return target;
}

static void apply_roads(filter_roads_t *filter, layer_t *layer)
{
    road_vox_t *plan = NULL;
    int nplan = 0;
    float box[4][4];
    int dims[3], start[3];
    int x, y, z_lo, z_hi;
    int thickness, radius;
    float aa, dither;
    const volume_t *src;
    layer_t *target_layer;
    layer_t *source_layer;
    volume_t *dest, *work = NULL;
    int plan_min[2], plan_max[2];
    int i, dx, dy;

    if (!layer || !layer->volume || volume_is_empty(layer->volume))
        return;
    source_layer = find_layer_by_id(filter->source_layer_id);
    if (!source_layer || !source_layer->volume) {
        gui_alert("Plan - Roads", "Select a terrain layer to pave.");
        return;
    }

    if (!collect_plan_voxels(layer->volume, &plan, &nplan) || nplan == 0) {
        free(plan);
        return;
    }
    project_plan_voxels(plan, &nplan, source_layer->volume);
    if (nplan == 0) {
        gui_alert("Plan - Roads",
                  "No plan columns intersect the selected layer.");
        free(plan);
        return;
    }

    thickness = clampi(filter->thickness, 1, 64);
    aa = (float)clampi(filter->anti_alias, 0, 16);
    dither = clamp(filter->dithering, 0.f, 16.f);
    radius = (int)ceilf((float)thickness * 0.5f + aa + dither);

    plan_min[0] = plan_max[0] = plan[0].x;
    plan_min[1] = plan_max[1] = plan[0].y;
    for (i = 1; i < nplan; i++) {
        if (plan[i].x < plan_min[0]) plan_min[0] = plan[i].x;
        if (plan[i].y < plan_min[1]) plan_min[1] = plan[i].y;
        if (plan[i].x > plan_max[0]) plan_max[0] = plan[i].x;
        if (plan[i].y > plan_max[1]) plan_max[1] = plan[i].y;
    }

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(source_layer->volume, true, box);
    box_get_dimensions(box, dims);
    box_get_start_pos(box, start);
    z_lo = start[2];
    z_hi = start[2] + dims[2] - 1;

    image_history_push(goxel.image);
    target_layer = prepare_target_layer(layer);
    if (!target_layer)
        goto end;
    dest = target_layer->volume;
    src = source_layer->volume;
    work = volume_new();
    if (!work)
        goto end;

    /* Copy terrain around each plan block on z-1 and z. */
    int copied_count = 0;
    for (i = 0; i < nplan; i++) {
        int ax = plan[i].x;
        int ay = plan[i].y;
        int plan_z = plan[i].z;
        for (dy = -radius; dy <= radius; dy++) {
            for (dx = -radius; dx <= radius; dx++) {
                int pos[3];
                uint8_t sc[4];
                float dist = sqrtf((float)(dx * dx + dy * dy));
                if (dist > (float)radius)
                    continue;
                copy_two_levels(work, src, ax + dx, ay + dy,
                                z_lo, z_hi, plan_z);
                pos[0] = ax + dx;
                pos[1] = ay + dy;
                pos[2] = plan_z - 1;
                if (pos[2] >= z_lo && pos[2] <= z_hi) {
                    volume_get_at(src, NULL, pos, sc);
                    if (sc[3]) copied_count++;
                }
                pos[2] = plan_z;
                if (pos[2] >= z_lo && pos[2] <= z_hi) {
                    volume_get_at(src, NULL, pos, sc);
                    if (sc[3]) copied_count++;
                }
            }
        }
    }
    if (!copied_count) {
        gui_alert("Plan - Roads",
                  "Nothing to copy from the selected layer at z-1/z in range.\n"
                  "Try a different source layer or adjust plan height.");
        target_layer->visible = false;
        layer->visible = true;
        goxel.image->active_layer = layer;
        goto end;
    }

    /* Paint road colour onto the copied surface using the plan XY shape. */
    for (y = plan_min[1] - radius; y <= plan_max[1] + radius; y++) {
        for (x = plan_min[0] - radius; x <= plan_max[0] + radius; x++) {
            int plan_z;
            float dist, cov;
            uint8_t probe[4];
            int pos[3];
            int z;

            dist = min_dist_xy(x, y, plan, nplan, &plan_z);
            cov = road_coverage(dist, thickness, aa, dither, x, y);
            if (cov <= 0.f)
                continue;

            /* Prefer plan z, then z-1, else nearest solid below. */
            pos[0] = x;
            pos[1] = y;
            pos[2] = plan_z;
            volume_get_at(work, NULL, pos, probe);
            if (probe[3]) {
                paint_road_voxel(work, filter, x, y, plan_z, cov);
                continue;
            }
            pos[2] = plan_z - 1;
            volume_get_at(work, NULL, pos, probe);
            if (probe[3]) {
                paint_road_voxel(work, filter, x, y, plan_z - 1, cov);
                continue;
            }
            for (z = plan_z - 2; z >= z_lo; z--) {
                pos[2] = z;
                volume_get_at(work, NULL, pos, probe);
                if (probe[3]) {
                    paint_road_voxel(work, filter, x, y, z, cov);
                    break;
                }
            }
        }
    }

    volume_set(dest, work);
end:
    if (work) volume_delete(work);
    free(plan);
}

static int gui(filter_t *filter_)
{
    filter_roads_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    layer_t *source_layer;
    const char *help_text =
        "Uses blocks on the active layer as a road layout.  For each plan "
        "block, terrain is copied from the chosen layer at z-1 and z within the "
        "road band onto a new Roads layer with a painted surface.  The plan "
        "layer is hidden and left unchanged - merge layers later if you want.";

    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_label_size_push(120.0f);
    gui_input_int("Thickness", &filter->thickness, 1, 64);
    gui_input_int("Anti-alias", &filter->anti_alias, 0, 16);
    gui_input_float("Dithering", &filter->dithering, 0.1f, 0.f, 16.f, "%.1f");

    gui_color_small("Color", filter->color);
    gui_input_int("Noise intensity", &filter->noise_intensity, 0, 100);
    gui_input_int("Noise saturation", &filter->noise_saturation, 0, 100);

    source_layer = find_layer_by_id(filter->source_layer_id);
    if (!source_layer && goxel.image) {
        source_layer = goxel.image->layers;
        filter->source_layer_id = source_layer ? source_layer->id : 0;
    }
    gui_text("Terrain layer");
    gui_same_line();
    if (gui_combo_begin("##roads_source_layer",
                        source_layer ? source_layer->name : "(none)")) {
        layer_t *cur;
        DL_FOREACH_REVERSE(goxel.image->layers, cur) {
            if (gui_combo_item(cur->name, cur == source_layer))
                filter->source_layer_id = cur->id;
        }
        gui_combo_end();
    }
    gui_tooltip_if_hovered(
        "Select the layer to paint the roads onto - we create a copy first so that it's non-destructive");
    gui_label_size_pop();

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    {
        bool has_layer = goxel.image && goxel.image->active_layer;

        gui_enabled_begin(has_layer);
        if (gui_button("Apply", -1, 0))
            apply_roads(filter, layer);
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_roads_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(roads, filter_roads_t,
                .name = "Roads",
                .menu = "effects",
                .submenu = "plan",
                .on_open = on_open,
                .panel_width = 300,
                .gui_fn = gui, )
