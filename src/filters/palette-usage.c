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

#include <stdio.h>

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
    int max_colors;
    bool current_layer_only;
    char palette_name[256];
    char name_at_conflict[256];
    char status_msg[256];

    bool analysis_valid;
    int blocks_analysed;
    int unique_colors;
    int colors_above_threshold;

    color_stat_entry_t *sorted;
    int sorted_count;

    bool overwrite_saved;
    bool show_overwrite;
    char overwrite_filename[256];
    char gpl_display_name[128];
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

static void trim_inplace(char *s)
{
    char *end;
    size_t len;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        memmove(s, s + 1, strlen(s) + 1);
    len = strlen(s);
    end = s + len;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r'))
        end--;
    *end = '\0';
}

static void sanitize_stem(char *s)
{
    char *p;

    for (p = s; *p; p++) {
        if (*p == '/' || *p == '\\')
            *p = '_';
    }
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
    int i, n, cap, above, max_keep;

    free(filter->sorted);
    filter->sorted = NULL;
    filter->sorted_count = 0;
    filter->analysis_valid = false;
    filter->blocks_analysed = 0;
    filter->unique_colors = 0;
    filter->colors_above_threshold = 0;
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

    max_keep = filter->max_colors;
    if (max_keep > 0 && n > max_keep) {
        cap = max_keep;
        filter->sorted = realloc(filter->sorted, cap * sizeof(*filter->sorted));
        n = cap;
    }
    filter->sorted_count = n;
    filter->analysis_valid = true;
}

