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
#include "utils/color_stats.h"

/*
 * Filter to reduce the number of distinct colours in layer volumes.
 */

enum {
    REDUCE_METHOD_MEDIAN_CUT = 0,
    REDUCE_METHOD_UNIFORM = 1,
};

typedef struct {
    filter_t filter;
    int method;
    int nb_colors;
    int uniform_step;
    bool analysis_valid;
    bool analysis_open;
    color_stats_breakdown_t analysis;
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

static void apply_median_cut(filter_reduce_colors_t *filter)
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

static void apply_uniform(filter_reduce_colors_t *filter)
{
    layer_t *layer;
    int step;

    step = clamp(filter->uniform_step, 1, 255);
    filter->uniform_step = step;

    DL_FOREACH(goxel.image->layers, layer) {
        if (!layer_in_scope(filter, layer))
            continue;
        quantization_remap_volume_uniform(layer->volume, step);
    }
}

static void clear_analysis(filter_reduce_colors_t *filter)
{
    filter->analysis_valid = false;
    filter->analysis_open = false;
    color_stats_breakdown_clear(&filter->analysis);
}

static void gui_analysis_line(const char *label, int unique, int uniform,
                              bool show_uniform)
{
    if (show_uniform && uniform > 0)
        gui_text("%s: %d unique colours (%d after uniform)",
                 label, unique, uniform);
    else
        gui_text("%s: %d unique colours", label, unique);
}

static void on_open(filter_t *filter_)
{
    filter_reduce_colors_t *filter = (void *)filter_;
    filter->method = REDUCE_METHOD_MEDIAN_CUT;
    filter->nb_colors = 16;
    filter->uniform_step = 8;
    clear_analysis(filter);
}

static void on_close(filter_t *filter_)
{
    filter_reduce_colors_t *filter = (void *)filter_;
    color_stats_breakdown_clear(&filter->analysis);
}

static int gui(filter_t *filter_)
{
    filter_reduce_colors_t *filter = (void *)filter_;
    static const char *method_names[] = {
        "Quantization (median-cut)",
        "Quantization (uniform)",
    };
    bool has_layer;
    bool can_apply;
    int uniform_step;

    const char *help_text =
        "Reduce the number of distinct colours.  "
        "Median-cut builds a shared palette from layers in scope and maps "
        "voxels to the nearest palette colour.  "
        "Uniform snaps each RGB channel to a fixed step in 0..255.  "
        "One shared palette (median-cut) or step (uniform) applies to all "
        "layers in scope.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    has_layer = goxel.image && goxel.image->active_layer;
    can_apply = goxel.image && (!filter->filter.current_only || has_layer);

    gui_label_size_push(60);

    {
        bool prev_current_only = filter->filter.current_only;

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
        if (prev_current_only != filter->filter.current_only)
            clear_analysis(filter);
    }

    gui_label_size_push(90);

    {
        int prev_method = filter->method;

        gui_combo("Method", &filter->method, method_names,
                  ARRAY_SIZE(method_names));
        if (prev_method != filter->method)
            clear_analysis(filter);
    }

    gui_group_begin(NULL);
    if (filter->method == REDUCE_METHOD_MEDIAN_CUT) {
        gui_input_int("Colors", &filter->nb_colors, 2, 256);
    } else {
        gui_input_int("Variation", &filter->uniform_step, 1, 255);
    }
    gui_group_end();

    gui_label_size_pop();

    uniform_step = clamp(filter->uniform_step, 1, 255);

    if (gui_button("Analyse", -1, 0)) {
        bool per_layer = !filter->filter.current_only;
        int analyse_uniform_step = filter->method == REDUCE_METHOD_UNIFORM ?
                                   uniform_step : 0;

        image_analyse_color_stats(goxel.image, filter->filter.current_only,
                                  false, true, per_layer, per_layer,
                                  analyse_uniform_step, &filter->analysis);
        filter->analysis_valid = true;
        filter->analysis_open = true;
    }

    if (filter->analysis_valid) {
        bool show_uniform = filter->method == REDUCE_METHOD_UNIFORM;
        const color_stats_summary_t *total = &filter->analysis.total;
        bool force_open = filter->analysis_open;

        if (force_open)
            filter->analysis_open = false;

        if (gui_collapsing_header_force_open("Analysis", force_open)) {
            if (filter->filter.current_only) {
                if (show_uniform && total->uniform_colors > 0)
                    gui_text("Unique colours: %d (%d after uniform)",
                             total->unique_colors, total->uniform_colors);
                else
                    gui_text("Unique colours: %d", total->unique_colors);
            } else {
                int i;

                gui_analysis_line("Total", total->unique_colors,
                                  total->uniform_colors, show_uniform);
                for (i = 0; i < filter->analysis.layer_count; i++) {
                    gui_analysis_line(filter->analysis.layers[i].name,
                                      filter->analysis.layers[i].stats.unique_colors,
                                      filter->analysis.layers[i].stats.uniform_colors,
                                      show_uniform);
                }
            }
        }
    }

    gui_enabled_begin(can_apply);
    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        if (filter->method == REDUCE_METHOD_MEDIAN_CUT)
            apply_median_cut(filter);
        else
            apply_uniform(filter);
    }
    gui_enabled_end();
    gui_alert_if_disabled_clicked(can_apply, "No layer selected",
                                  "Select a layer first.");

    gui_label_size_pop();
    return 0;
}

FILTER_REGISTER(reduce_colors, filter_reduce_colors_t,
    .name = "Reduce colors",
    .menu = "image",
    .on_open = on_open,
    .on_close = on_close,
    .panel_width = (GUI_PANEL_WIDTH_NORMAL * 3) / 2,
    .gui_fn = gui,
)
