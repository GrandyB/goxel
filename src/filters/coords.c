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
    if (gui_button("Log to console", 0, 0)) {
        LOG_I("(%.0f, %.0f, %.0f)", goxel.selection[3][0]-0.5, goxel.selection[3][1]-0.5, goxel.selection[3][2]-0.5);
    }
    if (gui_button("---------", 0, 0)) {
        LOG_I("------------");
    }
    
    return 0;
}

FILTER_REGISTER(coords, filter_coords_t,
                .name = "Log - Coordinates",
                .gui_fn = gui, )