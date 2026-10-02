/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

/* MagicaVoxel .vox (Trenchblocks) export settings / forced-palette window. */

#include "goxel.h"
#include "file_format.h"
#include "formats/vox_trenchblocks.h"
#include "utils/color_stats.h"

#include <string.h>

static const uint8_t TB_BORDER_RED[4] = {220, 40, 40, 255};
static const uint8_t TB_BORDER_GREY[4] = {140, 140, 140, 255};


typedef struct {
    int n_layers;
    int n_colors;
    uint8_t (*colors)[4];
} tb_cross_bucket_t;

typedef struct {
    int count;
    uint8_t (*colors)[4];
} tb_after_colors_t;

typedef struct {
    color_stats_breakdown_t layers;
    int *after_current; /* parallel to layers.layers[]; remaining uniques */
    tb_after_colors_t *after_lists;
    tb_cross_bucket_t *buckets;
    int nbuckets;
    bool valid;
    char palette_name[128];
    /* Cached map colour usage for grey borders (RGB packed, opaque only). */
    color_stat_hash_t *used_colors;
} tb_popup_state_t;

static bool *g_tb_layer_open = NULL;
static int g_tb_layer_open_count = 0;

static void tb_layer_open_ensure(int n)
{
    if (n == g_tb_layer_open_count)
        return;
    free(g_tb_layer_open);
    g_tb_layer_open = n > 0 ? calloc((size_t)n, sizeof(*g_tb_layer_open)) : NULL;
    g_tb_layer_open_count = n;
}

static void tb_layer_open_clear(void)
{
    free(g_tb_layer_open);
    g_tb_layer_open = NULL;
    g_tb_layer_open_count = 0;
}

static void tb_popup_clear_analysis(tb_popup_state_t *st)
{
    int i, n_layers;
    if (!st) return;
    n_layers = st->layers.layer_count;
    if (st->after_lists) {
        for (i = 0; i < n_layers; i++)
            free(st->after_lists[i].colors);
        free(st->after_lists);
        st->after_lists = NULL;
    }
    color_stats_breakdown_clear(&st->layers);
    free(st->after_current);
    st->after_current = NULL;
    if (st->buckets) {
        for (i = 0; i < st->nbuckets; i++)
            free(st->buckets[i].colors);
        free(st->buckets);
        st->buckets = NULL;
    }
    st->nbuckets = 0;
    st->valid = false;
}

static void tb_popup_clear_used(tb_popup_state_t *st)
{
    if (!st) return;
    color_stats_hash_clear(&st->used_colors);
}

static int tb_pack_rgb_key(const uint8_t c[3])
{
    return (int)((uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                 ((uint32_t)c[2] << 16) | (255u << 24));
}

static void tb_popup_refresh_used(tb_popup_state_t *st)
{
    const volume_t *vol;
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    color_stat_hash_t *el;
    int key;

    tb_popup_clear_used(st);
    if (!goxel.image) return;
    vol = goxel_get_layers_volume(goxel.image);
    if (!vol) return;
    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(vol, &iter, pos, v);
        if (!voxel_is_solid(v)) continue;
        v[3] = 255;
        key = tb_pack_rgb_key(v);
        HASH_FIND_INT(st->used_colors, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            if (!el) return;
            el->rgba_key = key;
            memcpy(el->color, v, 4);
            el->count = 1;
            HASH_ADD_INT(st->used_colors, rgba_key, el);
        }
    }
}

static bool tb_color_used_in_map(tb_popup_state_t *st, const uint8_t c[4])
{
    color_stat_hash_t *el;
    int key;
    if (!st || c[3] != 255) return false;
    key = tb_pack_rgb_key(c);
    HASH_FIND_INT(st->used_colors, &key, el);
    return el != NULL;
}

