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

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Plan - Explosions: blast craters into a chosen terrain layer where the
 * active (plan) layer has blocks.
 *
 * 1. Collect plan voxels on the active layer; collapse to one per XY.
 * 2. With Position using layer heights: from each plan Z, drop to the nearest
 *    occupied voxel beneath on the target terrain layer. Otherwise keep plan Z.
 * 3. Assign each blast a random radius / depth / strength in the UI min-max.
 * 4. Carve a smoothstep-lip bowl below the epicentre plus an upper hemisphere
 *    so blocks above the plan are cleared too (soft AA / dither edges, soft
 *    lip shelf so walls ease into surrounding height). Overlaps union
 *    (deepest floor / highest ceiling).
 * 5. Scorch remaining voxels toward black downward by the blast depth; always
 *    burn a darker half-radius core independent of Scorch. Bleed scorch into
 *    surrounding terrain (also downward by depth).
 * 6. Scatter debris by min-max density, settle onto terrain, bleed colour.
 *
 * Edits the chosen layer in place and hides the plan layer (one undo step).
 */

typedef struct {
    int x, y, z; /* surface top solid of terrain under the plan column */
    float radius;
    float depth;
    float strength; /* 0-1 fling distance for this blast */
    float scorch;   /* 0-1 darken toward black for this blast */
    float debris;   /* 0-1 scatter density for this blast */
} explosion_center_t;

typedef struct {
    filter_t filter;
    int min_radius;
    int max_radius;
    int min_depth;
    int max_depth;
    int min_strength; /* 0-100: fling force */
    int max_strength;
    int min_scorch;   /* 0-100: darken toward black, independent of strength */
    int max_scorch;
    int min_debris;   /* 0-100: scatter density; 0 = none */
    int max_debris;
    int color_bleed;  /* 0-100: how hard debris tints nearby solids */
    int anti_alias;
    float dithering;
    int seed;
    bool use_layer_heights; /* true: drop to terrain; false: blast at plan Z */
    /* Layer id, not a pointer: undo/redo swaps image snapshots. */
    int target_layer_id;
} filter_explosions_t;

static void reset_defaults(filter_explosions_t *filter)
{
    filter->min_radius = 4;
    filter->max_radius = 8;
    filter->min_depth = 1;
    filter->max_depth = 4;
    filter->min_strength = 40;
    filter->max_strength = 70;
    filter->min_scorch = 40;
    filter->max_scorch = 60;
    filter->min_debris = 25;
    filter->max_debris = 40;
    filter->color_bleed = 55;
    filter->anti_alias = 5;
    filter->dithering = 0.8f;
    filter->seed = 0;
    filter->use_layer_heights = true;
    filter->target_layer_id = 0;
}

