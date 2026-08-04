/* Mapmaker - Ace of Spades biome / heightmap utilities
 *
 * Ported from pyspades mapmaker.pyx by James Hofmann (2012), GPLv3.
 * Classicgen / genland excluded.
 */

#include "mapmaker.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --- RNG (xorshift32) -------------------------------------------------- */

void mm_rng_seed(mm_rng_t *rng, uint32_t seed)
{
    rng->state = seed ? seed : 1u;
}

static uint32_t mm_rng_u32(mm_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x ? x : 1u;
    return rng->state;
}

float mm_rng_float(mm_rng_t *rng)
{
    return (mm_rng_u32(rng) >> 8) * (1.0f / 16777216.0f);
}

int mm_rng_int(mm_rng_t *rng, int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int)(mm_rng_u32(rng) % (uint32_t)(hi - lo + 1));
}

/* --- Color helpers ----------------------------------------------------- */

static int lim_byte(int val)
{
    if (val < 0)
        return 0;
    if (val > 255)
        return 255;
    return val;
}

int mm_make_color(int r, int g, int b)
{
    return b | (g << 8) | (r << 16) | (128 << 24);
}

int mm_get_r(int color)
{
    return (color >> 16) & 0xFF;
}

int mm_get_g(int color)
{
    return (color >> 8) & 0xFF;
}

int mm_get_b(int color)
{
    return color & 0xFF;
}

static float mm_wrap01(float value)
{
    float r = value - floorf(value);
    if (r < 0.f)
        r += 1.f;
    return r;
}

void mm_rgb_to_hsb(int r, int g, int b, float *h, float *s, float *bri)
{
    float rf = r / 255.f, gf = g / 255.f, bf = b / 255.f;
    float maxv = rf > gf ? (rf > bf ? rf : bf) : (gf > bf ? gf : bf);
    float minv = rf < gf ? (rf < bf ? rf : bf) : (gf < bf ? gf : bf);
    float d = maxv - minv;
    *bri = maxv * 100.f;
    *s = (maxv <= 0.f) ? 0.f : (d / maxv) * 100.f;
    if (d <= 1e-6f) {
        *h = 0.f;
        return;
    }
    if (maxv == rf)
        *h = 60.f * fmodf(((gf - bf) / d), 6.f);
    else if (maxv == gf)
        *h = 60.f * (((bf - rf) / d) + 2.f);
    else
        *h = 60.f * (((rf - gf) / d) + 4.f);
    if (*h < 0.f)
        *h += 360.f;
}

void mm_hsb_to_rgb(float hue, float sat, float bri, int *r, int *g, int *bl)
{
    float bri_n = bri * 255.0f;
    float rr, gg, bb;
    if (sat == 0.0f) {
        rr = gg = bb = bri_n;
    } else {
        float hue_n = mm_wrap01(hue) * 6.f;
        float hue_i = floorf(hue_n);
        float hue_f = hue_n - hue_i;
        int hi = (int)hue_i;
        if (hi % 2 == 0)
            hue_f = 1.0f - hue_f;
        float m = bri_n * (1.0f - sat);
        float n = bri_n * (1.0f - (sat * hue_f));
        if (hi == 0 || hi == 6) {
            rr = bri_n;
            gg = n;
            bb = m;
        } else if (hi == 1) {
            rr = n;
            gg = bri_n;
            bb = m;
        } else if (hi == 2) {
            rr = m;
            gg = bri_n;
            bb = n;
        } else if (hi == 3) {
            rr = m;
            gg = n;
            bb = bri_n;
        } else if (hi == 4) {
            rr = n;
            gg = m;
            bb = bri_n;
        } else {
            rr = bri_n;
            gg = m;
            bb = n;
        }
    }
    *r = (int)rr;
    *g = (int)gg;
    *bl = (int)bb;
}

static void interpolate_rgb(const uint8_t a[3], const uint8_t b[3], float t,
                            int *r, int *g, int *bl)
{
    *r = (int)(a[0] + (b[0] - a[0]) * t);
    *g = (int)(a[1] + (b[1] - a[1]) * t);
    *bl = (int)(a[2] + (b[2] - a[2]) * t);
}

void mm_gradient_init(mm_gradient_t *g)
{
    memset(g, 0, sizeof(*g));
}

