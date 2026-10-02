/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Trenchblocks color-reduction pipeline: apply/simulate reduce steps used by
 * the export window Totals column and by Export/Preview.
 */

#include "goxel.h"
#include "formats/vox_trenchblocks.h"
#include "utils/color_stats.h"

#include <string.h>

typedef struct {
    int layer_id;
    volume_t *vol;
} tb_work_root_t;

bool tb_forced_map_has_rgb(const image_t *img, const uint8_t c[4])
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

int tb_count_forced_map(const image_t *img)
{
    int i, n = 0;
    if (!img) return 0;
    for (i = TB_PAL_MAP_FIRST; i < 256; i++) {
        if (img->tb_palette_slot_forced[i] && img->tb_palette[i][3] == 255)
            n++;
    }
    return n;
}

int tb_reduce_available_slots(const image_t *img)
{
    int i, available = 0;
    if (!img) return 0;
    for (i = TB_PAL_MAP_FIRST; i < 256; i++) {
        if (img->tb_palette[i][3] != 255)
            available++;
    }
    return available;
}

static volume_t *tb_merge_root_subtree(const image_t *img, const layer_t *root)
{
    layer_t *layer;
    volume_t *merged;

    if (!img || !root) return NULL;
    merged = volume_new();
    if (!merged) return NULL;
    DL_FOREACH(img->layers, layer) {
        if (!layer->volume) continue;
        if (!layer_effectively_visible(img, layer)) continue;
        if (!layer_is_volume(layer)) continue;
        if (!layer_is_ancestor(img, root, layer)) continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }
    return merged;
}

static void tb_work_roots_free(tb_work_root_t *roots, int n)
{
    int i;
    if (!roots) return;
    for (i = 0; i < n; i++)
        volume_delete(roots[i].vol);
    free(roots);
}

static int tb_work_roots_build(const image_t *img, tb_work_root_t **out_roots,
                               int *out_n)
{
    layer_t *layer;
    tb_work_root_t *roots = NULL;
    int n = 0, cap = 0;

    *out_roots = NULL;
    *out_n = 0;
    if (!img) return -1;

    DL_FOREACH(img->layers, layer) {
        volume_t *merged;
        tb_work_root_t *nr;

        if (layer->parent_id != 0) continue;
        if (!layer_effectively_visible(img, layer)) continue;
        merged = tb_merge_root_subtree(img, layer);
        if (!merged) {
            tb_work_roots_free(roots, n);
            return -1;
        }
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            nr = realloc(roots, (size_t)cap * sizeof(*roots));
            if (!nr) {
                volume_delete(merged);
                tb_work_roots_free(roots, n);
                return -1;
            }
            roots = nr;
        }
        roots[n].layer_id = layer->id;
        roots[n].vol = merged;
        n++;
    }
    *out_roots = roots;
    *out_n = n;
    return 0;
}

static int tb_pack_rgb_key(const uint8_t c[4])
{
    return (int)((uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                 ((uint32_t)c[2] << 16) | (255u << 24));
}

static int tb_add_volume_uniques(const volume_t *volume,
                                 color_stat_hash_t **colors)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    color_stat_hash_t *el;
    int key;

    if (!volume) return 0;
    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!voxel_is_solid(v)) continue;
        v[3] = 255;
        key = tb_pack_rgb_key(v);
        HASH_FIND_INT(*colors, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            if (!el) return -1;
            el->rgba_key = key;
            memcpy(el->color, v, 4);
            el->count = 1;
            HASH_ADD_INT(*colors, rgba_key, el);
        }
    }
    return 0;
}

static int tb_count_global_uniques(tb_work_root_t *roots, int n_roots)
{
    color_stat_hash_t *colors = NULL;
    int i, n;

    for (i = 0; i < n_roots; i++) {
        if (tb_add_volume_uniques(roots[i].vol, &colors) != 0) {
            color_stats_hash_clear(&colors);
            return -1;
        }
    }
    n = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);
    return n;
}

