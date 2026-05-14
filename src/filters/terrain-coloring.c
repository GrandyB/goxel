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

/*
 * Terrain coloring extracted from genland.cpp (Tom Dobrowolski / Ken Silverman):
 * slope-based grass tones, water tint, ambient, directional light, shadow
 * rays, shadow blur, and final merge — applied to existing voxel columns.
 *
 * Optional phantom height for normals; optional rugged luminance on grass albedo.
 */

#include "goxel.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Phantom height for normals: h' = z_top + k × noise3d (shadows / grass z use real tops). */
static const float k_terrain_normal_height_noise_amp = 0.5f;

/* ---- Improved Perlin-style noise (same logic as genland.cpp) ---- */

static unsigned char noisep[512], noisep15[512];

static inline float fgrad(long h, float x, float y, float z)
{
    switch (h & 15) {
    case 0: return (x + y);
    case 1: return (-x + y);
    case 2: return (x - y);
    case 3: return (-x - y);
    case 4: return (x + z);
    case 5: return (-x + z);
    case 6: return (x - z);
    case 7: return (-x - z);
    case 8: return (y + z);
    case 9: return (-y + z);
    case 10: return (y - z);
    case 11: return (-y - z);
    case 12: return (x + y);
    case 13: return (-x + y);
    case 14: return (y - z);
    case 15: return (-y - z);
    }
    return 0;
}

static void noiseinit(int seed)
{
    long i, j, k;

    srand(seed);
    for (i = 256 - 1; i >= 0; i--)
        noisep[i] = (unsigned char)i;
    for (i = 256 - 1; i > 0; i--) {
        long n = ((float)rand() / (float)RAND_MAX) * 32767;
        j = ((n * (i + 1)) >> 15);
        k = noisep[i];
        noisep[i] = noisep[j];
        noisep[j] = (unsigned char)k;
    }
    for (i = 256 - 1; i >= 0; i--)
        noisep[i + 256] = noisep[i];
    for (i = 512 - 1; i >= 0; i--)
        noisep15[i] = noisep[i] & 15;
}

static double noise3d(double fx, double fy, double fz, long mask)
{
    long i, l[6], a[4];
    float p[3], f[8];

    l[0] = (long)floor(fx);
    p[0] = (float)(fx - (double)l[0]);
    l[0] &= mask;
    l[3] = (l[0] + 1) & mask;
    l[1] = (long)floor(fy);
    p[1] = (float)(fy - (double)l[1]);
    l[1] &= mask;
    l[4] = (l[1] + 1) & mask;
    l[2] = (long)floor(fz);
    p[2] = (float)(fz - (double)l[2]);
    l[2] &= mask;
    l[5] = (l[2] + 1) & mask;
    i = noisep[l[0]];
    a[0] = noisep[i + l[1]];
    a[2] = noisep[i + l[4]];
    i = noisep[l[3]];
    a[1] = noisep[i + l[1]];
    a[3] = noisep[i + l[4]];
    f[0] = fgrad(noisep15[a[0] + l[2]], p[0], p[1], p[2]);
    f[1] = fgrad(noisep15[a[1] + l[2]], p[0] - 1, p[1], p[2]);
    f[2] = fgrad(noisep15[a[2] + l[2]], p[0], p[1] - 1, p[2]);
    f[3] = fgrad(noisep15[a[3] + l[2]], p[0] - 1, p[1] - 1, p[2]);
    p[2]--;
    f[4] = fgrad(noisep15[a[0] + l[5]], p[0], p[1], p[2]);
    f[5] = fgrad(noisep15[a[1] + l[5]], p[0] - 1, p[1], p[2]);
    f[6] = fgrad(noisep15[a[2] + l[5]], p[0], p[1] - 1, p[2]);
    f[7] = fgrad(noisep15[a[3] + l[5]], p[0] - 1, p[1] - 1, p[2]);
    p[2]++;
    p[2] = (float)((3.0 - 2.0 * p[2]) * p[2] * p[2]);
    p[1] = (float)((3.0 - 2.0 * p[1]) * p[1] * p[1]);
    p[0] = (float)((3.0 - 2.0 * p[0]) * p[0] * p[0]);
    f[0] = (f[4] - f[0]) * p[2] + f[0];
    f[1] = (f[5] - f[1]) * p[2] + f[1];
    f[2] = (f[6] - f[2]) * p[2] + f[2];
    f[3] = (f[7] - f[3]) * p[2] + f[3];
    f[0] = (f[2] - f[0]) * p[1] + f[0];
    f[1] = (f[3] - f[1]) * p[1] + f[1];
    return ((f[1] - f[0]) * p[0] + f[0]);
}

