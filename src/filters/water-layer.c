/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Flat 1-block water sheet on the image-box bottom. Colour is painted with a
 * single domain-warped directional FBM field (swells + ripples + foam crests).
 * Always overwrites the bottom voxel of each column.
 *
 * Optional colour bleed (independent of presets): when bleed_distance > 0,
 * colours from solids one block above the water (z = bottom + 1) are blended
 * into the water sheet, fading outwards over bleed_distance blocks at
 * bleed_strength opacity. bleed_dithering scatters the falloff; bleed_blur
 * box-blurs that dithered contribution; bleed_noise then adds limited-sat
 * luminance noise on the blurred colour before compositing.
 */

typedef struct {
    uint8_t color[4];       /* mid water */
    uint8_t deep_color[4];  /* troughs */
    uint8_t foam_color[4];  /* bright crests */
    float scale;            /* feature size in blocks (higher = larger swells) */
    float direction_deg;    /* swell travel direction */
    float stretch;          /* anisotropy along swell (>1 = elongated waves) */
    float warp;             /* domain-warp strength */
    float detail;           /* fine ripple mix 0..1 */
    float foam;             /* crest foam amount / sharpness 0..1 */
    float contrast;         /* overall deep↔foam separation 0..1 */
    int seed;
} water_layer_settings_t;

typedef struct {
    const char *name;
    water_layer_settings_t settings;
} water_layer_preset_t;

/* Bleed is filter-owned so Load preset / wave Reset leave it alone. */
typedef struct {
    filter_t filter;
    water_layer_settings_t settings;
    int preset_index;
    int bleed_distance;
    float bleed_strength;
    float bleed_lightness;  /* 0 = black, 1 = unchanged, 2 = white */
    float bleed_blur;       /* box-blur radius on dithered bleed (blocks) */
    float bleed_dithering;  /* edge scatter radius (brush-style) */
    float bleed_noise;      /* random RGB noise amplitude on bled colour */
    bool replace_current_layer;
} filter_water_layer_t;

static const water_layer_settings_t default_settings = {
    .color = {48, 96, 118, 255},
    .deep_color = {28, 58, 78, 255},
    .foam_color = {170, 210, 220, 255},
    .scale = 28.0f,
    .direction_deg = 28.0f,
    .stretch = 2.2f,
    .warp = 0.45f,
    .detail = 0.35f,
    .foam = 0.4f,
    .contrast = 0.75f,
    .seed = 0,
};

static const water_layer_preset_t presets[] = {
    {
        .name = "Open ocean",
        .settings = {
            .color = {40, 85, 115, 255},
            .deep_color = {18, 45, 70, 255},
            .foam_color = {135, 156, 163, 255},
            .scale = 32.0f,
            .direction_deg = 35.0f,
            .stretch = 2.8f,
            .warp = 0.25f,
            .detail = 0.4f,
            .foam = 0.3f,
            .contrast = 0.65f,
            .seed = 42,
        },
    },
    {
        .name = "Calm lake",
        .settings = {
            .color = {55, 105, 125, 255},
            .deep_color = {35, 70, 90, 255},
            .foam_color = {150, 195, 210, 255},
            .scale = 48.0f,
            .direction_deg = 15.0f,
            .stretch = 1.6f,
            .warp = 0.25f,
            .detail = 0.2f,
            .foam = 0.2f,
            .contrast = 0.55f,
            .seed = 11,
        },
    },
    {
        .name = "Choppy sea",
        .settings = {
            .color = {45, 80, 105, 255},
            .deep_color = {22, 42, 60, 255},
            .foam_color = {200, 225, 235, 255},
            .scale = 18.0f,
            .direction_deg = 50.0f,
            .stretch = 1.8f,
            .warp = 0.7f,
            .detail = 0.65f,
            .foam = 0.7f,
            .contrast = 0.9f,
            .seed = 77,
        },
    },
    {
        .name = "Tropical shallows",
        .settings = {
            .color = {55, 145, 155, 255},
            .deep_color = {30, 100, 120, 255},
            .foam_color = {210, 240, 245, 255},
            .scale = 36.0f,
            .direction_deg = 10.0f,
            .stretch = 2.0f,
            .warp = 0.4f,
            .detail = 0.3f,
            .foam = 0.35f,
            .contrast = 0.7f,
            .seed = 3,
        },
    },
    {
        .name = "Murky harbour",
        .settings = {
            .color = {55, 75, 70, 255},
            .deep_color = {35, 48, 42, 255},
            .foam_color = {140, 155, 145, 255},
            .scale = 40.0f,
            .direction_deg = 5.0f,
            .stretch = 1.4f,
            .warp = 0.35f,
            .detail = 0.25f,
            .foam = 0.15f,
            .contrast = 0.5f,
            .seed = 19,
        },
    },
    {
        /* Matches genland default water (#3C6478) with almost flat surface. */
        .name = "Classic gen",
        .settings = {
            .color = {57, 100, 120, 255},
            .deep_color = {40, 75, 93, 255},
            .foam_color = {76, 107, 123, 255},
            .scale = 30.0f,
            .direction_deg = 18.0f,
            .stretch = 1.3f,
            .warp = 0.12f,
            .detail = 0.28f,
            .foam = 0.3f,
            .contrast = 0.7f,
            .seed = 1,
        },
    },
};