void mm_gradient_set_step_rgb(mm_gradient_t *g, int step, int r, int g_, int b)
{
    if (step < 0 || step >= MM_GRAD_STEPS)
        return;
    g->steps[step].r = (uint8_t)lim_byte(r);
    g->steps[step].g = (uint8_t)lim_byte(g_);
    g->steps[step].b = (uint8_t)lim_byte(b);
    g->steps[step].a = 255;
}

void mm_gradient_rgb(mm_gradient_t *g, int start_pos, const uint8_t sc[3],
                     int end_pos, const uint8_t ec[3])
{
    int n, dist = end_pos - start_pos;
    if (dist <= 0)
        return;
    for (n = start_pos; n < end_pos; n++) {
        float pct = (float)(n - start_pos) / (float)dist;
        int r, gg, b;
        interpolate_rgb(sc, ec, pct, &r, &gg, &b);
        mm_gradient_set_step_rgb(g, n, r, gg, b);
    }
}

void mm_gradient_hsb(mm_gradient_t *g, int start_pos, float sh, float ss, float sb,
                     int end_pos, float eh, float es, float eb)
{
    int n, dist = end_pos - start_pos;
    float s0[3], e0[3];
    if (dist <= 0)
        return;
    s0[0] = sh / 360.f;
    s0[1] = ss / 100.f;
    s0[2] = sb / 100.f;
    e0[0] = eh / 360.f;
    e0[1] = es / 100.f;
    e0[2] = eb / 100.f;
    for (n = start_pos; n < end_pos; n++) {
        float pct = (float)(n - start_pos) / (float)dist;
        float h = s0[0] + (e0[0] - s0[0]) * pct;
        float s = s0[1] + (e0[1] - s0[1]) * pct;
        float b = s0[2] + (e0[2] - s0[2]) * pct;
        int r, gg, bb;
        mm_hsb_to_rgb(h, s, b, &r, &gg, &bb);
        mm_gradient_set_step_rgb(g, n, r, gg, bb);
    }
}

void mm_gradient_array(const mm_gradient_t *g, int out[MM_GRAD_STEPS * 3])
{
    int i;
    for (i = 0; i < MM_GRAD_STEPS; i++) {
        const mm_rgba_t *s = &g->steps[MM_GRAD_STEPS - 1 - i];
        out[i * 3 + 0] = s->r;
        out[i * 3 + 1] = s->g;
        out[i * 3 + 2] = s->b;
    }
}

static int paint_gradient(const int *zcoltable, int z, mm_rng_t *rng)
{
    int zz = z * 3;
    int rnd = mm_rng_int(rng, -4, 4);
    return mm_make_color(lim_byte(zcoltable[zz] + rnd),
                         lim_byte(zcoltable[zz + 1] + rnd),
                         lim_byte(zcoltable[zz + 2] + rnd));
}

/* --- Bresenham --------------------------------------------------------- */

typedef struct {
    int x, y;
} mm_pt_t;

static int bresenham_line(int x, int y, int x2, int y2, mm_pt_t *coords, int max_coords)
{
    int steep = 0;
    int dx, dy, sx, sy, d, i, n = 0;
    int ox = x2, oy = y2;
    dx = abs(x2 - x);
    sx = (x2 - x) > 0 ? 1 : -1;
    dy = abs(y2 - y);
    sy = (y2 - y) > 0 ? 1 : -1;
    if (dy > dx) {
        int tmp;
        steep = 1;
        tmp = x;
        x = y;
        y = tmp;
        tmp = dx;
        dx = dy;
        dy = tmp;
        tmp = sx;
        sx = sy;
        sy = tmp;
    }
    d = (2 * dy) - dx;
    for (i = 0; i < dx; i++) {
        if (n < max_coords) {
            if (steep) {
                coords[n].x = y;
                coords[n].y = x;
            } else {
                coords[n].x = x;
                coords[n].y = y;
            }
            n++;
        }
        while (d >= 0) {
            y = y + sy;
            d = d - (2 * dx);
        }
        x = x + sx;
        d = d + (2 * dy);
    }
    if (n < max_coords) {
        coords[n].x = ox;
        coords[n].y = oy;
        n++;
    }
    return n;
}

/* --- HeightMap --------------------------------------------------------- */