static int save_gpl(const char *path, const char *gpl_name,
                    const color_stat_entry_t *entries, int n)
{
    FILE *fp;
    int i, r, g, b;
    char entry_name[64];

    fp = fopen(path, "w");
    if (!fp)
        return -1;

    fprintf(fp, "GIMP Palette\nName: %s\nColumns: 16\n#\n", gpl_name);
    for (i = 0; i < n; i++) {
        r = entries[i].color[0];
        g = entries[i].color[1];
        b = entries[i].color[2];
        snprintf(entry_name, sizeof(entry_name), "#%02x%02x%02x", r, g, b);
        fprintf(fp, "%3d %3d %3d    %s\n", r, g, b, entry_name);
    }
    fclose(fp);
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_palette_usage_t *filter = (void *)filter_;

    filter->usage_threshold = 5;
    filter->max_colors = -1;
    filter->current_layer_only = false;
    filter->palette_name[0] = '\0';
    filter->status_msg[0] = '\0';
    filter->name_at_conflict[0] = '\0';
    filter->analysis_valid = false;
    filter->sorted = NULL;
    filter->sorted_count = 0;
    filter->overwrite_saved = false;
    filter->show_overwrite = false;
    filter->overwrite_filename[0] = '\0';
    filter->gpl_display_name[0] = '\0';
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
    int mc = filter->max_colors;
    const char *help_text =
        "Analyse counts RGBA voxel colours on plain voxel layers (optionally "
        "only the active layer). Results are sorted by usage. When usage "
        "threshold is not -1, only colours meeting that count are kept. Save "
        "writes a GIMP .gpl file; Add to current palette appends analysed "
        "colours to the palette selected in the palette panel.";

    goxel_set_help_text(help_text);

    if (filter->show_overwrite && filter->name_at_conflict[0] &&
        strcmp(filter->palette_name, filter->name_at_conflict) != 0) {
        filter->show_overwrite = false;
        filter->overwrite_saved = false;
        filter->overwrite_filename[0] = '\0';
        filter->name_at_conflict[0] = '\0';
    }

    gui_input_int("Usage threshold", &ut, 0, 0);
    gui_input_int("Max # of colours", &mc, 0, 0);
    filter->usage_threshold = ut;
    filter->max_colors = mc;

    gui_checkbox("Current layer only", &filter->current_layer_only,
                 "If checked, only the active layer is scanned (must be a "
                 "plain voxel layer).");

    if (gui_button("Analyse", -1, 0))
        run_analyse(filter);

    if (filter->analysis_valid) {
        gui_text("Blocks analysed: %d", filter->blocks_analysed);
        gui_text("Unique colours: %d", filter->unique_colors);
        if (filter->usage_threshold != -1) {
            gui_text("Colours above usage threshold: %d",
                     filter->colors_above_threshold);
        }
    }

    if (gui_button("Add to current palette", -1, 0)) {
        int before;
        int i;
        char entry_name[64];

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
            for (i = 0; i < filter->sorted_count; i++) {
                uint8_t *c = filter->sorted[i].color;

                snprintf(entry_name, sizeof(entry_name), "#%02x%02x%02x",
                         c[0], c[1], c[2]);
                palette_insert(goxel.palette, filter->sorted[i].color,
                               entry_name);
            }
            snprintf(filter->status_msg, sizeof(filter->status_msg),
                     "Added %d new swatches (%d already in palette).",
                     goxel.palette->size - before,
                     filter->sorted_count - (goxel.palette->size - before));
        }
    }

    if (filter->status_msg[0])
        gui_text("%s", filter->status_msg);

    gui_input_text("Palette name", filter->palette_name,
                   sizeof(filter->palette_name));

    if (gui_button("Save palette", -1, 0)) {
        char stem[256];
        char path[512];
        char fname[256];
        const char *udir;
        FILE *probe;

        filter->save_error[0] = '\0';
        snprintf(stem, sizeof(stem), "%s", filter->palette_name);
        trim_inplace(stem);
        sanitize_stem(stem);
        if (!stem[0]) {
            snprintf(filter->save_error, sizeof(filter->save_error),
                     "Enter a palette name.");
        } else {
            if (str_endswith(stem, ".gpl")) {
                snprintf(fname, sizeof(fname), "%s", stem);
            } else {
                /* 251 + strlen(".gpl") + '\0' fits in fname[256]. */
                snprintf(fname, sizeof(fname), "%.251s.gpl", stem);
            }

            snprintf(filter->gpl_display_name, sizeof(filter->gpl_display_name),
                     "%.127s", stem);
            if (str_endswith(filter->gpl_display_name, ".gpl"))
                filter->gpl_display_name[strlen(filter->gpl_display_name) - 4] =
                    '\0';

            udir = sys_get_user_dir();
            if (!udir) {
                snprintf(filter->save_error, sizeof(filter->save_error),
                         "User data directory is not available.");
            } else if (!filter->analysis_valid || filter->sorted_count == 0) {
                snprintf(filter->save_error, sizeof(filter->save_error),
                         "Run Analyse first (no colours to save).");
            } else {
                snprintf(path, sizeof(path), "%s/palettes/%s", udir, fname);
                sys_make_dir(path);
                probe = fopen(path, "rb");
                if (probe) {
                    fclose(probe);
                    if (!filter->overwrite_saved) {
                        filter->show_overwrite = true;
                        snprintf(filter->overwrite_filename,
                                 sizeof(filter->overwrite_filename), "%s",
                                 fname);
                        snprintf(filter->name_at_conflict,
                                 sizeof(filter->name_at_conflict), "%s",
                                 filter->palette_name);
                        snprintf(filter->save_error,
                                 sizeof(filter->save_error),
                                 "File exists. Enable overwrite to replace it.");
                    } else {
                        if (save_gpl(path, filter->gpl_display_name,
                                     filter->sorted, filter->sorted_count) != 0) {
                            LOG_E("Could not write %s", path);
                            snprintf(filter->save_error,
                                     sizeof(filter->save_error),
                                     "Could not write palette file.");
                        } else {
                            goxel.palette = palette_reload_all(
                                &goxel.palettes, filter->gpl_display_name);
                            if (!goxel.palette)
                                goxel.palette = goxel.palettes;
                            filter->show_overwrite = false;
                            filter->overwrite_saved = false;
                            filter->overwrite_filename[0] = '\0';
                            filter->name_at_conflict[0] = '\0';
                        }
                    }
                } else {
                    if (save_gpl(path, filter->gpl_display_name, filter->sorted,
                                 filter->sorted_count) != 0) {
                        LOG_E("Could not write %s", path);
                        snprintf(filter->save_error, sizeof(filter->save_error),
                                 "Could not write palette file.");
                    } else {
                        goxel.palette = palette_reload_all(
                            &goxel.palettes, filter->gpl_display_name);
                        if (!goxel.palette)
                            goxel.palette = goxel.palettes;
                        filter->show_overwrite = false;
                        filter->overwrite_saved = false;
                        filter->name_at_conflict[0] = '\0';
                    }
                }
            }
        }
    }

    if (filter->save_error[0])
        gui_text("%s", filter->save_error);

    if (filter->show_overwrite) {
        char lbl[320];

        snprintf(lbl, sizeof(lbl), "Overwrite saved %s",
                 filter->overwrite_filename);
        gui_checkbox(lbl, &filter->overwrite_saved, NULL);
    }

    return 0;
}

FILTER_REGISTER(palette_usage, filter_palette_usage_t,
                .name = "Palette - usage from layers",
                .on_open = on_open,
                .on_close = on_close,
                .panel_width = 275,
                .gui_fn = gui, )