static const int preset_count = (int)(sizeof(presets) / sizeof(presets[0]));

static void settings_copy(water_layer_settings_t *dst,
                          const water_layer_settings_t *src)
{
    memcpy(dst, src, sizeof(*dst));
}

static void reset_bleed_defaults(filter_water_layer_t *filter)
{
    filter->bleed_distance = 6;
    filter->bleed_strength = 0.8f;
    filter->bleed_lightness = 0.75f;
    filter->bleed_blur = 2.0f;
    filter->bleed_dithering = 4.0f;
    filter->bleed_noise = 6.0f;
}

static void reset_to_defaults(filter_water_layer_t *filter)
{
    settings_copy(&filter->settings, &default_settings);
    filter->preset_index = 0;
    reset_bleed_defaults(filter);
}

static void load_preset(filter_water_layer_t *filter, int index)
{
    if (index < 0 || index >= preset_count)
        return;
    settings_copy(&filter->settings, &presets[index].settings);
    filter->preset_index = index;
}

/* ---- Gradient noise + FBM ------------------------------------------------ */

static unsigned char g_perm[512];

static void noise_init(int seed)
{
    int i, j, k;
    unsigned char p[256];

    srand((unsigned)seed);
    for (i = 0; i < 256; i++)
        p[i] = (unsigned char)i;
    for (i = 255; i > 0; i--) {
        j = rand() % (i + 1);
        k = p[i];
        p[i] = p[j];
        p[j] = (unsigned char)k;
    }
    for (i = 0; i < 256; i++) {
        g_perm[i] = p[i];
        g_perm[i + 256] = p[i];
    }
}

static float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float grad2(int h, float x, float y)
{
    switch (h & 7) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x;
    case 5: return -x;
    case 6: return y;
    default: return -y;
    }
}

static float perlin2(float x, float y)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    int xi = x0 & 255;
    int yi = y0 & 255;
    float u = fade(fx);
    float v = fade(fy);
    int aa = g_perm[g_perm[xi] + yi];
    int ab = g_perm[g_perm[xi] + yi + 1];
    int ba = g_perm[g_perm[xi + 1] + yi];
    int bb = g_perm[g_perm[xi + 1] + yi + 1];
    float x1 = grad2(aa, fx, fy) +
               (grad2(ba, fx - 1.0f, fy) - grad2(aa, fx, fy)) * u;
    float x2 = grad2(ab, fx, fy - 1.0f) +
               (grad2(bb, fx - 1.0f, fy - 1.0f) - grad2(ab, fx, fy - 1.0f)) * u;
    return x1 + (x2 - x1) * v; /* roughly [-1, 1] */
}

static float fbm2(float x, float y, int octaves, float persistence, float lacunarity)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;
    int i;
    int oct = clamp(octaves, 1, 8);

    for (i = 0; i < oct; i++) {
        sum += amp * perlin2(x * freq, y * freq);
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    if (norm <= 1e-6f)
        return 0.0f;
    return sum / norm;
}

/*
 * Height field in [0, 1]:
 *  1. Anisotropic UV from swell direction / stretch
 *  2. Domain warp for organic flow
 *  3. Large-scale FBM swells
 *  4. Fine ripple layer
 *  5. Soft troughs + sharp foam crests
 */
