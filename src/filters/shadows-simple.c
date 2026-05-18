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

static void shadows_simple_debug_log_layers(const char *phase)
{
    image_t *img = goxel.image;
    layer_t *active = img ? img->active_layer : NULL;
    layer_t *layer;
    int idx = 0;
    int visible_with_volume = 0;

    LOG_I("[shadows-simple] --- %s ---", phase);

    if (!img) {
        LOG_W("[shadows-simple] goxel.image is NULL");
        return;
    }
    if (!active) {
        LOG_W("[shadows-simple] no active layer on image");
    } else {
        LOG_I("[shadows-simple] active (target) layer: \"%s\" id=%d visible=%d "
              "volume=%p empty=%d is_volume=%d",
              active->name, active->id, active->visible,
              (void *)active->volume,
              active->volume ? volume_is_empty(active->volume) : -1,
              layer_is_volume(active));
    }

    LOG_I("[shadows-simple] image box is_null=%d", box_is_null(img->box));
    if (!box_is_null(img->box)) {
        int dims[3], start[3];
        box_get_dimensions(img->box, dims);
        box_get_start_pos(img->box, start);
        LOG_I("[shadows-simple] image box dims=%d x %d x %d, start=%d,%d,%d",
              dims[0], dims[1], dims[2], start[0], start[1], start[2]);
    }

    DL_FOREACH(img->layers, layer) {
        bool has_vol = layer->volume != NULL;
        bool empty = !has_vol || volume_is_empty(layer->volume);
        bool is_active = (layer == active);
        /* Casters: visible layers merged into goxel_get_layers_volume while active is hidden. */
        bool casts_shadow = layer->visible && has_vol && !empty && !is_active;

        LOG_I("[shadows-simple]   layer[%d] \"%s\" id=%d active=%d visible=%d "
              "volume=%p empty=%d is_volume=%d casts_shadow=%d",
              idx++, layer->name, layer->id, is_active, layer->visible,
              (void *)layer->volume, empty ? 1 : 0, layer_is_volume(layer),
              casts_shadow);

        if (layer->visible && has_vol && !empty)
            visible_with_volume++;
    }
    LOG_I("[shadows-simple] %d layer(s) with visible non-empty volume (incl. active)",
          visible_with_volume);
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
    gui_label_size_push(220.0f);
    gui_input_float("Strength", &filter->strength, 0.01, 0, 1, "%.2f");
    gui_tooltip_if_hovered(
        "Brightness kept on the active layer's top voxel when at least one block casts "
        "shadow from above.\n"
        "1 = no change, 0 = black. This is the base shadow before extra blocks stack.");

    gui_input_float("Extra darken per block", &filter->multi_block_multiplier, 0.01, 0, 1, "%.2f");
    gui_tooltip_if_hovered(
        "How much darker the shadow gets for each additional block above the first "
        "(subtracted from Strength).\n"
        "0 = only Strength applies, no matter how many blocks are above.\n"
        "Higher = darker stacks; lower = lighter stacks when many blocks are above.");

    gui_input_float("Darkest allowed", &filter->multi_block_cap, 0.01, 0, 1, "%.2f");
    gui_tooltip_if_hovered(
        "Floor on brightness: the final multiplier will not go below this value.\n"
        "Prevents very tall stacks from crushing shadows to pure black.");

    gui_checkbox("Apply smoothing", &filter->do_smoothing, NULL);
    gui_tooltip_if_hovered(
        "Blend each cell's shadow with its neighbors (2x2 average) for softer edges.");
    gui_label_size_pop();
    gui_group_end();

    if (gui_button("Apply", -1, 0))
    {
        shadows_simple_debug_log_layers("Apply pressed");
        LOG_I("[shadows-simple] settings: strength=%.3f multi_block=%.3f cap=%.3f smoothing=%d",
              filter->strength, filter->multi_block_multiplier,
              filter->multi_block_cap, filter->do_smoothing);

        if (!goxel.image) {
            LOG_E("[shadows-simple] abort: no image");
            return 0;
        }
        image_history_push(goxel.image);
        layer_t *layer = goxel.image->active_layer;
        if (!layer) {
            LOG_E("[shadows-simple] abort: no active layer");
            return 0;
        }
        if (!layer->volume) {
            LOG_E("[shadows-simple] abort: active layer \"%s\" has no volume",
                  layer->name);
            return 0;
        }
        if (box_is_null(goxel.image->box)) {
            LOG_W("[shadows-simple] image box is null; dims may be wrong");
        }

        layer->visible = false;
        const volume_t *other_visible_layers_combined = goxel_get_layers_volume(goxel.image);
        layer->visible = true;

        LOG_I("[shadows-simple] shadow caster volume (visible layers, active hidden): "
              "ptr=%p empty=%d",
              (void *)other_visible_layers_combined,
              other_visible_layers_combined ?
                  volume_is_empty(other_visible_layers_combined) : -1);
        LOG_I("[shadows-simple] target volume (active layer \"%s\"): empty=%d",
              layer->name, volume_is_empty(layer->volume));

        int dims[3], start_pos[3], pos[3];
        float box[4][4];
        // layer_get_bounding_box(layer, box);
        mat4_copy(goxel.image->box, box);
        box_get_dimensions(box, dims);
        box_get_start_pos(box, start_pos);

        LOG_I("[shadows-simple] working grid %d x %d x %d at start %d,%d,%d "
              "(shadow_map cells=%d)",
              dims[0], dims[1], dims[2], start_pos[0], start_pos[1], start_pos[2],
              dims[0] * dims[1]);

        if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
            LOG_E("[shadows-simple] abort: invalid dimensions");
            return 0;
        }

        volume_iterator_t iter;
        iter = volume_get_iterator(other_visible_layers_combined, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

        float *shadow_map;
        shadow_map = malloc(sizeof(float) * dims[0] * dims[1]);
        if (!shadow_map) {
            LOG_E("[shadows-simple] abort: shadow_map malloc failed");
            return 0;
        }

        int *heights;
        allocate_heights(dims, &heights);
        volume_get_heights(layer->volume, heights);

        int surface_cells = 0;
        int no_surface_cells = 0;
        for (int hi = 0; hi < dims[0] * dims[1]; hi++) {
            if (heights[hi] >= 0)
                surface_cells++;
            else
                no_surface_cells++;
        }
        LOG_I("[shadows-simple] height map: %d surface cells, %d empty columns "
              "(no voxel on active layer)",
              surface_cells, no_surface_cells);
        LOG_D("[shadows-simple] setup complete");

        // Formulate the shadow map
        int num_blocks_above_current, height;
        uint8_t col[4];
        int globalIndex = 0;
        int x, y, z;
        int cells_with_casters = 0;
        int max_blocks_above = 0;

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

                if (num_blocks_above_current > 0) {
                    cells_with_casters++;
                    if (num_blocks_above_current > max_blocks_above)
                        max_blocks_above = num_blocks_above_current;
                }
                shadow_map[globalIndex] = num_blocks_above_current;
            }
        }
        LOG_I("[shadows-simple] shadow map: %d/%d cells have casters above "
              "(max blocks above a surface=%d)",
              cells_with_casters, dims[0] * dims[1], max_blocks_above);
        if (cells_with_casters == 0) {
            LOG_W("[shadows-simple] no shadow casters found above active layer "
                  "surface — check that other visible layers have voxels above");
        }

        // Now we pivot from the shadow_map containing the # of blocks found, to the color multiplier
        // If no blocks, we want a multiplier of 1, if there are blocks, we want to use 'strength' to multiply down
        int num_blocks = 0;
        float mult;
        int cells_darkened = 0;
        float min_mult = 1.f, max_mult = 1.f;
        globalIndex = 0;
        for (y = 0; y < dims[1]; y++)
        {
            for (x = 0; x < dims[0]; x++, globalIndex++)
            {
                // If there's no shadow, return 1
                if (shadow_map[globalIndex] == 0)
                {
                    shadow_map[globalIndex] = 1;
                    continue;
                }
                cells_darkened++;
                num_blocks = (int)shadow_map[globalIndex];
                mult = filter->strength;
                if (num_blocks > 1 && filter->multi_block_multiplier > 0.f) {
                    mult -= filter->multi_block_multiplier * (float)(num_blocks - 1);
                    if (mult < filter->multi_block_cap)
                        mult = filter->multi_block_cap;
                }
                shadow_map[globalIndex] = mult;
                if (mult < min_mult)
                    min_mult = mult;
                if (mult > max_mult)
                    max_mult = mult;
            }
        }
        LOG_I("[shadows-simple] multipliers: %d cells darkened, mult range %.4f .. %.4f",
              cells_darkened, min_mult, max_mult);

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
            LOG_D("[shadows-simple] smoothing completed");
        }

        // Apply the shadows to the volume
        iter = volume_get_iterator(layer->volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        globalIndex = 0;
        int voxels_touched = 0;
        int voxels_would_darken = 0;
        int columns_no_height = 0;
        for (y = 0; y < dims[1]; y++)
        {
            pos[1] = y + start_pos[1];
            for (x = 0; x < dims[0]; x++, globalIndex++)
            {
                pos[0] = x + start_pos[0];
                if (heights[globalIndex] < 0) {
                    columns_no_height++;
                    continue;
                }
                pos[2] = heights[globalIndex] + start_pos[2];
                volume_get_at(layer->volume, &iter, pos, col);
                if (col[3] != 0) {
                    voxels_touched++;
                    if (shadow_map[globalIndex] < 0.999f)
                        voxels_would_darken++;
                }
                adjust_colour_brightness(col, shadow_map[globalIndex]);
                volume_set_at(layer->volume, &iter, pos, col);
            }
        }

        LOG_I("[shadows-simple] apply done on \"%s\": %d top voxels written, "
              "%d with mult < 1, %d columns had no surface voxel",
              layer->name, voxels_touched, voxels_would_darken, columns_no_height);
        if (voxels_would_darken == 0) {
            LOG_W("[shadows-simple] no voxels received a darkening multiplier — "
                  "see shadow map / caster counts above");
        }
        free(shadow_map);
        free(heights);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_simple_shadows_t *filter = (void *)filter_;
    filter->multi_block_multiplier = 0.03f;
    filter->multi_block_cap = 0.5f;
    filter->strength = 0.9;
    filter->do_smoothing = true;
    LOG_I("[shadows-simple] filter opened (defaults: strength=%.2f)", filter->strength);
    shadows_simple_debug_log_layers("filter opened");
}

FILTER_REGISTER(simple_shadows, filter_simple_shadows_t,
                .name = "Generate - Shadows (Simple)",
                .on_open = on_open,
                .gui_fn = gui, 
                .panel_width = 450, )