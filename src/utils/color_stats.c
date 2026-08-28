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

static int pack_rgba_key(const uint8_t c[4])
{
    return (int)((uint32_t)c[0] | ((uint32_t)c[1] << 8) |
                 ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24));
}

static bool layer_in_stats_scope(const image_t *img, const layer_t *layer,
                                 bool current_layer_only,
                                 bool plain_voxel_layers_only,
                                 bool visible_layers_only)
{
    if (!layer || !layer->volume)
        return false;
    if (plain_voxel_layers_only && !layer_is_volume(layer))
        return false;
    if (current_layer_only && !layer_in_active_subtree(img, layer))
        return false;
    if (visible_layers_only && !layer_effectively_visible(img, layer))
        return false;
    return true;
}

static int add_volume_counts(const volume_t *volume, color_stat_hash_t **colors,
                             int *voxels_out)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    color_stat_hash_t *el;
    int key;

    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (v[3] == 0)
            continue;
        if (voxels_out)
            (*voxels_out)++;
        key = pack_rgba_key(v);
        HASH_FIND_INT(*colors, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            if (!el)
                return -1;
            el->rgba_key = key;
            memcpy(el->color, v, 4);
            el->count = 1;
            HASH_ADD_INT(*colors, rgba_key, el);
        } else {
            el->count++;
        }
    }
    return 0;
}

void color_stats_hash_clear(color_stat_hash_t **head)
{
    color_stat_hash_t *cur, *tmp;

    HASH_ITER(hh, *head, cur, tmp) {
        HASH_DEL(*head, cur);
        free(cur);
    }
    *head = NULL;
}

void color_stats_breakdown_clear(color_stats_breakdown_t *breakdown)
{
    if (!breakdown)
        return;
    free(breakdown->layers);
    breakdown->layers = NULL;
    breakdown->layer_count = 0;
    breakdown->total.unique_colors = 0;
    breakdown->total.voxels_analysed = 0;
    breakdown->total.uniform_colors = 0;
}

static int add_volume_uniform_counts(const volume_t *volume, int step,
                                     color_stat_hash_t **colors)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4], snapped[4];
    color_stat_hash_t *el;
    int key;

    if (step <= 0)
        return 0;

    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (v[3] == 0)
            continue;
        quantization_uniform_snap(v, step, snapped);
        key = pack_rgba_key(snapped);
        HASH_FIND_INT(*colors, &key, el);
        if (!el) {
            el = calloc(1, sizeof(*el));
            if (!el)
                return -1;
            el->rgba_key = key;
            memcpy(el->color, snapped, 4);
            el->count = 1;
            HASH_ADD_INT(*colors, rgba_key, el);
        } else {
            el->count++;
        }
    }
    return 0;
}

static int volume_uniform_color_stats(const volume_t *volume, int step,
                                      color_stats_summary_t *out)
{
    color_stat_hash_t *colors = NULL;
    int ret;

    out->uniform_colors = 0;
    if (!volume || step <= 0)
        return 0;
    ret = add_volume_uniform_counts(volume, step, &colors);
    if (ret != 0) {
        color_stats_hash_clear(&colors);
        return ret;
    }
    out->uniform_colors = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);
    return 0;
}

static int collect_uniform_color_stats(const image_t *img,
                                       bool current_layer_only,
                                       bool plain_voxel_layers_only,
                                       bool visible_layers_only,
                                       int step, color_stats_summary_t *out)
{
    layer_t *layer;
    color_stat_hash_t *colors = NULL;

    out->uniform_colors = 0;
    if (!img || step <= 0)
        return 0;

    DL_FOREACH(img->layers, layer) {
        if (!layer_in_stats_scope(img, layer, current_layer_only,
                                  plain_voxel_layers_only,
                                  visible_layers_only))
            continue;
        if (add_volume_uniform_counts(layer->volume, step, &colors) != 0) {
            color_stats_hash_clear(&colors);
            return -1;
        }
    }

    out->uniform_colors = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);
    return 0;
}

static int subtree_uniform_color_stats(const image_t *img, const layer_t *root,
                                       bool plain_voxel_layers_only,
                                       bool visible_layers_only, int step,
                                       color_stats_summary_t *out)
{
    layer_t *layer;
    volume_t *merged;
    int ret;

    out->uniform_colors = 0;
    if (!img || !root || step <= 0)
        return 0;

    merged = volume_new();
    DL_FOREACH(img->layers, layer) {
        if (!layer->volume)
            continue;
        if (plain_voxel_layers_only && !layer_is_volume(layer))
            continue;
        if (visible_layers_only &&
            !layer_effectively_visible(img, layer))
            continue;
        if (!layer_is_ancestor(img, root, layer))
            continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }
    ret = volume_uniform_color_stats(merged, step, out);
    volume_delete(merged);
    return ret;
}

static int volume_color_stats(const volume_t *volume, color_stats_summary_t *out)
{
    color_stat_hash_t *colors = NULL;
    int ret;

    out->unique_colors = 0;
    out->voxels_analysed = 0;
    if (!volume)
        return 0;
    ret = add_volume_counts(volume, &colors, &out->voxels_analysed);
    if (ret != 0) {
        color_stats_hash_clear(&colors);
        return ret;
    }
    out->unique_colors = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);
    return 0;
}

