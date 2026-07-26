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
#include "utils/noise.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Replace the active layer's blocks with choppy hedgerow foliage.
 * Occupied columns are treated as the hedge path; each is expanded to a
 * noisy width and stacked to a noisy height so the silhouette feels like
 * field-edge hedges rather than a solid wall.
 */

typedef struct {
    filter_t filter;
    int min_height;
    int max_height;
    int min_width;
    int max_width;
    int seed;
    uint8_t color[4];
    /* Colour variation (same idea as brush noise): intensity = mix amount,
     * saturation = how colourful that mix is (0 = luminance-only). */
    int noise_intensity;
    int noise_saturation;
} filter_hedges_t;

static const uint8_t k_default_color[4] = {48, 92, 38, 255};

static void reset_defaults(filter_hedges_t *filter)
{
    filter->min_height = 1;
    filter->max_height = 4;
    filter->min_width = 2;
    filter->max_width = 5;
    filter->seed = 0;
    filter->noise_intensity = 10;
    filter->noise_saturation = 10;
    memcpy(filter->color, k_default_color, 4);
}

static uint32_t hash3(int x, int y, int z, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u
               + (uint32_t)y * 668265263u
               + (uint32_t)z * 214613u
               + (uint32_t)seed * 362437u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float hash01(int x, int y, int z, int seed)
{
    return (float)(hash3(x, y, z, seed) & 0xffffffu) / (float)0xffffffu;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int lerpi(int a, int b, float t)
{
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    return a + (int)floorf((float)(b - a) * t + 0.5f);
}

static float fade(float t)
{
    return t * t * (3.f - 2.f * t);
}

/* Smooth value noise in [0,1] — interpolated so neighbours change gradually,
 * but cell corners still hit the full hash range so min/max are reachable. */
static float value_noise2(float x, float y, int seed)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    float u = fade(fx);
    float v = fade(fy);
    float a = hash01(x0, y0, 0, seed);
    float b = hash01(x0 + 1, y0, 0, seed);
    float c = hash01(x0, y0 + 1, 0, seed);
    float d = hash01(x0 + 1, y0 + 1, 0, seed);
    float ab = a + (b - a) * u;
    float cd = c + (d - c) * u;
    return ab + (cd - ab) * v;
}

/* scale = blocks per feature; larger = gentler changes along the hedge. */
static float hedge_noise_smooth(int x, int y, int seed, float scale)
{
    float n1 = value_noise2((float)x / scale, (float)y / scale, seed);
    float n2 = value_noise2((float)x / (scale * 2.4f),
                            (float)y / (scale * 2.4f), seed + 17);
    return n1 * 0.75f + n2 * 0.25f;
}

/* Push mid tones toward 0/1 so min/max heights show up more often. */
static float contrast01(float t)
{
    t = 0.5f + (t - 0.5f) * 1.85f;
    if (t < 0.f) return 0.f;
    if (t > 1.f) return 1.f;
    return t;
}

typedef struct {
    int x, y, base_z;
} hedge_seed_t;

static bool collect_seeds(const volume_t *vol, hedge_seed_t **out, int *nout)
{
    volume_iterator_t iter;
    int pos[3], bbox[2][3];
    int *base = NULL;
    int w, h, x, y, n = 0, cap = 0;
    hedge_seed_t *seeds = NULL;

    if (!volume_get_bbox(vol, bbox, true)) {
        *out = NULL;
        *nout = 0;
        return true;
    }

    w = bbox[1][0] - bbox[0][0];
    h = bbox[1][1] - bbox[0][1];
    if (w <= 0 || h <= 0)
        return false;

    base = malloc((size_t)w * (size_t)h * sizeof(*base));
    if (!base)
        return false;
    for (x = 0; x < w * h; x++)
        base[x] = INT_MAX;

    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        int lx, ly, idx;
        if (!volume_get_alpha_at(vol, &iter, pos))
            continue;
        lx = pos[0] - bbox[0][0];
        ly = pos[1] - bbox[0][1];
        if (lx < 0 || ly < 0 || lx >= w || ly >= h)
            continue;
        idx = ly * w + lx;
        if (pos[2] < base[idx])
            base[idx] = pos[2];
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int idx = y * w + x;
            hedge_seed_t *ns;
            if (base[idx] == INT_MAX)
                continue;
            if (n >= cap) {
                int ncap = cap ? cap * 2 : 256;
                ns = realloc(seeds, (size_t)ncap * sizeof(*ns));
                if (!ns) {
                    free(base);
                    free(seeds);
                    return false;
                }
                seeds = ns;
                cap = ncap;
            }
            seeds[n].x = x + bbox[0][0];
            seeds[n].y = y + bbox[0][1];
            seeds[n].base_z = base[idx];
            n++;
        }
    }
    free(base);

    *out = seeds;
    *nout = n;
    return true;
}

