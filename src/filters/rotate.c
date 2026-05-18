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

typedef struct
{
    filter_t filter;
} filter_rotate_t;

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

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
                memcpy(volume_pos, pos, sizeof(pos));

                for (i = 0; i < 3; i++)
                    volume_pos[i] += aabb[0][i];

                buffer_offset = 4 * (pos[2] * size[0] * size[1] +
                                     pos[1] * size[0] + pos[0]);

                volume_set_at(volume, NULL, volume_pos, &buffer[buffer_offset]);
            }
        }
    }

    free(buffer);
}

static void rotate_all_layers(int direction)
{
    float box[4][4] = {};
    int aabb[2][3];
    layer_t *layer;

    memcpy(box, goxel.image->active_layer->box, sizeof(box));

    if (box_is_null(box))
        memcpy(box, goxel.image->box, sizeof(box));

    if (box_is_null(box))
        return;

    bbox_to_aabb(box, aabb);

    image_history_push(goxel.image);

    DL_FOREACH(goxel.image->layers, layer)
        volume_rotate_z(layer->volume, direction, aabb);
}

static int gui(filter_t *filter)
{
    (void)filter;

    gui_group_begin(NULL);

    gui_row_begin(2);
    if (gui_button("Clockwise", 1.0, 0))
        rotate_all_layers(-1);
    if (gui_button("Counter-clockwise", 1.0, 0))
        rotate_all_layers(+1);
    gui_row_end();

    gui_group_end();

    return 0;
}

FILTER_REGISTER(rotate, filter_rotate_t,
                .name = "Translation - Rotate",
                .gui_fn = gui, )
