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

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Plan - Imprint: carve the active plan layer's per-column height into a
 * chosen terrain layer.
 *
 * 1. For each XY on the active (plan) layer, depth N = number of solid blocks.
 * 2. On the selected terrain layer, find the highest solid Z at that XY.
 * 3. Clear N cells downward from that top (z_top .. z_top - N + 1).
 * 4. Optionally surface-paint the new imprint base (floor / exposed shell)
 *    with brush-like colour, anti-alias, dithering, and noise.
 *
 * Edits the chosen layer in place and hides the plan layer (one undo step).
 */

typedef struct {
    int x, y;
    int depth;
} imprint_col_t;

typedef struct {
    filter_t filter;
    /* Layer id, not a pointer: undo/redo swaps image snapshots. */
    int target_layer_id;
    bool apply_color;
    uint8_t color[4];
    int anti_alias;
    float dithering; /* Outer expand ring only (imprint +1 stays solid). */
    int noise_intensity;
    int noise_saturation;
    int noise_coverage;
} filter_imprint_t;

/* Dirt brown #19100B */
static const uint8_t k_default_color[4] = {0x19, 0x10, 0x0B, 255};
/* Fixed paint footprint: +2 Chebyshev blocks; +1 solid, outer ring dithered. */
static const int k_paint_expand = 2;
static const int k_paint_solid_expand = 1;

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void reset_defaults(filter_imprint_t *filter)
{
    filter->target_layer_id = 0;
    filter->apply_color = true;
    memcpy(filter->color, k_default_color, 4);
    filter->anti_alias = 1;
    filter->dithering = 3.f;
    filter->noise_intensity = 5;
    filter->noise_saturation = 5;
    filter->noise_coverage = 100;
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

/* Highest solid at or below z_from in this column, or INT_MIN if none. */
static int find_top_solid_z(const volume_t *vol, int x, int y,
                            int z_from, int z_min)
{
    int pos[3] = {x, y, z_from};
    uint8_t c[4];

    if (z_from < z_min)
        return INT_MIN;
    for (; pos[2] >= z_min; pos[2]--) {
        volume_get_at(vol, NULL, pos, c);
        if (c[3])
            return pos[2];
    }
    return INT_MIN;
}

static bool collect_plan_depths(const volume_t *vol,
                                imprint_col_t **out, int *nout)
{
    volume_iterator_t iter;
    int pos[3], n = 0, cap = 0, i;
    imprint_col_t *cols = NULL;

    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        if (!volume_get_alpha_at(vol, &iter, pos))
            continue;

        for (i = 0; i < n; i++) {
            if (cols[i].x == pos[0] && cols[i].y == pos[1]) {
                cols[i].depth++;
                break;
            }
        }
        if (i < n)
            continue;

        if (n == cap) {
            int ncap = cap ? cap * 2 : 256;
            imprint_col_t *ncols = realloc(cols, (size_t)ncap * sizeof(*cols));
            if (!ncols) {
                free(cols);
                return false;
            }
            cols = ncols;
            cap = ncap;
        }
        cols[n].x = pos[0];
        cols[n].y = pos[1];
        cols[n].depth = 1;
        n++;
    }

    *out = cols;
    *nout = n;
    return true;
}

/* Chebyshev distance to nearest imprint column; *inside if (x,y) is imprint. */
static int min_cheb_xy(int x, int y, const imprint_col_t *cols, int ncols,
                       bool *inside)
{
    int best = INT_MAX;
    int i;

    *inside = false;
    for (i = 0; i < ncols; i++) {
        int dx = abs(x - cols[i].x);
        int dy = abs(y - cols[i].y);
        int d = dx > dy ? dx : dy;
        if (d < best)
            best = d;
        if (cols[i].x == x && cols[i].y == y)
            *inside = true;
    }
    return best;
}

/*
 * Paint footprint: imprint + solid_expand are full coverage (no dither).
 * The outer expand ring (solid_expand < cheb <= expand) is dithered.
 * Past expand, anti-alias softens the edge.
 */
static float imprint_paint_coverage(bool inside, int cheb, float aa,
                                    float dithering, int x, int y)
{
    float k;
    float v;
    float edge_dist;

    if (inside || cheb <= k_paint_solid_expand)
        return 1.f;

    edge_dist = (float)cheb - (float)k_paint_expand;
    if (edge_dist <= 0.f)
        k = aa > 0.f ? aa : 1.f;
    else
        k = 0.5f - edge_dist;

    /* Dither only the outer expand ring, not the AA fringe beyond it. */
    if (dithering > 0.f && cheb <= k_paint_expand) {
        float n = uniform_noise((float)x, (float)y, 0.f);
        k += (n * 2.f - 1.f) * dithering;
    }
    if (aa > 0.f)
        v = clamp(k / aa, -1.f, 1.f) / 2.f + 0.5f;
    else
        v = (k >= 0.f) ? 1.f : 0.f;
    return v;
}

