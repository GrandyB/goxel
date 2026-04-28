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
#include "palette.h"

typedef struct color_stat_hash {
    int rgba_key;
    uint8_t color[4];
    int count;
    UT_hash_handle hh;
} color_stat_hash_t;

typedef struct {
    uint8_t color[4];
    int count;
} color_stat_entry_t;

typedef struct {
    filter_t filter;
    int usage_threshold;
    bool current_layer_only;
    bool condense_similar;
    int degree_diff;
    char status_msg[256];

    bool analysis_valid;
    int blocks_analysed;
    int unique_colors;
    int colors_above_threshold;
    int colors_after_condense;

    color_stat_entry_t *sorted;
    int sorted_count;

    char save_error[256];
} filter_palette_usage_t;

static int pack_rgba(const uint8_t c[4])
{
    return (int)((uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                 ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24));
}

static void hash_clear(color_stat_hash_t **head)
{
    color_stat_hash_t *cur, *tmp;

    HASH_ITER(hh, *head, cur, tmp) {
        HASH_DEL(*head, cur);
        free(cur);
    }
    *head = NULL;
}

static int color_stat_cmp(const void *a, const void *b)
{
    const color_stat_entry_t *x = a;
    const color_stat_entry_t *y = b;
    int kx, ky;

    if (x->count != y->count)
        return y->count - x->count;
    kx = pack_rgba(x->color);
    ky = pack_rgba(y->color);
    if (kx < ky)
        return -1;
    if (kx > ky)
        return 1;
    return 0;
}

/** True if every channel differs by at most `diff` (Chebyshev on RGBA). */
static bool colors_within_degree_diff(const uint8_t a[4], const uint8_t b[4],
                                      int diff)
{
    int d0 = abs((int)a[0] - (int)b[0]);
    int d1 = abs((int)a[1] - (int)b[1]);
    int d2 = abs((int)a[2] - (int)b[2]);
    int d3 = abs((int)a[3] - (int)b[3]);
    int m = d0;

    if (d1 > m)
        m = d1;
    if (d2 > m)
        m = d2;
    if (d3 > m)
        m = d3;
    return m <= diff;
}

/**
 * Colours left if we walk popularity order and drop any later colour within
 * `degree_diff` of an earlier (more popular) kept colour.
 */
static int count_after_condensing(const color_stat_entry_t *sorted, int n,
                                  int degree_diff)
{
    bool *skipped;
    int i, j, count = 0;

    if (n <= 0)
        return 0;
    skipped = calloc((size_t)n, sizeof(bool));
    if (!skipped)
        return 0;
    for (i = 0; i < n; i++) {
        if (skipped[i])
            continue;
        count++;
        for (j = i + 1; j < n; j++) {
            if (skipped[j])
                continue;
            if (colors_within_degree_diff(sorted[i].color, sorted[j].color,
                                          degree_diff))
                skipped[j] = true;
        }
    }
    free(skipped);
    return count;
}

/**
 * Append analysed swatches to the current palette. When condense_similar is
 * set, uses the same greedy merge as count_after_condensing; otherwise adds
 * every entry in sorted. Returns the number of insert attempts (one per
 * representative colour), or -1 on allocation failure.
 */
static int append_analysed_swatches_to_palette(filter_palette_usage_t *filter)
{
    int n = filter->sorted_count;
    color_stat_entry_t *sorted = filter->sorted;
    bool *skipped = NULL;
    int i, j;
    int attempted = 0;
    char entry_name[64];

    if (n <= 0 || !sorted)
        return 0;

    if (filter->condense_similar) {
        skipped = calloc((size_t)n, sizeof(bool));
        if (!skipped) {
            snprintf(filter->save_error, sizeof(filter->save_error),
                     "Out of memory.");
            return -1;
        }
    }

    for (i = 0; i < n; i++) {
        if (skipped && skipped[i])
            continue;
        attempted++;
        snprintf(entry_name, sizeof(entry_name), "#%02x%02x%02x",
                 sorted[i].color[0], sorted[i].color[1], sorted[i].color[2]);
        palette_insert(goxel.palette, sorted[i].color, entry_name);
        if (skipped) {
            for (j = i + 1; j < n; j++) {
                if (skipped[j])
                    continue;
                if (colors_within_degree_diff(sorted[i].color, sorted[j].color,
                                              filter->degree_diff))
                    skipped[j] = true;
            }
        }
    }
    free(skipped);
    return attempted;
}

static void add_volume_layer_counts(filter_palette_usage_t *filter,
                                    layer_t *layer,
                                    color_stat_hash_t **colors)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    color_stat_hash_t *el;
    int key;

    if (!layer || !layer_is_volume(layer))
        return;
    iter = volume_get_iterator(layer->volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(layer->volume, &iter, pos, v);
        if (v[3] == 0)
            continue;
        filter->blocks_analysed++;
        key = pack_rgba(v);
        HASH_FIND_INT(*colors, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            el->rgba_key = key;
            memcpy(el->color, v, 4);
            el->count = 1;
            HASH_ADD_INT(*colors, rgba_key, el);
        } else {
            el->count++;
        }
    }
}

