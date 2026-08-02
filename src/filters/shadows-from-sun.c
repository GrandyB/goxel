/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
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
#include "volume.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

/*
 * Angular (genland-style) baked shadows on the active layer.
 * Occluders: effectively-visible layers above the active layer in the
 * layer list (later in the forward list = higher in the UI), optionally
 * including the active layer itself for self-shadowing.
 */
typedef struct
{
    filter_t filter;
    float strength;
    float sun_angle_deg;
    bool wrap_shadows;
    bool do_smoothing;
    int shadow_blur_blocks;
    bool include_current_layer;
} filter_shadows_from_sun_t;

static void adjust_colour_brightness(uint8_t colour[4], float multiplier)
{
    for (int i = 0; i < 3; i++) {
        int c = (int)(colour[i] * multiplier);
        if (c < 0)
            c = 0;
        if (c > 255)
            c = 255;
        colour[i] = (uint8_t)c;
    }
}

static int wrap_coord(int v, int m)
{
    v %= m;
    if (v < 0)
        v += m;
    return v;
}

/* Symmetric box blur: radius r uses (2r+1)^2 taps. */
static void shadow_box_blur(unsigned char *dst, const unsigned char *src,
                            int gw, int gh, int r, bool wrap)
{
    if (r <= 0 || gw <= 0 || gh <= 0)
        return;
    const int side = 2 * r + 1;
    const int area = side * side;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            int sum = 0;
            int count = 0;
            for (int dy = -r; dy <= r; dy++) {
                const int yy = y + dy;
                if (!wrap && (yy < 0 || yy >= gh))
                    continue;
                const int yyy = wrap ? wrap_coord(yy, gh) : yy;
                for (int dx = -r; dx <= r; dx++) {
                    const int xx = x + dx;
                    if (!wrap && (xx < 0 || xx >= gw))
                        continue;
                    const int xxx = wrap ? wrap_coord(xx, gw) : xx;
                    sum += (int)src[yyy * gw + xxx];
                    count++;
                }
            }
            if (count <= 0)
                count = area;
            dst[y * gw + x] = (unsigned char)((sum + count / 2) / count);
        }
    }
}

/* Merge visible layers above `active` (UI order); optionally include active. */
static volume_t *build_casters_volume(const image_t *img, const layer_t *active,
                                      bool include_current)
{
    volume_t *casters;
    layer_t *layer;
    bool past_active = false;
    int count = 0;

    if (!img || !active)
        return NULL;

    casters = volume_new();
    if (!casters)
        return NULL;

    /* Forward list is bottom→top in the layers panel; nodes after active are
     * above in the UI. Optionally merge active for self-shadowing. */
    DL_FOREACH(img->layers, layer) {
        if (layer == active) {
            past_active = true;
            if (!include_current)
                continue;
        } else if (!past_active) {
            continue;
        }
        if (layer != active && !layer_effectively_visible(img, layer))
            continue;
        if (!layer->volume || volume_is_empty(layer->volume))
            continue;
        volume_merge(casters, layer->volume, MODE_OVER, NULL);
        count++;
    }

    if (count == 0) {
        volume_delete(casters);
        return NULL;
    }
    return casters;
}

