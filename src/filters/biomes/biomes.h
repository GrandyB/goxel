/* Biomes terrain generator - Triplefox random.txt recipe via mapmaker.
 *
 * Mapmaker originally by James Hofmann (2012), GPLv3.
 */

#ifndef FILTERS_BIOMES_BIOMES_H
#define FILTERS_BIOMES_BIOMES_H

#include "goxel.h"
#include <stdbool.h>
#include <stdint.h>

#define BIOMES_MAX 16
#define BIOMES_MAX_GRAD_STOPS 8
#define BIOMES_MAX_FIXED_SEEDS 8
#define BIOMES_MAX_FOLIAGE 6
#define BIOMES_MIN_HEIGHT 64
#define BIOMES_MAP_TILES 32

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
    char name[32];
    float height;
    float variation;
    float noise;
    int n_stops;
    biomes_grad_stop_t stops[BIOMES_MAX_GRAD_STOPS];

    /* Fixed seed points on the 32x32 biome tile map. */
    int n_fixed_seeds;
    int8_t fixed_x[BIOMES_MAX_FIXED_SEEDS];
    int8_t fixed_y[BIOMES_MAX_FIXED_SEEDS];

    /* Extra random seeds inside region (tile coords). */
    int random_seeds;
    int region_x;
    int region_y;
    int region_w;
    int region_h;

    bool place_trees;
    /* -1 = own gradient; else index into settings->biomes[]. */
    int share_gradient_from;

    /* Surface colour noise after gradient paint (brush-style). */
    int noise_intensity;
    int noise_saturation;
    int noise_coverage;
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

    int n_biomes;
    biomes_biome_settings_t biomes[BIOMES_MAX];

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
    float dithering;
    bool smooth_colors;

    /* Trees (appearance; which biomes get trees is per-biome place_trees) */
    int trees_min_per_tile;
    int trees_max_per_tile;
    int trunk_h_min;
    int trunk_h_max;
    int foliage_count;
    uint8_t foliage_colors[BIOMES_MAX_FOLIAGE][4];
    uint8_t trunk_color[4];

    int seed;
    bool resize_image;
    layer_target_t layer_target;
} biomes_settings_t;

void biomes_settings_set_defaults(biomes_settings_t *s);
void generate_biomes_terrain(volume_t *volume, biomes_settings_t *settings);

#endif /* FILTERS_BIOMES_BIOMES_H */
