#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

#include "goxel.h"
EXTERNC void generate_tomland_terrain(volume_t *volume);

#undef EXTERNC