static int gui(filter_t *filter_)
{
    filter_shadows_from_sun_t *filter = (void *)filter_;

    const char *help_text =
        "Applies angular sun shadows to the current layer, cast by blocks from "
        "visible layers above it in the layer list (optionally including the "
        "current layer for self-shadowing).";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_group_begin(NULL);
    gui_label_size_push(220.0f);

    gui_input_float("Strength", &filter->strength, 0.01f, 0.f, 1.f, "%.2f");
    gui_tooltip_if_hovered(
        "How dark the shadow is on the active layer's top voxel.\n"
        "0 = no darkening, 1 = black.");

    gui_input_float("Sun angle", &filter->sun_angle_deg, 1.f, 0.f, 180.f, "%.0f");
    gui_tooltip_if_hovered(
        "Sun elevation around the map. 90 = straight overhead (vertical "
        "shadows only). 0 and 180 = sun on the horizon (long sideways "
        "shadows, opposite directions).");

    gui_checkbox("Include current layer", &filter->include_current_layer,
                "When on, the current layer also casts shadows onto itself "
                "(self-shadowing from hills, buildings on the same layer, etc.).");

    gui_checkbox("Wrap shadows", &filter->wrap_shadows,
                "When on, shadow rays and blur wrap around map edges (tiling). "
                "When off, shadows stop at the border.");

    gui_checkbox("Soften shadows", &filter->do_smoothing,
                "Enable shadow-map blur; extent is set below.");
    if (filter->do_smoothing) {
        gui_input_int("Shadow blur (blocks)", &filter->shadow_blur_blocks, 0, 64);
        gui_tooltip_if_hovered(
            "0 = sharp, 1 = 3x3 blur, 2 = 5x5 blur, etc.");
    }

    gui_label_size_pop();
    gui_group_end();

    {
        bool has_layer = goxel.image && goxel.image->active_layer;

        gui_enabled_begin(has_layer);
        if (gui_button_primary("Apply to current layer", -1, 0)) {
            layer_t *layer;
            volume_t *casters = NULL;
            float box[4][4];
            int dims[3], start_pos[3], pos[3];
            int *recv_heights = NULL;
            int *cast_heights = NULL;
            float *h_recv = NULL;
            float *h_cast = NULL;
            unsigned char *sh = NULL;
            unsigned char *sh_tmp = NULL;
            int gw, gh, n;
            volume_iterator_t iter;
            uint8_t col[4];
            int x, y, idx;

            if (!goxel.image || !goxel.image->active_layer)
                goto apply_end;
            layer = goxel.image->active_layer;
            if (!layer->volume)
                goto apply_end;

            mat4_copy(goxel.image->box, box);
            if (box_is_null(box))
                volume_get_box(layer->volume, true, box);
            box_get_dimensions(box, dims);
            box_get_start_pos(box, start_pos);
            gw = dims[0];
            gh = dims[1];
            if (gw <= 0 || gh <= 0 || dims[2] <= 0)
                goto apply_end;
            n = gw * gh;

            casters = build_casters_volume(goxel.image, layer,
                                           filter->include_current_layer);
            if (!casters) {
                LOG_W("[shadows-from-sun] no caster volume for \"%s\"",
                      layer->name);
                goto apply_end;
            }

            image_history_push(goxel.image);

            allocate_heights(dims, &recv_heights);
            allocate_heights(dims, &cast_heights);
            volume_get_heights_in_box(layer->volume, dims, start_pos, recv_heights);
            volume_get_heights_in_box(casters, dims, start_pos, cast_heights);

            h_recv = malloc(sizeof(float) * (size_t)n);
            h_cast = malloc(sizeof(float) * (size_t)n);
            sh = calloc((size_t)n, 1);
            if (!h_recv || !h_cast || !sh)
                goto apply_cleanup;

            for (idx = 0; idx < n; idx++) {
                h_recv[idx] = (recv_heights[idx] >= 0)
                                  ? (float)recv_heights[idx]
                                  : -1000.f;
                h_cast[idx] = (cast_heights[idx] >= 0)
                                  ? (float)cast_heights[idx]
                                  : -1000.f;
            }

            /* Sun angle 0–180°: elevation = 90 − |angle − 90|.
             * 90° → vertical-only; 0°/180° → horizon (long shadows);
             * angle < 90 casts toward −X, angle > 90 toward +X. */
            {
                const float angle = clamp(filter->sun_angle_deg, 0.f, 180.f);
                const float elev_deg = 90.f - fabsf(angle - 90.f);
                const int shadow_range = max(8, max(gw, gh) / 4);
                const int dir = (angle <= 90.f) ? -1 : 1;

                if (elev_deg >= 89.5f) {
                    for (idx = 0; idx < n; idx++) {
                        if (h_recv[idx] < -500.f)
                            continue;
                        if (h_cast[idx] > h_recv[idx])
                            sh[idx] = 255;
                    }
                } else {
                    const float elev_rad =
                        elev_deg * (float)(M_PI / 180.0);
                    const float sun_step = max(tanf(elev_rad), 1e-4f);

                    for (y = 0; y < gh; y++) {
                        for (x = 0; x < gw; x++) {
                            idx = y * gw + x;
                            if (h_recv[idx] < -500.f)
                                continue;
                            float shadowCheckValue = h_recv[idx] + sun_step;
                            for (int shadowIter = 1, octaveIndex = 1;
                                 octaveIndex < shadow_range;
                                 shadowIter++, octaveIndex++,
                                 shadowCheckValue += sun_step) {
                                int sy = y + dir * (shadowIter >> 1);
                                int sx = x + dir * octaveIndex;
                                if (filter->wrap_shadows) {
                                    sy = wrap_coord(sy, gh);
                                    sx = wrap_coord(sx, gw);
                                } else if (sx < 0 || sx >= gw || sy < 0 ||
                                           sy >= gh) {
                                    break;
                                }
                                if (h_cast[sy * gw + sx] > shadowCheckValue) {
                                    sh[idx] = 255;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (filter->do_smoothing) {
                const int r = clamp(filter->shadow_blur_blocks, 0, 16);
                if (r > 0) {
                    sh_tmp = malloc((size_t)n);
                    if (sh_tmp) {
                        shadow_box_blur(sh_tmp, sh, gw, gh, r,
                                        filter->wrap_shadows);
                        memcpy(sh, sh_tmp, (size_t)n);
                        free(sh_tmp);
                        sh_tmp = NULL;
                    }
                }
            }

            iter = volume_get_iterator(layer->volume,
                                       VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
            for (y = 0; y < gh; y++) {
                pos[1] = y + start_pos[1];
                for (x = 0; x < gw; x++) {
                    idx = y * gw + x;
                    if (recv_heights[idx] < 0)
                        continue;
                    pos[0] = x + start_pos[0];
                    pos[2] = recv_heights[idx] + start_pos[2];
                    volume_get_at(layer->volume, &iter, pos, col);
                    if (!col[3])
                        continue;
                    {
                        float t = (float)sh[idx] / 255.f;
                        float mult = 1.f - t * filter->strength;
                        adjust_colour_brightness(col, mult);
                        volume_set_at(layer->volume, &iter, pos, col);
                    }
                }
            }

        apply_cleanup:
            free(h_recv);
            free(h_cast);
            free(sh);
            free(sh_tmp);
            free(recv_heights);
            free(cast_heights);
            if (casters)
                volume_delete(casters);
        }
    apply_end:
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_shadows_from_sun_t *filter = (void *)filter_;
    filter->strength = 0.3f;
    filter->sun_angle_deg = 66.f;
    filter->wrap_shadows = false;
    filter->do_smoothing = true;
    filter->shadow_blur_blocks = 2;
    filter->include_current_layer = true;
}

FILTER_REGISTER(shadows_from_sun, filter_shadows_from_sun_t,
                .name = "Shadows (From Sun)",
                .menu = "effects",
                .submenu = "lighting",
                .on_open = on_open,
                .gui_fn = gui,
                .panel_width = 450, )
