/* Clone stamp volume apply — independent of volume_op. */

#ifndef CLONE_STAMP_OP_H
#define CLONE_STAMP_OP_H

#include "shape.h"
#include "volume.h"

/*
 * How to pick a colour in each source XY column.
 *
 * take_uppermost — absolute top solid in the column (full map height).
 * Otherwise — only search Z in [source_z - depth, source_z + depth],
 * taking the uppermost solid in that window.  depth 0 = exact source Z.
 */
typedef struct clone_stamp_sample {
    bool take_uppermost;
    int  depth;
} clone_stamp_sample_t;

/*
 * Paint colours from `sample` onto existing voxels in `dest` within the
 * brush shape centered at `target` (XY offset from source; Z via opts).
 *
 * smoothness / dithering match brush painter antialiasing (soft coverage
 * and scattered SDF edges).
 */
void clone_stamp_apply(volume_t *dest, const volume_t *sample,
                       const float target[3], const float source[3],
                       const float box[4][4], const shape_t *shape,
                       float smoothness, float dithering,
                       const clone_stamp_sample_t *opts);

/*
 * Highlight the exact voxels in `dest` that would be copied from for the
 * brush footprint at `source` (marker tint on those blocks only).
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
