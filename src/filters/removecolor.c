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
 * Filter to fill from bottom of map upwards till it meets a block.
 */
typedef struct
{
    filter_t filter;
    uint8_t color[4];
} filter_removecolor_t;

static int gui(filter_t *filter_)
{
    filter_removecolor_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    uint8_t empty_color[4] = {0, 0, 0, 0};
    float box[4][4];
    int x, y, z, pos[3], dimensions[3], start_pos[3];
    uint8_t cur_block_color[4];
    volume_iterator_t iter;

    const char *help_text = "This filter removes all blocks of the specified color from the current layer.";
    goxel_set_help_text(help_text);

    if(gui_collapsing_header("Hint", false)) {
        gui_text_wrapped(help_text);
    }

    gui_group_begin(NULL);
    gui_color_small("Color", filter->color);
    gui_group_end();

    if (gui_button("Copy current painter color", -1, 0))
    {
        memcpy(filter->color, goxel.painter.color, sizeof(goxel.painter.color));
    }

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        mat4_copy(goxel.image->box, box);
        if (box_is_null(box))
            volume_get_box(layer->volume, true, box);

        box_get_dimensions(box, dimensions);
        box_get_start_pos(box, start_pos);

        iter = volume_get_iterator(layer->volume,
            VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        for (x = 0; x < dimensions[0]; x++) {
            for (y = 0; y < dimensions[1]; y++) {
                for (z = 0; z < dimensions[2]; z++)
                {
                    pos[0] = x + start_pos[0];
                    pos[1] = y + start_pos[1];
                    pos[2] = z + start_pos[2];
                    volume_get_at(layer->volume, &iter, pos, cur_block_color);
                    if (memcmp(filter->color, cur_block_color, 4) == 0) {
                        volume_set_at(layer->volume, &iter, pos, empty_color);
                    }
                }
            }
        }
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_removecolor_t *filter = (void *)filter_;
    uint8_t default_color[4] = {103, 64, 40, 255};
    memcpy(filter->color, default_color, sizeof(default_color));
}

FILTER_REGISTER(removecolor, filter_removecolor_t,
                .name = "Bulk - Remove by color",
                .on_open = on_open,
                .gui_fn = gui, )