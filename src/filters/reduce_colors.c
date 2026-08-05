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
 * Filter to reduce the number of distinct colours in layer volumes.
 */

enum {
    REDUCE_METHOD_QUANTIZATION = 0,
};

typedef struct {
    filter_t filter;
    int method;
    int nb_colors;
} filter_reduce_colors_t;

static bool layer_in_scope(const filter_reduce_colors_t *filter,
                           const layer_t *layer)
{
    if (!layer || !layer->volume)
        return false;
    if (filter->filter.current_only &&
        !layer_in_active_subtree(goxel.image, layer))
        return false;
    return true;
}

static void apply_quantization(filter_reduce_colors_t *filter)
{
    layer_t *layer;
    volume_t *merged;
    uint8_t palette[256][4];
    int nb;

    nb = clamp(filter->nb_colors, 2, 256);
    filter->nb_colors = nb;
    memset(palette, 0, sizeof(palette));

    merged = volume_new();
    DL_FOREACH(goxel.image->layers, layer) {
        if (!layer_in_scope(filter, layer))
            continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }

    quantization_gen_palette(merged, nb, palette, NULL, 0);
    volume_delete(merged);

    DL_FOREACH(goxel.image->layers, layer) {
        if (!layer_in_scope(filter, layer))
            continue;
        quantization_remap_volume(layer->volume, palette, nb);
    }
}

static void on_open(filter_t *filter_)
{
    filter_reduce_colors_t *filter = (void *)filter_;
    filter->method = REDUCE_METHOD_QUANTIZATION;
    filter->nb_colors = 16;
}

static int gui(filter_t *filter_)
{
    filter_reduce_colors_t *filter = (void *)filter_;
    static const char *method_names[] = {"Quantization"};
    bool has_layer;

    const char *help_text =
        "Reduce the number of distinct colours.  "
        "Quantization uses median-cut when there are more unique colours "
        "than the target count; otherwise colours are left unchanged.  "
        "One shared palette is built from all layers in scope.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    has_layer = goxel.image && goxel.image->active_layer;

    gui_label_size_push(60);

    {
        if (!has_layer)
            filter->filter.current_only = false;
        gui_enabled_begin(has_layer);
        gui_checkbox(
            "Current layer only",
            &filter->filter.current_only,
            "If checked, only the current layer and its children "
            "(recursively) are reduced.\n"
            "If unchecked, voxels on all layers will be reduced.");
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }

    gui_combo("Method", &filter->method, method_names,
              ARRAY_SIZE(method_names));

    gui_group_begin(NULL);
    gui_input_int("Colors", &filter->nb_colors, 2, 256);
    gui_group_end();

    gui_enabled_begin(has_layer);
    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        if (filter->method == REDUCE_METHOD_QUANTIZATION)
            apply_quantization(filter);
    }
    gui_enabled_end();
    gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                  "Select a layer first.");

    gui_label_size_pop();
    return 0;
}

FILTER_REGISTER(reduce_colors, filter_reduce_colors_t,
    .name = "Reduce colors",
    .menu = "image",
    .on_open = on_open,
    .gui_fn = gui,
)