static void run_analyse(filter_palette_usage_t *filter)
{
    layer_t *layer;
    color_stat_hash_t *colors = NULL;
    color_stat_hash_t *el, *tmp;
    int i, n, above;

    free(filter->sorted);
    filter->sorted = NULL;
    filter->sorted_count = 0;
    filter->analysis_valid = false;
    filter->blocks_analysed = 0;
    filter->unique_colors = 0;
    filter->colors_above_threshold = 0;
    filter->colors_after_condense = 0;
    filter->status_msg[0] = '\0';

    if (filter->current_layer_only) {
        add_volume_layer_counts(filter, goxel.image->active_layer, &colors);
    } else {
        DL_FOREACH(goxel.image->layers, layer) {
            if (!layer_is_volume(layer))
                continue;
            add_volume_layer_counts(filter, layer, &colors);
        }
    }

    filter->unique_colors = HASH_COUNT(colors);
    if (filter->usage_threshold != -1) {
        above = 0;
        HASH_ITER(hh, colors, el, tmp) {
            if (el->count >= filter->usage_threshold)
                above++;
        }
        filter->colors_above_threshold = above;
    }

    n = HASH_COUNT(colors);
    if (n == 0) {
        hash_clear(&colors);
        filter->analysis_valid = true;
        return;
    }

    filter->sorted = calloc(n, sizeof(*filter->sorted));
    i = 0;
    HASH_ITER(hh, colors, el, tmp) {
        memcpy(filter->sorted[i].color, el->color, 4);
        filter->sorted[i].count = el->count;
        i++;
    }
    hash_clear(&colors);

    qsort(filter->sorted, n, sizeof(*filter->sorted), color_stat_cmp);

    if (filter->usage_threshold != -1) {
        int w = 0;

        for (i = 0; i < n; i++) {
            if (filter->sorted[i].count < filter->usage_threshold)
                continue;
            if (w != i)
                filter->sorted[w] = filter->sorted[i];
            w++;
        }
        n = w;
        if (n == 0) {
            free(filter->sorted);
            filter->sorted = NULL;
        } else {
            filter->sorted =
                realloc(filter->sorted, n * sizeof(*filter->sorted));
        }
    }

    if (n > 0) {
        filter->colors_after_condense =
            count_after_condensing(filter->sorted, n, filter->degree_diff);
    } else {
        filter->colors_after_condense = 0;
    }

    filter->sorted_count = n;
    filter->analysis_valid = true;
}

static void on_open(filter_t *filter_)
{
    filter_palette_usage_t *filter = (void *)filter_;

    filter->usage_threshold = 5;
    filter->current_layer_only = false;
    filter->condense_similar = false;
    filter->degree_diff = 5;
    filter->status_msg[0] = '\0';
    filter->analysis_valid = false;
    filter->sorted = NULL;
    filter->sorted_count = 0;
    filter->save_error[0] = '\0';
}

static void on_close(filter_t *filter_)
{
    filter_palette_usage_t *filter = (void *)filter_;

    free(filter->sorted);
    filter->sorted = NULL;
}

static int gui(filter_t *filter_)
{
    filter_palette_usage_t *filter = (void *)filter_;
    int ut = filter->usage_threshold;
    const char *help_text =
        "Analyse counts RGBA voxel colours on plain voxel layers (optionally "
        "only the active layer). Results are sorted by usage. When usage "
        "threshold is not -1, only colours meeting that count are kept. Add to "
        "current palette appends analysed colours to the palette selected in "
        "the palette panel.";

    goxel_set_help_text(help_text);

    gui_label_size_push(130.0f);
    gui_input_int("Usage threshold", &ut, 0, 0);
    gui_label_size_pop();
    filter->usage_threshold = ut;

    gui_checkbox("Current layer only", &filter->current_layer_only,
                 "If checked, only the active layer is scanned (must be a "
                 "plain voxel layer).");

    gui_checkbox("Condense similar colours", &filter->condense_similar,
                 "When reporting, merge colours within the degree difference "
                 "into the more popular swatch (per-channel max delta).");
    if (filter->condense_similar) {
        int dd = filter->degree_diff;

        gui_input_int("Degree diff", &dd, 0, 255);
        filter->degree_diff = dd;
    }

    if (gui_button("Analyse", -1, 0))
        run_analyse(filter);

    if (filter->analysis_valid) {
        gui_text("Blocks analysed: %d", filter->blocks_analysed);
        gui_text("Unique colours: %d", filter->unique_colors);
        if (filter->usage_threshold != -1) {
            gui_text("Colours above usage threshold: %d",
                     filter->colors_above_threshold);
        }
        if (filter->condense_similar) {
            gui_text("Colours after condensing similar: %d",
                     filter->colors_after_condense);
        }
    }

    if (gui_button("Add to current palette", -1, 0)) {
        int before;
        int attempted;

        filter->save_error[0] = '\0';
        filter->status_msg[0] = '\0';
        if (!goxel.palette) {
            snprintf(filter->save_error, sizeof(filter->save_error),
                     "No palette is selected.");
        } else if (!filter->analysis_valid || filter->sorted_count == 0) {
            snprintf(filter->save_error, sizeof(filter->save_error),
                     "Run Analyse first (no colours to add).");
        } else {
            before = goxel.palette->size;
            attempted = append_analysed_swatches_to_palette(filter);
            if (attempted >= 0) {
                snprintf(filter->status_msg, sizeof(filter->status_msg),
                         "Added %d new swatches (%d already in palette).",
                         goxel.palette->size - before,
                         attempted - (goxel.palette->size - before));
                if (goxel.palette->size > before &&
                    palette_save_user_gpl(goxel.palette) != 0) {
                    gui_alert("Palette",
                              "Could not save the palette to your palettes "
                              "folder.");
                }
            }
        }
    }

    if (filter->status_msg[0])
        gui_text("%s", filter->status_msg);

    if (filter->save_error[0])
        gui_text("%s", filter->save_error);

    return 0;
}

FILTER_REGISTER(palette_usage, filter_palette_usage_t,
                .name = "Palette - usage from layers",
                .on_open = on_open,
                .on_close = on_close,
                .panel_width = 325,
                .gui_fn = gui, )
