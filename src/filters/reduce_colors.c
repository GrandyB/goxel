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
    bool include_recent_colors;
    char include_palette_name[128];
    bool analysis_valid;
    bool analysis_open;
    color_stats_breakdown_t analysis;
    bool analysis_show_recent;
    bool analysis_show_palette;
    char analysis_palette_name[128];
    int analysis_recent_colors;
    int analysis_recent_blocks;
    int analysis_palette_colors;
    int analysis_palette_blocks;
} filter_reduce_colors_t;

typedef struct {
    uint8_t colors[256][4];
    int n;
    int n_recent;
    int blocks_recent;
    int n_palette;
    int blocks_palette;
} forced_colors_t;

static int pack_rgba(const uint8_t c[4])
{
    return (int)((uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                 ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24));
}

static bool forced_has_opaque_rgb(const forced_colors_t *forced,
                                  const uint8_t rgb[3])
{
    int i;

    for (i = 0; i < forced->n; i++) {
        if (forced->colors[i][3] != 255)
            continue;
        if (forced->colors[i][0] == rgb[0] &&
            forced->colors[i][1] == rgb[1] &&
            forced->colors[i][2] == rgb[2])
            return true;
    }
    return false;
}

static int usage_count_for_color(color_stat_hash_t *usage, const uint8_t c[4])
{
    color_stat_hash_t *el;
    int key;

    if (!usage)
        return 0;
    key = pack_rgba(c);
    HASH_FIND_INT(usage, &key, el);
    return el ? el->count : 0;
}

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

/*
 * Build forced median-cut colours: optional recent-bar opaques, then in-use
 * entries from the selected user palette.  Dedupes by opaque RGB.  `usage`
 * supplies block counts and palette membership; may be NULL when only the
 * recent bar is enabled.
 */
static void collect_forced_colors(const filter_reduce_colors_t *filter,
                                  color_stat_hash_t *usage,
                                  forced_colors_t *out)
{
    const image_t *img = goxel.image;
    const palette_t *pal;
    uint8_t c[4];
    int i;

    memset(out, 0, sizeof(*out));
    if (!img)
        return;

    if (filter->include_recent_colors) {
        for (i = 0; i < img->recent_color_count; i++) {
            memcpy(c, img->recent_colors[i].color, 4);
            if (c[3] != 255)
                continue;
            if (forced_has_opaque_rgb(out, c))
                continue;
            if (out->n >= 256)
                break;
            memcpy(out->colors[out->n], c, 4);
            out->n++;
            out->n_recent++;
            out->blocks_recent += usage_count_for_color(usage, c);
        }
    }

    if (filter->include_palette_name[0] == '\0')
        return;

    pal = palette_find_by_name(goxel.palettes, filter->include_palette_name);
    if (!pal || palette_is_readonly(pal) || !usage)
        return;

    for (i = 0; i < pal->size; i++) {
        memcpy(c, pal->entries[i].color, 4);
        if (c[3] != 255)
            continue;
        if (usage_count_for_color(usage, c) <= 0)
            continue;
        if (forced_has_opaque_rgb(out, c))
            continue;
        if (out->n >= 256)
            break;
        memcpy(out->colors[out->n], c, 4);
        out->n++;
        out->n_palette++;
        out->blocks_palette += usage_count_for_color(usage, c);
    }
}

