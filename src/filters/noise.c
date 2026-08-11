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

/*
 * Filter to apply brush-style colour noise to layer voxels.
 */

typedef struct {
    filter_t filter;
    int noise_intensity;
    int noise_saturation;
    int noise_coverage;
} filter_noise_t;

/* Wrap voxel coords the same way volume_op feeds uniform_noise. */
static float noise_coord(int w)
{
    int m = w % NOISE_TEXTURE_SIZE;
    if (m < 0)
        m += NOISE_TEXTURE_SIZE;
    return (float)m;
}

static void apply_noise_to_color(uint8_t col[4], int x, int y, int z,
                                 int intensity, int saturation, int coverage)
{
    float noise_value;
    int noise_col[3];

    if (intensity <= 0 || coverage <= 0)
        return;
    noise_value = uniform_noise(noise_coord(x), noise_coord(y), noise_coord(z));
    if (noise_value >= (float)coverage / 100.0f)
        return;
    noise_col[0] = col[0];
    noise_col[1] = col[1];
    noise_col[2] = col[2];
    blend_with_noise_alpha(noise_col, noise_value, (float)intensity,
                           (float)saturation, noise_col);
    col[0] = (uint8_t)noise_col[0];
    col[1] = (uint8_t)noise_col[1];
    col[2] = (uint8_t)noise_col[2];
}

static void noise_layer(filter_noise_t *filter, layer_t *layer)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t color[4];
    int intensity = clamp(filter->noise_intensity, 0, 100);
    int saturation = clamp(filter->noise_saturation, 0, 100);
    int coverage = clamp(filter->noise_coverage, 0, 100);

    if (!layer || !layer->volume || volume_is_empty(layer->volume))
        return;
    if (layer->shape || layer->image)
        return;

    filter->noise_intensity = intensity;
    filter->noise_saturation = saturation;
    filter->noise_coverage = coverage;

    iter = volume_get_iterator(layer->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(layer->volume, &iter, pos, color);
        if (!color[3])
            continue;
        apply_noise_to_color(color, pos[0], pos[1], pos[2],
                             intensity, saturation, coverage);
        volume_set_at(layer->volume, &iter, pos, color);
    }
}

static void on_open(filter_t *filter_)
{
    filter_noise_t *filter = (void *)filter_;
    /* Match painter defaults from goxel_init. */
    filter->noise_intensity = 5;
    filter->noise_saturation = 5;
    filter->noise_coverage = 100;
}

static int gui(filter_t *filter_)
{
    filter_noise_t *filter = (void *)filter_;
    layer_t *layer;
    bool has_layer;
    bool can_apply;

    const char *help_text =
        "Applies brush-style colour noise to voxels.  "
        "Intensity, saturation, and coverage match the brush Noise controls.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    has_layer = goxel.image && goxel.image->active_layer;

    {
        if (!has_layer)
            filter->filter.current_only = false;
        gui_enabled_begin(has_layer);
        gui_checkbox(
            "Current layer only",
            &filter->filter.current_only,
            "If checked, only the current layer and its children "
            "(recursively) are noised.\n"
            "If unchecked, voxels on all layers will be noised.");
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }

    gui_group_begin(NULL);
    {
        int intensity = filter->noise_intensity;
        int saturation = filter->noise_saturation;
        int coverage = filter->noise_coverage;
        if (gui_input_int("Intensity", &intensity, 0, 100))
            filter->noise_intensity = clamp(intensity, 0, 100);
        if (gui_input_int("Saturation", &saturation, 0, 100))
            filter->noise_saturation = clamp(saturation, 0, 100);
        if (gui_input_int("Coverage", &coverage, 0, 100))
            filter->noise_coverage = clamp(coverage, 0, 100);
        if (gui_button("Reset", 0, 0)) {
            /* Match tool_gui_noise Reset values. */
            filter->noise_intensity = 20;
            filter->noise_saturation = 5;
            filter->noise_coverage = 100;
        }
    }
    gui_group_end();

    can_apply = goxel.image &&
                (!filter->filter.current_only || has_layer);
    gui_enabled_begin(can_apply);
    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        DL_FOREACH(goxel.image->layers, layer) {
            if (filter->filter.current_only &&
                !layer_in_active_subtree(goxel.image, layer))
                continue;
            noise_layer(filter, layer);
        }
    }
    gui_enabled_end();
    gui_alert_if_disabled_clicked(can_apply, "No layer selected",
                                  "Select a layer first.");

    return 0;
}

FILTER_REGISTER(noise, filter_noise_t,
                .name = "Noise",
                .menu = "image",
                .on_open = on_open,
                .gui_fn = gui, )
