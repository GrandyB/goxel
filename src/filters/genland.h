#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include "goxel.h"

/* Floor for the generation height cap (voxels). Actual cap is
 * max(GENLAND_MIN_HEIGHT, image box Z). Peaks are clipped to that cap only. */
#define GENLAND_MIN_HEIGHT 64

typedef struct {
    // Generation
    int num_octaves;
    int seed;
    float amp_octave_mult;
    float river_width;
    float river_phase;
    float river_meander;
    int num_rivers;
    float amplitude;
    float base_height;
    float noise_terrain;
    float noise_river;

    // Colors
    uint8_t color_ground[4];
    uint8_t color_grass1[4];
    uint8_t color_grass2[4];
    uint8_t color_water[4];
    float grass_bias;

    // Lighting
    float shadow_factor;
    float ambience_factor;

    // Transform
    bool resize_image;
    layer_target_t layer_target;
} genland_settings_t;

EXTERNC void generate_tomland_terrain(volume_t *volume, genland_settings_t *settings);

#undef EXTERNC