static bool voxel_exposed(const volume_t *vol, int x, int y, int z)
{
    static const int offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1},
    };
    uint8_t cur[4], neigh[4];
    int i, pos[3] = {x, y, z};

    volume_get_at(vol, NULL, pos, cur);
    if (!voxel_is_solid(cur))
        return false;

    for (i = 0; i < 6; i++) {
        pos[0] = x + offsets[i][0];
        pos[1] = y + offsets[i][1];
        pos[2] = z + offsets[i][2];
        volume_get_at(vol, NULL, pos, neigh);
        if (!voxel_is_solid(neigh))
            return true;
    }
    return false;
}

static void make_paint_color(const filter_imprint_t *filter, int x, int y, int z,
                             uint8_t out[4])
{
    int rgb[3];
    float noise_value;

    memcpy(out, filter->color, 4);
    if (filter->noise_intensity <= 0 || filter->noise_coverage <= 0)
        return;

    noise_value = uniform_noise((float)x, (float)y, (float)z);
    if (noise_value >= (float)filter->noise_coverage / 100.0f)
        return;

    rgb[0] = out[0];
    rgb[1] = out[1];
    rgb[2] = out[2];
    blend_with_noise_alpha(rgb, noise_value,
                           (float)filter->noise_intensity,
                           (float)filter->noise_saturation, rgb);
    out[0] = (uint8_t)clampi(rgb[0], 0, 255);
    out[1] = (uint8_t)clampi(rgb[1], 0, 255);
    out[2] = (uint8_t)clampi(rgb[2], 0, 255);
}

/*
 * Surface-paint one column (brush surface-paint behaviour): from the top,
 * paint consecutive air-exposed solids until a buried voxel is hit.
 */
static void paint_surface_column(volume_t *vol, int x, int y,
                                 int z_lo, int z_hi, float cov,
                                 const filter_imprint_t *filter)
{
    int pos[3] = {x, y, 0};
    uint8_t existing[4], paint[4], out[4];
    bool seen_solid = false;
    int z;

    if (cov <= 0.f)
        return;

    for (z = z_hi; z >= z_lo; z--) {
        pos[2] = z;
        volume_get_at(vol, NULL, pos, existing);
        if (!voxel_is_solid(existing)) {
            if (seen_solid)
                break;
            continue;
        }
        seen_solid = true;
        if (!voxel_exposed(vol, x, y, z))
            break;

        make_paint_color(filter, x, y, z, paint);
        /* Coverage scales the chosen colour alpha (MODE_PAINT strength). */
        paint[3] = (uint8_t)clampi(
            (int)(cov * (float)paint[3] + 0.5f), 0, 255);
        if (!paint[3])
            continue;

        voxel_combine(existing, paint, MODE_PAINT, out);
        volume_set_at(vol, NULL, pos, out);
    }
}

static void apply_imprint_color(volume_t *work, const imprint_col_t *cols,
                                int ncols, const int bbox[2][3],
                                const filter_imprint_t *filter)
{
    int plan_min[2], plan_max[2];
    int i, x, y;
    float aa, dither;
    int band;
    int z_lo = bbox[0][2];
    int z_hi = bbox[1][2] - 1;

    if (ncols <= 0)
        return;

    aa = (float)clampi(filter->anti_alias, 0, 16);
    dither = clamp(filter->dithering, 0.f, 16.f);
    band = k_paint_expand + (int)ceilf(aa + dither) + 1;

    plan_min[0] = plan_max[0] = cols[0].x;
    plan_min[1] = plan_max[1] = cols[0].y;
    for (i = 1; i < ncols; i++) {
        if (cols[i].x < plan_min[0]) plan_min[0] = cols[i].x;
        if (cols[i].y < plan_min[1]) plan_min[1] = cols[i].y;
        if (cols[i].x > plan_max[0]) plan_max[0] = cols[i].x;
        if (cols[i].y > plan_max[1]) plan_max[1] = cols[i].y;
    }

    for (y = plan_min[1] - band; y <= plan_max[1] + band; y++) {
        for (x = plan_min[0] - band; x <= plan_max[0] + band; x++) {
            bool inside;
            int cheb = min_cheb_xy(x, y, cols, ncols, &inside);
            float cov = imprint_paint_coverage(inside, cheb, aa, dither, x, y);
            if (cov <= 0.f)
                continue;
            paint_surface_column(work, x, y, z_lo, z_hi, cov, filter);
        }
    }
}

