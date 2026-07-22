/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
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

#include "color.h"

#include <math.h>

void srgb8_to_rgb(const uint8_t srgb[3], float rgb[3])
{
    // https://en.wikipedia.org/wiki/SRGB
    float c;
    int i;
    for (i = 0; i < 3; i++) {
        c = srgb[i] / 255.f;
        c = (c <= 0.04045f) ? (c / 12.92f) : pow((c + 0.055) / 1.055, 2.4);
        rgb[i] = c;
    }
}

void rgb_to_srgb8(const float rgb[3], uint8_t srgb[3])
{
    // https://en.wikipedia.org/wiki/SRGB
    float c;
    int i, b;
    for (i = 0; i < 3; i++) {
        c = rgb[i];
        c = (c <= 0.0031308f) ? 12.92f * c
                              : (1.055f) * pow(c, 1 / 2.4f) - 0.055f;
        b = c * 255 + 0.5;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        srgb[i] = b;
    }
}

void srgba8_to_rgba(const uint8_t srgba[4], float rgba[4])
{
    srgb8_to_rgb(srgba, rgba);
    rgba[3] = srgba[3] / 255.f;
}

void hsv_to_rgb_u8(float h, float s, float v, uint8_t rgb[3])
{
    float r, g, b, f, p, q, t;
    int i;

    h = fmodf(h, 1.f);
    if (h < 0.f) h += 1.f;
    if (s < 0.f) s = 0.f;
    if (s > 1.f) s = 1.f;
    if (v < 0.f) v = 0.f;
    if (v > 1.f) v = 1.f;
    i = (int)(h * 6.f);
    f = h * 6.f - (float)i;
    p = v * (1.f - s);
    q = v * (1.f - f * s);
    t = v * (1.f - (1.f - f) * s);
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (uint8_t)(r * 255.f + 0.5f);
    rgb[1] = (uint8_t)(g * 255.f + 0.5f);
    rgb[2] = (uint8_t)(b * 255.f + 0.5f);
}