static float water_field(const water_layer_settings_t *s, float x, float y)
{
    float scale = max(s->scale, 1.0f);
    float ang = s->direction_deg * (float)(M_PI / 180.0);
    float ca = cosf(ang);
    float sa = sinf(ang);
    float stretch = max(s->stretch, 1.0f);
    float u = (x * ca + y * sa) / scale;
    float v = (-x * sa + y * ca) / (scale * stretch);
    float warp = clamp(s->warp, 0.0f, 2.0f);
    float detail = clamp(s->detail, 0.0f, 1.0f);
    float foam = clamp(s->foam, 0.0f, 1.0f);
    float wu, wv, swell, ripples, h, crest, t;

    wu = u + warp * fbm2(u * 0.7f + 3.1f, v * 0.7f + 1.7f, 3, 0.5f, 2.0f);
    wv = v + warp * fbm2(u * 0.7f + 8.4f, v * 0.7f + 5.2f, 3, 0.5f, 2.0f);

    swell = fbm2(wu, wv, 5, 0.52f, 2.05f);
    ripples = fbm2(wu * 4.5f + 12.0f, wv * 4.5f - 7.0f, 3, 0.45f, 2.2f);

    h = swell * (1.0f - detail * 0.55f) + ripples * detail * 0.55f;
    h = h * 0.5f + 0.5f; /* [-1,1] → [0,1] */

    /* Soften lows, sharpen highs into foam streaks. */
    t = h * h * (3.0f - 2.0f * h);
    crest = smoothstep(1.0f - foam * 0.55f - 0.2f, 1.0f, h);
    h = t * (1.0f - foam * 0.35f) + crest * foam;
    return clamp(h, 0.0f, 1.0f);
}

static void lerp_color(const uint8_t a[4], const uint8_t b[4], float t,
                       uint8_t out[4])
{
    float u = clamp(t, 0.0f, 1.0f);
    out[0] = (uint8_t)clamp((int)lroundf(a[0] + (b[0] - a[0]) * u), 0, 255);
    out[1] = (uint8_t)clamp((int)lroundf(a[1] + (b[1] - a[1]) * u), 0, 255);
    out[2] = (uint8_t)clamp((int)lroundf(a[2] + (b[2] - a[2]) * u), 0, 255);
    out[3] = 255;
}

/* Blend overlay RGB into base at opacity t; keep base alpha. */
static void blend_rgb(uint8_t base[4], const uint8_t overlay[4], float t)
{
    float u = clamp(t, 0.0f, 1.0f);
    base[0] = (uint8_t)clamp((int)lroundf(base[0] + (overlay[0] - base[0]) * u), 0, 255);
    base[1] = (uint8_t)clamp((int)lroundf(base[1] + (overlay[1] - base[1]) * u), 0, 255);
    base[2] = (uint8_t)clamp((int)lroundf(base[2] + (overlay[2] - base[2]) * u), 0, 255);
}

/* 0 = black, 1 = unchanged, 2 = white. */
static void apply_bleed_lightness(uint8_t c[4], float lightness)
{
    float L = clamp(lightness, 0.0f, 2.0f);
    float t;
    int i;

    if (fabsf(L - 1.0f) < 1e-6f)
        return;
    if (L <= 1.0f) {
        for (i = 0; i < 3; i++)
            c[i] = (uint8_t)clamp((int)lroundf((float)c[i] * L), 0, 255);
    } else {
        t = L - 1.0f;
        for (i = 0; i < 3; i++)
            c[i] = (uint8_t)clamp(
                (int)lroundf((float)c[i] + (255.0f - (float)c[i]) * t), 0, 255);
    }
}

/*
 * Random noise on bled RGB. Mostly luminance (±amount on all channels) with
 * a small chromatic component so saturation stays limited (brush-like).
 */
static void noise_bleed_color(uint8_t c[4], int x, int y, int z, float amount)
{
    const float sat = 0.05f; /* ~brush default noise_saturation (5/100) */
    float a = fabsf(amount);
    float n, cr, cg, cb;
    if (a < 1e-6f)
        return;
    n = uniform_noise((float)x, (float)y, (float)z) * 2.0f - 1.0f;
    cr = uniform_noise((float)x + 19.0f, (float)y, (float)z) * 2.0f - 1.0f;
    cg = uniform_noise((float)x, (float)y + 37.0f, (float)z) * 2.0f - 1.0f;
    cb = uniform_noise((float)x, (float)y, (float)z + 53.0f) * 2.0f - 1.0f;
    c[0] = (uint8_t)clamp(
        (int)lroundf((float)c[0] + (n + cr * sat) * a), 0, 255);
    c[1] = (uint8_t)clamp(
        (int)lroundf((float)c[1] + (n + cg * sat) * a), 0, 255);
    c[2] = (uint8_t)clamp(
        (int)lroundf((float)c[2] + (n + cb * sat) * a), 0, 255);
}

