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
 * Filter to squash a layer vertically by a %
 */
typedef struct
{
    filter_t filter;
    int percentage;
} filter_squash_t;

static int gui(filter_t *filter_)
{
    filter_squash_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    float box[4][4];
    int x, y, z, pos[3], new_pos[3], dimensions[3], start_pos[3];
    uint8_t cur_block_color[4];

    const char *help_text = "This filter condenses layer vertically by a %";
    goxel_set_help_text(help_text);

    if(gui_collapsing_header("Hint", false)) {
        gui_text_wrapped(help_text);
    }

    gui_group_begin(NULL);
    gui_input_int("Percentage", &filter->percentage, 0, 100);
    gui_group_end();

    if (gui_button("Apply", -1, 0))
    {
        volume_iterator_t iter;
        image_history_push(goxel.image);
        volume_get_box(layer->volume, true, box);

        box_get_dimensions(box, dimensions);
        box_get_start_pos(box, start_pos);

        iter = volume_get_iterator(layer->volume,
            VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        
        volume_t *copy = volume_new();
        for (z = 0; z < dimensions[2]; z++) {
            pos[2] = z + start_pos[2];
            for (x = 0; x < dimensions[0]; x++) {
                for (y = 0; y < dimensions[1]; y++) {
                    pos[0] = x + start_pos[0];
                    pos[1] = y + start_pos[1];
    
                    volume_get_at(layer->volume, &iter, pos, cur_block_color);
                    if (cur_block_color[3] != 0) {
                        new_pos[0] = x;
                        new_pos[1] = y;
                        new_pos[2] = pos[2] * (filter->percentage / 100.0f);
                        if (pos[2] > 0) new_pos[2] = max(new_pos[2], 1);
                        volume_set_at(copy, NULL, new_pos, cur_block_color);
                    }
                }
            }
        }
        volume_set(layer->volume, copy);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_squash_t *filter = (void *)filter_;
    filter->percentage = 50;
}

FILTER_REGISTER(squash, filter_squash_t,
                .name = "Transform - Squash",
                .on_open = on_open,
                .gui_fn = gui, )