bool mm_heightmap_init(mm_heightmap_t *hm, float fill_height)
{
    int i, n;
    hm->width = MM_MAP_SIZE;
    hm->height = MM_MAP_SIZE;
    n = hm->width * hm->height;
    hm->hmap = malloc(sizeof(float) * (size_t)n);
    hm->cmap = malloc(sizeof(int) * (size_t)n);
    if (!hm->hmap || !hm->cmap) {
        free(hm->hmap);
        free(hm->cmap);
        hm->hmap = NULL;
        hm->cmap = NULL;
        return false;
    }
    for (i = 0; i < n; i++) {
        hm->hmap[i] = fill_height;
        hm->cmap[i] = 0xFF00FFFF;
    }
    return true;
}

void mm_heightmap_free(mm_heightmap_t *hm)
{
    free(hm->hmap);
    free(hm->cmap);
    hm->hmap = NULL;
    hm->cmap = NULL;
}

float mm_hm_get(const mm_heightmap_t *hm, int x, int y)
{
    return hm->hmap[x + y * hm->width];
}

float mm_hm_get_repeat(const mm_heightmap_t *hm, int x, int y)
{
    int xx = ((x % hm->width) + hm->width) % hm->width;
    int yy = ((y % hm->height) + hm->height) % hm->height;
    return hm->hmap[xx + yy * hm->width];
}

void mm_hm_set(mm_heightmap_t *hm, int x, int y, float val)
{
    if (x < 0 || y < 0 || x >= hm->width || y >= hm->height)
        return;
    hm->hmap[x + y * hm->width] = val;
}

void mm_hm_set_repeat(mm_heightmap_t *hm, int x, int y, float val)
{
    int xx = ((x % hm->width) + hm->width) % hm->width;
    int yy = ((y % hm->height) + hm->height) % hm->height;
    hm->hmap[xx + yy * hm->width] = val;
}

void mm_hm_add_repeat(mm_heightmap_t *hm, int x, int y, float val)
{
    int xx = ((x % hm->width) + hm->width) % hm->width;
    int yy = ((y % hm->height) + hm->height) % hm->height;
    hm->hmap[xx + yy * hm->width] += val;
}

int mm_hm_get_col(const mm_heightmap_t *hm, int x, int y)
{
    return hm->cmap[x + y * hm->width];
}

int mm_hm_get_col_repeat(const mm_heightmap_t *hm, int x, int y)
{
    int xx = ((x % hm->width) + hm->width) % hm->width;
    int yy = ((y % hm->height) + hm->height) % hm->height;
    return hm->cmap[xx + yy * hm->width];
}

void mm_hm_set_col_repeat(mm_heightmap_t *hm, int x, int y, int val)
{
    int xx = ((x % hm->width) + hm->width) % hm->width;
    int yy = ((y % hm->height) + hm->height) % hm->height;
    hm->cmap[xx + yy * hm->width] = val;
}

void mm_hm_rect_noise(mm_heightmap_t *hm, mm_rng_t *rng,
                      int x, int y, int w, int h,
                      double jitter, double midpoint)
{
    int xx, yy;
    double halfjitter = jitter * 0.5;
    for (xx = x; xx < x + w; xx++) {
        for (yy = y; yy < y + h; yy++) {
            mm_hm_set(hm, xx, yy,
                      (float)(midpoint + (mm_rng_float(rng) * jitter - halfjitter)));
        }
    }
}

void mm_hm_rect_color(mm_heightmap_t *hm, int x, int y, int w, int h, int col)
{
    int xx, yy;
    for (xx = x; xx < x + w; xx++) {
        for (yy = y; yy < y + h; yy++) {
            mm_hm_set_col_repeat(hm, xx, yy, col);
        }
    }
}

void mm_hm_smoothing(mm_heightmap_t *hm)
{
    int x, y;
    float *tmp = malloc(sizeof(float) * (size_t)(hm->width * hm->height));
    if (!tmp)
        return;
    memcpy(tmp, hm->hmap, sizeof(float) * (size_t)(hm->width * hm->height));
    for (x = 0; x < hm->width; x++) {
        for (y = 0; y < hm->height; y++) {
            int w = hm->width;
            int h = hm->height;
            float top = tmp[x + ((y - 1 + h) % h) * w];
            float left = tmp[((x - 1 + w) % w) + y * w];
            float right = tmp[((x + 1) % w) + y * w];
            float bot = tmp[x + ((y + 1) % h) * w];
            float center = tmp[x + y * w];
            hm->hmap[x + y * w] = (top + left + right + bot + center) / 5.f;
        }
    }
    free(tmp);
}

