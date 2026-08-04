/* Goxel 3D voxels editor
 *
 * Biomes generate filter - Triplefox random.txt via James Hofmann mapmaker.
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#include "biomes.h"
#include "mapmaker.h"
#include "goxel.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    filter_t filter;
    biomes_settings_t *settings;
} filter_biomes_t;

static biomes_settings_t g_default_biomes;

static void ensure_defaults(void)
{
    static bool inited;
    if (!inited) {
        biomes_settings_set_defaults(&g_default_biomes);
        inited = true;
    }
}

static void reset_to_default(filter_biomes_t *filter)
{
    ensure_defaults();
    if (filter->settings)
        free(filter->settings);
    filter->settings = malloc(sizeof(biomes_settings_t));
    if (!filter->settings)
        return;
    memcpy(filter->settings, &g_default_biomes, sizeof(biomes_settings_t));
}

static void gui_tooltip_with_default(const char *tooltip, const char *default_fmt, ...)
{
    char default_str[128];
    char final_tooltip[256];
    va_list args;
    va_start(args, default_fmt);
    vsnprintf(default_str, sizeof(default_str), default_fmt, args);
    va_end(args);
    snprintf(final_tooltip, sizeof(final_tooltip), "%s. Default is '%s'",
             tooltip, default_str);
    gui_tooltip_if_hovered(final_tooltip);
}

static const char *biome_names[BIOMES_COUNT] = {
    "Grass", "Snow", "Hill", "Water", "Tundra",
};

static void sync_stop_hsb_from_rgb(biomes_grad_stop_t *st)
{
    if (!st->is_hsb)
        return;
    mm_rgb_to_hsb(st->rgb[0], st->rgb[1], st->rgb[2],
                  &st->hsb[0], &st->hsb[1], &st->hsb[2]);
}

static void gui_biome_section(biomes_settings_t *s, int idx,
                              const biomes_biome_settings_t *def)
{
    biomes_biome_settings_t *b = &s->biomes[idx];
    char label[64];
    char id[32];
    int i;

    snprintf(label, sizeof(label), "%s biome", biome_names[idx]);
    if (!gui_collapsing_header(label, false))
        return;

    /* Scope all child widgets so Height/Variation/Pos etc. don't clash. */
    snprintf(id, sizeof(id), "biome_%d", idx);
    gui_push_id(id);

    if (idx == 4) {
        gui_checkbox("Share snow gradient", &s->tundra_shares_snow_gradient,
                     "Use the Snow biome gradient (random.txt default)");
        gui_tooltip_with_default("Tundra reuses snow colors when enabled",
                                 "%s",
                                 g_default_biomes.tundra_shares_snow_gradient
                                     ? "on" : "off");
    }

    gui_input_float("Height", &b->height, 0.01f, -2.f, 2.f, "%.2f");
    gui_tooltip_with_default(
        "Typical heightmap value (0-1). In AoS space higher = lower terrain",
        "%.2f", def->height);
    gui_input_float("Variation", &b->variation, 0.01f, -2.f, 2.f, "%.2f");
    gui_tooltip_with_default("Random offset added to height per tile",
                             "%.2f", def->variation);
    gui_input_float("Noise", &b->noise, 0.01f, 0.f, 1.f, "%.2f");
    gui_tooltip_with_default("Per-tile height jitter amplitude",
                             "%.2f", def->noise);

    if (idx == 4 && s->tundra_shares_snow_gradient) {
        gui_pop_id();
        return;
    }

    for (i = 0; i < b->n_stops; i++) {
        char clabel[32];
        char stop_id[16];
        snprintf(stop_id, sizeof(stop_id), "stop_%d", i);
        gui_push_id(stop_id);
        snprintf(clabel, sizeof(clabel), "Stop %d @%d", i, b->stops[i].pos);
        if (gui_color_small(clabel, b->stops[i].rgb))
            sync_stop_hsb_from_rgb(&b->stops[i]);
        gui_input_int("Pos", &b->stops[i].pos, 0, 64);
        gui_pop_id();
    }

    gui_pop_id();
}