static int tb_collect_subtree_hash(const image_t *img, const layer_t *root,
                                   color_stat_hash_t **out)
{
    layer_t *layer;
    volume_t *merged;
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    color_stat_hash_t *el;
    int key;

    *out = NULL;
    if (!img || !root) return 0;
    merged = volume_new();
    DL_FOREACH(img->layers, layer) {
        if (!layer->volume) continue;
        if (!layer_effectively_visible(img, layer)) continue;
        if (!layer_is_volume(layer)) continue;
        if (!layer_is_ancestor(img, root, layer)) continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }
    iter = volume_get_iterator(merged,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(merged, &iter, pos, v);
        if (!voxel_is_solid(v)) continue;
        v[3] = 255;
        key = tb_pack_rgb_key(v);
        HASH_FIND_INT(*out, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            if (!el) {
                color_stats_hash_clear(out);
                volume_delete(merged);
                return -1;
            }
            el->rgba_key = key;
            memcpy(el->color, v, 4);
            el->count = 1;
            HASH_ADD_INT(*out, rgba_key, el);
        } else {
            el->count++;
        }
    }
    volume_delete(merged);
    return 0;
}

typedef struct {
    int rgba_key;
    uint8_t color[4];
    int layer_count;
    UT_hash_handle hh;
} tb_cross_hash_t;

static bool tb_forced_map_has_rgb(const image_t *img, const uint8_t c[4])
{
    int i;
    if (!img || c[3] != 255) return false;
    for (i = TB_PAL_MAP_FIRST; i < 256; i++) {
        if (!img->tb_palette_slot_forced[i]) continue;
        if (img->tb_palette[i][3] != 255) continue;
        if (img->tb_palette[i][0] == c[0] &&
            img->tb_palette[i][1] == c[1] &&
            img->tb_palette[i][2] == c[2])
            return true;
    }
    return false;
}

static void tb_run_analysis(tb_popup_state_t *st)
{
    image_t *img = goxel.image;
    layer_t *layer;
    color_stat_hash_t *layer_hash = NULL, *el, *tmp;
    tb_cross_hash_t *cross = NULL, *ce, *ctmp;
    int max_share = 0, share, i, n, layer_idx, remaining;
    tb_cross_bucket_t *bucket;

    tb_popup_clear_analysis(st);
    if (!img) return;

    image_analyse_color_stats(img, false, true, true, true, true, 0,
                              &st->layers);

    if (st->layers.layer_count > 0) {
        st->after_current = calloc((size_t)st->layers.layer_count,
                                   sizeof(*st->after_current));
        st->after_lists = calloc((size_t)st->layers.layer_count,
                                 sizeof(*st->after_lists));
    }
    tb_layer_open_ensure(st->layers.layer_count);

    layer_idx = 0;
    DL_FOREACH(img->layers, layer) {
        if (layer->parent_id != 0) continue;
        if (!layer_effectively_visible(img, layer)) continue;
        if (tb_collect_subtree_hash(img, layer, &layer_hash) != 0)
            continue;
        if (HASH_COUNT(layer_hash) == 0) {
            color_stats_hash_clear(&layer_hash);
            continue;
        }

        remaining = 0;
        HASH_ITER(hh, layer_hash, el, tmp) {
            if (!tb_forced_map_has_rgb(img, el->color))
                remaining++;
        }
        if (st->after_current && layer_idx < st->layers.layer_count)
            st->after_current[layer_idx] = remaining;
        if (st->after_lists && layer_idx < st->layers.layer_count) {
            tb_after_colors_t *list = &st->after_lists[layer_idx];
            free(list->colors);
            list->colors = NULL;
            list->count = 0;
            HASH_ITER(hh, layer_hash, el, tmp) {
                if (tb_forced_map_has_rgb(img, el->color))
                    continue;
                list->count++;
                list->colors = realloc(list->colors,
                                       (size_t)list->count * sizeof(*list->colors));
                if (!list->colors) {
                    list->count = 0;
                    break;
                }
                memcpy(list->colors[list->count - 1], el->color, 4);
            }
        }
        layer_idx++;

        HASH_ITER(hh, layer_hash, el, tmp) {
            /* Skip colours already forced into non-reserved swatches. */
            if (tb_forced_map_has_rgb(img, el->color))
                continue;
            HASH_FIND_INT(cross, &el->rgba_key, ce);
            if (!ce) {
                ce = calloc(1, sizeof(*ce));
                if (!ce) break;
                ce->rgba_key = el->rgba_key;
                memcpy(ce->color, el->color, 4);
                ce->layer_count = 1;
                HASH_ADD_INT(cross, rgba_key, ce);
            } else {
                ce->layer_count++;
            }
            if (ce->layer_count > max_share)
                max_share = ce->layer_count;
        }
        color_stats_hash_clear(&layer_hash);
    }

    for (share = 2; share <= max_share; share++) {
        n = 0;
        HASH_ITER(hh, cross, ce, ctmp) {
            if (ce->layer_count == share) n++;
        }
        if (n == 0) continue;
        st->buckets = realloc(st->buckets,
                              (size_t)(st->nbuckets + 1) * sizeof(*st->buckets));
        if (!st->buckets) break;
        bucket = &st->buckets[st->nbuckets];
        memset(bucket, 0, sizeof(*bucket));
        bucket->n_layers = share;
        bucket->colors = calloc((size_t)n, sizeof(*bucket->colors));
        if (!bucket->colors) break;
        i = 0;
        HASH_ITER(hh, cross, ce, ctmp) {
            if (ce->layer_count != share) continue;
            memcpy(bucket->colors[i], ce->color, 4);
            i++;
        }
        bucket->n_colors = i;
        st->nbuckets++;
    }

    HASH_ITER(hh, cross, ce, ctmp) {
        HASH_DEL(cross, ce);
        free(ce);
    }
    st->valid = true;
    tb_popup_refresh_used(st);
}