static int tb_count_outstanding_uniques(const image_t *img,
                                        tb_work_root_t *roots, int n_roots)
{
    color_stat_hash_t *colors = NULL, *el, *tmp;
    int i, n = 0;

    for (i = 0; i < n_roots; i++) {
        if (tb_add_volume_uniques(roots[i].vol, &colors) != 0) {
            color_stats_hash_clear(&colors);
            return -1;
        }
    }
    HASH_ITER(hh, colors, el, tmp) {
        if (!tb_forced_map_has_rgb(img, el->color))
            n++;
    }
    color_stats_hash_clear(&colors);
    return n;
}

static void tb_reduce_apply_to_volume(volume_t *vol, int method, int param)
{
    uint8_t palette[256][4];
    int nb;

    if (!vol) return;
    if (method == TB_REDUCE_UNIFORM) {
        param = clamp(param, 1, 255);
        quantization_remap_volume_uniform(vol, param);
        return;
    }
    nb = clamp(param, 2, 256);
    memset(palette, 0, sizeof(palette));
    switch (method) {
    case TB_REDUCE_OCTREE:
        quantization_gen_palette_octree(vol, nb, palette, NULL, 0);
        break;
    case TB_REDUCE_WU:
        quantization_gen_palette_wu(vol, nb, palette, NULL, 0);
        break;
    case TB_REDUCE_KMEANS:
        quantization_gen_palette_kmeans(vol, nb, palette, NULL, 0);
        break;
    case TB_REDUCE_MEDIAN_CUT:
    default:
        quantization_gen_palette(vol, nb, palette, NULL, 0);
        break;
    }
    quantization_remap_volume(vol, palette, nb);
}

static volume_t *tb_merge_work_roots(tb_work_root_t *roots, int n_roots)
{
    volume_t *merged;
    int i;

    merged = volume_new();
    if (!merged) return NULL;
    for (i = 0; i < n_roots; i++) {
        if (roots[i].vol)
            volume_merge(merged, roots[i].vol, MODE_OVER, NULL);
    }
    return merged;
}

static void tb_reduce_apply_step(tb_work_root_t *roots, int n_roots,
                                 const tb_reduce_step_t *step)
{
    int i;
    volume_t *merged;
    uint8_t palette[256][4];
    int nb, method, param;

    if (!step || n_roots <= 0) return;
    method = step->method;
    param = step->param;

    if (step->layer_id == 0) {
        /* All layers: one shared palette from the merge, remap each root. */
        if (method == TB_REDUCE_UNIFORM) {
            param = clamp(param, 1, 255);
            for (i = 0; i < n_roots; i++)
                quantization_remap_volume_uniform(roots[i].vol, param);
            return;
        }
        merged = tb_merge_work_roots(roots, n_roots);
        if (!merged) return;
        nb = clamp(param, 2, 256);
        memset(palette, 0, sizeof(palette));
        switch (method) {
        case TB_REDUCE_OCTREE:
            quantization_gen_palette_octree(merged, nb, palette, NULL, 0);
            break;
        case TB_REDUCE_WU:
            quantization_gen_palette_wu(merged, nb, palette, NULL, 0);
            break;
        case TB_REDUCE_KMEANS:
            quantization_gen_palette_kmeans(merged, nb, palette, NULL, 0);
            break;
        case TB_REDUCE_MEDIAN_CUT:
        default:
            quantization_gen_palette(merged, nb, palette, NULL, 0);
            break;
        }
        volume_delete(merged);
        for (i = 0; i < n_roots; i++)
            quantization_remap_volume(roots[i].vol, palette, nb);
        return;
    }

    for (i = 0; i < n_roots; i++) {
        if (roots[i].layer_id != step->layer_id)
            continue;
        tb_reduce_apply_to_volume(roots[i].vol, method, param);
        return;
    }
    LOG_W("TB reduce: layer id %d not found; skipping step", step->layer_id);
}

