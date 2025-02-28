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
} filter_fillz_t;

static int gui(filter_t *filter_)
{
    filter_fillz_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    float box[4][4];
    int start_x, start_y, start_z, vol_w, vol_h, vol_d, x, y, z, pos[3];
    uint8_t cur_block_color[4];
    volume_iterator_t iter;
    int found_block_index = -1;

    const char *help_text = "This tool navigates all columns of blocks, filling from bottom upwards with the given color until it meets the first block.";
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

        // Compute volume dimensions from the box (assumes box[0][0], etc., are half-dimensions)
        vol_w = box[0][0] * 2; // volume width
        vol_h = box[1][1] * 2; // volume height
        vol_d = box[2][2] * 2; // volume depth

        // Determine starting positions in the volume; these offsets map the image's (0,0) to the volume
        start_x = box[0][0];   // x starting position
        start_y = box[3][1] - box[1][1];   // y starting position
        start_z = box[3][2] - box[2][2];   // z starting position

        printf("Volume dimensions: %d x %d x %d\n", vol_w, vol_h, vol_d);
        debug_log_44_matrix("box", box);
        printf("Start pos: %d, %d, %d\n", start_x, start_y, start_z);

        iter = volume_get_iterator(layer->volume,
            VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        for (x = 0; x < vol_w; x++) {
            for (y = 0; y < vol_h; y++) {
                found_block_index = -1;
                for (z = 0; z < vol_d; z++)
                {
                    pos[0] = start_x - x - 1; // x seemed to be flipped, this fixes it despite looking like an outlier
                    pos[1] = y + start_y;
                    pos[2] = z + start_z;
                    // Work from bottom y upwards, filling with the color until we meet a block
                    volume_get_at(layer->volume, &iter, pos, cur_block_color);
                    if (cur_block_color[3] != 0) {
                        // Reached a block
                        found_block_index = z;
                        break;
                    }
                }
                if (found_block_index != -1) {
                    for (z = 0; z < found_block_index; z++) {
                        pos[0] = start_x - x - 1; // x seemed to be flipped, this fixes it despite looking like an outlier
                        pos[1] = y + start_y;
                        pos[2] = z + start_z;
                        volume_set_at(layer->volume, &iter, pos, filter->color);
                    }
                }
            }
        }
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_fillz_t *filter = (void *)filter_;
    uint8_t default_color[4] = {103, 64, 40, 255};
    memcpy(filter->color, default_color, sizeof(default_color));
}

FILTER_REGISTER(fillz, filter_fillz_t,
                .name = "Fill Z by color",
                .on_open = on_open,
                .gui_fn = gui, )