static void tb_popup_add_recent(image_t *img)
{
    int i;
    for (i = 0; i < img->recent_color_count; i++)
        tb_palette_add_rgb(img, img->recent_colors[i].color);
}

static void tb_popup_add_palette(image_t *img, const char *name)
{
    const palette_t *pal;
    int i;
    if (!name || !name[0]) return;
    pal = palette_find_by_name(goxel.palettes, name);
    if (!pal) return;
    for (i = 0; i < pal->size; i++)
        tb_palette_add_rgb(img, pal->entries[i].color);
}

/* Returns true if a colour was added to the palette via click. */
static bool tb_draw_palette_grid(tb_popup_state_t *st, image_t *img)
{
    int vis_row, col, idx, click;
    char id[32];
    const uint8_t *border;
    uint8_t empty[4] = {0, 0, 0, 0};
    const uint8_t *color;
    float size = 18.f;
    bool added = false;

    /* Magica layout: index 1 bottom-left through 255, 8 columns. */
    for (vis_row = 0; vis_row < 32; vis_row++) {
        int row_from_bottom = 31 - vis_row;
        int drawn = 0;
        for (col = 0; col < 8; col++) {
            idx = 1 + row_from_bottom * 8 + col;
            if (idx > 255) continue;
            if (drawn > 0)
                gui_same_line();
            drawn++;
            snprintf(id, sizeof(id), "tbpal%d", idx);
            color = img->tb_palette[idx];
            if (color[3] != 255)
                color = empty;
            border = NULL;
            if (idx <= TB_PAL_RESERVED_LAST)
                border = TB_BORDER_RED;
            else if (img->tb_palette_slot_forced[idx] &&
                     img->tb_palette[idx][3] == 255 &&
                     !tb_color_used_in_map(st, img->tb_palette[idx]))
                border = TB_BORDER_GREY;

            click = gui_color_swatch_bordered(id, color, size, border);
            if (idx <= TB_PAL_RESERVED_LAST) {
                gui_tooltip_if_hovered(idx <= TB_PAL_META_LAST
                    ? "Reserved metadata colour"
                    : "Reserved empty slot");
            } else if (click == 1) {
                if (img->tb_palette[idx][3] != 255 &&
                    tb_palette_set_at(img, idx, goxel.painter.color))
                    added = true;
            } else if (click == 2) {
                tb_palette_remove_at(img, idx);
            }
        }
    }
    return added;
}

static tb_popup_state_t g_tb_popup;

static void tb_save_forced_palette_to_gox(void)
{
    image_t *img = goxel.image;
    const char *path;
    const char *filters[] = {"*.gox", NULL};

    if (!img) return;
    tb_palette_ensure_init(img);
    path = img->path;
    if (!path)
        path = sys_get_save_path("untitled.gox", filters, "gox");
    if (!path) return;
    if (path != img->path) {
        free(img->path);
        img->path = strdup(path);
    }
    save_to_file(img, img->path, false);
    img->saved_key = image_get_key(img);
    sys_on_saved(img->path);
    goxel_track_opened_file(img->path);
}