static layer_t *find_layer_by_id(int id)
{
    layer_t *layer;

    if (!goxel.image || id <= 0)
        return NULL;
    DL_FOREACH(goxel.image->layers, layer) {
        if (layer->id == id)
            return layer;
    }
    return NULL;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void order_minmax_int(int *lo, int *hi, int min_v, int max_v)
{
    int a = clampi(*lo, min_v, max_v);
    int b = clampi(*hi, min_v, max_v);
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    *lo = a;
    *hi = b;
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

static float lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float fade(float t)
{
    return t * t * (3.f - 2.f * t);
}

/* Smooth value noise in [0,1] — continuous neighbours (no dotted holes). */
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

/* Two-octave shape field in [0,1]; larger scale = broader lobes. */
static float shape_noise(float x, float y, int seed, float scale)
{
    float s = fmaxf(scale, 1.5f);
    float n1 = value_noise2(x / s, y / s, seed);
    float n2 = value_noise2(x / (s * 2.15f), y / (s * 2.15f), seed + 19);
    return n1 * 0.68f + n2 * 0.32f;
}

/*
 * Domain-warp distance from blast centre: smoothly pushes the silhouette into
 * lobes / dents without per-cell random holes.
 */
static float warped_blast_dist(int x, int y, const explosion_center_t *c,
                               int seed)
{
    float dx = (float)(x - c->x);
    float dy = (float)(y - c->y);
    float scale = fmaxf(c->radius * 0.5f, 2.2f);
    float amp = c->radius * 0.38f;
    float nx = value_noise2((float)x / scale,
                            (float)y / scale,
                            seed + 221 + c->x * 3);
    float ny = value_noise2((float)x / scale + 37.f,
                            (float)y / scale,
                            seed + 229 + c->y * 5);
    dx += (nx * 2.f - 1.f) * amp;
    dy += (ny * 2.f - 1.f) * amp;
    return sqrtf(dx * dx + dy * dy);
}

/* Local effective radius — continuous ±~30% bulge/indent. */
static float warped_blast_radius(int x, int y, const explosion_center_t *c,
                                 int seed)
{
    float scale = fmaxf(c->radius * 0.55f, 2.5f);
    float n = shape_noise((float)(x + c->x * 2), (float)(y + c->y * 2),
                          seed + 201 + c->x + c->y * 17, scale);
    return c->radius * (0.70f + 0.60f * n);
}

static bool collect_plan_voxels(const volume_t *vol, explosion_center_t **out,
                                int *nout)
{
    volume_iterator_t iter;
    int pos[3], n = 0, cap = 0;
    explosion_center_t *voxels = NULL;

    iter = volume_get_iterator(vol, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        explosion_center_t *nv;
        if (!volume_get_alpha_at(vol, &iter, pos))
            continue;
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 256;
            nv = realloc(voxels, (size_t)ncap * sizeof(*voxels));
            if (!nv) {
                free(voxels);
                return false;
            }
            voxels = nv;
            cap = ncap;
        }
        voxels[n].x = pos[0];
        voxels[n].y = pos[1];
        voxels[n].z = pos[2];
        voxels[n].radius = 0.f;
        voxels[n].depth = 0.f;
        voxels[n].strength = 0.f;
        voxels[n].scorch = 0.f;
        voxels[n].debris = 0.f;
        n++;
    }

    *out = voxels;
    *nout = n;
    return true;
}

/* Collapse to one seed per XY, keeping the lowest plan Z in each column. */
static void collapse_plan_xy(explosion_center_t *plan, int *nplan)
{
    int i, j, n = 0;

    for (i = 0; i < *nplan; i++) {
        bool found = false;

        for (j = 0; j < n; j++) {
            if (plan[j].x == plan[i].x && plan[j].y == plan[i].y) {
                if (plan[i].z < plan[j].z)
                    plan[j].z = plan[i].z;
                found = true;
                break;
            }
        }
        if (found)
            continue;

        plan[n] = plan[i];
        plan[n].radius = 0.f;
        plan[n].depth = 0.f;
        plan[n].strength = 0.f;
        plan[n].scorch = 0.f;
        plan[n].debris = 0.f;
        n++;
    }
    *nplan = n;
}

/*
 * Drop each plan seed to the nearest occupied voxel at or below its Z on
 * the target layer. Blast centre sits on that solid. Columns with no solid
 * beneath are removed.
 */
static void project_plan_voxels(explosion_center_t *plan, int *nplan,
                                const volume_t *surface)
{
    int bbox[2][3];
    int i, kept = 0;
    uint8_t c[4];

    if (!volume_get_bbox(surface, bbox, true)) {
        *nplan = 0;
        return;
    }

    for (i = 0; i < *nplan; i++) {
        int pos[3] = {plan[i].x, plan[i].y, plan[i].z};
        int z_min = bbox[0][2];

        if (pos[2] > bbox[1][2] - 1)
            pos[2] = bbox[1][2] - 1;
        for (; pos[2] >= z_min; pos[2]--) {
            volume_get_at(surface, NULL, pos, c);
            if (c[3])
                break;
        }
        if (pos[2] < z_min)
            continue;

        plan[kept].x = pos[0];
        plan[kept].y = pos[1];
        plan[kept].z = pos[2];
        plan[kept].radius = 0.f;
        plan[kept].depth = 0.f;
        plan[kept].strength = 0.f;
        plan[kept].scorch = 0.f;
        plan[kept].debris = 0.f;
        kept++;
    }
    *nplan = kept;
}

/* Roll per-blast size / strength / scorch / debris inside min-max ranges. */
static void assign_blast_params(explosion_center_t *plan, int nplan,
                                const filter_explosions_t *filter)
{
    int i;
    int r0 = filter->min_radius, r1 = filter->max_radius;
    int d0 = filter->min_depth, d1 = filter->max_depth;
    int s0 = filter->min_strength, s1 = filter->max_strength;
    int c0 = filter->min_scorch, c1 = filter->max_scorch;
    int e0 = filter->min_debris, e1 = filter->max_debris;

    order_minmax_int(&r0, &r1, 1, 64);
    order_minmax_int(&d0, &d1, 1, 64);
    order_minmax_int(&s0, &s1, 0, 100);
    order_minmax_int(&c0, &c1, 0, 100);
    order_minmax_int(&e0, &e1, 0, 100);

    for (i = 0; i < nplan; i++) {
        float tr = hash01(plan[i].x, plan[i].y, plan[i].z, filter->seed + 101);
        float td = hash01(plan[i].x, plan[i].y, plan[i].z, filter->seed + 103);
        float ts = hash01(plan[i].x, plan[i].y, plan[i].z, filter->seed + 107);
        float tc = hash01(plan[i].x, plan[i].y, plan[i].z, filter->seed + 109);
        float te = hash01(plan[i].x, plan[i].y, plan[i].z, filter->seed + 113);
        plan[i].radius = lerpf((float)r0, (float)r1, tr);
        plan[i].depth = lerpf((float)d0, (float)d1, td);
        plan[i].strength = lerpf((float)s0, (float)s1, ts) / 100.f;
        plan[i].scorch = lerpf((float)c0, (float)c1, tc) / 100.f;
        plan[i].debris = lerpf((float)e0, (float)e1, te) / 100.f;
        if (plan[i].radius < 1.f)
            plan[i].radius = 1.f;
        if (plan[i].depth < 1.f)
            plan[i].depth = 1.f;
    }
}

/* Soft blast edge in [0,1]. Mild dither only — strong dither creates dotted holes. */
static float crater_edge(float dist, float radius, float aa, float dithering,
                         int x, int y, int seed)
{
    float k = radius - dist;
    float d = dithering * 0.28f;

    if (d > 0.f) {
        float n = uniform_noise((float)x + (float)seed * 0.017f,
                                (float)y, (float)seed * 0.31f);
        k += (n * 2.f - 1.f) * d;
    }
    if (aa > 0.f)
        return clamp(k / aa, -1.f, 1.f) / 2.f + 0.5f;
    return (k >= 0.f) ? 1.f : 0.f;
}

/*
 * Vertical carve range at XY for one centre: bowl floor below the epicentre
 * plus upper-hemisphere ceiling so overhangs / stacked blocks go.
 * Lip uses smoothstep radial falloff and soft-edge depth so walls ease into
 * surrounding terrain instead of a hard cliff. Returns soft edge in [0,1].
 */
static float blast_column_range(int x, int y, const explosion_center_t *c,
                                float aa, float dithering, int seed,
                                int *out_floor_z, int *out_ceil_z)
{
    float dist, radius, edge, t, lip, n, bowl, hemi;
    float floor_f, ceil_f;
    float edge_depth;

    dist = warped_blast_dist(x, y, c, seed);
    radius = warped_blast_radius(x, y, c, seed);
    if (radius < 1.f)
        radius = 1.f;

    edge = crater_edge(dist, radius, aa, dithering, x, y, seed);
    if (edge <= 0.f)
        return 0.f;

    t = clamp(dist / radius, 0.f, 1.f);
    /* Smoothstep lip: depth dwindles sooner near the rim. */
    lip = t * t * (3.f - 2.f * t);
    /* Soft AA rim digs less so the wall slopes into surrounding height. */
    edge_depth = powf(fmaxf(edge, 0.f), 0.62f);
    /* Smooth depth ripple (not hash speckles). */
    n = shape_noise((float)x, (float)y, seed + 91 + c->z, fmaxf(radius * 0.4f, 2.f));
    bowl = c->depth * (1.f - lip) * (0.88f + 0.24f * n) * edge_depth;
    hemi = radius * sqrtf(fmaxf(0.f, 1.f - t * t)) * edge_depth * edge;

    floor_f = (float)c->z - bowl;
    ceil_f = (float)c->z + hemi;

    if (out_floor_z)
        *out_floor_z = (int)floorf(floor_f);
    if (out_ceil_z)
        *out_ceil_z = (int)ceilf(ceil_f);
    return edge;
}

/*
 * Union of all blasts at XY: deepest floor, highest ceiling, strongest
 * debris strength, and soft coverage of the winning (deepest) blast.
 */
static float max_blast_range(int x, int y, const explosion_center_t *centers,
                             int ncenters, float aa, float dithering, int seed,
                             int *out_floor_z, int *out_ceil_z,
                             float *out_strength, float *out_scorch,
                             float *out_debris, float *out_radius,
                             float *out_depth, int *out_nearest)
{
    float best_edge = 0.f;
    int best_floor = INT_MAX;
    int best_ceil = INT_MIN;
    float best_str = 0.f;
    float best_scorch = 0.f;
    float best_debris = 0.f;
    float best_r = 1.f;
    float best_depth = 1.f;
    int best_i = 0;
    int i;
    bool any = false;

    for (i = 0; i < ncenters; i++) {
        int floor_z, ceil_z;
        float edge = blast_column_range(x, y, &centers[i], aa, dithering, seed,
                                        &floor_z, &ceil_z);
        if (edge <= 0.f)
            continue;
        any = true;
        if (floor_z < best_floor)
            best_floor = floor_z;
        if (ceil_z > best_ceil)
            best_ceil = ceil_z;
        if (edge > best_edge) {
            best_edge = edge;
            best_str = centers[i].strength;
            best_scorch = centers[i].scorch;
            best_debris = centers[i].debris;
            best_r = centers[i].radius;
            best_depth = centers[i].depth;
            best_i = i;
        }
    }

    if (!any)
        return 0.f;

    if (out_floor_z)
        *out_floor_z = best_floor;
    if (out_ceil_z)
        *out_ceil_z = best_ceil;
    if (out_strength)
        *out_strength = best_str;
    if (out_scorch)
        *out_scorch = best_scorch;
    if (out_debris)
        *out_debris = best_debris;
    if (out_radius)
        *out_radius = best_r;
    if (out_depth)
        *out_depth = best_depth;
    if (out_nearest)
        *out_nearest = best_i;
    return best_edge;
}

/*
 * Half-radius core mark independent of Scorch: soft ~80% toward black with
 * anti-alias + dither so it blends rather than punching a hard stain.
 */
static float core_black_amount(int x, int y, const explosion_center_t *c,
                               float aa, float dithering, int seed)
{
    float dist = warped_blast_dist(x, y, c, seed);
    float half = warped_blast_radius(x, y, c, seed) * 0.5f;
    float k, cover, t;

    if (half < 0.5f)
        half = 0.5f;

    k = half - dist;
    if (dithering > 0.f) {
        float n = uniform_noise((float)x + (float)seed * 0.023f,
                                (float)y + 17.f,
                                (float)seed * 0.41f);
        k += (n * 2.f - 1.f) * (dithering * 0.28f);
    }
    if (aa > 0.f)
        cover = clamp(k / aa, -1.f, 1.f) / 2.f + 0.5f;
    else
        cover = (k >= 0.f) ? 1.f : 0.f;
    if (cover <= 0.f)
        return 0.f;

    t = 1.f - clamp(dist / half, 0.f, 1.f);
    return 0.80f * cover * (0.75f + 0.25f * t);
}

/*
 * Crush terrain colour toward black (keep relative channel ratios / hue).
 * amount in [0,1]; light per-voxel luminance noise so scorches are not flat.
 */
static void darken_toward_black(uint8_t color[4], float amount,
                                int x, int y, int z, int seed)
{
    float a, n;

    if (amount <= 0.f || !color[3])
        return;
    n = 0.90f + 0.20f * hash01(x, y, z, seed + 61);
    a = clamp(amount * n, 0.f, 1.f);
    color[0] = (uint8_t)clampi((int)((float)color[0] * (1.f - a) + 0.5f), 0, 255);
    color[1] = (uint8_t)clampi((int)((float)color[1] * (1.f - a) + 0.5f), 0, 255);
    color[2] = (uint8_t)clampi((int)((float)color[2] * (1.f - a) + 0.5f), 0, 255);
}

static void mix_color_toward(uint8_t dst[4], const uint8_t src[4], float t)
{
    t = clamp(t, 0.f, 1.f);
    if (!dst[3] || t <= 0.f)
        return;
    dst[0] = (uint8_t)clampi((int)((1.f - t) * dst[0] + t * src[0] + 0.5f), 0, 255);
    dst[1] = (uint8_t)clampi((int)((1.f - t) * dst[1] + t * src[1] + 0.5f), 0, 255);
    dst[2] = (uint8_t)clampi((int)((1.f - t) * dst[2] + t * src[2] + 0.5f), 0, 255);
}

/*
 * Influence past the soft blast edge for staining surroundings.
 * 1 at/inside radius, fades to 0 across `reach` blocks beyond.
 */
static float outer_bleed_cover(float dist, float radius, float aa,
                               float dithering, float reach,
                               int x, int y, int seed)
{
    float k;
    float outer = radius + fmaxf(reach, 0.f);

    if (dist > outer + aa + dithering)
        return 0.f;

    k = outer - dist;
    if (dithering > 0.f) {
        float n = uniform_noise((float)x + (float)seed * 0.029f,
                                (float)y - 9.f,
                                (float)seed * 0.37f);
        k += (n * 2.f - 1.f) * (dithering * 0.25f);
    }
    if (aa > 0.f)
        return clamp(k / fmaxf(aa + reach * 0.35f, 1.f), -1.f, 1.f) / 2.f + 0.5f;
    return (k >= 0.f) ? 1.f : 0.f;
}

/* Strongest surrounding-bleed cover across all blast centres. */
static float max_outer_bleed(int x, int y, const explosion_center_t *centers,
                             int ncenters, float aa, float dithering,
                             float reach, int seed, float *out_scorch,
                             float *out_depth, int *out_nearest)
{
    float best = 0.f;
    float best_scorch = 0.f;
    float best_depth = 1.f;
    int best_i = 0;
    int i;

    for (i = 0; i < ncenters; i++) {
        float dist = warped_blast_dist(x, y, &centers[i], seed);
        float radius = warped_blast_radius(x, y, &centers[i], seed);
        float cover = outer_bleed_cover(dist, radius, aa, dithering,
                                        reach, x, y, seed);
        if (cover > best) {
            best = cover;
            best_scorch = centers[i].scorch;
            best_depth = centers[i].depth;
            best_i = i;
        }
    }
    if (out_scorch)
        *out_scorch = best_scorch;
    if (out_depth)
        *out_depth = best_depth;
    if (out_nearest)
        *out_nearest = best_i;
    return best;
}

/* Tint solid neighbours toward the debris colour (stain / smear). */
static void bleed_debris_color(volume_t *vol, int x, int y, int z,
                               const uint8_t src[4], float amount, int seed)
{
    static const int offs[][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
        {1, 1, 0}, {1, -1, 0}, {-1, 1, 0}, {-1, -1, 0},
        {1, 0, -1}, {-1, 0, -1}, {0, 1, -1}, {0, -1, -1},
        {2, 0, -1}, {-2, 0, -1}, {0, 2, -1}, {0, -2, -1},
        {1, 1, -1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, -1},
        {0, 0, -2}, {1, 0, -2}, {-1, 0, -2}, {0, 1, -2}, {0, -1, -2},
        {0, 0, 2}, {1, 0, 1}, {-1, 0, 1}, {0, 1, 1}, {0, -1, 1},
    };
    int i;

    if (amount <= 0.f || !src[3])
        return;

    for (i = 0; i < (int)(sizeof(offs) / sizeof(offs[0])); i++) {
        int pos[3] = {x + offs[i][0], y + offs[i][1], z + offs[i][2]};
        uint8_t col[4];
        float falloff;
        float t;
        int manhattan;

        volume_get_at(vol, NULL, pos, col);
        if (!col[3])
            continue;
        manhattan = abs(offs[i][0]) + abs(offs[i][1]) + abs(offs[i][2]);
        falloff = (offs[i][2] < 0) ? 1.f :
                  (manhattan <= 1) ? 0.9f :
                  (manhattan == 2) ? 0.6f : 0.4f;
        t = amount * falloff *
            (0.75f + 0.25f * hash01(pos[0], pos[1], pos[2], seed + 71));
        mix_color_toward(col, src, t);
        volume_set_at(vol, NULL, pos, col);
    }
}

/* Highest solid at or below z_from in this column, or INT_MIN if none. */
static int find_top_solid_z(const volume_t *vol, int x, int y,
                            int z_from, int z_min)
{
    int pos[3] = {x, y, z_from};
    uint8_t c[4];

    if (z_from < z_min)
        return INT_MIN;
    for (; pos[2] >= z_min; pos[2]--) {
        volume_get_at(vol, NULL, pos, c);
        if (c[3])
            return pos[2];
    }
    return INT_MIN;
}

/*
 * Drop debris onto the highest solid under (x,y). Writes landing z into
 * out_z. Returns false if the column has no ground or the landing cell is
 * already occupied.
 */
static bool settle_debris_onto_surface(const volume_t *vol, int x, int y,
                                       int z_hint, int z_min, int z_max,
                                       int *out_z)
{
    int start = z_hint;
    int support;
    int land;
    int pos[3];
    uint8_t c[4];

    if (start > z_max)
        start = z_max;
    if (start < z_min)
        start = z_min;

    support = find_top_solid_z(vol, x, y, start, z_min);
    if (support == INT_MIN)
        support = find_top_solid_z(vol, x, y, z_max, z_min);
    if (support == INT_MIN)
        return false;

    land = support + 1;
    pos[0] = x;
    pos[1] = y;
    pos[2] = land;
    volume_get_at(vol, NULL, pos, c);
    if (c[3])
        return false;
    *out_z = land;
    return true;
}

static void apply_explosions(filter_explosions_t *filter, layer_t *plan_layer)
{
    explosion_center_t *plan = NULL;
    int nplan = 0;
    layer_t *target;
    volume_t *work = NULL;
    volume_t *debris = NULL;
    int seed;
    float aa, dither;
    float bleed;
    int i, x, y, z;
    int minx, maxx, miny, maxy;
    int bbox[2][3];
    int margin;
    float max_r;
    const uint8_t empty[4] = {0, 0, 0, 0};

    if (!plan_layer || !plan_layer->volume ||
        volume_is_empty(plan_layer->volume)) {
        gui_alert("Plan - Explosions", "Active layer has no voxels.");
        return;
    }

    target = find_layer_by_id(filter->target_layer_id);
    if (!target || !target->volume) {
        gui_alert("Plan - Explosions", "Select a terrain layer to blast.");
        return;
    }
    if (target == plan_layer) {
        gui_alert("Plan - Explosions",
                  "Choose a different layer than the active plan layer.");
        return;
    }
    if (volume_is_empty(target->volume)) {
        gui_alert("Plan - Explosions", "Target layer is empty.");
        return;
    }

    if (!collect_plan_voxels(plan_layer->volume, &plan, &nplan) || nplan == 0) {
        free(plan);
        return;
    }
    collapse_plan_xy(plan, &nplan);
    if (filter->use_layer_heights) {
        project_plan_voxels(plan, &nplan, target->volume);
        if (nplan == 0) {
            gui_alert("Plan - Explosions",
                      "No plan columns intersect the selected layer.");
            free(plan);
            return;
        }
    }

    assign_blast_params(plan, nplan, filter);

    aa = (float)clampi(filter->anti_alias, 0, 16);
    dither = clamp(filter->dithering, 0.f, 16.f);
    seed = filter->seed;
    bleed = (float)clampi(filter->color_bleed, 0, 100) / 100.f;

    max_r = plan[0].radius;
    for (i = 1; i < nplan; i++) {
        if (plan[i].radius > max_r)
            max_r = plan[i].radius;
    }
    margin = (int)ceilf(max_r + aa + dither) + 4;

    minx = maxx = plan[0].x;
    miny = maxy = plan[0].y;
    for (i = 1; i < nplan; i++) {
        if (plan[i].x < minx) minx = plan[i].x;
        if (plan[i].x > maxx) maxx = plan[i].x;
        if (plan[i].y < miny) miny = plan[i].y;
        if (plan[i].y > maxy) maxy = plan[i].y;
    }
    minx -= margin;
    maxx += margin;
    miny -= margin;
    maxy += margin;

    if (!volume_get_bbox(target->volume, bbox, true)) {
        free(plan);
        return;
    }

    image_history_push(goxel.image);
    /* Hide the plan layer as part of the same undo step. */
    plan_layer->visible = false;

    work = volume_copy(target->volume);
    if (!work)
        goto end;
    debris = volume_new();
    if (!debris)
        goto end;

    /* Pass 1: carve bowl + upper hemisphere, collect debris. */
    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            int floor_z, ceil_z;
            float edge;
            float strength;
            float scorch;
            float debris_amt;
            float blast_r;
            float blast_depth;
            int nearest;
            int pos[3];
            uint8_t col[4];
            int z_lo, z_hi;
            int soft_pad;
            int depth_i;
            int scorch_lo, scorch_hi;

            edge = max_blast_range(x, y, plan, nplan, aa, dither, seed,
                                   &floor_z, &ceil_z, &strength, &scorch,
                                   &debris_amt, &blast_r, &blast_depth,
                                   &nearest);
            /* Continuous soft threshold — no per-column random skips (those
             * read as dotted holes around the blast). */
            if (edge <= 0.08f)
                continue;

            /*
             * Soft lip shelf: near the AA rim leave uppermost blocks so the
             * wall eases into surrounding terrain instead of a hard cut.
             */
            soft_pad = (int)floorf((1.f - fade(edge)) * fmaxf(aa * 0.55f, 1.2f));
            z_lo = floor_z + 1 + soft_pad;
            z_hi = ceil_z;
            if (z_lo < bbox[0][2])
                z_lo = bbox[0][2];
            if (z_hi > bbox[1][2] - 1)
                z_hi = bbox[1][2] - 1;

            for (z = z_hi; z >= z_lo; z--) {
                pos[0] = x;
                pos[1] = y;
                pos[2] = z;
                volume_get_at(work, NULL, pos, col);
                if (!col[3])
                    continue;

                if (debris_amt > 0.001f) {
                    float p = debris_amt * (0.15f + 0.55f * edge);
                    if (hash01(x, y, z, seed + 7) < p) {
                        float ndx, ndy, len;
                        int ox, oy, oz;
                        int dpos[3];
                        uint8_t dcol[4];

                        ndx = (float)(x - plan[nearest].x);
                        ndy = (float)(y - plan[nearest].y);
                        len = sqrtf(ndx * ndx + ndy * ndy);
                        if (len < 0.01f) {
                            ndx = hash01(x, y, z, seed + 11) * 2.f - 1.f;
                            ndy = hash01(x, y, z, seed + 13) * 2.f - 1.f;
                            len = sqrtf(ndx * ndx + ndy * ndy);
                        }
                        if (len > 0.01f) {
                            ndx /= len;
                            ndy /= len;
                        }
                        {
                            float fling = blast_r * (0.35f + 0.5f * strength +
                                0.9f * hash01(x, y, z, seed + 17));
                            ox = x + (int)floorf(ndx * fling +
                                (hash01(x, y, z, seed + 19) * 2.f - 1.f) * 2.f);
                            oy = y + (int)floorf(ndy * fling +
                                (hash01(x, y, z, seed + 23) * 2.f - 1.f) * 2.f);
                            /* Keep a hint Z near the source; settle later. */
                            oz = z;
                        }
                        memcpy(dcol, col, 4);
                        darken_toward_black(dcol, scorch * (0.55f + 0.35f * edge),
                                            ox, oy, oz, seed);
                        dpos[0] = ox;
                        dpos[1] = oy;
                        dpos[2] = oz;
                        volume_set_at(debris, NULL, dpos, dcol);
                    }
                }

                volume_set_at(work, NULL, pos, empty);
            }

            /*
             * Scorch remaining solids from the crater floor downward by the
             * full blast depth, plus a short band on walls / lip above floor.
             */
            depth_i = (int)ceilf(fmaxf(blast_depth, 1.f));
            scorch_lo = floor_z - depth_i;
            scorch_hi = floor_z + 2 + soft_pad;
            if (scorch_hi > ceil_z + 1)
                scorch_hi = ceil_z + 1;
            if (scorch_lo < bbox[0][2])
                scorch_lo = bbox[0][2];
            if (scorch_hi > bbox[1][2] - 1)
                scorch_hi = bbox[1][2] - 1;

            for (z = scorch_lo; z <= scorch_hi; z++) {
                float amt;
                float core;
                float depth_w;
                int below;

                pos[0] = x;
                pos[1] = y;
                pos[2] = z;
                volume_get_at(work, NULL, pos, col);
                if (!col[3])
                    continue;

                below = floor_z - z;
                if (below >= 0) {
                    /* Strongest at floor, fades across full blast depth. */
                    depth_w = 1.f - clamp((float)below / (float)depth_i, 0.f, 1.f);
                    depth_w = depth_w * depth_w * (3.f - 2.f * depth_w);
                } else {
                    /* Short fade onto remaining lip / wall above floor. */
                    depth_w = 1.f - clamp((float)(-below) / 3.f, 0.f, 1.f);
                }

                amt = scorch * (0.65f + 0.35f * clamp(edge, 0.f, 1.f)) * depth_w;
                if (z == floor_z)
                    amt = fmaxf(amt, scorch * 0.9f);
                core = core_black_amount(x, y, &plan[nearest], aa, dither, seed);
                if (below >= 0 && below <= depth_i)
                    amt = fmaxf(amt, core * depth_w);
                darken_toward_black(col, amt, x, y, z, seed);
                volume_set_at(work, NULL, pos, col);
            }
        }
    }

    /*
     * Pass 1b: bleed scorched tint into surrounding terrain (radial + down
     * by blast depth). Driven by scorch; colour bleed boosts reach/intensity.
     */
    {
        float max_scorch = 0.f;
        float max_depth = 1.f;
        for (i = 0; i < nplan; i++) {
            if (plan[i].scorch > max_scorch)
                max_scorch = plan[i].scorch;
            if (plan[i].depth > max_depth)
                max_depth = plan[i].depth;
        }

        if (max_scorch > 0.001f || bleed > 0.001f) {
            float scorch_drive = fmaxf(max_scorch, bleed);
            float reach = 3.f + scorch_drive * 8.f + bleed * 4.f;
            int bleed_margin = (int)ceilf(max_r + aa + dither + reach) + 2;
            int bx0 = plan[0].x, bx1 = plan[0].x;
            int by0 = plan[0].y, by1 = plan[0].y;

            for (i = 1; i < nplan; i++) {
                if (plan[i].x < bx0) bx0 = plan[i].x;
                if (plan[i].x > bx1) bx1 = plan[i].x;
                if (plan[i].y < by0) by0 = plan[i].y;
                if (plan[i].y > by1) by1 = plan[i].y;
            }
            bx0 -= bleed_margin;
            bx1 += bleed_margin;
            by0 -= bleed_margin;
            by1 += bleed_margin;

            for (y = by0; y <= by1; y++) {
                for (x = bx0; x <= bx1; x++) {
                    float cover;
                    float scorch;
                    float blast_depth;
                    int nearest;
                    int pos[3];
                    uint8_t col[4];
                    int top_z;
                    int z_lo, z_hi;
                    int depth_i;
                    float depth_span;

                    cover = max_outer_bleed(x, y, plan, nplan, aa, dither, reach,
                                            seed, &scorch, &blast_depth, &nearest);
                    if (cover <= 0.03f)
                        continue;

                    top_z = find_top_solid_z(work, x, y, bbox[1][2] - 1, bbox[0][2]);
                    if (top_z == INT_MIN)
                        continue;

                    depth_i = (int)ceilf(fmaxf(blast_depth, 1.f));
                    /* Down by full blast depth; bleed can push a little further. */
                    depth_span = (float)depth_i + bleed * 3.f;
                    z_hi = top_z + 1;
                    z_lo = top_z - (int)ceilf(depth_span);
                    if (z_lo < bbox[0][2])
                        z_lo = bbox[0][2];
                    if (z_hi > bbox[1][2] - 1)
                        z_hi = bbox[1][2] - 1;

                    for (z = z_lo; z <= z_hi; z++) {
                        float depth_fall;
                        float amt;
                        int dz;

                        pos[0] = x;
                        pos[1] = y;
                        pos[2] = z;
                        volume_get_at(work, NULL, pos, col);
                        if (!col[3])
                            continue;
                        dz = top_z - z;
                        if (dz < 0)
                            depth_fall = 1.f - clamp((float)(-dz) / 2.f, 0.f, 1.f);
                        else {
                            depth_fall = 1.f - clamp((float)dz / fmaxf(depth_span, 1.f),
                                                     0.f, 1.f);
                            depth_fall = depth_fall * depth_fall *
                                         (3.f - 2.f * depth_fall);
                        }
                        /* Scorch stains surrounding terrain; bleed boosts it. */
                        amt = scorch * (0.55f + 0.55f * bleed) * cover * depth_fall *
                              (0.94f + 0.08f * hash01(x, y, z, seed + 151));
                        /* Favour sidewalls / rim: slightly stronger mid-cover. */
                        if (cover > 0.2f && cover < 0.85f)
                            amt *= 1.2f;
                        darken_toward_black(col, clamp(amt, 0.f, 0.95f),
                                            x, y, z, seed);
                        volume_set_at(work, NULL, pos, col);
                    }
                }
            }
        }
    }

    /* Rim displacement removed — it produced dotted blocks around the blast. */

    /* Settle debris onto terrain, then bleed colour into neighbours. */
    {
        volume_iterator_t iter;
        int pos[3];
        uint8_t dcol[4];
        int z_min = bbox[0][2];
        int z_max = bbox[1][2] - 1;

        iter = volume_get_iterator(debris,
                                   VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
        while (volume_iter(&iter, pos)) {
            int land_z;
            int land[3];

            volume_get_at(debris, &iter, pos, dcol);
            if (!dcol[3])
                continue;
            if (!settle_debris_onto_surface(work, pos[0], pos[1], pos[2],
                                            z_min, z_max, &land_z))
                continue;
            land[0] = pos[0];
            land[1] = pos[1];
            land[2] = land_z;
            volume_set_at(work, NULL, land, dcol);
            if (bleed > 0.f) {
                /* Strong stain on the support block under the debris. */
                int under[3] = {land[0], land[1], land[2] - 1};
                uint8_t base[4];
                volume_get_at(work, NULL, under, base);
                if (base[3]) {
                    mix_color_toward(base, dcol, bleed * 0.95f);
                    volume_set_at(work, NULL, under, base);
                }
                bleed_debris_color(work, land[0], land[1], land[2], dcol,
                                   bleed * 1.15f, seed);
            }
        }
    }

    volume_set(target->volume, work);

end:
    if (work)
        volume_delete(work);
    if (debris)
        volume_delete(debris);
    free(plan);
}