static void apply_imprint(filter_imprint_t *filter, layer_t *plan_layer)
{
    imprint_col_t *cols = NULL;
    int ncols = 0;
    layer_t *target;
    volume_t *work = NULL;
    int bbox[2][3];
    int i, z, z_top, z_lo;
    int pos[3];
    const uint8_t empty[4] = {0, 0, 0, 0};

    if (!plan_layer || !plan_layer->volume ||
        volume_is_empty(plan_layer->volume)) {
        gui_alert("Plan - Imprint", "Active layer has no voxels.");
        return;
    }

    target = find_layer_by_id(filter->target_layer_id);
    if (!target || !target->volume) {
        gui_alert("Plan - Imprint", "Select a terrain layer to imprint.");
        return;
    }
    if (target == plan_layer) {
        gui_alert("Plan - Imprint",
                  "Choose a different layer than the active plan layer.");
        return;
    }
    if (volume_is_empty(target->volume)) {
        gui_alert("Plan - Imprint", "Target layer is empty.");
        return;
    }

    if (!collect_plan_depths(plan_layer->volume, &cols, &ncols)) {
        gui_alert("Plan - Imprint", "Out of memory while scanning plan.");
        return;
    }
    if (ncols == 0) {
        free(cols);
        gui_alert("Plan - Imprint", "Active layer has no voxels.");
        return;
    }

    if (!volume_get_bbox(target->volume, bbox, true)) {
        free(cols);
        gui_alert("Plan - Imprint", "Target layer is empty.");
        return;
    }

    image_history_push(goxel.image);
    plan_layer->visible = false;

    work = volume_copy(target->volume);
    for (i = 0; i < ncols; i++) {
        if (cols[i].depth <= 0)
            continue;

        z_top = find_top_solid_z(work, cols[i].x, cols[i].y,
                                 bbox[1][2] - 1, bbox[0][2]);
        if (z_top == INT_MIN)
            continue;

        z_lo = z_top - cols[i].depth + 1;
        if (z_lo < bbox[0][2])
            z_lo = bbox[0][2];

        pos[0] = cols[i].x;
        pos[1] = cols[i].y;
        for (z = z_top; z >= z_lo; z--) {
            pos[2] = z;
            volume_set_at(work, NULL, pos, empty);
        }
    }

    if (filter->apply_color)
        apply_imprint_color(work, cols, ncols, bbox, filter);

    volume_set(target->volume, work);
    volume_delete(work);
    free(cols);
}

static int gui(filter_t *filter_)
{
    filter_imprint_t *filter = (void *)filter_;
    layer_t *layer = goxel.image ? goxel.image->active_layer : NULL;
    layer_t *target_layer;
    const char *help_text =
        "Uses blocks on the active layer as an imprint mask.  For each X/Y "
        "column, the number of solid plan blocks is the carve depth.  Digs "
        "that many voxels downward from the highest solid on the chosen "
        "terrain layer.  Optionally paints the new imprint base like a "
        "surface-paint brush.  The plan layer is hidden afterward.";

    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_text_wrapped(
        "WARNING: This permanently edits the chosen terrain layer "
        "(undoable). It is a destructive action.");

    gui_label_size_push(120.0f);

    target_layer = find_layer_by_id(filter->target_layer_id);
    if (!target_layer && goxel.image) {
        target_layer = goxel.image->layers;
        filter->target_layer_id = target_layer ? target_layer->id : 0;
    }
    gui_text("Terrain layer");
    gui_same_line();
    if (gui_combo_begin("##imprint_target_layer",
                        target_layer ? target_layer->name : "(none)")) {
        layer_t *cur;
        DL_FOREACH_REVERSE(goxel.image->layers, cur) {
            if (gui_combo_item(cur->name, cur == target_layer))
                filter->target_layer_id = cur->id;
        }
        gui_combo_end();
    }
    gui_tooltip_if_hovered(
        "Layer that will be carved in place from the active plan mask.");

    gui_separator();
    gui_checkbox("Apply color", &filter->apply_color,
                 "Paint the imprint base with a brush-like colour pass "
                 "(surface paint, anti-alias, noise).");
    if (filter->apply_color) {
        gui_color_small("Color", filter->color);
        gui_color_opacity(filter->color);
        gui_input_int("Anti-alias", &filter->anti_alias, 0, 16);
        gui_tooltip_if_hovered(
            "0 = hard edges, 1 = soft edge (~1 block), 2+ = wider blur");
        gui_input_float("Dithering", &filter->dithering, 0.1f, 0.f, 16.f,
                        "%.1f");
        gui_tooltip_if_hovered(
            "Scatters only the outer (+2) expand ring; imprint +1 stays solid");
        gui_input_int("Noise intensity", &filter->noise_intensity, 0, 100);
        gui_input_int("Noise saturation", &filter->noise_saturation, 0, 100);
        gui_input_int("Noise coverage", &filter->noise_coverage, 0, 100);
    }

    gui_label_size_pop();

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    {
        bool has_layer = goxel.image && goxel.image->active_layer;
        bool ready = has_layer && find_layer_by_id(filter->target_layer_id);

        gui_enabled_begin(ready);
        if (gui_button("Apply", -1, 0))
            apply_imprint(filter, layer);
        gui_enabled_end();
        gui_alert_if_disabled_clicked(ready, "Cannot imprint",
                                      has_layer
                                          ? "Select a terrain layer first."
                                          : "Select a plan layer first.");
    }

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_imprint_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(imprint, filter_imprint_t,
                .name = "Imprint",
                .menu = "effects",
                .submenu = "plan",
                .on_open = on_open,
                .panel_width = 300,
                .gui_fn = gui, )
