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
    int active_biome;
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
    filter->active_biome = 0;
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

static void sync_stop_hsb_from_rgb(biomes_grad_stop_t *st)
{
    if (!st->is_hsb)
        return;
    mm_rgb_to_hsb(st->rgb[0], st->rgb[1], st->rgb[2],
                  &st->hsb[0], &st->hsb[1], &st->hsb[2]);
}

static void clamp_active_biome(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    if (!s || s->n_biomes < 1) {
        filter->active_biome = 0;
        return;
    }
    if (filter->active_biome < 0)
        filter->active_biome = 0;
    if (filter->active_biome >= s->n_biomes)
        filter->active_biome = s->n_biomes - 1;
}

/* After deleting index `removed`, remap share_gradient_from values. */
static void remap_share_after_remove(biomes_settings_t *s, int removed)
{
    int i;
    for (i = 0; i < s->n_biomes; i++) {
        int src = s->biomes[i].share_gradient_from;
        if (src == removed)
            s->biomes[i].share_gradient_from = -1;
        else if (src > removed)
            s->biomes[i].share_gradient_from = src - 1;
    }
}

static void init_new_biome(biomes_biome_settings_t *b, int index)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "Biome %d", index + 1);
    b->height = 0.9f;
    b->variation = -0.1f;
    b->noise = 0.05f;
    b->n_stops = 2;
    b->stops[0].pos = 0;
    b->stops[0].rgb[0] = 80;
    b->stops[0].rgb[1] = 140;
    b->stops[0].rgb[2] = 80;
    b->stops[0].rgb[3] = 255;
    b->stops[0].is_hsb = false;
    b->stops[1].pos = 64;
    b->stops[1].rgb[0] = 40;
    b->stops[1].rgb[1] = 100;
    b->stops[1].rgb[2] = 40;
    b->stops[1].rgb[3] = 255;
    b->stops[1].is_hsb = false;
    b->n_fixed_seeds = 1;
    b->fixed_x[0] = (int8_t)(BIOMES_MAP_TILES / 2);
    b->fixed_y[0] = (int8_t)(BIOMES_MAP_TILES / 2);
    b->random_seeds = 0;
    b->region_x = 0;
    b->region_y = 0;
    b->region_w = BIOMES_MAP_TILES;
    b->region_h = BIOMES_MAP_TILES;
    b->place_trees = false;
    b->share_gradient_from = -1;
    b->noise_intensity = 12;
    b->noise_saturation = 6;
    b->noise_coverage = 100;
}

static void biome_add(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    if (!s || s->n_biomes >= BIOMES_MAX)
        return;
    init_new_biome(&s->biomes[s->n_biomes], s->n_biomes);
    filter->active_biome = s->n_biomes;
    s->n_biomes++;
}

static void biome_duplicate(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    biomes_biome_settings_t *dst;
    char base_name[32];
    int src;
    if (!s || s->n_biomes < 1 || s->n_biomes >= BIOMES_MAX)
        return;
    clamp_active_biome(filter);
    src = filter->active_biome;
    snprintf(base_name, sizeof(base_name), "%s", s->biomes[src].name);
    dst = &s->biomes[s->n_biomes];
    *dst = s->biomes[src];
    /* Build "… copy" without snprintf overlap / truncation warnings. */
    {
        size_t len = strlen(base_name);
        if (len > 26)
            len = 26;
        memcpy(dst->name, base_name, len);
        memcpy(dst->name + len, " copy", 6); /* includes NUL */
    }
    filter->active_biome = s->n_biomes;
    s->n_biomes++;
}

static void biome_remove(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    int i, removed;
    if (!s || s->n_biomes <= 1)
        return;
    clamp_active_biome(filter);
    removed = filter->active_biome;
    for (i = removed; i < s->n_biomes - 1; i++)
        s->biomes[i] = s->biomes[i + 1];
    s->n_biomes--;
    memset(&s->biomes[s->n_biomes], 0, sizeof(s->biomes[0]));
    remap_share_after_remove(s, removed);
    if (filter->active_biome >= s->n_biomes)
        filter->active_biome = s->n_biomes - 1;
}