static int gui(filter_t *filter_)
{
    filter_explosions_t *filter = (void *)filter_;
    layer_t *layer = goxel.image ? goxel.image->active_layer : NULL;
    layer_t *target_layer;
    const char *help_text =
        "Uses blocks on the active layer as blast centres.  With Position "
        "using layer heights (default), each plan column drops to the nearest "
        "occupied voxel beneath it on the chosen terrain layer; when off, "
        "blasts sit at the plan blocks' own Z.  A bowl crater plus upper "
        "hemisphere is carved - clearing blocks above that point.  Scorch "
        "darkens remaining solids downward by the blast depth and bleeds into "
        "surrounding terrain.  Soft anti-aliased / dithered lips blend into "
        "the surroundings, with optional debris that stains neighbours.  Each "
        "blast rolls its own radius, depth, strength and scorch within the "
        "min-max ranges.";

    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_text_wrapped(
        "WARNING: This permanently edits the chosen terrain layer "
        "(undoable). It is a destructive action.");

    gui_label_size_push(120.0f);

    gui_group_begin("Size");
    gui_input_int("Min radius", &filter->min_radius, 1, 64);
    gui_input_int("Max radius", &filter->max_radius, 1, 64);
    gui_input_int("Min depth", &filter->min_depth, 1, 64);
    gui_input_int("Max depth", &filter->max_depth, 1, 64);
    gui_group_end();

    gui_group_begin("Blast");
    gui_input_int("Min strength", &filter->min_strength, 0, 100);
    gui_tooltip_if_hovered(
        "Lower end of per-blast debris fling distance.");
    gui_input_int("Max strength", &filter->max_strength, 0, 100);
    gui_input_int("Min scorch", &filter->min_scorch, 0, 100);
    gui_tooltip_if_hovered(
        "Lower end of how hard crater / debris colours crush toward black. "
        "Applied downward through the blast depth under the crater floor, "
        "and bled into surrounding terrain. A darker half-radius core is "
        "always applied on top of this.");
    gui_input_int("Max scorch", &filter->max_scorch, 0, 100);
    gui_input_int("Min debris", &filter->min_debris, 0, 100);
    gui_tooltip_if_hovered(
        "Lower end of how densely carved voxels are flung as debris. 0 = none.");
    gui_input_int("Max debris", &filter->max_debris, 0, 100);
    gui_group_end();

    gui_input_int("Anti-alias", &filter->anti_alias, 0, 16);
    gui_input_float("Dithering", &filter->dithering, 0.1f, 0.f, 16.f, "%.1f");
    gui_tooltip_if_hovered(
        "Softens colour / edge blends lightly. Keep low to avoid speckled rims.");

    if (filter->min_scorch > 0 || filter->max_scorch > 0 ||
        filter->min_debris > 0 || filter->max_debris > 0) {
        gui_input_int("Colour bleed", &filter->color_bleed, 0, 100);
        gui_tooltip_if_hovered(
            "Boosts how far / hard scorch stains surrounding terrain "
            "(radial + down by blast depth) and how hard settled debris "
            "stains neighbours. Scorch still bleeds a base amount when > 0.");
    }

    gui_input_int("Seed", &filter->seed, 0, RAND_MAX);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        filter->seed = rand();
    }

    gui_checkbox("Position using layer heights", &filter->use_layer_heights,
                 "When enabled, drop each plan column to the nearest occupied "
                 "height on the terrain layer below (ignore plan Z). When off, "
                 "explode at each plan block's XYZ.");

    target_layer = find_layer_by_id(filter->target_layer_id);
    if (!target_layer && goxel.image) {
        target_layer = goxel.image->layers;
        filter->target_layer_id = target_layer ? target_layer->id : 0;
    }
    gui_text("Terrain layer");
    gui_same_line();
    if (gui_combo_begin("##explosions_target_layer",
                        target_layer ? target_layer->name : "(none)")) {
        layer_t *cur;
        DL_FOREACH_REVERSE(goxel.image->layers, cur) {
            if (gui_combo_item(cur->name, cur == target_layer))
                filter->target_layer_id = cur->id;
        }
        gui_combo_end();
    }
    gui_tooltip_if_hovered(
        filter->use_layer_heights
            ? "Layer whose heights under the plan blocks are used as blast "
              "centres, then carved and darkened in place."
            : "Layer that will be carved and darkened in place.");

    gui_label_size_pop();

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    {
        bool has_layer = goxel.image && goxel.image->active_layer;
        bool ready = has_layer && find_layer_by_id(filter->target_layer_id);

        gui_enabled_begin(ready);
        if (gui_button("Detonate", -1, 0))
            apply_explosions(filter, layer);
        gui_enabled_end();
        gui_alert_if_disabled_clicked(ready, "Cannot detonate",
                                      has_layer
                                          ? "Select a terrain layer first."
                                          : "Select a plan layer first.");
    }

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_explosions_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(explosions, filter_explosions_t,
                .name = "Explosions",
                .menu = "effects",
                .submenu = "plan",
                .on_open = on_open,
                .panel_width = 300,
                .gui_fn = gui, )