/* Uncorrelated [-1, 1] per (x,y) for blocky grass / soil variation. */
static double terrain_cell_hash(int x, int y, int seed)
{
    unsigned u = (unsigned)x * 1664525u + (unsigned)y * 1013904223u +
                 (unsigned)seed * 1103515245u;
    u ^= u >> 16;
    u *= 2654435769u;
    u ^= u >> 16;
    return (double)(u & 65535u) / 32768.0 - 1.0;
}

/* Perlin (tight in xy) + cell hash: multiplicative scale on RGB (preserves hue).
 * strength is usually (UI × grass mask); clamped below. */
static void terrain_apply_rugged_color(double *r, double *g, double *b, int x, int y,
                                       int seed, double strength)
{
    if (strength <= 1e-9)
        return;
    const double st = min(max(strength, 0.0), 24.0);
    /* ~64% Perlin / 36% uncorrelated cell noise (two hashes averaged). */
    const double kP = 0.64;
    /* Higher xy scale → finer Perlin variation over the voxel grid. */
    const double ps = 0.74;
    const double p =
        noise3d((double)x * ps + 0.41, (double)y * ps - 0.19, 1.07, 31);
    const double h =
        0.5 * terrain_cell_hash(x, y, seed + 2011) +
        0.5 * terrain_cell_hash(x + 97, y - 43, seed + 2711);
    const double n = min(max(kP * p + (1.0 - kP) * h, -1.0), 1.0);
    const double swing = min(0.22 * st, 0.58);
    const double m = 1.0 + swing * n;
    *r = min(max(*r * m, 0.0), 255.0);
    *g = min(max(*g * m, 0.0), 255.0);
    *b = min(max(*b * m, 0.0), 255.0);
}

/* ---- Filter state ---- */

typedef struct
{
    int seed;
    uint8_t color_ground[4];
    uint8_t color_grass1[4];
    uint8_t color_grass2[4];
    uint8_t color_water[4];
    float shadow_factor;
    float ambience_factor;
} terrain_coloring_settings_t;

typedef struct
{
    filter_t filter;
    terrain_coloring_settings_t settings;

    bool step_grass_tones;
    bool step_water_tint;
    bool step_ambient;
    bool step_directional;
    bool step_shadow_cast;
    bool step_shadow_smooth;
    /*
     * Slope / normal estimate: central difference (h(x+k)-h(x-k)) / (2k) in
     * voxel units. k=1 uses only immediate neighbors; k=2 includes two steps
     * out (softer grass when column tops are discrete integers).
     */
    int normal_half_span;
    /* High-frequency Perlin + per-cell hash added to raw grass signal. */
    float grass_detail_noise;
    /* Scales max(-normalZ,0)^exponent · gain in rawGrass (exponent 1 = Genland-like). */
    float grass_slope_exponent;
    float grass_slope_gain;
    /* Voxel units in denominator for height term h0/scale (was fixed 32). */
    float grass_height_scale;
    /* Bottom voxel rows (z = 0 .. N-1) use water albedo + lighting. */
    int water_bottom_layers;
    /* Scales Perlin + hash variation on water (tint strength + water RGB). */
    float water_noise_strength;
    /* When Soften shadow map is on: symmetric box radius in cells (1 = 3×3
     * average, 2 = 5×5, …). Uses toroidal wrap like the shadow cast pass. */
    int shadow_blur_blocks;
    /* Multiplicative rugged on grass-tinted land albedo only (0 = off). */
    float rugged_color_noise;
} filter_terrain_coloring_t;

static const terrain_coloring_settings_t default_terrain_coloring_settings = {
    .seed = 0,
    .color_ground = {140, 125, 115, 255},
    .color_grass1 = {72, 80, 32, 255},
    .color_grass2 = {68, 78, 40, 255},
    .color_water = {60, 100, 120, 255},
    .shadow_factor = 33.f,
    .ambience_factor = 0.22f,
};

static void reset_settings(filter_terrain_coloring_t *filter)
{
    filter->settings = default_terrain_coloring_settings;
}

static int wrap_coord(int v, int m)
{
    v %= m;
    if (v < 0)
        v += m;
    return v;
}