static void paint_hedge_column(volume_t *vol, int x, int y, int base_z,
                               int height, float edge_t,
                               const filter_hedges_t *filter)
{
    int z, h;
    uint8_t c[4];
    int pos[3] = {x, y, 0};
    float intensity = (float)clampi(filter->noise_intensity, 0, 100);
    float saturation = (float)clampi(filter->noise_saturation, 0, 100);

    if (height < 1)
        return;
    h = height;

    /* Solid stack from the base — never leave gaps (no floating blocks). */
    for (z = 0; z < h; z++) {
        float topness = (h <= 1) ? 1.f : (float)z / (float)(h - 1);
        float shade;
        int rgb[3];
        float n;

        memcpy(c, filter->color, 4);

        shade = 0.94f + 0.08f * topness - 0.05f * edge_t;
        shade += (hash01(x, y, z, filter->seed + 70) - 0.5f) * 0.06f
               * (intensity / 100.f);
        c[0] = (uint8_t)clampi((int)((float)c[0] * shade), 0, 255);
        c[1] = (uint8_t)clampi((int)((float)c[1] * shade), 0, 255);
        c[2] = (uint8_t)clampi((int)((float)c[2] * shade), 0, 255);

        if (intensity > 0.f) {
            rgb[0] = c[0];
            rgb[1] = c[1];
            rgb[2] = c[2];
            n = hash01(x, y, z, filter->seed + 88);
            blend_with_noise_alpha(rgb, n, intensity, saturation, rgb);
            c[0] = (uint8_t)clampi(rgb[0], 0, 255);
            c[1] = (uint8_t)clampi(rgb[1], 0, 255);
            c[2] = (uint8_t)clampi(rgb[2], 0, 255);
        }

        pos[2] = base_z + z;
        volume_set_at(vol, NULL, pos, c);
    }
}

static int seed_is_nearest(const hedge_seed_t *seeds, int nseeds, int si,
                           int x, int y)
{
    int i;
    int dx = seeds[si].x - x;
    int dy = seeds[si].y - y;
    float d0 = (float)(dx * dx + dy * dy);

    for (i = 0; i < nseeds; i++) {
        float d;
        if (i == si)
            continue;
        dx = seeds[i].x - x;
        dy = seeds[i].y - y;
        d = (float)(dx * dx + dy * dy);
        if (d < d0)
            return 0;
        if (d == d0 && i < si)
            return 0;
    }
    return 1;
}

static void stamp_seed(volume_t *vol, const hedge_seed_t *seeds, int nseeds,
                       int si, int min_h, int max_h, int min_w, int max_w,
                       const filter_hedges_t *filter)
{
    const hedge_seed_t *seed = &seeds[si];
    float along = hedge_noise_smooth(seed->x, seed->y, filter->seed, 8.f);
    int local_w = lerpi(min_w, max_w, along);
    float seed_half = (float)(local_w - 1) * 0.5f;
    /* Shorter features + contrast so stretches actually hit min and max. */
    float height_along = contrast01(
        hedge_noise_smooth(seed->x, seed->y, filter->seed + 31, 4.f));
    int r, ox, oy;

    if (seed_half < 0.f)
        seed_half = 0.f;
    r = (int)ceilf(seed_half + 0.75f);

    for (oy = -r; oy <= r; oy++) {
        for (ox = -r; ox <= r; ox++) {
            int x = seed->x + ox;
            int y = seed->y + oy;
            float dist = sqrtf((float)(ox * ox + oy * oy));
            float edge_noise, edge_t, falloff, half_w;
            float height_t;
            int height;

            if (!seed_is_nearest(seeds, nseeds, si, x, y))
                continue;

            half_w = seed_half;
            /* Tiny outline wobble only — not per-cell width chaos. */
            edge_noise = (hash01(x / 3, y / 3, 3, filter->seed + 19) - 0.5f) * 0.35f;
            if (dist > half_w + edge_noise)
                continue;

            edge_t = (half_w < 1e-3f) ? 0.f : dist / (half_w + 0.25f);
            if (edge_t < 0.f) edge_t = 0.f;
            if (edge_t > 1.f) edge_t = 1.f;

            height_t = height_along * 0.70f
                     + contrast01(hedge_noise_smooth(x, y, filter->seed + 47, 3.f))
                       * 0.30f;
            height = lerpi(min_h, max_h, height_t);

            /* Taper only the outer fringe so the spine keeps full height range. */
            if (edge_t > 0.45f) {
                falloff = 1.f - 0.30f * ((edge_t - 0.45f) / 0.55f);
                height = (int)floorf((float)height * falloff + 0.5f);
                if (edge_t > 0.78f
                    && hash01(x, y, 6, filter->seed + 41) < 0.35f)
                    height -= 1;
            }

            if (height < 1)
                height = (ox == 0 && oy == 0) ? 1 : 0;
            if (height < 1)
                continue;

            paint_hedge_column(vol, x, y, seed->base_z, height, edge_t, filter);
        }
    }
}

