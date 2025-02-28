#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include "goxel.h"

typedef struct {
    int max_height;
    int num_octaves;
    float amp_octave_mult;
    float river_width;
    float variety;
    float offset;

    uint8_t color_ground[4];
    uint8_t color_grass1[4];
    uint8_t color_grass2[4];
    uint8_t color_water[4];
} genland_settings_t;

EXTERNC void generate_tomland_terrain(volume_t *volume, genland_settings_t *settings);

#undef EXTERNC