/* Symmetric toroidal box blur: radius r uses (2r+1)^2 taps (no diagonal bias). */
static void terrain_shadow_box_blur(unsigned char *dst, const unsigned char *src,
                                    int gw, int gh, int r)
{
    if (r <= 0 || gw <= 0 || gh <= 0)
        return;
    const int side = 2 * r + 1;
    const int area = side * side;
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            int sum = 0;
            for (int dy = -r; dy <= r; dy++) {
                const int yy = wrap_coord(y + dy, gh);
                for (int dx = -r; dx <= r; dx++) {
                    const int xx = wrap_coord(x + dx, gw);
                    sum += (int)src[yy * gw + xx];
                }
            }
            dst[y * gw + x] = (unsigned char)((sum + area / 2) / area);
        }
    }
}

static float sample_h_valid(const float *hgt, int gw, int gh, int x, int y,
                            float fallback)
{
    x = clamp(x, 0, gw - 1);
    y = clamp(y, 0, gh - 1);
    float v = hgt[y * gw + x];
    return (v < -500.f) ? fallback : v;
}

/* dh/dx, dh/dy from discrete column tops; larger half_span = smoother slopes. */
static void terrain_column_slopes(const float *hgt, int gw, int gh, int x, int y,
                                  float h0, int half_span, float *out_hx,
                                  float *out_hy)
{
    half_span = clamp(half_span, 1, 8);
    const float denom = 2.0f * (float)half_span;
    *out_hx = (sample_h_valid(hgt, gw, gh, x + half_span, y, h0) -
               sample_h_valid(hgt, gw, gh, x - half_span, y, h0)) /
              denom;
    *out_hy = (sample_h_valid(hgt, gw, gh, x, y + half_span, h0) -
               sample_h_valid(hgt, gw, gh, x, y - half_span, h0)) /
              denom;
}

static int column_top_z(volume_t *volume, volume_iterator_t *it,
                          const int start_pos[3], int dz, int x, int y)
{
    int pos[3];
    uint8_t c[4];

    pos[0] = x + start_pos[0];
    pos[1] = y + start_pos[1];
    for (int z = dz - 1; z >= 0; z--) {
        pos[2] = z + start_pos[2];
        volume_get_at(volume, it, pos, c);
        if (c[3])
            return z;
    }
    return -1;
}

