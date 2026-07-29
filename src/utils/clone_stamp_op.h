/* Clone stamp volume apply — independent of volume_op. */

#ifndef CLONE_STAMP_OP_H
#define CLONE_STAMP_OP_H

#include "shape.h"
#include "volume.h"

/*
 * How to pick a colour in each source column.
 *
 * take_uppermost — absolute outermost solid along the inherit axis
 * (full map extent).  Otherwise — only search
 * [source_depth ± depth] along that axis.
 *
 * When source_face / target_face are in 0..5 (cube faces), inheritance
 * and column mapping follow those face normals/tangents (wall mode).
 * When either is < 0, classic XY columns + world-Z inherit.
 */
typedef struct clone_stamp_sample {
    bool take_uppermost;
    bool surface_mode; /* top-down exposed destination shell; ignores brush Z */
    int  depth;
    int  source_face; /* -1 = classic Z; else 0..5 */
    int  target_face; /* -1 = classic Z; else 0..5 */
} clone_stamp_sample_t;

/*
 * Paint colours from `sample` onto existing voxels in `dest` within the
 * brush shape centered at `target` (tangential offset from source; depth
 * via opts).
 *
 * smoothness / dithering match brush painter antialiasing (soft coverage
 * and scattered SDF edges).  opacity (0..1) scales MODE_PAINT strength.
 */
void clone_stamp_apply(volume_t *dest, const volume_t *sample,
                       const float target[3], const float source[3],
                       const float box[4][4], const shape_t *shape,
                       float smoothness, float dithering, float opacity,
                       const clone_stamp_sample_t *opts);

/*
 * Write marker voxels into `dest` at the exact sample positions for the
 * brush footprint at `source`.  `dest` is typically a sparse overlay volume
 * rendered on top of the scene (so other layers' blocks stay visible).
 */
void clone_stamp_preview_source(volume_t *dest, const volume_t *sample,
                                const float source[3],
                                const float box[4][4], const shape_t *shape,
                                float smoothness,
                                const clone_stamp_sample_t *opts,
                                const uint8_t marker_color[4]);

/*
 * MODE_PAINT a fixed tint onto existing voxels inside the brush shape.
 */
void clone_stamp_highlight(volume_t *dest, const float center[3],
                           const float box[4][4], const shape_t *shape,
                           float smoothness, const uint8_t color[4]);

#endif // CLONE_STAMP_OP_H
