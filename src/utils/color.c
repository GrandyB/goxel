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

static float color_clampf(float x, float a, float b)
{
    if (x < a) return a;
    if (x > b) return b;
    return x;
}

static float color_hsl_mod(float x, float y)
{
    while (x < 0.f)
        x += y;
    return fmodf(x, y);
}

void srgb8_to_hsl(const uint8_t srgb[3], float hsl[3])
{
    float h = 0.f, s, v, m, c, l;
    const float r = srgb[0] / 255.f;
    const float g = srgb[1] / 255.f;
    const float b = srgb[2] / 255.f;

    v = r > g ? (r > b ? r : b) : (g > b ? g : b);
    m = r < g ? (r < b ? r : b) : (g < b ? g : b);
    l = (v + m) * 0.5f;
    c = v - m;
    if (c == 0.f) {
        hsl[0] = 0.f;
        hsl[1] = 0.f;
        hsl[2] = l;
        return;
    }
    if (v == r)
        h = (g - b) / c + (g < b ? 6.f : 0.f);
    else if (v == g)
        h = (b - r) / c + 2.f;
    else
        h = (r - g) / c + 4.f;
    h *= 60.f;
    s = (l > 0.5f) ? c / (2.f - v - m) : c / (v + m);
    hsl[0] = h;
    hsl[1] = s;
    hsl[2] = l;
}

void hsl_to_srgb8(const float hsl[3], uint8_t srgb[3])
{
    float r = 0.f, g = 0.f, b = 0.f, c, x, m;
    const float h = hsl[0] / 60.f, s = hsl[1], l = hsl[2];
    c = (1.f - fabsf(2.f * l - 1.f)) * s;
    x = c * (1.f - fabsf(fmodf(h, 2.f) - 1.f));
    if (h < 1.f)      { r = c; g = x; b = 0.f; }
    else if (h < 2.f) { r = x; g = c; b = 0.f; }
    else if (h < 3.f) { r = 0.f; g = c; b = x; }
    else if (h < 4.f) { r = 0.f; g = x; b = c; }
    else if (h < 5.f) { r = x; g = 0.f; b = c; }
    else              { r = c; g = 0.f; b = x; }
    m = l - 0.5f * c;
    srgb[0] = (uint8_t)color_clampf((r + m) * 255.f, 0.f, 255.f);
    srgb[1] = (uint8_t)color_clampf((g + m) * 255.f, 0.f, 255.f);
    srgb[2] = (uint8_t)color_clampf((b + m) * 255.f, 0.f, 255.f);
}

void hsl_move_value(float *x, float v)
{
    float dst = v >= 0.f ? 1.f : 0.f;
    v = fabsf(v);
    *x = (1.f - v) * (*x) + v * dst;
}

void srgb8_adjust_hsl(uint8_t rgb[3], float hue_deg, float sat_pct,
                      float lit_pct)
{
    float hsl[3];
    if (hue_deg == 0.f && sat_pct == 100.f && lit_pct == 0.f)
        return;
    srgb8_to_hsl(rgb, hsl);
    hsl[0] = color_hsl_mod(hsl[0] + hue_deg, 360.f);
    /* 100 = original saturation; 0 = grey; 200 = 2× (clamped). */
    hsl[1] = color_clampf(hsl[1] * (sat_pct / 100.f), 0.f, 1.f);
    hsl_move_value(&hsl[2], lit_pct / 100.f);
    hsl_to_srgb8(hsl, rgb);
}