static void gui_biome_list(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    int i;
    bool can_add, can_remove;

    clamp_active_biome(filter);

    for (i = 0; i < s->n_biomes; i++) {
        bool selected = (i == filter->active_biome);
        char label[40];
        const char *name = s->biomes[i].name[0] ? s->biomes[i].name : "(unnamed)";
        snprintf(label, sizeof(label), "%s", name);
        if (gui_selectable(label, &selected, NULL, -1) && selected)
            filter->active_biome = i;
    }

    can_add = s->n_biomes < BIOMES_MAX;
    can_remove = s->n_biomes > 1;

    gui_row_begin(3);
    gui_enabled_begin(can_add);
    if (gui_button("Add", 1, ICON_ADD))
        biome_add(filter);
    gui_enabled_end();
    gui_enabled_begin(can_add && s->n_biomes > 0);
    if (gui_button("Duplicate", 1, 0))
        biome_duplicate(filter);
    gui_enabled_end();
    gui_enabled_begin(can_remove);
    if (gui_button("Remove", 1, ICON_REMOVE))
        biome_remove(filter);
    gui_enabled_end();
    gui_row_end();
}

static void gui_fixed_seeds(biomes_biome_settings_t *b)
{
    int i;
    gui_text("Fixed seeds");
    for (i = 0; i < b->n_fixed_seeds; i++) {
        char id[16];
        int x = (int)b->fixed_x[i];
        int y = (int)b->fixed_y[i];
        snprintf(id, sizeof(id), "fs_%d", i);
        gui_push_id(id);
        gui_row_begin(3);
        gui_input_int("X", &x, 0, BIOMES_MAP_TILES - 1);
        gui_input_int("Y", &y, 0, BIOMES_MAP_TILES - 1);
        /* Hidden label: must not collide with the "X" input above. */
        if (gui_button("##rm_seed", 0, ICON_REMOVE)) {
            int j;
            for (j = i; j < b->n_fixed_seeds - 1; j++) {
                b->fixed_x[j] = b->fixed_x[j + 1];
                b->fixed_y[j] = b->fixed_y[j + 1];
            }
            b->n_fixed_seeds--;
            gui_row_end();
            gui_pop_id();
            break;
        }
        gui_row_end();
        b->fixed_x[i] = (int8_t)clamp(x, 0, BIOMES_MAP_TILES - 1);
        b->fixed_y[i] = (int8_t)clamp(y, 0, BIOMES_MAP_TILES - 1);
        gui_pop_id();
    }
    gui_enabled_begin(b->n_fixed_seeds < BIOMES_MAX_FIXED_SEEDS);
    if (gui_button("Add fixed seed", -1, 0)) {
        int i = b->n_fixed_seeds++;
        b->fixed_x[i] = (int8_t)(BIOMES_MAP_TILES / 2);
        b->fixed_y[i] = (int8_t)(BIOMES_MAP_TILES / 2);
    }
    gui_enabled_end();
}