static void apply_hedges(filter_hedges_t *filter, layer_t *layer)
{
    volume_t *src;
    hedge_seed_t *seeds = NULL;
    int nseeds = 0;
    int min_h, max_h, min_w, max_w;
    int i;

    if (!layer || !layer->volume || volume_is_empty(layer->volume))
        return;

    min_h = filter->min_height;
    max_h = filter->max_height;
    min_w = filter->min_width;
    max_w = filter->max_width;
    if (max_h < min_h) { int t = min_h; min_h = max_h; max_h = t; }
    if (max_w < min_w) { int t = min_w; min_w = max_w; max_w = t; }
    if (min_h < 1) min_h = 1;
    if (min_w < 1) min_w = 1;

    src = volume_copy(layer->volume);
    if (!src)
        return;
    if (!collect_seeds(src, &seeds, &nseeds) || nseeds == 0) {
        volume_delete(src);
        free(seeds);
        return;
    }

    image_history_push(goxel.image);
    volume_clear(layer->volume);

    for (i = 0; i < nseeds; i++)
        stamp_seed(layer->volume, seeds, nseeds, i, min_h, max_h, min_w, max_w,
                   filter);

    /* Keep the centreline populated if noise ate the seed column. */
    for (i = 0; i < nseeds; i++) {
        uint8_t probe[4];
        int pos[3] = {seeds[i].x, seeds[i].y, seeds[i].base_z};
        volume_get_at(layer->volume, NULL, pos, probe);
        if (probe[3] != 0)
            continue;
        paint_hedge_column(layer->volume, seeds[i].x, seeds[i].y, seeds[i].base_z,
                           clampi(lerpi(min_h, max_h,
                                        contrast01(hedge_noise_smooth(
                                            seeds[i].x, seeds[i].y,
                                            filter->seed + 31, 4.f))),
                                  1, max_h),
                           0.f, filter);
    }

    free(seeds);
    volume_delete(src);
}

static int gui(filter_t *filter_)
{
    filter_hedges_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;

    const char *help_text =
        "Replaces blocks on the active layer with choppy hedgerow foliage. "
        "Paint a path (or any footprint) of blocks, then Apply — occupied "
        "columns become the hedge centreline and expand to a noisy width/height "
        "like field-edge hedges.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_group_begin("Size");
    gui_input_int("Min height", &filter->min_height, 1, 32);
    gui_input_int("Max height", &filter->max_height, 1, 32);
    gui_input_int("Min width", &filter->min_width, 1, 32);
    gui_input_int("Max width", &filter->max_width, 1, 32);
    gui_group_end();

    gui_color_small("Colour", filter->color);
    gui_input_int("Intensity", &filter->noise_intensity, 0, 100);
    gui_tooltip_if_hovered(
        "How strongly colour variation mixes in. 0 = flat base colour.");
    gui_input_int("Saturation", &filter->noise_saturation, 0, 100);
    gui_tooltip_if_hovered(
        "How colourful the variation is. 0 = lightness-only mottling.");

    gui_separator();
    gui_input_int("Seed", &filter->seed, 0, RAND_MAX);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        filter->seed = rand();
    }

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    if (gui_button("Apply", -1, 0))
        apply_hedges(filter, layer);

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_hedges_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(hedges, filter_hedges_t,
                .name = "Generation - Hedges",
                .on_open = on_open,
                .panel_width = 260,
                .gui_fn = gui, )