int image_collect_color_stats(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only,
                              bool visible_layers_only,
                              color_stat_hash_t **out, int *voxels_out)
{
    layer_t *layer;
    int voxels = 0;

    if (!img || !out)
        return -1;
    *out = NULL;
    if (voxels_out)
        *voxels_out = 0;

    DL_FOREACH(img->layers, layer) {
        if (!layer_in_stats_scope(img, layer, current_layer_only,
                                  plain_voxel_layers_only,
                                  visible_layers_only))
            continue;
        if (add_volume_counts(layer->volume, out, &voxels) != 0) {
            color_stats_hash_clear(out);
            return -1;
        }
    }

    if (voxels_out)
        *voxels_out = voxels;
    return 0;
}

int image_count_unique_colors(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only,
                              bool visible_layers_only,
                              color_stats_summary_t *out)
{
    color_stat_hash_t *colors = NULL;
    int ret;

    if (!out)
        return -1;
    out->unique_colors = 0;
    out->voxels_analysed = 0;
    if (!img)
        return 0;

    ret = image_collect_color_stats(img, current_layer_only,
                                    plain_voxel_layers_only,
                                    visible_layers_only,
                                    &colors, &out->voxels_analysed);
    if (ret != 0)
        return ret;
    out->unique_colors = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);
    return 0;
}

static int subtree_color_stats(const image_t *img, const layer_t *root,
                               bool plain_voxel_layers_only,
                               bool visible_layers_only,
                               color_stats_summary_t *out)
{
    layer_t *layer;
    volume_t *merged;
    int ret;

    merged = volume_new();
    DL_FOREACH(img->layers, layer) {
        if (!layer->volume)
            continue;
        if (plain_voxel_layers_only && !layer_is_volume(layer))
            continue;
        if (visible_layers_only &&
            !layer_effectively_visible(img, layer))
            continue;
        if (!layer_is_ancestor(img, root, layer))
            continue;
        volume_merge(merged, layer->volume, MODE_OVER, NULL);
    }
    ret = volume_color_stats(merged, out);
    volume_delete(merged);
    return ret;
}

int image_analyse_color_stats(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only,
                              bool visible_layers_only, bool per_layer,
                              bool merge_layer_subtrees, int uniform_step,
                              color_stats_breakdown_t *out)
{
    layer_t *layer;
    color_stat_hash_t *colors = NULL;
    int n;

    if (!out)
        return -1;
    color_stats_breakdown_clear(out);
    if (!img)
        return 0;

    if (image_collect_color_stats(img, current_layer_only,
                                  plain_voxel_layers_only,
                                  visible_layers_only,
                                  &colors,
                                  &out->total.voxels_analysed) != 0) {
        color_stats_hash_clear(&colors);
        return -1;
    }
    out->total.unique_colors = HASH_COUNT(colors);
    color_stats_hash_clear(&colors);

    if (collect_uniform_color_stats(img, current_layer_only,
                                    plain_voxel_layers_only,
                                    visible_layers_only, uniform_step,
                                    &out->total) != 0) {
        color_stats_breakdown_clear(out);
        return -1;
    }

    if (!per_layer)
        return 0;

    DL_FOREACH(img->layers, layer) {
        if (merge_layer_subtrees) {
            if (layer->parent_id != 0)
                continue;
            if (visible_layers_only &&
                !layer_effectively_visible(img, layer))
                continue;
        } else if (!layer_in_stats_scope(img, layer, false,
                                         plain_voxel_layers_only,
                                         visible_layers_only)) {
            continue;
        }

        n = out->layer_count + 1;
        out->layers = realloc(out->layers, (size_t)n * sizeof(*out->layers));
        if (!out->layers)
            return -1;
        snprintf(out->layers[out->layer_count].name,
                 sizeof(out->layers[out->layer_count].name), "%s",
                 layer->name[0] ? layer->name : "(unnamed)");
        if (merge_layer_subtrees) {
            if (subtree_color_stats(img, layer, plain_voxel_layers_only,
                                    visible_layers_only,
                                    &out->layers[out->layer_count].stats) != 0) {
                color_stats_breakdown_clear(out);
                return -1;
            }
            if (subtree_uniform_color_stats(img, layer,
                                            plain_voxel_layers_only,
                                            visible_layers_only,
                                            uniform_step,
                                            &out->layers[out->layer_count].stats) != 0) {
                color_stats_breakdown_clear(out);
                return -1;
            }
        } else {
            if (volume_color_stats(layer->volume,
                                   &out->layers[out->layer_count].stats) != 0) {
                color_stats_breakdown_clear(out);
                return -1;
            }
            if (volume_uniform_color_stats(layer->volume, uniform_step,
                                           &out->layers[out->layer_count].stats) != 0) {
                color_stats_breakdown_clear(out);
                return -1;
            }
        }
        if (out->layers[out->layer_count].stats.unique_colors == 0) {
            out->layer_count = n - 1;
            continue;
        }
        out->layer_count = n;
    }

    return 0;
}