/* Box-blur premultiplied RGBA float buffer (rgb*w, w). OOB = zero. */
static void box_blur_premul(float *dst, const float *src,
                            int width, int height, int r)
{
    int x, y, dx, dy, sx, sy, idx, nidx, area;
    float sum_r, sum_g, sum_b, sum_w;

    if (r <= 0) {
        memcpy(dst, src, (size_t)width * (size_t)height * 4 * sizeof(float));
        return;
    }
    area = (2 * r + 1) * (2 * r + 1);
    for (x = 0; x < width; x++) {
        for (y = 0; y < height; y++) {
            sum_r = sum_g = sum_b = sum_w = 0.0f;
            for (dy = -r; dy <= r; dy++) {
                for (dx = -r; dx <= r; dx++) {
                    sx = x + dx;
                    sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                        continue;
                    nidx = (sx * height + sy) * 4;
                    sum_r += src[nidx + 0];
                    sum_g += src[nidx + 1];
                    sum_b += src[nidx + 2];
                    sum_w += src[nidx + 3];
                }
            }
            idx = (x * height + y) * 4;
            /* Divide by full kernel so edges fade; empty taps are zero. */
            dst[idx + 0] = sum_r / (float)area;
            dst[idx + 1] = sum_g / (float)area;
            dst[idx + 2] = sum_b / (float)area;
            dst[idx + 3] = sum_w / (float)area;
        }
    }
}

/*
 * Nearest solid at z = water+1 within Chebyshev radius. On distance ties,
 * average source colours. Returns false if nothing in range.
 */
static bool bleed_sample(const uint8_t *src_rgba, const uint8_t *src_solid,
                         int width, int height, int x, int y, int radius,
                         uint8_t out[4], int *out_dist)
{
    int dx, dy, sx, sy, d, best = radius + 1;
    int sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
    int idx;

    for (dy = -radius; dy <= radius; dy++) {
        for (dx = -radius; dx <= radius; dx++) {
            d = max(abs(dx), abs(dy)); /* Chebyshev */
            if (d > radius || d > best)
                continue;
            sx = x + dx;
            sy = y + dy;
            if (sx < 0 || sy < 0 || sx >= width || sy >= height)
                continue;
            idx = sx * height + sy;
            if (!src_solid[idx])
                continue;
            if (d < best) {
                best = d;
                sum_r = src_rgba[idx * 4 + 0];
                sum_g = src_rgba[idx * 4 + 1];
                sum_b = src_rgba[idx * 4 + 2];
                count = 1;
            } else { /* d == best */
                sum_r += src_rgba[idx * 4 + 0];
                sum_g += src_rgba[idx * 4 + 1];
                sum_b += src_rgba[idx * 4 + 2];
                count++;
            }
        }
    }
    if (count <= 0)
        return false;
    out[0] = (uint8_t)((sum_r + count / 2) / count);
    out[1] = (uint8_t)((sum_g + count / 2) / count);
    out[2] = (uint8_t)((sum_b + count / 2) / count);
    out[3] = 255;
    *out_dist = best;
    return true;
}

static void field_to_color(const water_layer_settings_t *s, float h,
                           uint8_t out[4])
{
    float c = clamp(s->contrast, 0.0f, 1.0f);
    float mid = 0.5f + (h - 0.5f) * c;
    uint8_t tmp[4];

    mid = clamp(mid, 0.0f, 1.0f);
    if (mid < 0.5f) {
        lerp_color(s->deep_color, s->color, mid * 2.0f, out);
    } else {
        lerp_color(s->color, s->foam_color, (mid - 0.5f) * 2.0f, tmp);
        /* Extra foam push on the brightest crests. */
        lerp_color(tmp, s->foam_color,
                   smoothstep(0.75f, 1.0f, h) * clamp(s->foam, 0.0f, 1.0f),
                   out);
    }
    out[3] = s->color[3] ? s->color[3] : 255;
}