static void apply_terrain_coloring(volume_t *volume, terrain_coloring_settings_t *s,
                                   bool step_grass_tones, bool step_water_tint,
                                   bool step_ambient, bool step_directional,
                                   bool step_shadow_cast, bool step_shadow_smooth,
                                   int normal_half_span, float grass_detail_noise,
                                   float grass_slope_exponent, float grass_slope_gain,
                                   float grass_height_scale,
                                   int water_bottom_layers,
                                   float water_noise_strength, int shadow_blur_blocks,
                                   float rugged_color_noise)
{
    float box[4][4];
    int dimensions[3], start_pos[3];
    volume_iterator_t it = {0};

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    const int gw = dimensions[0];
    const int gh = dimensions[1];
    const int dz = dimensions[2];
    const int n = gw * gh;

    if (gw <= 0 || gh <= 0 || dz <= 0)
        return;

    float *hgt = NULL;
    uint8_t *amb_r = NULL;
    uint8_t *amb_g = NULL;
    uint8_t *amb_b = NULL;
    uint8_t *buf_r = NULL;
    uint8_t *buf_g = NULL;
    uint8_t *buf_b = NULL;
    unsigned char *sh = NULL;
    uint8_t *water_pack = NULL;
    uint8_t *wbuf_r = NULL;
    uint8_t *wbuf_g = NULL;
    uint8_t *wbuf_b = NULL;
    uint8_t *wamb_r = NULL;
    uint8_t *wamb_g = NULL;
    uint8_t *wamb_b = NULL;
    float *h_normal = NULL;

    hgt = calloc((size_t)n, sizeof(float));
    amb_r = calloc((size_t)n, 1);
    amb_g = calloc((size_t)n, 1);
    amb_b = calloc((size_t)n, 1);
    buf_r = calloc((size_t)n, 1);
    buf_g = calloc((size_t)n, 1);
    buf_b = calloc((size_t)n, 1);
    sh = calloc((size_t)n, 1);

    if (!hgt || !amb_r || !amb_g || !amb_b || !buf_r || !buf_g || !buf_b || !sh)
        goto cleanup;

    const int w_layers = clamp(water_bottom_layers, 0, 64);
    const bool water_column = step_water_tint && w_layers > 0;
    if (water_column) {
        water_pack = calloc((size_t)n * 6, 1);
        if (!water_pack)
            goto cleanup;
        wbuf_r = water_pack;
        wbuf_g = water_pack + n;
        wbuf_b = water_pack + 2 * n;
        wamb_r = water_pack + 3 * n;
        wamb_g = water_pack + 4 * n;
        wamb_b = water_pack + 5 * n;
    }

    noiseinit(s->seed);

    /* Pass 1: surface height per column */
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            int zi = column_top_z(volume, &it, start_pos, dz, x, y);
            int idx = y * gw + x;
            hgt[idx] = (zi >= 0) ? (float)zi : -1000.0f;
        }
    }

    const float amp = k_terrain_normal_height_noise_amp;
    const float *h_slope = hgt;
    if (amp > 1e-5f) {
        h_normal = calloc((size_t)n, sizeof(float));
        if (h_normal) {
            for (int y = 0; y < gh; y++) {
                for (int x = 0; x < gw; x++) {
                    const int idx = y * gw + x;
                    if (hgt[idx] < -500.0f) {
                        h_normal[idx] = -1000.0f;
                        continue;
                    }
                    const double n =
                        noise3d((double)x * 0.18 + 0.03, (double)y * 0.18 - 0.07,
                                0.71, 31);
                    h_normal[idx] = hgt[idx] + amp * (float)n;
                }
            }
            h_slope = h_normal;
        }
    }

    /* Pass 2: material + lighting terms (genland inner loop, height from mesh) */
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const int idx = y * gw + x;
            const float h0 = hgt[idx];
            if (h0 < -500.0f)
                continue;

            const float h0_slope = h_normal ? h_normal[idx] : h0;

            float hx, hy;
            terrain_column_slopes(h_slope, gw, gh, x, y, h0_slope, normal_half_span,
                                  &hx, &hy);
            double normalX = (double)hx;
            double normalY = (double)hy;
            double normalZ = -1.0;
            double tempValue =
                1.0 / sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
            normalX *= tempValue;
            normalY *= tempValue;
            normalZ *= tempValue;

            double groundRed = s->color_ground[0];
            double groundGreen = s->color_ground[1];
            double groundBlue = s->color_ground[2];
            double grass_mask = 0.0;

            if (step_grass_tones) {
                const double gdn =
                    min(max((double)grass_detail_noise, 0.0), 3.0);
                const double slope_exp =
                    min(max((double)grass_slope_exponent, 0.05), 8.0);
                const double slope_g =
                    min(max((double)grass_slope_gain, 0.0), 10.0);
                const double h_scale =
                    max((double)grass_height_scale, 0.5);

                const double macroN =
                    noise3d(x * (1.0 / 64.0), y * (1.0 / 64.0), 0.3, 15) * 0.3;
                const double fineN =
                    noise3d((double)x * 0.41 + 2.17, (double)y * 0.41 - 1.03,
                            1.88, 31) *
                    0.38;
                const double cellN = terrain_cell_hash(x, y, s->seed);

                const double s_up =
                    min(max(-normalZ, 0.0), 1.0);
                const double slope_term =
                    slope_g * pow(s_up, slope_exp);
                const double detail_raw =
                    gdn * (0.55 * fineN + 0.4 * cellN);
                const double rawGrass =
                    slope_term - (double)h0 / h_scale + macroN +
                    detail_raw;
                double grassBlend = min(max(rawGrass, 0.0), 1.0);
                grass_mask = grassBlend;
                groundRed += (s->color_grass1[0] - groundRed) * grassBlend;
                groundGreen += (s->color_grass1[1] - groundGreen) * grassBlend;
                groundBlue += (s->color_grass1[2] - groundBlue) * grassBlend;

                double secondaryBlend = (1 - fabs(grassBlend - 0.5) * 2) * 0.7;
                groundRed += (s->color_grass2[0] - groundRed) * secondaryBlend;
                groundGreen += (s->color_grass2[1] - groundGreen) * secondaryBlend;
                groundBlue += (s->color_grass2[2] - groundBlue) * secondaryBlend;
            }

            if (step_water_tint) {
                const double wns =
                    min(max((double)water_noise_strength, 0.0), 3.0);
                int nn = 0;
                double sum = 0.0;
                double h_max = (double)h0;
                double h_min = (double)h0;
                static const int dx[4] = {-1, 1, 0, 0};
                static const int dy[4] = {0, 0, -1, 1};
                for (int k = 0; k < 4; k++) {
                    int nx = x + dx[k];
                    int ny = y + dy[k];
                    if (nx < 0 || nx >= gw || ny < 0 || ny >= gh)
                        continue;
                    float hn = hgt[ny * gw + nx];
                    if (hn < -500.0f)
                        continue;
                    double hd = (double)hn;
                    sum += hd;
                    if (hd > h_max)
                        h_max = hd;
                    if (hd < h_min)
                        h_min = hd;
                    nn++;
                }
                double secondaryBlend = 0.0;
                if (nn > 0) {
                    double neighborAvg = sum / (double)nn;
                    /* Bowl vs local average only (no h_max−h0: that tags cliff sides
                     * on mountains as “water”). Damp by neighbour height spread. */
                    double bowl = neighborAvg - (double)h0;
                    if (bowl > 0.0) {
                        double spread = h_max - h_min;
                        if (spread < 1e-3)
                            spread = 1.0;
                        double rawW =
                            bowl * 0.26 / (1.0 + spread * 0.14);
                        secondaryBlend = max(0.0, min(rawW, 1.0));
                        /* Soften onset / saturation like a gentle flood fill */
                        secondaryBlend =
                            secondaryBlend * secondaryBlend *
                            (3.0 - 2.0 * secondaryBlend);
                    }
                }
                /* Water tint strength: Perlin + hash, scaled by UI. */
                double wAmp =
                    wns * (noise3d((double)x * 0.11 + 0.9, (double)y * 0.11, 4.25, 15) *
                               0.055 +
                           terrain_cell_hash(x, y ^ 333, s->seed + 419) * 0.045);
                secondaryBlend =
                    max(0.0, min(secondaryBlend * (1.0 + wAmp), 1.0));

                /* Genland: land tint in water mix via grassBlend only. */
                double grassBlend = 1.0 - secondaryBlend * 0.2;
                double wJit =
                    wns * (noise3d((double)x * 0.14 + 2.0, (double)y * 0.14, 1.05, 31) *
                              0.10 +
                          terrain_cell_hash(x, y + 617, s->seed + 503) * 0.07);
                wJit = max(-0.22, min(0.22, wJit));
                double wR_t = max(0.0, min(255.0, s->color_water[0] * grassBlend * (1.0 + wJit)));
                double wG_t = max(0.0, min(255.0, s->color_water[1] * grassBlend * (1.0 + wJit)));
                double wB_t = max(0.0, min(255.0, s->color_water[2] * grassBlend * (1.0 + wJit)));
                groundRed += (wR_t - groundRed) * secondaryBlend;
                groundGreen += (wG_t - groundGreen) * secondaryBlend;
                groundBlue += (wB_t - groundBlue) * secondaryBlend;
            }

            terrain_apply_rugged_color(&groundRed, &groundGreen, &groundBlue, x, y,
                                       s->seed,
                                       (double)rugged_color_noise * grass_mask);

            double sunLight = 1.0;
            if (step_directional) {
                sunLight = (normalX * 0.5 + normalY * 0.25 - normalZ) /
                    sqrt(0.5 * 0.5 + 0.25 * 0.25 + 1.0 * 1.0);
                sunLight *= 1.2;
            }

            int maxAmbient = 0;
            if (step_ambient) {
                const double ambFact = s->ambience_factor;
                amb_r[idx] = (uint8_t)min(max(groundRed * ambFact, 0), 255);
                amb_g[idx] = (uint8_t)min(max(groundGreen * ambFact, 0), 255);
                amb_b[idx] = (uint8_t)min(max(groundBlue * ambFact, 0), 255);
                maxAmbient = max(max((int)amb_r[idx], (int)amb_g[idx]), (int)amb_b[idx]);
            } else {
                amb_r[idx] = amb_g[idx] = amb_b[idx] = 0;
            }

            if (step_directional) {
                buf_r[idx] =
                    (uint8_t)min(max(groundRed * sunLight, 0), 255 - maxAmbient);
                buf_g[idx] =
                    (uint8_t)min(max(groundGreen * sunLight, 0), 255 - maxAmbient);
                buf_b[idx] =
                    (uint8_t)min(max(groundBlue * sunLight, 0), 255 - maxAmbient);
            } else {
                buf_r[idx] = (uint8_t)min(max(groundRed, 0), 255 - maxAmbient);
                buf_g[idx] = (uint8_t)min(max(groundGreen, 0), 255 - maxAmbient);
                buf_b[idx] = (uint8_t)min(max(groundBlue, 0), 255 - maxAmbient);
            }

            if (water_column) {
                const double wns =
                    min(max((double)water_noise_strength, 0.0), 3.0);
                double wJit =
                    wns * (noise3d((double)x * 0.14 + 2.0, (double)y * 0.14, 1.05, 31) *
                              0.10 +
                          terrain_cell_hash(x, y + 101, s->seed + 227) * 0.07);
                wJit = max(-0.22, min(0.22, wJit));
                double wR = max(0.0, min(255.0, s->color_water[0] * (1.0 + wJit)));
                double wG = max(0.0, min(255.0, s->color_water[1] * (1.0 + wJit)));
                double wB = max(0.0, min(255.0, s->color_water[2] * (1.0 + wJit)));
                int maxAmbientW = 0;
                if (step_ambient) {
                    const double ambFact = s->ambience_factor;
                    wamb_r[idx] = (uint8_t)min(max(wR * ambFact, 0), 255);
                    wamb_g[idx] = (uint8_t)min(max(wG * ambFact, 0), 255);
                    wamb_b[idx] = (uint8_t)min(max(wB * ambFact, 0), 255);
                    maxAmbientW =
                        max(max((int)wamb_r[idx], (int)wamb_g[idx]), (int)wamb_b[idx]);
                } else {
                    wamb_r[idx] = wamb_g[idx] = wamb_b[idx] = 0;
                }
                if (step_directional) {
                    wbuf_r[idx] =
                        (uint8_t)min(max(wR * sunLight, 0), 255 - maxAmbientW);
                    wbuf_g[idx] =
                        (uint8_t)min(max(wG * sunLight, 0), 255 - maxAmbientW);
                    wbuf_b[idx] =
                        (uint8_t)min(max(wB * sunLight, 0), 255 - maxAmbientW);
                } else {
                    wbuf_r[idx] = (uint8_t)min(max(wR, 0), 255 - maxAmbientW);
                    wbuf_g[idx] = (uint8_t)min(max(wG, 0), 255 - maxAmbientW);
                    wbuf_b[idx] = (uint8_t)min(max(wB, 0), 255 - maxAmbientW);
                }
            }
        }
    }

    /* Shadows (genland): height field must stay float for comparison */
    if (step_shadow_cast) {
        const int shadow_range = max(8, max(gw, gh) / 4);
        memset(sh, 0, (size_t)n);
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                const int idx = y * gw + x;
                if (hgt[idx] < -500.0f)
                    continue;
                float shadowCheckValue = hgt[idx] + 0.44f;
                for (int shadowIter = 1, octaveIndex = 1; octaveIndex < shadow_range;
                     shadowIter++, octaveIndex++, shadowCheckValue += 0.44f) {
                    int sy = wrap_coord(y - (shadowIter >> 1), gh);
                    int sx = wrap_coord(x - octaveIndex, gw);
                    if (hgt[sy * gw + sx] > shadowCheckValue) {
                        sh[idx] = (unsigned char)clamp(
                            (int)(s->shadow_factor + 0.5f), 0, 255);
                        break;
                    }
                }
            }
        }

        if (step_shadow_smooth) {
            const int r = clamp(shadow_blur_blocks, 0, 16);
            if (r > 0) {
                unsigned char *sh_tmp = malloc((size_t)n);
                if (sh_tmp) {
                    terrain_shadow_box_blur(sh_tmp, sh, gw, gh, r);
                    memcpy(sh, sh_tmp, (size_t)n);
                    free(sh_tmp);
                }
            }
        }
    }

    /* Final merge + paint columns */
    for (int y = 0; y < gh; y++) {
        for (int x = 0; x < gw; x++) {
            const int idx = y * gw + x;
            if (hgt[idx] < -500.0f)
                continue;

            int ztop = (int)hgt[idx];
            ztop = clamp(ztop, 0, dz - 1);

            int colorIndex = 256 - ((int)sh[idx] << 2);
            if (!step_shadow_cast)
                colorIndex = 256;

            int pos[3];
            pos[0] = x + start_pos[0];
            pos[1] = y + start_pos[1];
            for (int z = 0; z <= ztop; z++) {
                uint8_t cr, cg, cb;
                if (water_column && z < w_layers) {
                    cr = (uint8_t)clamp(
                        ((int)wbuf_r[idx] * colorIndex >> 8) + (int)wamb_r[idx], 0,
                        255);
                    cg = (uint8_t)clamp(
                        ((int)wbuf_g[idx] * colorIndex >> 8) + (int)wamb_g[idx], 0,
                        255);
                    cb = (uint8_t)clamp(
                        ((int)wbuf_b[idx] * colorIndex >> 8) + (int)wamb_b[idx], 0,
                        255);
                } else {
                    cr = (uint8_t)clamp(
                        ((int)buf_r[idx] * colorIndex >> 8) + (int)amb_r[idx], 0,
                        255);
                    cg = (uint8_t)clamp(
                        ((int)buf_g[idx] * colorIndex >> 8) + (int)amb_g[idx], 0,
                        255);
                    cb = (uint8_t)clamp(
                        ((int)buf_b[idx] * colorIndex >> 8) + (int)amb_b[idx], 0,
                        255);
                }
                uint8_t color[4] = {cr, cg, cb, 255};
                pos[2] = z + start_pos[2];
                volume_set_at(volume, NULL, pos, color);
            }
        }
    }

