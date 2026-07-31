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
 * Rotate voxels 90° around Z within an AABB (same approach as mirror/wrap).
 */

static void volume_rotate_z(volume_t *volume, int direction, const int aabb[2][3])
{
    volume_iterator_t iter;
    volume_t *out;
    int pos[3], dst[3], local[3];
    int size[3];
    uint8_t color[4];

    size[0] = aabb[1][0] - aabb[0][0];
    size[1] = aabb[1][1] - aabb[0][1];
    size[2] = aabb[1][2] - aabb[0][2];

    if (!volume || (size[0] <= 1 && size[1] <= 1))
        return;
    if (volume_is_empty(volume))
        return;

    out = volume_new();
    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, color);
        if (!color[3])
            continue;
        local[0] = pos[0] - aabb[0][0];
        local[1] = pos[1] - aabb[0][1];
        local[2] = pos[2] - aabb[0][2];
        if (direction < 0) {
            dst[0] = aabb[0][0] + (size[1] - 1 - local[1]);
            dst[1] = aabb[0][1] + local[0];
        } else {
            dst[0] = aabb[0][0] + local[1];
            dst[1] = aabb[0][1] + (size[0] - 1 - local[0]);
        }
        dst[2] = pos[2];
        volume_set_at(out, NULL, dst, color);
    }
    volume_set(volume, out);
    volume_delete(out);
}

void goxel_rotate_90(int direction, bool current_layer_only)
{
    int aabb[2][3];
    layer_t *layer;

    if (!goxel.image || !goxel.image->active_layer)
        return;

    if (current_layer_only && !goxel.image->active_layer->visible)
        return;

    if (!goxel_get_filter_aabb(current_layer_only, aabb))
        return;

    image_history_push(goxel.image);

    DL_FOREACH(goxel.image->layers, layer) {
        if (current_layer_only &&
            !layer_in_active_subtree(goxel.image, layer))
            continue;
        if (!layer->volume)
            continue;
        volume_rotate_z(layer->volume, direction, aabb);
    }
}