static void generate_water_layer(volume_t *volume,
                                 const water_layer_settings_t *settings,
                                 int bleed_distance, float bleed_strength,
                                 float bleed_lightness, float bleed_blur,
                                 float bleed_dithering, float bleed_noise)
{
    float box[4][4];
    int dimensions[3], start_pos[3];
    int x, y, pos[3], bottom_z, above_z, idx, bidx;
    int width, height, radius, search_r, dist, blur_r;
    uint8_t color[4], bleed[4], sample[4];
    uint8_t *src_rgba = NULL;
    uint8_t *src_solid = NULL;
    uint8_t *water_rgba = NULL;
    float *bleed_buf = NULL;
    float *bleed_blurred = NULL;
    volume_iterator_t iter;
    float h, strength, dither, t, n, k, fade, w;

    if (!volume || !goxel.image)
        return;

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);
    if (box_is_null(box))
        return;

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);
    if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] <= 0)
        return;

    noise_init(settings->seed);
    bottom_z = start_pos[2];
    above_z = bottom_z + 1;
    width = dimensions[0];
    height = dimensions[1];
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);

    radius = bleed_distance;
    strength = clamp(bleed_strength, 0.0f, 1.0f);
    dither = max(bleed_dithering, 0.0f);
    blur_r = (int)lroundf(max(bleed_blur, 0.0f));
    search_r = radius + (int)ceilf(dither);

    water_rgba = calloc((size_t)width * (size_t)height * 4, 1);
    if (!water_rgba)
        return;

    if (radius > 0) {
        src_rgba = calloc((size_t)width * (size_t)height * 4, 1);
        src_solid = calloc((size_t)width * (size_t)height, 1);
        bleed_buf = calloc((size_t)width * (size_t)height * 4, sizeof(float));
        if (!src_rgba || !src_solid || !bleed_buf) {
            free(src_rgba);
            free(src_solid);
            free(bleed_buf);
            free(water_rgba);
            return;
        }
        for (x = 0; x < width; x++) {
            for (y = 0; y < height; y++) {
                pos[0] = start_pos[0] + x;
                pos[1] = start_pos[1] + y;
                pos[2] = above_z;
                volume_get_at(volume, &iter, pos, sample);
                idx = x * height + y;
                if (sample[3] != 0) {
                    src_solid[idx] = 1;
                    src_rgba[idx * 4 + 0] = sample[0];
                    src_rgba[idx * 4 + 1] = sample[1];
                    src_rgba[idx * 4 + 2] = sample[2];
                    src_rgba[idx * 4 + 3] = sample[3];
                }
            }
        }
    }

    /* Pass 1: water sheet + dithered bleed contribution (premultiplied). */
    for (x = 0; x < width; x++) {
        for (y = 0; y < height; y++) {
            pos[0] = start_pos[0] + x;
            pos[1] = start_pos[1] + y;
            pos[2] = bottom_z;
            idx = x * height + y;
            h = water_field(settings, (float)x, (float)y);
            field_to_color(settings, h, color);
            water_rgba[idx * 4 + 0] = color[0];
            water_rgba[idx * 4 + 1] = color[1];
            water_rgba[idx * 4 + 2] = color[2];
            water_rgba[idx * 4 + 3] = color[3];

            if (radius <= 0)
                continue;
            if (!bleed_sample(src_rgba, src_solid, width, height, x, y,
                              search_r, bleed, &dist))
                continue;

            /* Brush-style: scatter the falloff boundary. */
            k = (float)(radius + 1) - (float)dist;
            if (dither > 0.0f) {
                n = uniform_noise((float)pos[0], (float)pos[1], (float)pos[2]);
                k += (n * 2.0f - 1.0f) * dither;
            }
            fade = clamp(k / (float)(radius + 1), 0.0f, 1.0f);
            t = strength * fade;
            if (t <= 0.0f)
                continue;

            apply_bleed_lightness(bleed, bleed_lightness);
            bidx = idx * 4;
            bleed_buf[bidx + 0] = (float)bleed[0] * t;
            bleed_buf[bidx + 1] = (float)bleed[1] * t;
            bleed_buf[bidx + 2] = (float)bleed[2] * t;
            bleed_buf[bidx + 3] = t;
        }
    }

    /* Pass 2: blur dithered bleed, then noise, then composite onto water. */
    if (radius > 0) {
        if (blur_r > 0) {
            bleed_blurred = calloc((size_t)width * (size_t)height * 4,
                                   sizeof(float));
            if (bleed_blurred) {
                box_blur_premul(bleed_blurred, bleed_buf, width, height,
                                blur_r);
            } else {
                bleed_blurred = bleed_buf; /* fall back: no blur */
            }
        } else {
            bleed_blurred = bleed_buf;
        }

        for (x = 0; x < width; x++) {
            for (y = 0; y < height; y++) {
                idx = x * height + y;
                bidx = idx * 4;
                color[0] = water_rgba[bidx + 0];
                color[1] = water_rgba[bidx + 1];
                color[2] = water_rgba[bidx + 2];
                color[3] = water_rgba[bidx + 3];
                w = bleed_blurred[bidx + 3];
                if (w > 1e-6f) {
                    bleed[0] = (uint8_t)clamp(
                        (int)lroundf(bleed_blurred[bidx + 0] / w), 0, 255);
                    bleed[1] = (uint8_t)clamp(
                        (int)lroundf(bleed_blurred[bidx + 1] / w), 0, 255);
                    bleed[2] = (uint8_t)clamp(
                        (int)lroundf(bleed_blurred[bidx + 2] / w), 0, 255);
                    pos[0] = start_pos[0] + x;
                    pos[1] = start_pos[1] + y;
                    pos[2] = bottom_z;
                    noise_bleed_color(bleed, pos[0], pos[1], pos[2],
                                      bleed_noise);
                    blend_rgb(color, bleed, clamp(w, 0.0f, 1.0f));
                }
                pos[0] = start_pos[0] + x;
                pos[1] = start_pos[1] + y;
                pos[2] = bottom_z;
                volume_set_at(volume, &iter, pos, color);
            }
        }

        if (bleed_blurred != bleed_buf)
            free(bleed_blurred);
    } else {
        for (x = 0; x < width; x++) {
            for (y = 0; y < height; y++) {
                idx = x * height + y;
                pos[0] = start_pos[0] + x;
                pos[1] = start_pos[1] + y;
                pos[2] = bottom_z;
                volume_set_at(volume, &iter, pos, &water_rgba[idx * 4]);
            }
        }
    }

    free(src_rgba);
    free(src_solid);
    free(bleed_buf);
    free(water_rgba);
}

