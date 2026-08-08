/* Wrap-preview around the image box: bake once, draw translated copies. */
#ifndef WRAP_VIEW_H
#define WRAP_VIEW_H

#include "render.h"

enum { WRAP_VIEW_NEIGHBOR_COUNT = 8 };

void goxel_wrap_view_set(bool enabled);

/* Image-box neighbor offsets in voxel units (edges + corners). */
void wrap_view_neighbor_offsets(const float box[4][4],
                                float out[WRAP_VIEW_NEIGHBOR_COUNT][3]);

/* Queue baked wrap instances for the current view (live goxel.rend lighting). */
void wrap_view_render(renderer_t *rend, int effects);

#endif