void mm_hm_midpoint_displace(mm_heightmap_t *hm, mm_rng_t *rng,
                             double jittervalue, double spanscalingmultiplier,
                             int skip)
{
    int span = hm->width + 1;
    float spanscaling = 1.f;
    int iterations;
    for (iterations = 0; iterations < 9; iterations++) {
        if (skip > 0) {
            skip--;
            span = span >> 1;
            spanscaling = (float)(spanscaling * spanscalingmultiplier);
            continue;
        }
        {
            float jitterrange = (float)(jittervalue * spanscaling);
            float jitteroffset = -jitterrange / 2.f;
            int x, y;
            for (x = 0; x < hm->width; x += span) {
                for (y = 0; y < hm->height; y += span) {
                    int halfspan = span >> 1;
                    float topleft = mm_hm_get_repeat(hm, x, y);
                    float topright = mm_hm_get_repeat(hm, x + span, y);
                    float botleft = mm_hm_get_repeat(hm, x, y + span);
                    float botright = mm_hm_get_repeat(hm, x + span, y + span);
                    float center = (topleft + topright + botleft + botright) * 0.25f
                        + (mm_rng_float(rng) * jitterrange + jitteroffset);
                    mm_hm_set_repeat(hm, x + halfspan, y,
                                     (topleft + topright + center) * 0.33f);
                    mm_hm_set_repeat(hm, x, y + halfspan,
                                     (topleft + botleft + center) * 0.33f);
                    mm_hm_set_repeat(hm, x + halfspan, y + span,
                                     (botleft + botright + center) * 0.33f);
                    mm_hm_set_repeat(hm, x + span, y + halfspan,
                                     (topright + botright + center) * 0.33f);
                    mm_hm_set_repeat(hm, x + halfspan, y + halfspan, center);
                }
            }
        }
        span = span >> 1;
        spanscaling = (float)(spanscaling * spanscalingmultiplier);
    }
}

void mm_hm_jitter_colors(mm_heightmap_t *hm, mm_rng_t *rng, double amount)
{
    int idx = 0;
    int n = hm->width * hm->height;
    int *tmp = malloc(sizeof(int) * (size_t)n);
    if (!tmp)
        return;
    memcpy(tmp, hm->cmap, sizeof(int) * (size_t)n);
    while (idx < n) {
        int nx = (int)((idx % hm->width) + (mm_rng_float(rng) - 0.5f) * amount);
        int ny = (int)((idx / hm->width) + (mm_rng_float(rng) - 0.5f) * amount);
        int xx = ((nx % hm->width) + hm->width) % hm->width;
        int yy = ((ny % hm->height) + hm->height) % hm->height;
        hm->cmap[idx] = tmp[xx + yy * hm->width];
        idx++;
    }
    free(tmp);
}

void mm_hm_truncate(mm_heightmap_t *hm)
{
    int i, n = hm->width * hm->height;
    for (i = 0; i < n; i++) {
        float v = hm->hmap[i];
        if (v < 0.f)
            v = 0.f;
        if (v > 1.f)
            v = 1.f;
        hm->hmap[i] = v;
    }
}

void mm_hm_line_add(mm_heightmap_t *hm, int x, int y, int x2, int y2,
                    int radius, double depth)
{
    mm_pt_t coords[2048];
    int n = bresenham_line(x, y, x2, y2, coords, 2048);
    int i, ox, oy;
    for (i = 0; i < n; i++) {
        int posx = coords[i].x;
        int posy = coords[i].y;
        for (ox = -radius; ox <= radius; ox++) {
            for (oy = -radius; oy <= radius; oy++) {
                mm_hm_add_repeat(hm, posx + ox, posy + oy, (float)depth);
            }
        }
    }
}