static void tb_export_panel_gui(void)
{
    tb_popup_state_t *st = &g_tb_popup;
    image_t *img = goxel.image;
    palette_t *it, *selected = NULL;
    const char *preview;
    int i;
    bool reanalyze = false;

    if (!img) return;
    tb_palette_ensure_init(img);

    /* Left: swatch grid. Right: add / analysis / actions.
     * Groups (not Columns) so Analysis can still use gui_columns. */
    gui_group_begin("##tb_swatches");
    if (tb_draw_palette_grid(st, img))
        reanalyze = true;
    gui_group_end();

    gui_same_line();

    gui_group_begin("##tb_controls");
    if (gui_button("Add recent", 0, 0)) {
        tb_popup_add_recent(img);
        reanalyze = true;
    }
    gui_same_line();
    if (gui_button("Add current", 0, 0)) {
        if (tb_palette_add_rgb(img, goxel.painter.color))
            reanalyze = true;
    }

    if (st->palette_name[0])
        selected = palette_find_by_name(goxel.palettes, st->palette_name);
    if (selected && palette_is_readonly(selected))
        selected = NULL;
    if (!selected && st->palette_name[0])
        st->palette_name[0] = '\0';
    preview = selected ? selected->name : "(palette)";

    gui_row_begin(2);
    if (gui_combo_begin("##tbpal", preview)) {
        DL_FOREACH(goxel.palettes, it) {
            if (palette_is_readonly(it)) continue;
            if (gui_combo_item(it->name, it == selected)) {
                snprintf(st->palette_name, sizeof(st->palette_name),
                         "%s", it->name);
            }
        }
        gui_combo_end();
    }
    if (gui_button("Add palette", 0, 0) && st->palette_name[0]) {
        tb_popup_add_palette(img, st->palette_name);
        reanalyze = true;
    }
    gui_row_end();

    gui_separator();
    gui_text_bold("Analysis");
    if (st->valid) {
        gui_push_id("tb_an_hdr");
        gui_columns(3);
        gui_text_bold("Layer");
        gui_next_column();
        gui_text_bold("Unique colours");
        gui_next_column();
        gui_text_bold("After current");
        gui_next_column();
        gui_columns(1);
        gui_pop_id();

        for (i = 0; i < st->layers.layer_count; i++) {
            char row_id[24];
            int n_after = st->after_current ? st->after_current[i] : 0;
            tb_after_colors_t *alist =
                st->after_lists ? &st->after_lists[i] : NULL;

            snprintf(row_id, sizeof(row_id), "tb_an_%d", i);
            gui_push_id(row_id);
            gui_columns(3);
            {
                bool fold = false;
                int fold_icon = (g_tb_layer_open && i < g_tb_layer_open_count &&
                                 g_tb_layer_open[i])
                                    ? ICON_ARROW_DOWNWARD
                                    : ICON_CHEVRON_RIGHT;

                if (n_after > 0 && g_tb_layer_open && i < g_tb_layer_open_count) {
                    char fold_id[24];
                    snprintf(fold_id, sizeof(fold_id), "##tbfold%d", i);
                    if (gui_condensed_selectable_icon(fold_id, &fold,
                                                      fold_icon))
                        g_tb_layer_open[i] = !g_tb_layer_open[i];
                    gui_same_line();
                } else {
                    gui_spacing_f(gui_icon_height(true));
                    gui_same_line();
                }
                gui_text("%s", st->layers.layers[i].name[0]
                         ? st->layers.layers[i].name : "(unnamed)");
            }
            gui_next_column();
            gui_text("%d", st->layers.layers[i].stats.unique_colors);
            gui_next_column();
            gui_text("%d", n_after);
            gui_next_column();
            gui_columns(1);

            if (g_tb_layer_open && i < g_tb_layer_open_count &&
                g_tb_layer_open[i] && alist && alist->count > 0) {
                char grid_id[24];
                int clicked;
                /* Cap height so opening a multi-thousand colour fold does not
                 * explode the floating window layout / scrollbar. */
                snprintf(grid_id, sizeof(grid_id), "tbaft%d", i);
                clicked = gui_color_swatches_scroll(
                    grid_id, (const uint8_t (*)[4])alist->colors,
                    alist->count, 14.f, 200.f);
                if (clicked >= 0 && clicked < alist->count) {
                    if (tb_palette_add_rgb(img, alist->colors[clicked]))
                        reanalyze = true;
                }
            }
            gui_pop_id();
        }

        {
            int available = 0;
            int after_total = 0;
            color_stat_hash_t *el, *tmp;

            for (i = TB_PAL_MAP_FIRST; i < 256; i++) {
                if (img->tb_palette[i][3] != 255)
                    available++;
            }
            HASH_ITER(hh, st->used_colors, el, tmp) {
                if (!tb_forced_map_has_rgb(img, el->color))
                    after_total++;
            }
            gui_text("Total in use colours: %d",
                     st->layers.total.unique_colors);
            gui_text("If current applied: %d", after_total);
            gui_text("Available atlas indexes: %d", available);
        }

        gui_separator();
        gui_text_bold("Unique colours across layers");
        gui_push_id("tb_cross");
        gui_columns(3);
        gui_text_bold("Across layers");
        gui_next_column();
        gui_text_bold("Colours");
        gui_next_column();
        gui_text_bold("Add");
        gui_next_column();
        if (st->nbuckets == 0) {
            gui_text("(none)");
            gui_next_column();
            gui_text("-");
            gui_next_column();
            gui_text("");
            gui_next_column();
        } else {
            for (i = 0; i < st->nbuckets; i++) {
                char add_id[32];
                snprintf(add_id, sizeof(add_id), "Add##tbx%d", i);
                gui_text("%d", st->buckets[i].n_layers);
                gui_next_column();
                gui_text("%d", st->buckets[i].n_colors);
                gui_next_column();
                if (gui_button(add_id, 0, 0)) {
                    int ci;
                    for (ci = 0; ci < st->buckets[i].n_colors; ci++)
                        tb_palette_add_rgb(img, st->buckets[i].colors[ci]);
                    reanalyze = true;
                }
                gui_next_column();
            }
        }
        gui_columns(1);
        gui_pop_id();
    } else {
        gui_text("(run Analyze)");
    }

    gui_separator();
    if (gui_button("Reset", 0, 0)) {
        tb_palette_reset(img);
        tb_popup_refresh_used(st);
        reanalyze = true;
    }
    gui_same_line();
    if (gui_button("Analyze", 0, 0))
        tb_run_analysis(st);
    gui_same_line();
    if (gui_button("Preview", 0, 0)) {
        const file_format_t *f = file_format_by_name("vox (Trenchblocks)");
        if (f && f->export_func)
            f->export_func(f, img, NULL);
    }
    gui_same_line();
    if (gui_button("Save", 0, 0))
        tb_save_forced_palette_to_gox();
    gui_same_line();
    if (gui_button("Export", 0, 0))
        goxel_export_to_file(NULL, "vox (Trenchblocks)");
    gui_group_end();

    if (reanalyze)
        tb_run_analysis(st);
}

static void tb_popup_on_closed(void)
{
    tb_popup_clear_analysis(&g_tb_popup);
    tb_popup_clear_used(&g_tb_popup);
    tb_layer_open_clear();
}

void gui_vox_trenchblocks_export_window(void)
{
    if (!gui_floating_window_begin("vox (Trenchblocks)",
                                   &goxel.gui.tb_export_win_open, 720.f, 690.f))
        return;
    tb_export_panel_gui();
    gui_floating_window_end();
    if (!goxel.gui.tb_export_win_open)
        tb_popup_on_closed();
}

void goxel_open_vox_trenchblocks_export_popup(void)
{
    if (!goxel.image) return;
    tb_palette_ensure_init(goxel.image);
    if (!goxel.gui.tb_export_win_open) {
        tb_popup_clear_analysis(&g_tb_popup);
        tb_popup_clear_used(&g_tb_popup);
        tb_layer_open_clear();
        g_tb_popup.palette_name[0] = '\0';
        tb_popup_refresh_used(&g_tb_popup);
    }
    goxel.gui.tb_export_win_open = true;
}

