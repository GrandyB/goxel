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
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

/*
 * Filter that uses Tom Dobrowolski's terrain generator.
 */
typedef struct
{
    filter_t filter;
    genland_settings_t *settings;
} filter_genland_t;

// Define a static instance containing all default values.
static const genland_settings_t default_genland_settings = {
    .seed = 0,
    .max_height = 64,
    .num_octaves = 10,
    .amp_octave_mult = 0.4,
    .river_phase = 0.75,
    .river_width = 0.02,
    .variety = 20.0,
    .offset = 28.0,
    .noise_terrain = 9.5,
    .noise_river = 13.2,
    .color_ground = {140, 125, 115, 255},
    .color_grass1 = {72, 80, 32, 255},
    .color_grass2 = {68, 78, 40, 255},
    .color_water = {60, 100, 120, 255},
    .shadow_factor = 32,
    .ambience_factor = 0.3,
    .resize_image = true,
    .replace_current_layer = true,
};


static void reset_to_default(filter_genland_t *filter) {
    if (filter->settings)
        free(filter->settings);
    filter->settings = malloc(sizeof(genland_settings_t));
    if (!filter->settings)
        return;
    memcpy(filter->settings, &default_genland_settings, sizeof(genland_settings_t));
}

static void gui_tooltip_with_default(const char *tooltip, const char *default_fmt, ...)
{
    char default_str[128];
    va_list args;
    va_start(args, default_fmt);
    vsnprintf(default_str, sizeof(default_str), default_fmt, args);
    va_end(args);

    char final_tooltip[256];
    snprintf(final_tooltip, sizeof(final_tooltip), "%s. Default is '%s'", tooltip, default_str);
    gui_tooltip_if_hovered(final_tooltip);
}

static int gui(filter_t *filter_)
{
    filter_genland_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;

    const char *help_text = "Genland by Tom Dobrowolski.\n"
        "Hover over each field to get some information about how it affects the end terrain";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
    {
        gui_text_wrapped(help_text);
    }

    gui_input_int("Max height", &filter->settings->max_height, 0, 9999);
    gui_tooltip_with_default("The maximum height the generator can place blocks (and the height the image will be resized to, if enabled)", "%i", default_genland_settings.max_height);
    
    gui_input_int("# Octaves", &filter->settings->num_octaves, 0, 100);
    gui_tooltip_with_default("# of times noise is applied", "%i", default_genland_settings.num_octaves);

    gui_input_int("Seed", &filter->settings->seed, 0, RAND_MAX);
    gui_tooltip_with_default("'Seeding' allows for consistent generations (if using the same number)", "%i", default_genland_settings.seed);
    if (gui_button("Randomize seed", -1, 0))
    {
        srand(time(NULL));
        filter->settings->seed = rand();
    }

    gui_input_float("Octave mult", &filter->settings->amp_octave_mult, 0.01, 0, 1, "%.2f");
    gui_tooltip_with_default("How aggressively each octave of noise affects the final result, lower = less aggressive", "%.2f", default_genland_settings.amp_octave_mult);

    gui_input_float("River width", &filter->settings->river_width, 0.01, 0, 1, "%.2f");
    gui_tooltip_with_default("How wide the river(s) should generate", "%.2f", default_genland_settings.river_width);

    gui_input_float("River phase", &filter->settings->river_phase, 0.01, 0, 1, "%.2f");
    gui_tooltip_with_default("Where the rivers begin, 0 = far left, 1 = far right", "%.2f", default_genland_settings.river_phase);

    // TODO: Rework?/add tooltips
    gui_input_float("Variety", &filter->settings->variety, 1.00, 0, 100, "%.0f");
    gui_input_float("Offset", &filter->settings->offset, 1.00, 0, 100, "%.0f");
    gui_input_float("Terrain noise", &filter->settings->noise_terrain, 0.1, 0, 100, "%.1f");
    gui_input_float("River noise", &filter->settings->noise_river, 0.1, 0, 100, "%.1f");

    gui_group_begin("Colors");
    gui_color_small("Ground", filter->settings->color_ground);
    gui_color_small("Grass1", filter->settings->color_grass1);
    gui_color_small("Grass2", filter->settings->color_grass2);
    gui_color_small("Water", filter->settings->color_water);
    gui_group_end();

    gui_group_begin("Lighting");
    gui_input_float("Shadow", &filter->settings->shadow_factor, 1.00, 0, 255, "%.0f");
    gui_tooltip_with_default("How strong shadows from a simulated sun are", "%.0f", default_genland_settings.shadow_factor);

    gui_input_float("Ambient", &filter->settings->ambience_factor, 0.01, 0, 1, "%.2f");
    gui_tooltip_with_default("How strongly lighting normals affect blocks", "%.2f", default_genland_settings.ambience_factor);
    gui_group_end();

    gui_group_begin("Transform");
    gui_checkbox("Replace layer", &filter->settings->replace_current_layer,
        "If checked, this will clear the active layer before generating\n"
        "If unchecked, we will not clear the layer before generating");
    gui_checkbox("Resize image", &filter->settings->resize_image,
        "If checked, we will automatically resize the image box after generating\n"
        "If unchecked, the image box will remain as it was.");
    gui_group_end();

    if (gui_button("Reset to defaults", -1, 0))
    {
        reset_to_default(filter);
    }

    if (gui_button("Generate", -1, 0))
    {
        image_history_push(goxel.image);
        generate_tomland_terrain(layer->volume, filter->settings);

        if (filter->settings->resize_image) {
            float box[4][4];
            volume_get_box(goxel_get_layers_volume(goxel.image), true, box);
            int dimensions[3];
            box_get_dimensions(box, dimensions);
            image_set_image_dimensions_and_center(goxel.image, dimensions[0], dimensions[1], filter->settings->max_height);
        }
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_genland_t *filter = (void *)filter_;
    reset_to_default(filter);
}

FILTER_REGISTER(genland, filter_genland_t,
                .name = "Generation - Genland",
                .on_open = on_open,
                .panel_width = 275,
                .gui_fn = gui, )