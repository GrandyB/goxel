/* Biomes terrain generator - Triplefox random.txt recipe via mapmaker.
 *
 * Mapmaker originally by James Hofmann (2012), GPLv3.
 */

#ifndef FILTERS_BIOMES_BIOMES_H
#define FILTERS_BIOMES_BIOMES_H

#include "goxel.h"
#include <stdbool.h>
#include <stdint.h>

#define BIOMES_COUNT 5
#define BIOMES_MAX_GRAD_STOPS 8
#define BIOMES_MAX_FOLIAGE 6
#define BIOMES_MIN_HEIGHT 64

/* Gradient built as: set_step_rgb(stops[0]), then hsb segments between
 * consecutive authoring stops when use_hsb, else rgb segments. */
typedef struct {
    int pos;
    uint8_t rgb[4];
    /* If HSB segment endpoint: H 0-360, S/B 0-100 (also mirrored in rgb for GUI). */
    float hsb[3];
    bool is_hsb;
} biomes_grad_stop_t;

typedef struct {
    float height;
    float variation;
    float noise;
    int n_stops;
    biomes_grad_stop_t stops[BIOMES_MAX_GRAD_STOPS];
} biomes_biome_settings_t;

typedef struct {
    /* Terrain */
    float displace_jitter;
    float displace_span_scale;
    int displace_skip;
    bool biome_jitter;
    /* Extra float smooth passes before truncate (random.txt used 1). */
    int height_smooth_passes;
    /* 3x3 median on quantized tops - kills checkerboard flecks. */
    int height_despeckle_passes;

    /* Biomes: 0 grass, 1 snow, 2 hill, 3 water, 4 tundra */
    biomes_biome_settings_t biomes[BIOMES_COUNT];
    /* Tundra shares snow gradient stops when true (random.txt). */
    bool tundra_shares_snow_gradient;

    /* River */
    bool river_enabled;
    int river_x_half_width;
    int river_y_increment;
    int river_x_increment;
    int river_add_radius;
    float river_add_depth;
    int river_set_radius;
    float river_set_height;

    /* Color post */
    float color_jitter;
    int rgb_noise_low;
    int rgb_noise_high;
    bool smooth_colors;

    /* Trees */
    bool trees_enabled;
    int trees_min_per_tile;
    int trees_max_per_tile;
    int trunk_h_min;
    int trunk_h_max;
    int foliage_count;
    uint8_t foliage_colors[BIOMES_MAX_FOLIAGE][4];
    uint8_t trunk_color[4];

    int seed;
    bool resize_image;
    bool replace_current_layer;
} biomes_settings_t;

void biomes_settings_set_defaults(biomes_settings_t *s);
void generate_biomes_terrain(volume_t *volume, biomes_settings_t *settings);

#endif /* FILTERS_BIOMES_BIOMES_H */
