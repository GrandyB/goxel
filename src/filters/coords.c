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
 * Log the current position of the selection box.
 */
typedef struct
{
    filter_t filter;
} filter_coords_t;

static int gui(filter_t *filter_)
{
    gui_text("This tool uses your current\nselection to print coordinates\nout to the console for later use.");
    gui_separator();
    if (box_is_null(goxel.selection)) {
        if (goxel.tool->id != TOOL_SELECTION) {
            if (gui_button("Switch to selection tool", 0, 0)) {
                action_exec(action_get(ACTION_tool_set_selection, true));
            }
        } else {
            gui_text("\nPlease create a 1x1x1 selection");
        }
    } else {
        if (gui_button("Log to console", 0, 0)) {
            float sx = goxel.selection[3][0] - 0.5f;
            float sy = goxel.selection[3][1] - 0.5f;
            float sz = goxel.selection[3][2] - 0.5f;

            float ox = round(goxel.image->box[3][0] - goxel.image->box[0][0]);   // box origin x
            float oy = round(goxel.image->box[3][1] - goxel.image->box[1][1]);   // box origin y
            float oz = round(goxel.image->box[3][2] - goxel.image->box[2][2]);   // box origin z

            //LOG_I("(%.0f, %.0f, %.0f)", goxel.selection[3][0]-0.5, goxel.selection[3][1]-0.5, goxel.selection[3][2]-0.5);
            LOG_I("(%.0f, %.0f, %.0f)", sx - ox, sy - oy, sz - oz);
        }
        if (gui_button("Add separator", 0, 0)) {
            LOG_I("------------");
        }
        gui_action_button(ACTION_reset_selection, "Reset selection", 1.0);
    }
    
    return 0;
}

FILTER_REGISTER(coords, filter_coords_t,
                .name = "Log - Coordinates",
                .gui_fn = gui, )