void mm_hm_line_set(mm_heightmap_t *hm, int x, int y, int x2, int y2,
                    int radius, double height)
{
    mm_pt_t coords[2048];
    int n = bresenham_line(x, y, x2, y2, coords, 2048);
    int i, ox, oy;
    for (i = 0; i < n; i++) {
        int posx = coords[i].x;
        int posy = coords[i].y;
        for (ox = -radius; ox <= radius; ox++) {
            for (oy = -radius; oy <= radius; oy++) {
                mm_hm_set_repeat(hm, posx + ox, posy + oy, (float)height);
            }
        }
    }
}

void mm_hm_rewrite_gradient_fill(mm_heightmap_t *hm, mm_rng_t *rng,
                                 const int *const *zcoldefs, int n_gradients)
{
    int idx = 0;
    int n = hm->width * hm->height;
    while (idx < n) {
        int h = (int)(hm->hmap[idx] * 63);
        int gid = hm->cmap[idx];
        if (gid < 0 || gid >= n_gradients)
            gid = 0;
        hm->cmap[idx] = paint_gradient(zcoldefs[gid], h, rng);
        idx++;
    }
}

void mm_hm_rgb_noise_colors(mm_heightmap_t *hm, mm_rng_t *rng, int low, int high)
{
    int patterns[101];
    int i, idx = 0;
    int n = hm->width * hm->height;
    for (i = 0; i < 101; i++)
        patterns[i] = mm_rng_int(rng, low, high);
    while (idx < n) {
        int mid = hm->cmap[idx];
        int r = lim_byte(mm_get_r(mid) + patterns[idx % 101]);
        int g = lim_byte(mm_get_g(mid) + patterns[(idx + 1) % 101]);
        int b = lim_byte(mm_get_b(mid) + patterns[(idx + 2) % 101]);
        hm->cmap[idx] = mm_make_color(r, g, b);
        idx++;
    }
}

void mm_hm_smooth_colors(mm_heightmap_t *hm)
{
    int x = 0, y = 0;
    int n = hm->width * hm->height;
    int *swap = malloc(sizeof(int) * (size_t)n);
    if (!swap)
        return;
    memcpy(swap, hm->cmap, sizeof(int) * (size_t)n);
    while (y < hm->height) {
        int left = swap[((x - 1 + hm->width) % hm->width) + y * hm->width];
        int right = swap[((x + 1) % hm->width) + y * hm->width];
        int up = swap[x + ((y - 1 + hm->height) % hm->height) * hm->width];
        int down = swap[x + ((y + 1) % hm->height) * hm->width];
        int mid = swap[x + y * hm->width];
        int r = (mm_get_r(left) + mm_get_r(right) + mm_get_r(up)
                 + mm_get_r(down) + mm_get_r(mid)) / 5;
        int g = (mm_get_g(left) + mm_get_g(right) + mm_get_g(up)
                 + mm_get_g(down) + mm_get_g(mid)) / 5;
        int b = (mm_get_b(left) + mm_get_b(right) + mm_get_b(up)
                 + mm_get_b(down) + mm_get_b(mid)) / 5;
        mm_hm_set_col_repeat(hm, x, y, mm_make_color(r, g, b));
        x++;
        if (x >= hm->width) {
            x = 0;
            y++;
        }
    }
    free(swap);
}

/* --- BiomeMap ---------------------------------------------------------- */

bool mm_biomemap_init(mm_biomemap_t *bm, mm_biome_t *biomes, int n_biomes,
                      int width, int height)
{
    int i;
    bm->biomes = biomes;
    bm->n_biomes = n_biomes;
    bm->width = width;
    bm->height = height;
    bm->twidth = MM_MAP_SIZE / width;
    bm->theight = MM_MAP_SIZE / height;
    bm->tmap = malloc(sizeof(mm_biome_t *) * (size_t)(width * height));
    bm->gradients = malloc(sizeof(mm_gradient_t *) * (size_t)n_biomes);
    if (!bm->tmap || !bm->gradients) {
        free(bm->tmap);
        free(bm->gradients);
        bm->tmap = NULL;
        bm->gradients = NULL;
        return false;
    }
    for (i = 0; i < width * height; i++)
        bm->tmap[i] = &biomes[0];
    for (i = 0; i < n_biomes; i++) {
        biomes[i].id = i;
        bm->gradients[i] = biomes[i].gradient;
    }
    return true;
}

void mm_biomemap_free(mm_biomemap_t *bm)
{
    free(bm->tmap);
    free(bm->gradients);
    bm->tmap = NULL;
    bm->gradients = NULL;
}