static int gui(filter_t *filter_)
{
    filter_biomes_t *filter = (void *)filter_;
    biomes_settings_t *s;
    layer_t *layer;
    int i;
    const char *help_text =
        "Biomes: Triplefox random map via James Hofmann mapmaker.\n"
        "Hover fields for details. Reset restores random.txt defaults.";

    ensure_defaults();
    if (!filter->settings)
        reset_to_default(filter);
    s = filter->settings;
    if (!s)
        return 0;

    goxel_set_help_text(help_text);
    gui_label_size_push(140);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    if (gui_collapsing_header("Terrain", false)) {
        gui_input_float("Displace jitter", &s->displace_jitter, 0.01f, 0.f, 2.f,
                        "%.2f");
        gui_tooltip_with_default("Midpoint-displace noise strength",
                                 "%.2f", g_default_biomes.displace_jitter);
        gui_input_float("Span scale", &s->displace_span_scale, 0.01f, 0.f, 2.f,
                        "%.2f");
        gui_tooltip_with_default(
            "How fast displace amplitude falls each octave",
            "%.2f", g_default_biomes.displace_span_scale);
        gui_input_int("Displace skip", &s->displace_skip, 0, 8);
        gui_tooltip_with_default(
            "Skip coarse displace iterations (higher = smoother bases)",
            "%i", g_default_biomes.displace_skip);
        gui_checkbox("Biome edge jitter", &s->biome_jitter,
                     "Jitter biome tile borders after flood fill");
        gui_input_int("Height smooth", &s->height_smooth_passes, 1, 16);
        gui_tooltip_with_default(
            "Float height blur passes before voxelize (random.txt used 1). "
            "Raise this to soften noisy flats",
            "%i", g_default_biomes.height_smooth_passes);
        gui_input_int("Height despeckle", &s->height_despeckle_passes, 0, 8);
        gui_tooltip_with_default(
            "Median-filters integer column tops (3x3). Removes checkerboard "
            "single-block flecks after quantizing 0-1 heights to 64 levels. "
            "0 = off",
            "%i", g_default_biomes.height_despeckle_passes);
    }

    if (gui_section_begin("Biomes", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        for (i = 0; i < BIOMES_COUNT; i++)
            gui_biome_section(s, i, &g_default_biomes.biomes[i]);
    }
    gui_section_end();

    if (gui_collapsing_header("River", false)) {
        gui_checkbox("Enable river", &s->river_enabled, NULL);
        gui_input_int("Corridor half-width", &s->river_x_half_width, 0, 256);
        gui_tooltip_with_default("River stays within 256 +/- this many blocks",
                                 "%i", g_default_biomes.river_x_half_width);
        gui_input_int("Y increment", &s->river_y_increment, 1, 64);
        gui_input_int("X increment", &s->river_x_increment, 1, 64);
        gui_input_int("Add radius", &s->river_add_radius, 0, 32);
        gui_input_float("Add depth", &s->river_add_depth, 0.001f, 0.f, 1.f,
                        "%.3f");
        gui_tooltip_with_default("Height added along a wide river stroke",
                                 "%.3f", g_default_biomes.river_add_depth);
        gui_input_int("Set radius", &s->river_set_radius, 0, 32);
        gui_input_float("Set height", &s->river_set_height, 0.1f, 0.f, 4.f,
                        "%.1f");
        gui_tooltip_with_default(
            "Forces water height along the channel (then truncated to 0-1)",
            "%.1f", g_default_biomes.river_set_height);
    }

    if (gui_collapsing_header("Colors", false)) {
        gui_input_float("Color jitter", &s->color_jitter, 1.f, 0.f, 64.f,
                        "%.0f");
        gui_tooltip_with_default(
            "Jitter biome-id colors before gradient paint (pixels)",
            "%.0f", g_default_biomes.color_jitter);
        gui_input_int("RGB noise low", &s->rgb_noise_low, -32, 32);
        gui_input_int("RGB noise high", &s->rgb_noise_high, -32, 32);
        gui_checkbox("Smooth colors", &s->smooth_colors,
                     "Average neighboring surface colors");
    }

    if (gui_collapsing_header("Trees", false)) {
        gui_checkbox("Place trees", &s->trees_enabled,
                     "Spawn trees on hill biome tiles (random.txt)");
        gui_input_int("Min per hill tile", &s->trees_min_per_tile, 0, 64);
        gui_input_int("Max per hill tile", &s->trees_max_per_tile, 0, 64);
        gui_input_int("Trunk h min", &s->trunk_h_min, 1, 16);
        gui_input_int("Trunk h max", &s->trunk_h_max, 1, 16);
        gui_color_small("Trunk", s->trunk_color);
        for (i = 0; i < s->foliage_count && i < BIOMES_MAX_FOLIAGE; i++) {
            char fl[32];
            snprintf(fl, sizeof(fl), "Foliage %d", i + 1);
            gui_color_small(fl, s->foliage_colors[i]);
        }
    }

    gui_separator();

    {
        bool has_layer = goxel.image && goxel.image->active_layer;
        int target_mode;
        if (!has_layer)
            s->replace_current_layer = false;
        target_mode = s->replace_current_layer ? 1 : 0;
        gui_row_begin(2);
        gui_selectable_toggle("In new layer", &target_mode, 0,
                              "Create a Biomes layer (child or top-level).",
                              -1);
        gui_enabled_begin(has_layer);
        gui_selectable_toggle("Replace current layer", &target_mode, 1,
                              "Clear and write into the selected layer.",
                              -1);
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
        gui_row_end();
        s->replace_current_layer = (target_mode == 1);
    }

    gui_checkbox("Resize image", &s->resize_image,
                 "Resize the image box to fit generated voxels");

    gui_separator();
    gui_input_int("Seed", &s->seed, 0, RAND_MAX);
    gui_tooltip_with_default("Reproducible generation seed", "%i",
                             g_default_biomes.seed);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        s->seed = rand();
    }
    gui_separator();

    if (gui_button("Reset to defaults", -1, 0))
        reset_to_default(filter);

    if (gui_button_primary("Generate", -1, 0)) {
        image_history_push(goxel.image);
        layer = image_ensure_layer_for_generation(
            goxel.image, "Biomes", s->replace_current_layer);
        if (layer && layer->volume) {
            generate_biomes_terrain(layer->volume, s);
            if (s->resize_image) {
                float box[4][4];
                int dimensions[3];
                volume_get_box(goxel_get_layers_volume(goxel.image), true, box);
                box_get_dimensions(box, dimensions);
                image_set_image_dimensions_and_center(
                    goxel.image, dimensions[0], dimensions[1], dimensions[2]);
            }
        }
    }

    gui_label_size_pop();
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_biomes_t *filter = (void *)filter_;
    reset_to_default(filter);
}

FILTER_REGISTER(biomes, filter_biomes_t,
                .name = "Biomes",
                .menu = "effects",
                .submenu = "generate",
                .on_open = on_open,
                .panel_width = 350,
                .gui_fn = gui, )