static void gui_biome_detail(filter_biomes_t *filter)
{
    biomes_settings_t *s = filter->settings;
    biomes_biome_settings_t *b;
    biomes_biome_settings_t *def = NULL;
    int i;
    int share_combo;
    const char *share_names[BIOMES_MAX + 1];
    int share_indices[BIOMES_MAX + 1];
    int n_share = 0;

    clamp_active_biome(filter);
    if (s->n_biomes < 1)
        return;
    b = &s->biomes[filter->active_biome];
    if (filter->active_biome < g_default_biomes.n_biomes)
        def = &g_default_biomes.biomes[filter->active_biome];

    gui_input_text("Name", b->name, sizeof(b->name));

    gui_input_float("Height", &b->height, 0.01f, -2.f, 2.f, "%.2f");
    gui_tooltip_with_default(
        "Typical heightmap value (0-1). In AoS space higher = lower terrain",
        "%.2f", def ? def->height : 0.9f);
    gui_input_float("Variation", &b->variation, 0.01f, -2.f, 2.f, "%.2f");
    gui_tooltip_with_default("Random offset added to height per tile",
                             "%.2f", def ? def->variation : -0.1f);
    gui_input_float("Noise", &b->noise, 0.01f, 0.f, 1.f, "%.2f");
    gui_tooltip_with_default("Per-tile height jitter amplitude",
                             "%.2f", def ? def->noise : 0.05f);

    gui_checkbox("Place trees", &b->place_trees,
                 "Spawn trees on tiles of this biome");

    /* Share gradient combo: None + other biomes */
    share_names[0] = "None (own)";
    share_indices[0] = -1;
    n_share = 1;
    for (i = 0; i < s->n_biomes; i++) {
        if (i == filter->active_biome)
            continue;
        share_names[n_share] = s->biomes[i].name[0] ? s->biomes[i].name
                                                    : "(unnamed)";
        share_indices[n_share] = i;
        n_share++;
    }
    share_combo = 0;
    for (i = 0; i < n_share; i++) {
        if (share_indices[i] == b->share_gradient_from) {
            share_combo = i;
            break;
        }
    }
    if (gui_combo("Share gradient", &share_combo, share_names, n_share))
        b->share_gradient_from = share_indices[share_combo];
    gui_tooltip_if_hovered(
        "Reuse another biome's color gradient (classic Tundra uses Snow)");

    if (gui_collapsing_header("Color noise", true)) {
        gui_input_int("Intensity", &b->noise_intensity, 0, 100);
        gui_tooltip_with_default(
            "How strongly colour variation mixes in after gradient paint. "
            "0 = flat gradient",
            "%i", def ? def->noise_intensity : 12);
        gui_input_int("Saturation", &b->noise_saturation, 0, 100);
        gui_tooltip_with_default(
            "How colourful the variation is. 0 = lightness-only mottling",
            "%i", def ? def->noise_saturation : 6);
        gui_input_int("Coverage", &b->noise_coverage, 0, 100);
        gui_tooltip_with_default(
            "Fraction of this biome's surface pixels that receive noise",
            "%i", def ? def->noise_coverage : 100);
    }

    if (gui_collapsing_header("Placement", true)) {
        gui_fixed_seeds(b);
        gui_input_int("Random seeds", &b->random_seeds, 0, 32);
        gui_tooltip_if_hovered(
            "Extra random flood-fill seeds inside the region below");
        gui_input_int("Region X", &b->region_x, 0, BIOMES_MAP_TILES - 1);
        gui_input_int("Region Y", &b->region_y, 0, BIOMES_MAP_TILES - 1);
        gui_input_int("Region W", &b->region_w, 1, BIOMES_MAP_TILES);
        gui_input_int("Region H", &b->region_h, 1, BIOMES_MAP_TILES);
    }

    if (b->share_gradient_from < 0 ||
        b->share_gradient_from >= s->n_biomes) {
        if (gui_collapsing_header("Gradient", true)) {
            for (i = 0; i < b->n_stops; i++) {
                char clabel[32];
                char stop_id[16];
                snprintf(stop_id, sizeof(stop_id), "stop_%d", i);
                gui_push_id(stop_id);
                snprintf(clabel, sizeof(clabel), "Stop %d @%d", i,
                         b->stops[i].pos);
                if (gui_color_small(clabel, b->stops[i].rgb))
                    sync_stop_hsb_from_rgb(&b->stops[i]);
                gui_input_int("Pos", &b->stops[i].pos, 0, 64);
                gui_pop_id();
            }
        }
    }
}

static int gui(filter_t *filter_)
{
    filter_biomes_t *filter = (void *)filter_;
    biomes_settings_t *s;
    layer_t *layer;
    int i;
    const char *help_text =
        "Biomes: Triplefox random map via James Hofmann mapmaker.\n"
        "Add/remove biomes in the list. Hover fields for details. "
        "Reset restores random.txt defaults.";

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
        gui_biome_list(filter);
        gui_separator();
        gui_biome_detail(filter);
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
        gui_input_float("Dithering", &s->dithering, 1.f, 0.f, 64.f, "%.0f");
        gui_tooltip_with_default(
            "Spatially dither biome ids before gradient paint (pixels)",
            "%.0f", g_default_biomes.dithering);
        gui_checkbox("Smooth colors", &s->smooth_colors,
                     "Average neighboring surface colors");
    }

    if (gui_collapsing_header("Trees", false)) {
        gui_input_int("Min per tile", &s->trees_min_per_tile, 0, 64);
        gui_input_int("Max per tile", &s->trees_max_per_tile, 0, 64);
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
