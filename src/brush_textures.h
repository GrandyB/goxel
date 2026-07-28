/* Goxel 3D voxels editor
 *
 * copyright (c) 2015-2022 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BRUSH_TEXTURES_H
#define BRUSH_TEXTURES_H

#include "utils/texture.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    BRUSH_SOURCE_COLOR = 0,
    BRUSH_SOURCE_TEXTURE = 1,
};

typedef struct brush_texture brush_texture_t;
struct brush_texture {
    char *name;      // Display name (derived from filename).
    char *path;      // Absolute path in user texture directory.
    int w, h, bpp;   // Decoded image dimensions / channels.
    uint8_t *pixels; // CPU-side RGBA/RGB data, used for brush sampling.
    texture_t *preview; // Lazily-created GL texture for brush UI thumbnails.
    /* Per-texture HSL / opacity (remembered when flicking between textures).
     * Hue degrees [-180,+180], sat % [0,200] (100=identity),
     * lightness % [-100,+100] (0=identity), opacity 0..255. */
    float hue;
    float saturation;
    float lightness;
    uint8_t opacity;
    /* Last values baked into preview (for dirty detection). */
    float preview_hue;
    float preview_saturation;
    float preview_lightness;
    uint8_t preview_opacity;
};

void goxel_brush_textures_reload(void);
/* Fill out with the user textures directory path. Returns false if unavailable. */
bool goxel_brush_textures_dir(char *out, size_t out_size);
int goxel_brush_textures_count(void);
const brush_texture_t *goxel_brush_texture_get(int idx);
const brush_texture_t *goxel_brush_texture_current(void);
void goxel_brush_texture_set_current(int idx);
texture_t *goxel_brush_texture_preview_get(int idx);

/* Free catalog entries and CPU pixels (app shutdown). */
void goxel_brush_textures_clear(void);
/* Drop GL preview textures before the graphics context is destroyed. */
void goxel_brush_textures_release_graphics(void);

#endif // BRUSH_TEXTURES_H
