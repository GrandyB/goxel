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
} filter_smooth_t;

static int gui(filter_t *filter_) {
    //filter_smooth_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    float box[4][4];
    int start_x, start_y, start_z, vol_w, vol_h, vol_d, x, y, z, pos[3];
    uint8_t cur_block_color[4];
    volume_iterator_t iter;

    const char *help_text = "This filter smooths out the current layer using averages of blocks";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false)) {
        gui_text_wrapped(help_text);
    }

    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        mat4_copy(goxel.image->box, box);
        if (box_is_null(box))
            volume_get_box(layer->volume, true, box);

        // Compute volume dimensions
        vol_w = box[0][0] * 2;
        vol_h = box[1][1] * 2;
        vol_d = box[2][2] * 2;

        // Determine starting positions
        start_x = box[0][0];
        start_y = box[3][1] - box[1][1];
        start_z = box[3][2] - box[2][2];

        printf("Volume dimensions: %d x %d x %d\n", vol_w, vol_h, vol_d);
        debug_log_44_matrix("box", box);
        printf("Start pos: %d, %d, %d\n", start_x, start_y, start_z);

        iter = volume_get_iterator(layer->volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

        int heights[vol_w][vol_h];  // Stores highest Z per (x, y)
        memset(heights, -1, sizeof(heights));  // Initialize heights to -1 (no block found)

        // Step 1: Find the highest block for each (x, y)
        for (x = 0; x < vol_w; x++) {
            for (y = 0; y < vol_h; y++) {
                for (z = vol_d - 1; z >= 0; z--) {  // Start from the top and move down
                    pos[0] = start_x - x - 1;
                    pos[1] = y + start_y;
                    pos[2] = z + start_z;

                    volume_get_at(layer->volume, &iter, pos, cur_block_color);
                    if (cur_block_color[3] != 0) {  // If a block exists
                        heights[x][y] = z;  // Store highest z for this (x, y)
                        break;
                    }
                }
            }
        }

        // Step 2: Find the height at the center of the box
        int mid_x = vol_w / 2;
        int mid_y = vol_h / 2;
        int midpointHeight = (heights[mid_x][mid_y] != -1) ? heights[mid_x][mid_y] : 0;

        // Step 3: Apply smoothing based on (x, y) distance from midpoint
        for (x = 0; x < vol_w; x++) {
            for (y = 0; y < vol_h; y++) {
                if (heights[x][y] != -1) {
                    // Compute distance from midpoint in (x, y)
                    float distance = sqrtf((x - mid_x) * (x - mid_x) + (y - mid_y) * (y - mid_y));
                    float max_distance = sqrtf((vol_w / 2) * (vol_w / 2) + (vol_h / 2) * (vol_h / 2));
                    float scaleFactor = (distance / max_distance); // Normalize

                    // Compute new height using scale factor
                    int newHeight = midpointHeight + (int)((heights[x][y] - midpointHeight) * scaleFactor);

                    // Set new voxel height
                    pos[0] = start_x - x - 1;
                    pos[1] = y + start_y;
                    pos[2] = newHeight + start_z;
                    volume_set_at(layer->volume, &iter, pos, goxel.painter.color);
                }
            }
        }
    }
    return 0;
}

static void on_open(filter_t *filter_)
{

}

FILTER_REGISTER(smooth, filter_smooth_t,
                .name = "Terrain - Smoothing",
                .on_open = on_open,
                .gui_fn = gui, )