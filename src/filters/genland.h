#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include "goxel.h"

typedef struct {
    int max_height;
} genland_settings_t;

EXTERNC void generate_tomland_terrain(volume_t *volume, genland_settings_t *settings);

#undef EXTERNC