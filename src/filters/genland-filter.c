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

#include "genland.h"
#include "goxel.h"

/*
 * Filter that uses Tom Dobrowolski's terrain generator.
 */
typedef struct
{
    filter_t filter;
} filter_genland_t;

static int gui(filter_t *filter_)
{
    //filter_genland_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;

    const char *help_text = "Genland by Tom Dobrowolski";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
    {
        gui_text_wrapped(help_text);
    }

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        generate_tomland_terrain(layer->volume);
    }
    return 0;
}

FILTER_REGISTER(genland, filter_genland_t,
                .name = "Generation - Genland",
                .gui_fn = gui, )