static void tb_fill_remaining_slots(volume_t *merged, uint8_t (*palette)[4],
                                    int n_forced, int method)
{
    int quant_first, quant_count;
    const uint8_t (*exclude)[4];

    if (!merged || !palette) return;
    quant_first = TB_PAL_MAP_FIRST + n_forced;
    quant_count = 256 - quant_first;
    if (quant_count <= 0) return;
    exclude = (const uint8_t (*)[4])(palette + TB_PAL_MAP_FIRST);
    switch (method) {
    case TB_REDUCE_OCTREE:
        quantization_gen_palette_octree(
            merged, quant_count, (void *)(palette + quant_first),
            exclude, n_forced);
        break;
    case TB_REDUCE_WU:
        quantization_gen_palette_wu(
            merged, quant_count, (void *)(palette + quant_first),
            exclude, n_forced);
        break;
    case TB_REDUCE_KMEANS:
        quantization_gen_palette_kmeans(
            merged, quant_count, (void *)(palette + quant_first),
            exclude, n_forced);
        break;
    case TB_REDUCE_MEDIAN_CUT:
    default:
        quantization_gen_palette(
            merged, quant_count, (void *)(palette + quant_first),
            exclude, n_forced);
        break;
    }
}

static void tb_remap_roots_to_map_palette(tb_work_root_t *roots, int n_roots,
                                          uint8_t (*palette)[4])
{
    int i, map_n;

    map_n = 256 - TB_PAL_MAP_FIRST;
    for (i = 0; i < n_roots; i++) {
        if (!roots[i].vol) continue;
        quantization_remap_volume(roots[i].vol,
                                  (const uint8_t (*)[4])(palette + TB_PAL_MAP_FIRST),
                                  map_n);
    }
}

int tb_reduce_simulate_totals(const image_t *img,
                              const tb_reduce_step_t *steps, int n_steps,
                              int *totals, int totals_len)
{
    tb_work_root_t *roots = NULL;
    int n_roots = 0, i, need, outstanding, global, used;
    uint8_t palette[256][4];
    int n_forced;
    volume_t *merged;

    if (!img || !totals) return -1;
    need = n_steps + 2;
    if (totals_len < need) return -1;
    for (i = 0; i < need; i++)
        totals[i] = 0;

    if (tb_work_roots_build(img, &roots, &n_roots) != 0)
        return -1;

    outstanding = tb_count_outstanding_uniques(img, roots, n_roots);
    if (outstanding < 0) {
        tb_work_roots_free(roots, n_roots);
        return -1;
    }
    totals[0] = outstanding;

    for (i = 0; i < n_steps; i++) {
        tb_reduce_apply_step(roots, n_roots, &steps[i]);
        global = tb_count_global_uniques(roots, n_roots);
        if (global < 0) {
            tb_work_roots_free(roots, n_roots);
            return -1;
        }
        totals[1 + i] = global;
    }

    /* Final: fill remaining atlas slots, remap, count used. */
    memcpy(palette, img->tb_palette, sizeof(palette));
    n_forced = tb_count_forced_map(img);
    merged = tb_merge_work_roots(roots, n_roots);
    if (!merged) {
        tb_work_roots_free(roots, n_roots);
        return -1;
    }
    tb_fill_remaining_slots(merged, palette, n_forced,
                            img->tb_reduce_final_method);
    volume_delete(merged);
    tb_remap_roots_to_map_palette(roots, n_roots, palette);
    used = tb_count_global_uniques(roots, n_roots);
    if (used < 0) {
        tb_work_roots_free(roots, n_roots);
        return -1;
    }
    totals[1 + n_steps] = used;

    tb_work_roots_free(roots, n_roots);
    return 0;
}

/* Collect outstanding (not in forced atlas) colours from hash into list.
 * Returns count, or -1.  If out is NULL, only counts. */
static int tb_outstanding_from_hash(const image_t *img,
                                    color_stat_hash_t *colors,
                                    tb_color_list_t *out)
{
    color_stat_hash_t *el, *tmp;
    int n = 0, i;

    HASH_ITER(hh, colors, el, tmp) {
        if (!tb_forced_map_has_rgb(img, el->color))
            n++;
    }
    if (!out)
        return n;
    out->colors = NULL;
    out->count = 0;
    if (n <= 0)
        return 0;
    out->colors = calloc((size_t)n, sizeof(*out->colors));
    if (!out->colors)
        return -1;
    i = 0;
    HASH_ITER(hh, colors, el, tmp) {
        if (tb_forced_map_has_rgb(img, el->color))
            continue;
        memcpy(out->colors[i], el->color, 4);
        i++;
    }
    out->count = i;
    return i;
}

