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

#ifndef COLOR_STATS_H
#define COLOR_STATS_H

#include "uthash.h"

#include <stdbool.h>
#include <stdint.h>

struct image;
typedef struct image image_t;

typedef struct color_stat_hash {
    int rgba_key;
    uint8_t color[4];
    int count;
    UT_hash_handle hh;
} color_stat_hash_t;

typedef struct {
    int unique_colors;
    int voxels_analysed;
    int uniform_colors;
} color_stats_summary_t;

typedef struct {
    char name[256];
    color_stats_summary_t stats;
} color_stats_layer_entry_t;

typedef struct {
    color_stats_summary_t total;
    color_stats_layer_entry_t *layers;
    int layer_count;
} color_stats_breakdown_t;

void color_stats_hash_clear(color_stat_hash_t **head);
void color_stats_breakdown_clear(color_stats_breakdown_t *breakdown);

/*
 * Build a hash of distinct non-transparent RGBA values on layers with a
 * volume. When plain_voxel_layers_only is true, skip non-voxel layers
 * (image/shape/group layers). Returns 0, or -1 on allocation failure.
 */
int image_collect_color_stats(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only,
                              color_stat_hash_t **out, int *voxels_out);

/*
 * Count distinct colours without retaining the hash. Returns 0, or -1 on
 * allocation failure.
 */
int image_count_unique_colors(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only,
                              color_stats_summary_t *out);

/*
 * Analyse distinct colours. When per_layer is true, fills one entry per layer
 * in scope. When merge_layer_subtrees is also true, only top-level layers
 * (parent_id 0) are listed and each entry merges that layer's full subtree.
 * Always fills total with merged stats for layers in scope.
 */
int image_analyse_color_stats(const image_t *img, bool current_layer_only,
                              bool plain_voxel_layers_only, bool per_layer,
                              bool merge_layer_subtrees, int uniform_step,
                              color_stats_breakdown_t *out);

#endif // COLOR_STATS_H
