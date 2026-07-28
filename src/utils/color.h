/* Goxel 3D voxels editor
 *
 * copyright (c) 2019 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef COLOR_H
#define COLOR_H

#include <stdint.h>

/*
 * Convert from sRGB uint8 to linear RGB float.
 */
void srgb8_to_rgb(const uint8_t srgba[3], float rgba[3]);

/*
 * Convert from linear RGB float to sRGB uint8.
 */
void rgb_to_srgb8(const float rgb[3], uint8_t srgb[3]);

/*
 * Convert from sRGBA uint8 to linear RGBA float.
 */
void srgba8_to_rgba(const uint8_t srgba[4], float rgba[4]);

/*
 * Convert HSV (h in [0,1) or any real; s,v in [0,1]) to 8-bit RGB.
 */
void hsv_to_rgb_u8(float h, float s, float v, uint8_t rgb[3]);

/*
 * HSL: h in degrees [0,360), s and l in [0,1].
 */
void srgb8_to_hsl(const uint8_t srgb[3], float hsl[3]);
void hsl_to_srgb8(const float hsl[3], uint8_t srgb[3]);

/*
 * Move x in [0,1] toward 0 (v<0) or 1 (v>0) by |v|.
 */
void hsl_move_value(float *x, float v);

/*
 * Adjust sRGB in HSL space.
 *   hue_deg  - degrees added to H
 *   sat_pct  - saturation scale percent (100 = identity)
 *   lit_pct  - lightness move percent (0 = identity; see hsl_move_value)
 */
void srgb8_adjust_hsl(uint8_t rgb[3], float hue_deg, float sat_pct,
                      float lit_pct);


#endif // COLOR_H