static int tb_collect_volume_outstanding(const image_t *img,
                                         const volume_t *vol,
                                         tb_color_list_t *out)
{
    color_stat_hash_t *colors = NULL;
    int n;

    if (tb_add_volume_uniques(vol, &colors) != 0) {
        color_stats_hash_clear(&colors);
        return -1;
    }
    n = tb_outstanding_from_hash(img, colors, out);
    color_stats_hash_clear(&colors);
    return n;
}

static int tb_collect_global_outstanding(const image_t *img,
                                         tb_work_root_t *roots, int n_roots,
                                         tb_color_list_t *out)
{
    color_stat_hash_t *colors = NULL;
    int i, n;

    for (i = 0; i < n_roots; i++) {
        if (tb_add_volume_uniques(roots[i].vol, &colors) != 0) {
            color_stats_hash_clear(&colors);
            return -1;
        }
    }
    n = tb_outstanding_from_hash(img, colors, out);
    color_stats_hash_clear(&colors);
    return n;
}

int tb_reduce_per_layer_after_middle(const image_t *img,
                                     const tb_reduce_step_t *steps,
                                     int n_steps,
                                     int *out_layer_ids,
                                     int *out_after,
                                     int max_out,
                                     int *out_global_after,
                                     tb_color_list_t *out_lists,
                                     tb_color_list_t *out_global_list)
{
    tb_work_root_t *roots = NULL;
    int n_roots = 0, i, n, global_after;

    if (!img || !out_layer_ids || !out_after || max_out <= 0)
        return -1;
    if (out_global_after)
        *out_global_after = 0;
    if (out_global_list) {
        out_global_list->colors = NULL;
        out_global_list->count = 0;
    }
    if (out_lists) {
        for (i = 0; i < max_out; i++) {
            out_lists[i].colors = NULL;
            out_lists[i].count = 0;
        }
    }

    if (tb_work_roots_build(img, &roots, &n_roots) != 0)
        return -1;

    for (i = 0; i < n_steps; i++)
        tb_reduce_apply_step(roots, n_roots, &steps[i]);

    n = n_roots < max_out ? n_roots : max_out;
    for (i = 0; i < n; i++) {
        tb_color_list_t *list = out_lists ? &out_lists[i] : NULL;
        int after = tb_collect_volume_outstanding(img, roots[i].vol, list);
        if (after < 0) {
            if (out_lists) {
                int j;
                for (j = 0; j <= i; j++) {
                    free(out_lists[j].colors);
                    out_lists[j].colors = NULL;
                    out_lists[j].count = 0;
                }
            }
            tb_work_roots_free(roots, n_roots);
            return -1;
        }
        out_layer_ids[i] = roots[i].layer_id;
        out_after[i] = after;
    }

    global_after = tb_collect_global_outstanding(img, roots, n_roots,
                                                 out_global_list);
    if (global_after < 0) {
        if (out_lists) {
            for (i = 0; i < n; i++) {
                free(out_lists[i].colors);
                out_lists[i].colors = NULL;
                out_lists[i].count = 0;
            }
        }
        if (out_global_list) {
            free(out_global_list->colors);
            out_global_list->colors = NULL;
            out_global_list->count = 0;
        }
        tb_work_roots_free(roots, n_roots);
        return -1;
    }
    if (out_global_after)
        *out_global_after = global_after;

    tb_work_roots_free(roots, n_roots);
    return n;
}

int tb_reduce_prepare_export_volume(const image_t *img,
                                    uint8_t (*palette)[4], int n_forced,
                                    volume_t **out_volume)
{
    tb_work_root_t *roots = NULL;
    int n_roots = 0, i;
    volume_t *merged;
    const tb_reduce_step_t *steps;
    int n_steps;

    if (!img || !palette || !out_volume) return -1;
    *out_volume = NULL;

    if (tb_work_roots_build(img, &roots, &n_roots) != 0)
        return -1;

    steps = img->tb_reduce_steps;
    n_steps = img->tb_reduce_step_count;
    for (i = 0; i < n_steps; i++)
        tb_reduce_apply_step(roots, n_roots, &steps[i]);

    merged = tb_merge_work_roots(roots, n_roots);
    tb_work_roots_free(roots, n_roots);
    if (!merged) return -1;

    tb_fill_remaining_slots(merged, palette, n_forced,
                            img->tb_reduce_final_method);
    *out_volume = merged;
    return 0;
}
