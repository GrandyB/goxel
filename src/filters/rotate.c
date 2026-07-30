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
    int pos[3];
    int buffer_pos[3];
    int volume_pos[3];
    int size[3];
    uint8_t *buffer;
    int i;
    size_t buffer_offset;

    size[0] = aabb[1][0] - aabb[0][0];
    size[1] = aabb[1][1] - aabb[0][1];
    size[2] = aabb[1][2] - aabb[0][2];

    if (size[0] <= 1 && size[1] <= 1)
        return;

    buffer = calloc(size[0] * size[1] * size[2], 4);
    if (!buffer)
        return;

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
                memcpy(buffer_pos, pos, sizeof(pos));
                memcpy(volume_pos, pos, sizeof(pos));

                for (i = 0; i < 3; i++)
                    volume_pos[i] += aabb[0][i];

                if (direction < 0) {
                    buffer_pos[0] = size[1] - 1 - pos[1];
                    buffer_pos[1] = pos[0];
                } else {
                    buffer_pos[0] = pos[1];
                    buffer_pos[1] = size[0] - 1 - pos[0];
                }

                buffer_offset = 4 * (buffer_pos[2] * size[0] * size[1] +
                                     buffer_pos[1] * size[0] + buffer_pos[0]);

                volume_get_at(volume, NULL, volume_pos, &buffer[buffer_offset]);
            }
        }
    }

    volume_write_aabb_from_buffer(volume, buffer, aabb);
    free(buffer);
}

void goxel_rotate_90(int direction, bool current_layer_only)
{
    float box[4][4] = {};
    int aabb[2][3];
    layer_t *layer;

    if (!goxel.image || !goxel.image->active_layer)
        return;

    memcpy(box, goxel.image->active_layer->box, sizeof(box));

    if (box_is_null(box))
        memcpy(box, goxel.image->box, sizeof(box));

    if (box_is_null(box))
        return;

    if (current_layer_only && !goxel.image->active_layer->visible)
        return;

    bbox_to_aabb(box, aabb);

    image_history_push(goxel.image);

    DL_FOREACH(goxel.image->layers, layer) {
        if (current_layer_only && layer != goxel.image->active_layer)
            continue;
        volume_rotate_z(layer->volume, direction, aabb);
    }
}