mm_biome_t *mm_bm_get_repeat(const mm_biomemap_t *bm, int x, int y)
{
    int xx = ((x % bm->width) + bm->width) % bm->width;
    int yy = ((y % bm->height) + bm->height) % bm->height;
    return bm->tmap[xx + yy * bm->width];
}

void mm_bm_set_repeat(mm_biomemap_t *bm, int x, int y, mm_biome_t *val)
{
    int xx = ((x % bm->width) + bm->width) % bm->width;
    int yy = ((y % bm->height) + bm->height) % bm->height;
    bm->tmap[xx + yy * bm->width] = val;
}

int mm_bm_random_points(mm_biomemap_t *bm, mm_rng_t *rng,
                        int qty, mm_biome_t *biome,
                        int x, int y, int w, int h,
                        mm_biome_point_t *out, int max_out)
{
    int n, written = 0;
    if (w <= 0)
        w = bm->width;
    if (h <= 0)
        h = bm->height;
    for (n = 0; n < qty && written < max_out; n++) {
        out[written].x = mm_rng_int(rng, x, x + w);
        out[written].y = mm_rng_int(rng, y, y + h);
        out[written].biome = biome;
        written++;
    }
    return written;
}

/* Round-robin multi-source flood fill (matches mapmaker point_flood). */
void mm_bm_point_flood(mm_biomemap_t *bm, const mm_biome_point_t *points,
                       int n_points)
{
    typedef struct {
        int x, y;
        mm_biome_t *biome;
    } node_t;

    node_t **queues = NULL;
    int *qhead = NULL, *qtail = NULL, *qcap = NULL;
    int *open_ids = NULL;
    int open_n = 0;
    char *closed = NULL;
    int i, map_cells = bm->width * bm->height;

    if (n_points <= 0)
        return;

    queues = calloc((size_t)n_points, sizeof(node_t *));
    qhead = calloc((size_t)n_points, sizeof(int));
    qtail = calloc((size_t)n_points, sizeof(int));
    qcap = calloc((size_t)n_points, sizeof(int));
    open_ids = malloc(sizeof(int) * (size_t)n_points);
    closed = calloc((size_t)map_cells, 1);
    if (!queues || !qhead || !qtail || !qcap || !open_ids || !closed)
        goto cleanup;

    for (i = 0; i < n_points; i++) {
        qcap[i] = 64;
        queues[i] = malloc(sizeof(node_t) * (size_t)qcap[i]);
        if (!queues[i])
            goto cleanup;
        queues[i][0].x = points[i].x;
        queues[i][0].y = points[i].y;
        queues[i][0].biome = points[i].biome;
        qhead[i] = 0;
        qtail[i] = 1;
        open_ids[open_n++] = i;
    }

    while (open_n > 0) {
        int qi = open_ids[0];
        node_t p;
        memmove(&open_ids[0], &open_ids[1], sizeof(int) * (size_t)(open_n - 1));
        open_n--;
        if (qhead[qi] >= qtail[qi])
            continue;
        p = queues[qi][qhead[qi]++];

        if (p.x >= 0 && p.y >= 0 && p.x < bm->width && p.y < bm->height) {
            int ci = p.x + p.y * bm->width;
            closed[ci] = 1;
            mm_bm_set_repeat(bm, p.x, p.y, p.biome);

            if (p.x > 0 && !closed[(p.x - 1) + p.y * bm->width]) {
                if (qtail[qi] >= qcap[qi]) {
                    int ncap = qcap[qi] * 2;
                    node_t *nq = realloc(queues[qi], sizeof(node_t) * (size_t)ncap);
                    if (nq) {
                        queues[qi] = nq;
                        qcap[qi] = ncap;
                    }
                }
                if (qtail[qi] < qcap[qi]) {
                    queues[qi][qtail[qi]].x = p.x - 1;
                    queues[qi][qtail[qi]].y = p.y;
                    queues[qi][qtail[qi]].biome = p.biome;
                    qtail[qi]++;
                }
            }
            if (p.x < bm->width - 1 && !closed[(p.x + 1) + p.y * bm->width]) {
                if (qtail[qi] >= qcap[qi]) {
                    int ncap = qcap[qi] * 2;
                    node_t *nq = realloc(queues[qi], sizeof(node_t) * (size_t)ncap);
                    if (nq) {
                        queues[qi] = nq;
                        qcap[qi] = ncap;
                    }
                }
                if (qtail[qi] < qcap[qi]) {
                    queues[qi][qtail[qi]].x = p.x + 1;
                    queues[qi][qtail[qi]].y = p.y;
                    queues[qi][qtail[qi]].biome = p.biome;
                    qtail[qi]++;
                }
            }
            if (p.y > 0 && !closed[p.x + (p.y - 1) * bm->width]) {
                if (qtail[qi] >= qcap[qi]) {
                    int ncap = qcap[qi] * 2;
                    node_t *nq = realloc(queues[qi], sizeof(node_t) * (size_t)ncap);
                    if (nq) {
                        queues[qi] = nq;
                        qcap[qi] = ncap;
                    }
                }
                if (qtail[qi] < qcap[qi]) {
                    queues[qi][qtail[qi]].x = p.x;
                    queues[qi][qtail[qi]].y = p.y - 1;
                    queues[qi][qtail[qi]].biome = p.biome;
                    qtail[qi]++;
                }
            }
            if (p.y < bm->height - 1 && !closed[p.x + (p.y + 1) * bm->width]) {
                if (qtail[qi] >= qcap[qi]) {
                    int ncap = qcap[qi] * 2;
                    node_t *nq = realloc(queues[qi], sizeof(node_t) * (size_t)ncap);
                    if (nq) {
                        queues[qi] = nq;
                        qcap[qi] = ncap;
                    }
                }
                if (qtail[qi] < qcap[qi]) {
                    queues[qi][qtail[qi]].x = p.x;
                    queues[qi][qtail[qi]].y = p.y + 1;
                    queues[qi][qtail[qi]].biome = p.biome;
                    qtail[qi]++;
                }
            }
        }
        if (qhead[qi] < qtail[qi] && open_n < n_points)
            open_ids[open_n++] = qi;
    }

cleanup:
    if (queues) {
        for (i = 0; i < n_points; i++)
            free(queues[i]);
    }
    free(queues);
    free(qhead);
    free(qtail);
    free(qcap);
    free(open_ids);
    free(closed);
}

