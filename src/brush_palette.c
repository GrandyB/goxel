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

#include "goxel.h"

#include <stdlib.h>
#include <string.h>

void goxel_brush_palette_clear(void)
{
    goxel.brush_palette_count = 0;
}

bool goxel_brush_palette_contains(const uint8_t color[4])
{
    int i;

    if (!color)
        return false;
    for (i = 0; i < goxel.brush_palette_count; i++) {
        if (memcmp(goxel.brush_palette_colors[i], color, 4) == 0)
            return true;
    }
    return false;
}

bool goxel_brush_palette_toggle(const uint8_t color[4])
{
    int i;

    if (!color)
        return false;
    for (i = 0; i < goxel.brush_palette_count; i++) {
        if (memcmp(goxel.brush_palette_colors[i], color, 4) == 0) {
            if (i + 1 < goxel.brush_palette_count) {
                memmove(goxel.brush_palette_colors[i],
                        goxel.brush_palette_colors[i + 1],
                        (size_t)(goxel.brush_palette_count - i - 1) * 4);
            }
            goxel.brush_palette_count--;
            return false;
        }
    }
    if (goxel.brush_palette_count >= BRUSH_PALETTE_MAX)
        return true; /* already full; treat as selected */
    memcpy(goxel.brush_palette_colors[goxel.brush_palette_count], color, 4);
    goxel.brush_palette_count++;
    return true;
}

bool goxel_brush_palette_add(const uint8_t color[4])
{
    if (!color)
        return false;
    if (goxel_brush_palette_contains(color))
        return true;
    if (goxel.brush_palette_count >= BRUSH_PALETTE_MAX)
        return true;
    memcpy(goxel.brush_palette_colors[goxel.brush_palette_count], color, 4);
    goxel.brush_palette_count++;
    return true;
}

void goxel_brush_palette_shift_click(const uint8_t color[4])
{
    bool in_mode;

    if (!color)
        return;

    in_mode = (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE &&
               goxel.brush_palette_count > 0);
    if (in_mode) {
        goxel_brush_palette_toggle(color);
        if (goxel.brush_palette_count > 0)
            goxel.brush_source_mode = BRUSH_SOURCE_PALETTE;
        else
            goxel.brush_source_mode = BRUSH_SOURCE_COLOR;
        return;
    }

    /* Enter palette mode from a single colour: keep the brush colour, and
     * include the clicked swatch when it differs. */
    goxel_brush_palette_clear();
    goxel_brush_palette_add(goxel.painter.color);
    if (memcmp(goxel.painter.color, color, 4) != 0)
        goxel_brush_palette_add(color);
    goxel.brush_source_mode = BRUSH_SOURCE_PALETTE;
}

void goxel_brush_palette_first_color(uint8_t out[4])
{
    if (!out)
        return;
    if (goxel.brush_palette_count > 0)
        memcpy(out, goxel.brush_palette_colors[0], 4);
    else
        memcpy(out, (uint8_t[4]){255, 255, 255, 255}, 4);
}

void goxel_brush_palette_exit_to_color(void)
{
    goxel_brush_palette_exit_to_mode(BRUSH_SOURCE_COLOR);
}

void goxel_brush_palette_exit_to_mode(int mode)
{
    if (goxel.brush_palette_count > 0)
        memcpy(goxel.painter.color, goxel.brush_palette_colors[0], 4);
    goxel_brush_palette_clear();
    goxel.brush_source_mode = mode;
}

uint32_t goxel_brush_palette_fingerprint(void)
{
    uint32_t h = 2166136261u;
    int i, c;

    h ^= goxel.brush_palette_stroke_seed;
    h *= 16777619u;
    h ^= (uint32_t)goxel.brush_palette_count;
    h *= 16777619u;
    for (i = 0; i < goxel.brush_palette_count; i++) {
        for (c = 0; c < 4; c++) {
            h ^= goxel.brush_palette_colors[i][c];
            h *= 16777619u;
        }
    }
    return h;
}

static uint32_t brush_palette_hash_pos(uint32_t seed, int x, int y, int z)
{
    uint32_t h = seed ? seed : 0xA341316Cu;

    h ^= (uint32_t)x * 374761393u;
    h ^= (uint32_t)y * 668265263u;
    h ^= (uint32_t)z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

bool goxel_brush_palette_sample_at(const int pos[3], uint8_t out[4])
{
    uint32_t h;
    int idx;

    if (!out || !pos || goxel.brush_palette_count <= 0)
        return false;
    h = brush_palette_hash_pos(goxel.brush_palette_stroke_seed,
                               pos[0], pos[1], pos[2]);
    idx = (int)(h % (uint32_t)goxel.brush_palette_count);
    memcpy(out, goxel.brush_palette_colors[idx], 4);
    return true;
}

void goxel_brush_palette_reroll_seed(void)
{
    uint32_t a = (uint32_t)rand();
    uint32_t b = (uint32_t)rand();

    goxel.brush_palette_stroke_seed =
        (a << 16) ^ b ^ ((uint32_t)goxel.frame_count * 0x9e3779b9u);
    if (goxel.brush_palette_stroke_seed == 0)
        goxel.brush_palette_stroke_seed = 1;
}