/* ---- GUI ----------------------------------------------------------------- */

static int gui(filter_t *filter_)
{
    filter_water_layer_t *filter = (void *)filter_;
    water_layer_settings_t *s = &filter->settings;
    const char *preset_names[16];
    int i;
    const char *help_text =
        "Paints a flat 1-block water sheet on the image-box bottom.\n"
        "Uses domain-warped directional noise for swells, ripples, and foam.\n"
        "Always overwrites the bottom block of each column.\n"
        "Color bleed optionally tints water from solids one block above.";

    goxel_set_help_text(help_text);
    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    for (i = 0; i < preset_count && i < 16; i++)
        preset_names[i] = presets[i].name;

    if (gui_combo("Preset", &filter->preset_index, preset_names, preset_count))
        load_preset(filter, filter->preset_index);
    gui_separator();

    if(gui_collapsing_header("Colors", false)) {
        gui_color_small("Mid", s->color);
        gui_color_small("Deep", s->deep_color);
        gui_color_small("Foam", s->foam_color);
    }

    if(gui_collapsing_header("Wave parameters", false)) {
        gui_input_float("Scale", &s->scale, 1.0f, 1.0f, 256.0f, "%.0f");
        gui_tooltip_if_hovered("Size of the large swells in blocks (higher = broader).");
        gui_input_float("Direction", &s->direction_deg, 1.0f, 0.0f, 360.0f, "%.0f");
        gui_tooltip_if_hovered("Swell travel direction in degrees.");
        gui_input_float("Stretch", &s->stretch, 0.05f, 1.0f, 6.0f, "%.2f");
        gui_tooltip_if_hovered(
            "Elongates waves along the swell direction (1 = round cells).");
        gui_input_float("Warp", &s->warp, 0.01f, 0.0f, 2.0f, "%.2f");
        gui_tooltip_if_hovered("Domain warp - bends the pattern into flowing shapes.");
        gui_input_float("Detail", &s->detail, 0.01f, 0.0f, 1.0f, "%.2f");
        gui_tooltip_if_hovered("Amount of fine ripple noise on top of the swells.");
        gui_input_float("Foam", &s->foam, 0.01f, 0.0f, 1.0f, "%.2f");
        gui_tooltip_if_hovered("How strongly bright foam appears on wave crests.");
        gui_input_float("Contrast", &s->contrast, 0.01f, 0.0f, 1.0f, "%.2f");
        gui_tooltip_if_hovered("Separation between deep, mid, and foam colours.");
    }

    if (gui_collapsing_header("Color bleed", false)) {
        gui_input_int("Distance", &filter->bleed_distance, 0, 64);
        gui_tooltip_if_hovered(
            "How far colours from blocks above the water (z+1) spread into "
            "the water sheet. 0 disables bleed.");
        gui_input_float("Strength", &filter->bleed_strength, 0.01f, 0.0f, 1.0f,
                        "%.2f");
        gui_tooltip_if_hovered(
            "Opacity of the bled colour, including the direct copy under a "
            "z+1 block.");
        gui_input_float("Lightness", &filter->bleed_lightness, 0.05f, 0.0f, 2.0f,
                        "%.2f");
        gui_tooltip_if_hovered(
            "0 = pitch black, 1 = no change, 2 = white.");
        gui_input_float("Blur", &filter->bleed_blur, 0.1f, 0.0f, 16.0f, "%.1f");
        gui_tooltip_if_hovered(
            "Box-blur radius (blocks) applied to the dithered bleed colours. "
            "0 = raw dither, higher = softer speckles.");
        gui_input_float("Dithering", &filter->bleed_dithering, 0.1f, 0.0f, 32.0f,
                        "%.1f");
        gui_tooltip_if_hovered(
            "0 = none; higher scatters/dithers the bleed falloff edge "
            "(same idea as brush dithering).");
        gui_input_float("Noise", &filter->bleed_noise, 0.5f, 0.0f, 64.0f,
                        "%.1f");
        gui_tooltip_if_hovered(
            "Random luminance noise amplitude on bled colours (±value). "
            "Chroma is kept low so hues stay close to the source.");
    }

    gui_separator();

    {
        bool has_layer = goxel.image && goxel.image->active_layer;
        int target_mode;

        if (!has_layer)
            filter->replace_current_layer = false;
        target_mode = filter->replace_current_layer ? 1 : 0;
        gui_row_begin(2);
        gui_selectable_toggle("In new layer", &target_mode, 0,
            "With a layer selected: create a child named Water layer.\n"
            "With nothing selected: create a top-level Water layer.",
            -1);
        gui_enabled_begin(has_layer);
        gui_selectable_toggle("Replace current layer", &target_mode, 1,
            "Clear the selected layer then paint the water sheet.",
            -1);
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
        gui_row_end();
        filter->replace_current_layer = (target_mode == 1);
    }

    gui_input_int("Seed", &s->seed, 0, RAND_MAX);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        s->seed = rand();
    }

    gui_separator();

    if (gui_button("Reset to defaults", -1, 0))
        reset_to_defaults(filter);
    if (gui_button_primary("Generate", -1, 0)) {
        layer_t *layer;
        if (!goxel.image)
            return 0;
        image_history_push(goxel.image);
        layer = image_ensure_layer_for_generation(
            goxel.image, "Water layer", filter->replace_current_layer);
        if (!layer || !layer->volume)
            return 0;
        if (filter->replace_current_layer)
            volume_clear(layer->volume);
        generate_water_layer(layer->volume, s,
                             filter->bleed_distance, filter->bleed_strength,
                             filter->bleed_lightness, filter->bleed_blur,
                             filter->bleed_dithering, filter->bleed_noise);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_water_layer_t *filter = (void *)filter_;
    reset_to_defaults(filter);
    reset_bleed_defaults(filter);
    filter->replace_current_layer = false;
}

FILTER_REGISTER(water_layer, filter_water_layer_t,
                .name = "Water layer",
                .menu = "effects",
                .submenu = "generate",
                .on_open = on_open,
                .panel_width = 300,
                .gui_fn = gui, )