static void apply_median_cut(filter_reduce_colors_t *filter)
{
    layer_t *layer;
    volume_t *merged;
    uint8_t palette[256][4];
    color_stat_hash_t *usage = NULL;
    forced_colors_t forced;
    int nb, n_forced, rem, i;

    nb = clamp(filter->nb_colors, 2, 256);
    filter->nb_colors = nb;
    memset(palette, 0, sizeof(palette));

    merged = volume_new();
    DL_FOREACH(goxel.image->layers, layer) {
        if (!layer_in_scope(filter, layer))
            continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }

    /* Palette membership needs usage; recent-only apply does not. */
    if (filter->include_palette_name[0]) {
        image_collect_color_stats(goxel.image, filter->filter.current_only,
                                  false, false, &usage, NULL);
    }
    collect_forced_colors(filter, usage, &forced);
    color_stats_hash_clear(&usage);

    n_forced = forced.n;
    if (n_forced > nb)
        n_forced = nb;
    for (i = 0; i < n_forced; i++)
        memcpy(palette[i], forced.colors[i], 4);

    rem = nb - n_forced;
    if (rem > 0) {
        quantization_gen_palette(merged, rem, palette + n_forced,
                                 (const uint8_t (*)[4])forced.colors,
                                 n_forced);
    }
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
    filter->analysis_show_recent = false;
    filter->analysis_show_palette = false;
    filter->analysis_palette_name[0] = '\0';
    filter->analysis_recent_colors = 0;
    filter->analysis_recent_blocks = 0;
    filter->analysis_palette_colors = 0;
    filter->analysis_palette_blocks = 0;
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
    filter->include_recent_colors = false;
    filter->include_palette_name[0] = '\0';
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

    const char *bar_help =
        "Reduce distinct colours via median-cut or uniform quantization.";
    const char *help_text =
        "Reduce the number of distinct colours.  "
        "Median-cut builds a shared palette from layers in scope and maps "
        "voxels to the nearest palette colour.  "
        "Optionally reserve exact colours from the recent-colours bar and/or "
        "in-use entries from a user palette (those take slots from the colour "
        "budget first).  "
        "Uniform snaps each RGB channel to a fixed step in 0..255.  "
        "One shared palette (median-cut) or step (uniform) applies to all "
        "layers in scope.";
    goxel_set_help_text(bar_help);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    has_layer = goxel.image && goxel.image->active_layer;
    can_apply = goxel.image && (!filter->filter.current_only || has_layer);

    gui_label_size_push(120);

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

    gui_label_size_push(180);

    {
        int prev_method = filter->method;

        gui_combo("Method", &filter->method, method_names,
                  ARRAY_SIZE(method_names));
        if (prev_method != filter->method)
            clear_analysis(filter);
    }

    gui_group_begin(NULL);
    if (filter->method == REDUCE_METHOD_MEDIAN_CUT) {
        bool prev_recent = filter->include_recent_colors;
        char prev_palette[128];
        const char *preview;
        const palette_t *it;
        const palette_t *selected;

        gui_input_int("Colors", &filter->nb_colors, 2, 256);

        gui_checkbox(
            "Include recent colours",
            &filter->include_recent_colors,
            "Reserve opaque colours from the map recent-colours bar in the "
            "median-cut palette (they take slots from the colour budget).");
        if (prev_recent != filter->include_recent_colors)
            clear_analysis(filter);

        memcpy(prev_palette, filter->include_palette_name,
               sizeof(prev_palette));
        selected = filter->include_palette_name[0]
                       ? palette_find_by_name(goxel.palettes,
                                              filter->include_palette_name)
                       : NULL;
        if (selected && palette_is_readonly(selected))
            selected = NULL;
        if (!selected && filter->include_palette_name[0])
            filter->include_palette_name[0] = '\0';
        preview = selected ? selected->name : "None";

        if (gui_combo_begin("Include palette", preview)) {
            if (gui_combo_item("None", !selected))
                filter->include_palette_name[0] = '\0';
            DL_FOREACH(goxel.palettes, it) {
                if (palette_is_readonly(it))
                    continue;
                if (gui_combo_item(it->name, it == selected)) {
                    snprintf(filter->include_palette_name,
                             sizeof(filter->include_palette_name),
                             "%s", it->name);
                }
            }
            gui_combo_end();
        }
        if (strcmp(prev_palette, filter->include_palette_name) != 0)
            clear_analysis(filter);
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

        filter->analysis_show_recent = false;
        filter->analysis_show_palette = false;
        filter->analysis_palette_name[0] = '\0';
        filter->analysis_recent_colors = 0;
        filter->analysis_recent_blocks = 0;
        filter->analysis_palette_colors = 0;
        filter->analysis_palette_blocks = 0;

        if (filter->method == REDUCE_METHOD_MEDIAN_CUT &&
            (filter->include_recent_colors ||
             filter->include_palette_name[0])) {
            color_stat_hash_t *usage = NULL;
            forced_colors_t forced;

            /* Same flags as image_analyse_color_stats totals above. */
            image_collect_color_stats(goxel.image,
                                      filter->filter.current_only,
                                      false, true, &usage, NULL);
            collect_forced_colors(filter, usage, &forced);
            color_stats_hash_clear(&usage);

            filter->analysis_show_recent = filter->include_recent_colors;
            filter->analysis_recent_colors = forced.n_recent;
            filter->analysis_recent_blocks = forced.blocks_recent;
            if (filter->include_palette_name[0]) {
                filter->analysis_show_palette = true;
                snprintf(filter->analysis_palette_name,
                         sizeof(filter->analysis_palette_name),
                         "%s", filter->include_palette_name);
                filter->analysis_palette_colors = forced.n_palette;
                filter->analysis_palette_blocks = forced.blocks_palette;
            }
        }

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

            if (filter->method == REDUCE_METHOD_MEDIAN_CUT) {
                if (filter->analysis_show_recent)
                    gui_text("Recent colours bar: %d colours (%d blocks)",
                             filter->analysis_recent_colors,
                             filter->analysis_recent_blocks);
                if (filter->analysis_show_palette)
                    gui_text("Palette \"%s\": %d colours (%d out of %d blocks)",
                             filter->analysis_palette_name,
                             filter->analysis_palette_colors,
                             filter->analysis_palette_blocks,
                             total->voxels_analysed);
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
