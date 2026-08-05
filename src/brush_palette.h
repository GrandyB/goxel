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

#ifndef BRUSH_PALETTE_H
#define BRUSH_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

/* Cap for multi-colour (Palette) brush selection set. */
#define BRUSH_PALETTE_MAX 64

/* Clear the multi-colour selection set. */
void goxel_brush_palette_clear(void);

/* Toggle colour in the set. Returns true if now selected. */
bool goxel_brush_palette_toggle(const uint8_t color[4]);

/* Add colour if missing (no remove). Returns true if present after. */
bool goxel_brush_palette_add(const uint8_t color[4]);

bool goxel_brush_palette_contains(const uint8_t color[4]);

/*
 * Shift+click a palette swatch:
 * - Already in palette mode: toggle colour in/out of the set.
 * - Otherwise enter palette mode: seed with painter.color; if `color` differs,
 *   add it too (same colour → only one entry).
 */
void goxel_brush_palette_shift_click(const uint8_t color[4]);

/* Copy first selected colour into out (or white if empty). */
void goxel_brush_palette_first_color(uint8_t out[4]);

/* Leave palette mode: set painter.color from first selected, clear set,
 * set brush_source_mode = BRUSH_SOURCE_COLOR. */
void goxel_brush_palette_exit_to_color(void);

/* Apply first selected colour, clear set, set brush_source_mode = mode. */
void goxel_brush_palette_exit_to_mode(int mode);

uint32_t goxel_brush_palette_fingerprint(void);

/*
 * Pick a colour for voxel `pos` using the current stroke seed.
 * Stable for a given (seed, pos) so previews do not flicker mid-stroke.
 * Returns false if the selection set is empty.
 */
bool goxel_brush_palette_sample_at(const int pos[3], uint8_t out[4]);

/*
 * Advance the stroke seed so the next paint/hover pass gets a new random
 * pattern. Call after a brush gesture END once voxels are committed.
 */
void goxel_brush_palette_reroll_seed(void);

#endif // BRUSH_PALETTE_H