cleanup:
    free(h_normal);
    free(hgt);
    free(amb_r);
    free(amb_g);
    free(amb_b);
    free(buf_r);
    free(buf_g);
    free(buf_b);
    free(sh);
    free(water_pack);
}

static int gui(filter_t *filter_)
{
    filter_terrain_coloring_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    terrain_coloring_settings_t *st = &filter->settings;

    gui_label_size_push(170.0f);
    goxel_set_help_text(
        "Re-colors solid columns using the genland material and lighting "
        "pipeline, driven by the current top surface height.");

    if (gui_collapsing_header("Steps", false)) {
        gui_checkbox("Grass tones", &filter->step_grass_tones,
                     "Blend ground toward grass colors using slope and 3D noise.");
        gui_input_int("Normal half-span (voxels)", &filter->normal_half_span, 1,
                      8);
        gui_tooltip_if_hovered(
            "Slope uses (height[x+k]-height[x-k]) / (2k) with k = this value. "
            "Integer column tops make k=1 look blocky; k=2 uses neighbors two "
            "steps away for a softer normal (default 2).");
        gui_input_float("Grass detail noise", &filter->grass_detail_noise, 0.02f,
                        0.0f, 3.0f, "%.2f");
        gui_tooltip_if_hovered(
            "Adds high-frequency Perlin plus per-cell variation on top of the "
            "smooth genland macro noise (0 = macro only).");
        gui_input_float("Rugged grass noise", &filter->rugged_color_noise, 0.05f,
                        0.f, 24.f, "%.2f");
        gui_tooltip_if_hovered(
            "After grass/water surface tint: scales land RGB (multiplicative) only "
            "where grass tones apply — strength scales with grass blend. "
            "Does not affect pure ground or water-bottom voxels. 0 = off.");
        if (gui_collapsing_header("Grass slope & height", false)) {
            gui_input_float("Slope exponent", &filter->grass_slope_exponent, 0.05f,
                            0.2f, 8.f, "%.2f");
            gui_tooltip_if_hovered(
                "Exponent on flatness max(-n_z,0) (clamped 0–1) before slope gain. "
                "1 = linear like the old fixed formula; >1 favors ground on "
                "steeper faces.");
            gui_input_float("Slope gain", &filter->grass_slope_gain, 0.05f, 0.f,
                            10.f, "%.2f");
            gui_tooltip_if_hovered(
                "Multiplier on the slope term (old hardcoded 1.4). Lower = more "
                "ground on slopes.");
            gui_input_float("Height scale (voxels)", &filter->grass_height_scale,
                            1.f, 1.f, 200.f, "%.1f");
            gui_tooltip_if_hovered(
                "Grass raw term uses h_top / scale; larger = slower falloff of "
                "grass with height (default 32).");
        }
        gui_checkbox("Water tint (low vs neighbors)", &filter->step_water_tint,
                     "Blend toward water colour in local bowls (lower than "
                     "neighbour average, damped by local height spread).");
        gui_input_int("Water bottom layers", &filter->water_bottom_layers, 0, 16);
        gui_tooltip_if_hovered(
            "When water tint is on, lowest N voxels in each column use the water "
            "colour through the same lighting (0 = surface tint only).");
        gui_input_float("Water noise", &filter->water_noise_strength, 0.02f, 0.0f,
                        3.0f, "%.2f");
        gui_tooltip_if_hovered(
            "Scales Perlin + cell hash on water: varies tint strength and "
            "breaks up flat water colour (surface tint and bottom layers). "
            "0 disables water noise.");
        gui_checkbox("Ambient term", &filter->step_ambient,
                     "Add low ambient color capped against directional light.");
        gui_checkbox("Directional light", &filter->step_directional,
                     "Modulate albedo using the same fixed sun direction as genland.");
        gui_checkbox("Cast shadows", &filter->step_shadow_cast,
                     "Ray-march along the grid using column heights.");
        gui_checkbox("Soften shadow map", &filter->step_shadow_smooth,
                     "Enable shadow-map blur; extent is set below.");
        gui_input_int("Shadow blur (blocks)", &filter->shadow_blur_blocks, 0, 16);
        gui_tooltip_if_hovered(
            "When Soften shadow map is on: symmetric box blur radius. 0 = sharp "
            "shadows. 1 averages a 3×3 cell region (~1 voxel), 2 uses 5×5, etc. "
            "Toroidal wrap matches the cast pass (no diagonal shift).");
    }

    gui_group_begin("Colors");
    gui_color_small("Ground", st->color_ground);
    gui_color_small("Grass1", st->color_grass1);
    gui_color_small("Grass2", st->color_grass2);
    gui_color_small("Water", st->color_water);
    gui_group_end();

    gui_group_begin("Lighting");
    gui_input_float("Shadow strength", &st->shadow_factor, 1.f, 0, 255, "%.0f");
    gui_input_float("Ambient", &st->ambience_factor, 0.01f, 0, 1, "%.2f");
    gui_group_end();

    gui_input_int("Noise seed", &st->seed, 0, RAND_MAX);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        st->seed = rand();
    }

    if (gui_button("Reset colors to defaults", -1, 0)) {
        memcpy(st->color_ground, default_terrain_coloring_settings.color_ground, 4);
        memcpy(st->color_grass1, default_terrain_coloring_settings.color_grass1, 4);
        memcpy(st->color_grass2, default_terrain_coloring_settings.color_grass2, 4);
        memcpy(st->color_water, default_terrain_coloring_settings.color_water, 4);
        st->shadow_factor = default_terrain_coloring_settings.shadow_factor;
        st->ambience_factor = default_terrain_coloring_settings.ambience_factor;
    }

    if (gui_button("Apply to layer", -1, 0)) {
        image_history_push(goxel.image);
        apply_terrain_coloring(
            layer->volume, &filter->settings, filter->step_grass_tones, filter->step_water_tint,
            filter->step_ambient, filter->step_directional, filter->step_shadow_cast,
            filter->step_shadow_smooth, filter->normal_half_span,
            filter->grass_detail_noise,
            filter->grass_slope_exponent, filter->grass_slope_gain,
            filter->grass_height_scale,
            filter->water_bottom_layers, filter->water_noise_strength,
            filter->shadow_blur_blocks, filter->rugged_color_noise);
    }
    gui_label_size_pop();
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_terrain_coloring_t *filter = (void *)filter_;
    reset_settings(filter);
    filter->step_grass_tones = true;
    filter->step_water_tint = true;
    filter->step_ambient = true;
    filter->step_directional = true;
    filter->step_shadow_cast = true;
    filter->step_shadow_smooth = true;
    filter->normal_half_span = 3;
    filter->grass_detail_noise = 0.2f;
    filter->grass_slope_exponent = 1.65f;
    filter->grass_slope_gain = 1.3f;
    filter->grass_height_scale = 32.f;
    filter->water_bottom_layers = 1;
    filter->water_noise_strength = 0.32f;
    filter->shadow_blur_blocks = 1;
    filter->rugged_color_noise = 1.7f;
}

FILTER_REGISTER(terrain_coloring, filter_terrain_coloring_t,
                .name = "Generation - Terrain Coloring",
                .on_open = on_open,
                .panel_width = 400,
                .gui_fn = gui, )
