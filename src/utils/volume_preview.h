/* Goxel
 *
 * Single-volume off-screen preview: rasterize a volume to a 2D texture or RGBA
 * buffer for UI thumbnails. Does not use the main editor camera or merged
 * scene.
 */

#ifndef VOLUME_PREVIEW_H
#define VOLUME_PREVIEW_H

#include "texture.h"

#include "volume.h"

/*
 * Renders vol into a GL RGBA texture (power-of-2) suitable for ImGui. Returns
 * NULL if vol is NULL or has no voxels. Caller must texture_delete() when done.
 */
texture_t *volume_preview_to_texture(const volume_t *vol, int size);

/*
 * Renders vol into a caller-provided w×h RGBA (bpp 4) or RGB (bpp 3) buffer.
 * Returns 0 on success, -1 on failure or empty volume.
 */
int volume_preview_to_rgba(
        const volume_t *vol, int w, int h, uint8_t *buf, int bpp);

#endif
