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
#include "volume.h"
#include <math.h>
#include <stdbool.h>
#include <time.h>

/*
 * Filter to apply baked shadows.
 */
typedef struct
{
    filter_t filter;
    float strength;
    float multi_block_multiplier;
    float multi_block_cap;
    bool do_smoothing;
} filter_simple_shadows_t;

void adjust_colour_brightness(uint8_t colour[4], float multiplier)
{
    for (int i = 0; i < 3; i++)
    { // Only R, G, B
        int c = (int)(colour[i] * multiplier);
        if (c < 0)
            c = 0;
        if (c > 255)
            c = 255;
        colour[i] = (uint8_t)c;
    }
}

int ind(int x, int y, int width, int height)
{
    int wrapped_x = (x + width) % width;
    int wrapped_y = (y + height) % height;
    return wrapped_y * width + wrapped_x;
}

static int gui(filter_t *filter_)
{
    filter_simple_shadows_t *filter = (void *)filter_;

    const char *help_text = "This filter applies shadow to the current layer, using blocks from other visible layers, directly vertically downwards.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
    {
        gui_text_wrapped(help_text);
    }

    gui_group_begin(NULL);
    gui_input_float("Strength", &filter->strength, 0.01, 0, 1, "%.2f");
    gui_input_float("Multi-block multiplier\n(per block)", &filter->multi_block_multiplier, 0.01, 0, 1, "%.2f");
    gui_input_float("Multi-block multiplier limit", &filter->multi_block_cap, 0.01, 0, 1, "%.2f");

    gui_tooltip_if_hovered("This makes columns with more blocks apply a darker shadow beneath it.\n"
                           "If this is 1, if there's 10 blocks above, only 'Strength' darkening is applied.\n"
                           "With multi-block multiplier, each additional block between the active layer's top block and the top of the map multiplies by this.");

    gui_checkbox("Apply smoothing", &filter->do_smoothing, "If checked, apply 2x2 smoothing\n"
                                                           "If unchecked, do no smoothing");
    gui_group_end();

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        layer_t *layer = goxel.image->active_layer;

        layer->visible = false;
        const volume_t *other_visible_layers_combined = goxel_get_layers_volume(goxel.image);
        layer->visible = true;

        int dims[3], start_pos[3], pos[3];
        float box[4][4];
        // layer_get_bounding_box(layer, box);
        mat4_copy(goxel.image->box, box);
        box_get_dimensions(box, dims);
        box_get_start_pos(box, start_pos);

        volume_iterator_t iter;
        iter = volume_get_iterator(other_visible_layers_combined, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

        float *shadow_map;
        shadow_map = malloc(sizeof(float) * dims[0] * dims[1]);

        int *heights;
        allocate_heights(dims, &heights);
        volume_get_heights(layer->volume, heights);
        printf("Simple shadow setup complete\n");

        // Formulate the shadow map
        int num_blocks_above_current, height;
        uint8_t col[4];
        int globalIndex = 0;
        int x, y, z;

        for (y = 0; y < dims[1]; y++)
        {
            pos[1] = y + start_pos[1];

            for (x = 0; x < dims[0]; x++, globalIndex++)
            {
                pos[0] = x + start_pos[0];
                num_blocks_above_current = 0;

                // Loop from top of map (max Z) down to the height of the current layer
                height = heights[globalIndex];
                for (z = dims[2]; z > height; z--)
                {
                    pos[2] = z + start_pos[2];
                    volume_get_at(other_visible_layers_combined, &iter, pos, col);
                    if (col[3] != 0)
                    {
                        num_blocks_above_current++;
                    }
                }

                // printf("%u/%u: %u active height, %u blocks found\n", x, y, height, num_blocks_above_current);
                shadow_map[globalIndex] = num_blocks_above_current;
            }
        }
        printf("Shadow map formulated\n");

        // Now we pivot from the shadow_map containing the # of blocks found, to the color multiplier
        // If no blocks, we want a multiplier of 1, if there are blocks, we want to use 'strength' to multiply down
        int num_blocks = 0;
        float mult;
        globalIndex = 0;
        for (y = 0; y < dims[1]; y++)
        {
            for (x = 0; x < dims[0]; x++, globalIndex++)
            {
                // If there's no shadow, return 1
                if (shadow_map[globalIndex] == 0)
                {
                    shadow_map[globalIndex] = 1;
                    // printf("No blocks above found, skipping\n");
                    continue;
                }
                // There are blocks, start at strength and mult down
                mult = filter->strength;
                if (filter->multi_block_multiplier != 1)
                {
                    for (num_blocks = shadow_map[globalIndex]; num_blocks >= 0; num_blocks--)
                    {
                        mult *= filter->multi_block_multiplier;
                        if (mult < filter->multi_block_cap) {
                            mult = filter->multi_block_cap;
                            break;
                        }
                    }
                }
                // printf("Applying mult: %f\n", mult);
                shadow_map[globalIndex] = mult;
            }
        }
        printf("Shadow multiplier set\n");

        float amt = 0;
        if (filter->do_smoothing)
        {
            for (y = 0; y < dims[1]; y++)
            {
                for (x = 0; x < dims[0]; x++)
                {
                    // Apply smoothing (if applicable)
                    globalIndex = ind(x, y, dims[0], dims[1]);
                    amt = (shadow_map[globalIndex] +
                        shadow_map[ind(x, y + 1, dims[0], dims[1])] +
                        shadow_map[ind(x + 1, y, dims[0], dims[1])] +
                        shadow_map[ind(x + 1, y + 1, dims[0], dims[1])]) /
                       4;
                    shadow_map[globalIndex] = amt;
                }
            }
            printf("Smoothing completed\n");
        }

        // Apply the shadows to the volume
        iter = volume_get_iterator(layer->volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        globalIndex = 0;
        for (y = 0; y < dims[1]; y++)
        {
            pos[1] = y + start_pos[1];
            for (x = 0; x < dims[0]; x++, globalIndex++)
            {
                pos[0] = x + start_pos[0];
                pos[2] = heights[globalIndex];
                // printf("Pos: %i/%i/%i\n", pos[0], pos[1], pos[2]);
                volume_get_at(layer->volume, &iter, pos, col);
                // printf("Color at: %u/%u/%u\n", (unsigned int)col[0], (unsigned int)col[1], (unsigned int)col[2]);
                adjust_colour_brightness(col, shadow_map[globalIndex]);
                volume_set_at(layer->volume, &iter, pos, col);
            }
        }

        printf("Shadows finished\n");
        free(shadow_map);
        free(heights);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_simple_shadows_t *filter = (void *)filter_;
    filter->multi_block_multiplier = 0.9;
    filter->multi_block_cap = 0.5;
    filter->strength = 0.9;
    filter->do_smoothing = true;
}

FILTER_REGISTER(simple_shadows, filter_simple_shadows_t,
                .name = "Generate - Shadows (Simple)",
                .on_open = on_open,
                .gui_fn = gui, 
                .panel_width = 300, )