void mm_bm_jitter(mm_biomemap_t *bm, mm_rng_t *rng)
{
    int idx;
    mm_biome_t **tmp = malloc(sizeof(mm_biome_t *) * (size_t)(bm->width * bm->height));
    if (!tmp)
        return;
    memcpy(tmp, bm->tmap, sizeof(mm_biome_t *) * (size_t)(bm->width * bm->height));
    for (idx = 0; idx < bm->width * bm->height; idx++) {
        int x = idx % bm->width;
        int y = idx / bm->width;
        int nx = x + mm_rng_int(rng, -1, 1);
        int ny = y + mm_rng_int(rng, -1, 1);
        int xx = ((nx % bm->width) + bm->width) % bm->width;
        int yy = ((ny % bm->height) + bm->height) % bm->height;
        bm->tmap[idx] = tmp[xx + yy * bm->width];
    }
    free(tmp);
}

bool mm_bm_create_heightmap(mm_biomemap_t *bm, mm_rng_t *rng,
                            mm_heightmap_t *out_hm)
{
    int idx;
    if (!mm_heightmap_init(out_hm, 0.f))
        return false;
    for (idx = 0; idx < bm->width * bm->height; idx++) {
        int x = idx % bm->width;
        int y = idx / bm->width;
        mm_biome_t *biome = bm->tmap[idx];
        float mid = biome->height + mm_rng_float(rng) * biome->variation;
        mm_hm_rect_noise(out_hm, rng,
                         x * bm->twidth, y * bm->theight,
                         bm->twidth, bm->theight,
                         biome->noise, mid);
        mm_hm_rect_color(out_hm,
                         x * bm->twidth, y * bm->theight,
                         bm->twidth, bm->theight,
                         biome->id);
    }
    return true;
}

void mm_bm_rect_of_point(const mm_biomemap_t *bm, int x, int y,
                         int *left, int *top, int *right, int *bottom)
{
    *left = x * bm->twidth;
    *top = y * bm->theight;
    *right = *left + bm->twidth;
    *bottom = *top + bm->theight;
}
