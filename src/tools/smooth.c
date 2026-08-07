/* Goxel 3D voxels editor
 *
 * Smooth brush: 2D top-surface heightfield, or 3D volume morphology.
 * In Paint mode, only neighbour colour blending (occupancy unchanged).
 */

#include "goxel.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    SMOOTH_MODE_2D = 0, /* Absolute column-top heightfield. */
    SMOOTH_MODE_3D = 1, /* Morphological smooth inside X/Y/Z ellipsoid. */
};

enum {
    SMOOTH_EMPTY_CREATE_WITHIN = 0, /* Empty XY may fill toward neighbour avg. */
    SMOOTH_EMPTY_HARD_CUT = 1,      /* Empty stays empty; solids can fall. */
};

typedef struct {
    tool_t tool;

    volume_t *volume_orig;
    float last_pos[3];
    int mode; /* SMOOTH_MODE_* */
    float strength; /* 0..1 how far each dab moves toward the local average. */
    bool add_noise; /* Master switch for Perlin; hides noise fields when off. */
    float noise;    /* 0..1 Perlin warp / strength mod / residual detail. */
    float noise_scale; /* Spatial frequency multiplier (1 = ~1 cell per diameter). */
    /* When true, keep a fixed Perlin seed across strokes. */
    bool lock_noise;
    /* SMOOTH_EMPTY_*: how empty XY columns participate as dab targets (2D). */
    int empty_column;
    /* Paint-mode only: brush-style dither + random colour noise (local). */
    float paint_dithering;
    bool paint_noise_enabled;
    int paint_noise_intensity;
    int paint_noise_saturation;
    int paint_noise_coverage;
    unsigned stroke_seed;
    bool defaults_inited;

    struct {
        float    pos[3];
        bool     pressed;
        uint64_t volume_key;
        int      mode;
        int      painter_mode; /* MODE_OVER / MODE_SUB / MODE_PAINT */
        float    radius_x, radius_y, radius_z;
        float    strength;
        bool     add_noise;
        float    noise;
        float    noise_scale;
        bool     lock_noise;
        int      empty_column;
        float    paint_dithering;
        bool     paint_noise_enabled;
        int      paint_noise_intensity;
        int      paint_noise_saturation;
        int      paint_noise_coverage;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;
} tool_smooth_t;

/* Shared 2D Perlin: utils/noise.h (perlin2 / perlin2_init_seed). */
#define SMOOTH_NOISE_SEED_FIXED 0x51a007u

static void smooth_noise_init(void)
{
    /* Only seed if never inited — do not clobber a stroke-specific seed. */
    perlin2_ensure_seeded(SMOOTH_NOISE_SEED_FIXED);
}

static void ensure_defaults(tool_smooth_t *sm)
{
    if (sm->defaults_inited) return;
    sm->mode = SMOOTH_MODE_2D;
    sm->strength = 0.85f;
    sm->add_noise = true;
    sm->noise = 0.5f;
    sm->noise_scale = 2.f;
    sm->lock_noise = false;
    sm->empty_column = SMOOTH_EMPTY_CREATE_WITHIN;
    /* Soft colour-noise defaults. */
    sm->paint_dithering = 1.f;
    sm->paint_noise_enabled = true;
    sm->paint_noise_intensity = 20;
    sm->paint_noise_saturation = 10;
    sm->paint_noise_coverage = 100;
    sm->stroke_seed = SMOOTH_NOISE_SEED_FIXED;
    sm->defaults_inited = true;
    perlin2_init_seed(SMOOTH_NOISE_SEED_FIXED);
}

/* Wrap voxel coords the same way volume_op feeds uniform_noise. */
static float smooth_noise_coord(int w)
{
    int m = w % NOISE_TEXTURE_SIZE;
    if (m < 0)
        m += NOISE_TEXTURE_SIZE;
    return (float)m;
}

/* Brush-style random colour noise on an RGB result (alpha unchanged).
 * Intensity/saturation UI is remapped softer than the brush so low values
 * stay subtle (smooth dabs revisit voxels; a linear 0..100% mix reads harsh). */
static void smooth_apply_paint_noise(uint8_t col[4], int x, int y, int z,
                                     bool enabled, int intensity,
                                     int saturation, int coverage)
{
    float noise_value;
    float i_ui, s_ui, i_eff, s_eff;
    int noise_col[3];

    if (!enabled || intensity == 0 || coverage == 0)
        return;
    noise_value = uniform_noise(smooth_noise_coord(x),
                                smooth_noise_coord(y),
                                smooth_noise_coord(z));
    if (noise_value >= (float)coverage / 100.0f)
        return;

    i_ui = clamp((float)intensity, 0.f, 100.f) / 100.f;
    s_ui = clamp((float)saturation, 0.f, 100.f) / 100.f;
    /* Quadratic, and capped: UI 1≈0.25%, 10≈2.5%, 50≈6%, 100≈25%. */
    i_eff = i_ui * i_ui * 25.f;
    /* Saturation chroma is also milder than brush (max ~40%). */
    s_eff = s_ui * s_ui * 40.f;

    noise_col[0] = col[0];
    noise_col[1] = col[1];
    noise_col[2] = col[2];
    blend_with_noise_alpha(noise_col, noise_value, i_eff, s_eff, noise_col);
    col[0] = (uint8_t)clamp(noise_col[0], 0, 255);
    col[1] = (uint8_t)clamp(noise_col[1], 0, 255);
    col[2] = (uint8_t)clamp(noise_col[2], 0, 255);
}

static void get_brush_box(const float p[3], float r_x, float r_y, float r_z,
                          float out[4][4])
{
    float box[4][4];
    bbox_from_extents(box, p, r_x, r_y, r_z);
    box_swap_axis(box, 2, 0, 1, box);
    mat4_copy(box, out);
}

/*
 * Fast e^x for Gaussian weights / falloff (x typically in [-8, 0]).
 * (1 + x/256)^256 via eight squares; relative error ~1% on that range,
 * and far cheaper than libm expf.
 */
static float smooth_fast_exp(float x)
{
    float y;

    if (x <= -16.f)
        return 0.f;
    if (x >= 0.f)
        return 1.f + x; /* callers pass x <= 0; keep positive tiny-safe */

    y = 1.f + x * (1.f / 256.f);
    y *= y; y *= y; y *= y; y *= y;
    y *= y; y *= y; y *= y; y *= y;
    return y;
}

/* Absolute world-Z top of column (x,y), or INT_MIN if empty. */
static int column_top_z(const volume_t *volume, volume_accessor_t *acc,
                        int x, int y, int z_lo, int z_hi)
{
    int pos[3];
    uint8_t c[4];
    int z;

    pos[0] = x;
    pos[1] = y;
    for (z = z_hi; z >= z_lo; z--) {
        pos[2] = z;
        volume_get_at(volume, acc, pos, c);
        if (c[3])
            return z;
    }
    return INT_MIN;
}

/*
 * Raise/lower a solid column top, or (when old_z is INT_MIN and grow_color is
 * set) fill an empty column from fill_lo through new_z.
 */
static void set_column_top(volume_t *volume, volume_accessor_t *acc,
                           int x, int y, int old_z, int new_z,
                           int fill_lo, const uint8_t *grow_color)
{
    int pos[3];
    uint8_t color[4];
    uint8_t empty[4] = {0, 0, 0, 0};
    int z;

    pos[0] = x;
    pos[1] = y;

    if (old_z == INT_MIN) {
        if (!grow_color || !grow_color[3] || new_z < fill_lo)
            return;
        for (z = fill_lo; z <= new_z; z++) {
            pos[2] = z;
            volume_set_at(volume, acc, pos, grow_color);
        }
        return;
    }

    if (new_z == old_z)
        return;

    pos[2] = old_z;
    volume_get_at(volume, acc, pos, color);
    if (!color[3])
        return;

    if (new_z > old_z) {
        for (z = old_z + 1; z <= new_z; z++) {
            pos[2] = z;
            volume_set_at(volume, acc, pos, color);
        }
    } else {
        for (z = old_z; z > new_z; z--) {
            pos[2] = z;
            volume_set_at(volume, acc, pos, empty);
        }
    }
}

/*
 * Gaussian-smooth absolute column tops under an elliptical XY brush.
 * Neighbourhood and falloff both scale with brush radii.
 * noise (0..1): world-space Perlin warps the kernel, modulates blend strength,
 * and reintroduces coherent residual detail.
 * noise_scale: multiplies Perlin frequency (1 = ~one cell per brush diameter).
 * empty_column: SMOOTH_EMPTY_* - whether empty XY targets may be filled.
 */
static void smooth_dab_2d(volume_t *volume, int cx, int cy,
                          float r_x, float r_y, float strength, float noise,
                          float noise_scale, int empty_column)
{
    int margin, gw, gh, x0, y0;
    int *heights = NULL;
    int *out_h = NULL;
    uint8_t *grow_colors = NULL; /* RGBA per cell; used when growing into empty. */
    int x, y, ix, iy, nx, ny;
    int z_scan_lo, z_scan_hi, z_clamp_lo, z_clamp_hi;
    float box[4][4];
    float vol_box[4][4];
    int bbox[2][3];
    int vol_bbox[2][3];
    float sx, sy, inv_2sx2, inv_2sy2;
    float rx2, ry2;
    float namp, freq_x, freq_y, res_amp, nscale;
    volume_accessor_t acc;

    if (r_x < 0.5f) r_x = 0.5f;
    if (r_y < 0.5f) r_y = 0.5f;
    strength = clamp(strength, 0.f, 1.f);
    namp = clamp(noise, 0.f, 1.f);
    nscale = clamp(noise_scale, 0.05f, 16.f);
    if (strength <= 0.f)
        return;

    smooth_noise_init();
    /* Wavelength follows Diameter X/Y; noise_scale densifies the field. */
    freq_x = nscale / max(2.f, r_x * 2.f);
    freq_y = nscale / max(2.f, r_y * 2.f);
    res_amp = max(1.f, (r_x + r_y) * 0.1f);

    /* Sample past the brush edge so cells near the rim still get neighbours. */
    margin = (int)ceilf(2.f * max(r_x, r_y));
    if (margin < 1) margin = 1;
    x0 = cx - margin;
    y0 = cy - margin;
    gw = 2 * margin + 1;
    gh = 2 * margin + 1;

    heights = malloc((size_t)gw * (size_t)gh * sizeof(*heights));
    out_h = malloc((size_t)gw * (size_t)gh * sizeof(*out_h));
    grow_colors = calloc((size_t)gw * (size_t)gh, 4);
    if (!heights || !out_h || !grow_colors)
        goto done;

    for (iy = 0; iy < gw * gh; iy++) {
        heights[iy] = INT_MIN;
        out_h[iy] = INT_MIN;
    }

    volume_get_box(volume, true, vol_box);
    if (box_is_null(vol_box))
        goto done;
    bbox_to_aabb(vol_box, vol_bbox);
    z_scan_lo = vol_bbox[0][2];
    z_scan_hi = vol_bbox[1][2] - 1;
    if (z_scan_hi < z_scan_lo)
        goto done;

    mat4_copy(goxel.image->box, box);
    if (!box_is_null(box)) {
        bbox_to_aabb(box, bbox);
        z_clamp_lo = bbox[0][2];
        z_clamp_hi = bbox[1][2] - 1;
    } else {
        /* Allow a little headroom above current content when raising. */
        z_clamp_lo = z_scan_lo;
        z_clamp_hi = z_scan_hi + (int)ceilf(max(r_x, r_y)) + 8;
    }

    acc = volume_get_accessor(volume);
    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            x = x0 + ix;
            heights[iy * gw + ix] =
                column_top_z(volume, &acc, x, y, z_scan_lo, z_scan_hi);
        }
    }

    /* σ ≈ radius/2 so the averaging kernel matches brush size. */
    sx = max(0.5f, r_x * 0.5f);
    sy = max(0.5f, r_y * 0.5f);
    inv_2sx2 = 1.f / (2.f * sx * sx);
    inv_2sy2 = 1.f / (2.f * sy * sy);
    rx2 = r_x * r_x;
    ry2 = r_y * r_y;

    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            float dx, dy, d2, falloff, wsum, hsum, avg, t;
            float kox, koy, n_str, n_res, blended;
            float best_w;
            int h0, krad_x, krad_y;
            int best_jx, best_jy;

            x = x0 + ix;
            h0 = heights[iy * gw + ix];
            out_h[iy * gw + ix] = h0;
            if (h0 == INT_MIN && empty_column != SMOOTH_EMPTY_CREATE_WITHIN)
                continue;

            dx = (float)(x - cx);
            dy = (float)(y - cy);
            d2 = (dx * dx) / rx2 + (dy * dy) / ry2;
            if (d2 > 1.f)
                continue;

            /* Soft brush falloff (1 at centre, ~0 at rim). */
            falloff = smooth_fast_exp(-2.f * d2);

            /* Coherent Perlin: warp kernel, modulate strength, residual. */
            kox = 0.f;
            koy = 0.f;
            n_str = 1.f;
            n_res = 0.f;
            if (namp > 0.f) {
                float px = (float)x * freq_x;
                float py = (float)y * freq_y;
                float nx_w = perlin2(px, py);
                float ny_w = perlin2(px + 19.7f, py + 7.3f);
                float ns = perlin2(px + 41.1f, py + 23.9f);
                float nr = perlin2(px + 3.1f, py + 61.4f);

                /* Stronger warp so neighbourhood choice clearly differs. */
                kox = nx_w * namp * sx * 1.75f;
                koy = ny_w * namp * sy * 1.75f;
                /* At noise=1, blend factor spans ~0.1..1 across patches. */
                n_str = (1.f - namp) +
                        namp * (0.1f + 0.9f * (0.5f + 0.5f * ns));
                n_res = nr;
            }

            wsum = 0.f;
            hsum = 0.f;
            best_w = -1.f;
            best_jx = -1;
            best_jy = -1;
            krad_x = (int)ceilf(2.f * sx + fabsf(kox));
            krad_y = (int)ceilf(2.f * sy + fabsf(koy));
            {
                float w_solid = 0.f, hsum_solid = 0.f, w_empty = 0.f;

                for (ny = -krad_y; ny <= krad_y; ny++) {
                    int jy = iy + ny;
                    if (jy < 0 || jy >= gh) continue;
                    for (nx = -krad_x; nx <= krad_x; nx++) {
                        int jx = ix + nx;
                        int hn;
                        float wx, wy, w;
                        if (jx < 0 || jx >= gw) continue;
                        hn = heights[jy * gw + jx];
                        wx = (float)nx - kox;
                        wy = (float)ny - koy;
                        w = smooth_fast_exp(
                            -(wx * wx * inv_2sx2 + wy * wy * inv_2sy2));
                        if (hn == INT_MIN) {
                            /* Create within: ignore empty. Hard cut:
                             * accumulate empty weight to pull toward floor. */
                            if (empty_column == SMOOTH_EMPTY_HARD_CUT)
                                w_empty += w;
                            continue;
                        }
                        w_solid += w;
                        hsum_solid += w * (float)hn;
                        if (h0 == INT_MIN && w > best_w) {
                            best_w = w;
                            best_jx = jx;
                            best_jy = jy;
                        }
                    }
                }

                if (empty_column == SMOOTH_EMPTY_HARD_CUT &&
                    w_empty > 0.f) {
                    float empty_frac, avg_solid;
                    /* ~50% empty neighbourhood → full floor target so cliff
                     * rims can fall all the way down (plain avg stalls mid). */
                    empty_frac = w_empty / (w_empty + w_solid);
                    if (empty_frac > 0.5f)
                        empty_frac = 1.f;
                    else
                        empty_frac *= 2.f;
                    avg_solid = (w_solid > 0.f) ? (hsum_solid / w_solid)
                                                : (float)z_clamp_lo;
                    avg = avg_solid * (1.f - empty_frac) +
                          (float)z_clamp_lo * empty_frac;
                    wsum = w_solid + w_empty;
                } else {
                    wsum = w_solid;
                    hsum = hsum_solid;
                    if (wsum <= 0.f)
                        continue;
                    avg = hsum / wsum;
                }
            }
            if (wsum <= 0.f)
                continue;

            t = strength * falloff * n_str;
            if (h0 == INT_MIN)
                blended = (float)z_clamp_lo + (avg - (float)z_clamp_lo) * t;
            else
                blended = (float)h0 + (avg - (float)h0) * t;
            /* Residual scales with brush size and Noise. */
            if (namp > 0.f)
                blended += n_res * namp * falloff * res_amp;
            out_h[iy * gw + ix] = (int)roundf(blended);
            if (out_h[iy * gw + ix] < z_clamp_lo)
                out_h[iy * gw + ix] = z_clamp_lo;
            if (out_h[iy * gw + ix] > z_clamp_hi)
                out_h[iy * gw + ix] = z_clamp_hi;

            if (h0 == INT_MIN) {
                uint8_t *gc = grow_colors + (iy * gw + ix) * 4;
                if (best_jx >= 0) {
                    int pos[3];
                    pos[0] = x0 + best_jx;
                    pos[1] = y0 + best_jy;
                    pos[2] = heights[best_jy * gw + best_jx];
                    volume_get_at(volume, &acc, pos, gc);
                }
                if (!gc[3]) {
                    /* Fallback: first solid neighbour in the sample grid. */
                    int fy, fx;
                    for (fy = 0; fy < gh && !gc[3]; fy++) {
                        for (fx = 0; fx < gw; fx++) {
                            int hn = heights[fy * gw + fx];
                            int pos[3];
                            if (hn == INT_MIN) continue;
                            pos[0] = x0 + fx;
                            pos[1] = y0 + fy;
                            pos[2] = hn;
                            volume_get_at(volume, &acc, pos, gc);
                            if (gc[3]) break;
                        }
                    }
                }
                if (!gc[3])
                    out_h[iy * gw + ix] = INT_MIN;
            }
        }
    }

    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            x = x0 + ix;
            if (out_h[iy * gw + ix] == heights[iy * gw + ix])
                continue;
            set_column_top(volume, &acc, x, y,
                           heights[iy * gw + ix], out_h[iy * gw + ix],
                           z_clamp_lo,
                           grow_colors + (iy * gw + ix) * 4);
        }
    }

done:
    free(heights);
    free(out_h);
    free(grow_colors);
}

/*
 * Morphological smooth inside an X/Y/Z ellipsoid centred on (cx, cy, cz).
 * Two-pass: sample neighbour occupancy from the current volume, then apply
 * fills/erases so neighbourhoods stay stable for the dab.
 * Noise (when > 0) modulates strength via world-XY Perlin (no height residual).
 * Returns number of voxels changed.
 */
static int smooth_dab_3d(volume_t *volume, int cx, int cy, int cz,
                         float r_x, float r_y, float r_z,
                         float strength, float noise, float noise_scale)
{
    int x0, y0, z0, gw, gh, gd;
    int x, y, z, ix, iy, iz, nx, ny, nz;
    int *ops = NULL; /* 0=keep, 1=fill, 2=clear */
    uint8_t *fill_colors = NULL;
    float rx2, ry2, rz2;
    float namp, nscale, freq_x, freq_y;
    int changed = 0;
    uint8_t empty[4] = {0, 0, 0, 0};

    if (r_x < 0.5f) r_x = 0.5f;
    if (r_y < 0.5f) r_y = 0.5f;
    if (r_z < 0.5f) r_z = 0.5f;
    strength = clamp(strength, 0.f, 1.f);
    namp = clamp(noise, 0.f, 1.f);
    nscale = clamp(noise_scale, 0.05f, 16.f);
    if (strength <= 0.f)
        return 0;

    smooth_noise_init();
    freq_x = nscale / max(2.f, r_x * 2.f);
    freq_y = nscale / max(2.f, r_y * 2.f);

    x0 = cx - (int)ceilf(r_x);
    y0 = cy - (int)ceilf(r_y);
    z0 = cz - (int)ceilf(r_z);
    gw = 2 * (int)ceilf(r_x) + 1;
    gh = 2 * (int)ceilf(r_y) + 1;
    gd = 2 * (int)ceilf(r_z) + 1;
    if (gw < 1 || gh < 1 || gd < 1)
        return 0;

    ops = calloc((size_t)gw * (size_t)gh * (size_t)gd, sizeof(*ops));
    fill_colors = calloc((size_t)gw * (size_t)gh * (size_t)gd, 4);
    if (!ops || !fill_colors)
        goto done;

    rx2 = r_x * r_x;
    ry2 = r_y * r_y;
    rz2 = r_z * r_z;

    for (iz = 0; iz < gd; iz++) {
        z = z0 + iz;
        for (iy = 0; iy < gh; iy++) {
            y = y0 + iy;
            for (ix = 0; ix < gw; ix++) {
                float dx, dy, dz, d2, falloff, dens, t, n_str;
                int idx, solid_n, total_n, is_solid, want_solid;
                int pos[3];
                uint8_t c[4], best_c[4];
                int best_dist, dist;

                x = x0 + ix;
                dx = (float)(x - cx);
                dy = (float)(y - cy);
                dz = (float)(z - cz);
                d2 = (dx * dx) / rx2 + (dy * dy) / ry2 + (dz * dz) / rz2;
                if (d2 > 1.f)
                    continue;

                falloff = smooth_fast_exp(-2.f * d2);
                n_str = 1.f;
                if (namp > 0.f) {
                    float px = (float)x * freq_x;
                    float py = (float)y * freq_y;
                    float ns = perlin2(px + 41.1f, py + 23.9f);
                    n_str = (1.f - namp) +
                            namp * (0.1f + 0.9f * (0.5f + 0.5f * ns));
                }
                t = strength * falloff * n_str;
                /*
                 * Soft gate via falloff / strength / noise. Do NOT use
                 * occupancy blend crossing 0.5 - that formula can never flip
                 * when t < 0.5, which is common with default noise.
                 */
                if (t < 0.2f)
                    continue;

                pos[0] = x;
                pos[1] = y;
                pos[2] = z;
                /* NULL accessor: brush AABB walks tiles irregularly. */
                volume_get_at(volume, NULL, pos, c);
                is_solid = c[3] != 0;

                solid_n = 0;
                total_n = 0;
                best_c[3] = 0;
                best_dist = INT_MAX;
                for (nz = -1; nz <= 1; nz++) {
                    for (ny = -1; ny <= 1; ny++) {
                        for (nx = -1; nx <= 1; nx++) {
                            uint8_t nc[4];
                            int npos[3];
                            if (nx == 0 && ny == 0 && nz == 0)
                                continue;
                            npos[0] = x + nx;
                            npos[1] = y + ny;
                            npos[2] = z + nz;
                            volume_get_at(volume, NULL, npos, nc);
                            total_n++;
                            if (!nc[3])
                                continue;
                            solid_n++;
                            dist = abs(nx) + abs(ny) + abs(nz);
                            if (dist < best_dist) {
                                best_dist = dist;
                                memcpy(best_c, nc, 4);
                            }
                        }
                    }
                }
                dens = (total_n > 0) ? ((float)solid_n / (float)total_n) : 0.f;
                want_solid = dens >= 0.5f;
                if (is_solid == want_solid)
                    continue;

                idx = (iz * gh + iy) * gw + ix;
                if (is_solid && !want_solid) {
                    ops[idx] = 2; /* clear */
                } else if (!is_solid && want_solid) {
                    if (!best_c[3]) {
                        int fx, fy, fz;
                        for (fz = 0; fz < gd && !best_c[3]; fz++) {
                            for (fy = 0; fy < gh && !best_c[3]; fy++) {
                                for (fx = 0; fx < gw; fx++) {
                                    float fdx, fdy, fdz, fd2;
                                    int fpos[3];
                                    uint8_t fc[4];
                                    fdx = (float)((x0 + fx) - cx);
                                    fdy = (float)((y0 + fy) - cy);
                                    fdz = (float)((z0 + fz) - cz);
                                    fd2 = (fdx * fdx) / rx2 +
                                          (fdy * fdy) / ry2 +
                                          (fdz * fdz) / rz2;
                                    if (fd2 > 1.f) continue;
                                    fpos[0] = x0 + fx;
                                    fpos[1] = y0 + fy;
                                    fpos[2] = z0 + fz;
                                    volume_get_at(volume, NULL, fpos, fc);
                                    if (fc[3]) {
                                        memcpy(best_c, fc, 4);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (best_c[3]) {
                        ops[idx] = 1; /* fill */
                        memcpy(fill_colors + idx * 4, best_c, 4);
                    }
                }
            }
        }
    }

    for (iz = 0; iz < gd; iz++) {
        z = z0 + iz;
        for (iy = 0; iy < gh; iy++) {
            y = y0 + iy;
            for (ix = 0; ix < gw; ix++) {
                int idx = (iz * gh + iy) * gw + ix;
                int pos[3];
                if (!ops[idx])
                    continue;
                pos[0] = x0 + ix;
                pos[1] = y0 + iy;
                pos[2] = z;
                if (ops[idx] == 2)
                    volume_set_at(volume, NULL, pos, empty);
                else
                    volume_set_at(volume, NULL, pos, fill_colors + idx * 4);
                changed++;
            }
        }
    }

done:
    free(ops);
    free(fill_colors);
    return changed;
}

/*
 * Paint mode (2D): Gaussian-blend RGB of absolute column tops only.
 * Occupancy / heights are never changed.
 * dithering scatters brush membership (brush-style); intensity/sat/coverage
 * apply uniform random colour noise after the blend.
 */
static void smooth_dab_2d_color(volume_t *volume, int cx, int cy,
                                float r_x, float r_y, float strength,
                                float dithering,
                                bool noise_enabled, int noise_intensity,
                                int noise_saturation, int noise_coverage)
{
    int margin, gw, gh, x0, y0;
    int *heights = NULL;
    uint8_t *colors = NULL; /* RGBA per sample cell (a=0 if empty). */
    uint8_t *out_c = NULL;
    int x, y, ix, iy, nx, ny;
    int z_scan_lo, z_scan_hi;
    float vol_box[4][4];
    int vol_bbox[2][3];
    float sx, sy, inv_2sx2, inv_2sy2;
    float rx2, ry2, rmin;
    volume_accessor_t acc;

    if (r_x < 0.5f) r_x = 0.5f;
    if (r_y < 0.5f) r_y = 0.5f;
    strength = clamp(strength, 0.f, 1.f);
    dithering = clamp(dithering, 0.f, 16.f);
    if (strength <= 0.f && !noise_enabled)
        return;

    /* Neighbourhood + dither scatter band past the hard ellipse. */
    margin = (int)ceilf(2.f * max(r_x, r_y) + dithering);
    if (margin < 1) margin = 1;
    x0 = cx - margin;
    y0 = cy - margin;
    gw = 2 * margin + 1;
    gh = 2 * margin + 1;

    heights = malloc((size_t)gw * (size_t)gh * sizeof(*heights));
    colors = calloc((size_t)gw * (size_t)gh, 4);
    out_c = calloc((size_t)gw * (size_t)gh, 4);
    if (!heights || !colors || !out_c)
        goto done;

    for (iy = 0; iy < gw * gh; iy++)
        heights[iy] = INT_MIN;

    volume_get_box(volume, true, vol_box);
    if (box_is_null(vol_box))
        goto done;
    bbox_to_aabb(vol_box, vol_bbox);
    z_scan_lo = vol_bbox[0][2];
    z_scan_hi = vol_bbox[1][2] - 1;
    if (z_scan_hi < z_scan_lo)
        goto done;

    acc = volume_get_accessor(volume);
    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            int h;
            uint8_t *c;
            x = x0 + ix;
            h = column_top_z(volume, &acc, x, y, z_scan_lo, z_scan_hi);
            heights[iy * gw + ix] = h;
            if (h == INT_MIN)
                continue;
            c = colors + (iy * gw + ix) * 4;
            {
                int pos[3] = {x, y, h};
                volume_get_at(volume, &acc, pos, c);
            }
            memcpy(out_c + (iy * gw + ix) * 4, c, 4);
        }
    }

    sx = max(0.5f, r_x * 0.5f);
    sy = max(0.5f, r_y * 0.5f);
    inv_2sx2 = 1.f / (2.f * sx * sx);
    inv_2sy2 = 1.f / (2.f * sy * sy);
    rx2 = r_x * r_x;
    ry2 = r_y * r_y;
    rmin = min(r_x, r_y);

    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            float dx, dy, d2, nd, k, falloff, wsum, t;
            float rsum, gsum, bsum;
            int h0, krad_x, krad_y;
            const uint8_t *c0;
            uint8_t *dst;

            x = x0 + ix;
            h0 = heights[iy * gw + ix];
            if (h0 == INT_MIN)
                continue;

            dx = (float)(x - cx);
            dy = (float)(y - cy);
            d2 = (dx * dx) / rx2 + (dy * dy) / ry2;
            nd = sqrtf(d2);
            /* Positive inside ellipse rim, in approximate voxel units. */
            k = (1.f - nd) * rmin;
            if (dithering > 0.f) {
                float n = uniform_noise((float)x, (float)y, (float)h0);
                k += (n * 2.f - 1.f) * dithering;
            }
            if (k < 0.f)
                continue;

            falloff = smooth_fast_exp(-2.f * max(d2, 0.f));

            wsum = 0.f;
            rsum = gsum = bsum = 0.f;
            krad_x = (int)ceilf(2.f * sx);
            krad_y = (int)ceilf(2.f * sy);
            for (ny = -krad_y; ny <= krad_y; ny++) {
                int jy = iy + ny;
                if (jy < 0 || jy >= gh) continue;
                for (nx = -krad_x; nx <= krad_x; nx++) {
                    int jx = ix + nx;
                    const uint8_t *nc;
                    float wx, wy, w;
                    if (jx < 0 || jx >= gw) continue;
                    nc = colors + (jy * gw + jx) * 4;
                    if (!nc[3]) continue;
                    wx = (float)nx;
                    wy = (float)ny;
                    w = smooth_fast_exp(
                        -(wx * wx * inv_2sx2 + wy * wy * inv_2sy2));
                    wsum += w;
                    rsum += w * (float)nc[0];
                    gsum += w * (float)nc[1];
                    bsum += w * (float)nc[2];
                }
            }
            if (wsum <= 0.f)
                continue;

            t = clamp(strength * falloff, 0.f, 1.f);
            c0 = colors + (iy * gw + ix) * 4;
            dst = out_c + (iy * gw + ix) * 4;
            dst[0] = (uint8_t)roundf(
                mix((float)c0[0], rsum / wsum, t));
            dst[1] = (uint8_t)roundf(
                mix((float)c0[1], gsum / wsum, t));
            dst[2] = (uint8_t)roundf(
                mix((float)c0[2], bsum / wsum, t));
            dst[3] = c0[3];
            smooth_apply_paint_noise(dst, x, y, h0,
                                     noise_enabled, noise_intensity,
                                     noise_saturation, noise_coverage);
        }
    }

    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            int h;
            const uint8_t *src, *dst;
            int pos[3];
            x = x0 + ix;
            h = heights[iy * gw + ix];
            if (h == INT_MIN)
                continue;
            src = colors + (iy * gw + ix) * 4;
            dst = out_c + (iy * gw + ix) * 4;
            if (src[0] == dst[0] && src[1] == dst[1] &&
                src[2] == dst[2])
                continue;
            pos[0] = x;
            pos[1] = y;
            pos[2] = h;
            volume_set_at(volume, &acc, pos, dst);
        }
    }

done:
    free(heights);
    free(colors);
    free(out_c);
}

/*
 * Paint mode (3D): blend RGB of solid voxels inside the brush ellipsoid
 * toward a Gaussian neighbourhood average. Never fills or clears.
 * dithering scatters membership; intensity/sat/coverage add brush-style noise.
 * Returns number of voxels whose colour changed.
 */
static int smooth_dab_3d_color(volume_t *volume, int cx, int cy, int cz,
                               float r_x, float r_y, float r_z,
                               float strength, float dithering,
                               bool noise_enabled, int noise_intensity,
                               int noise_saturation, int noise_coverage)
{
    int x0, y0, z0, gw, gh, gd;
    int x, y, z, ix, iy, iz, nx, ny, nz;
    uint8_t *colors = NULL;
    uint8_t *out_c = NULL;
    float rx2, ry2, rz2;
    float sx, sy, sz, inv_2sx2, inv_2sy2, inv_2sz2;
    float rmin;
    int changed = 0;

    if (r_x < 0.5f) r_x = 0.5f;
    if (r_y < 0.5f) r_y = 0.5f;
    if (r_z < 0.5f) r_z = 0.5f;
    strength = clamp(strength, 0.f, 1.f);
    dithering = clamp(dithering, 0.f, 16.f);
    if (strength <= 0.f && !noise_enabled)
        return 0;

    /* Extra margin for Gaussian neighbours and dither scatter. */
    {
        int mx = (int)ceilf(2.f * r_x + dithering);
        int my = (int)ceilf(2.f * r_y + dithering);
        int mz = (int)ceilf(2.f * r_z + dithering);
        if (mx < 1) mx = 1;
        if (my < 1) my = 1;
        if (mz < 1) mz = 1;
        x0 = cx - mx;
        y0 = cy - my;
        z0 = cz - mz;
        gw = 2 * mx + 1;
        gh = 2 * my + 1;
        gd = 2 * mz + 1;
    }
    if (gw < 1 || gh < 1 || gd < 1)
        return 0;

    colors = calloc((size_t)gw * (size_t)gh * (size_t)gd, 4);
    out_c = calloc((size_t)gw * (size_t)gh * (size_t)gd, 4);
    if (!colors || !out_c)
        goto done;

    rx2 = r_x * r_x;
    ry2 = r_y * r_y;
    rz2 = r_z * r_z;
    sx = max(0.5f, r_x * 0.5f);
    sy = max(0.5f, r_y * 0.5f);
    sz = max(0.5f, r_z * 0.5f);
    inv_2sx2 = 1.f / (2.f * sx * sx);
    inv_2sy2 = 1.f / (2.f * sy * sy);
    inv_2sz2 = 1.f / (2.f * sz * sz);
    rmin = min(r_x, min(r_y, r_z));

    for (iz = 0; iz < gd; iz++) {
        z = z0 + iz;
        for (iy = 0; iy < gh; iy++) {
            y = y0 + iy;
            for (ix = 0; ix < gw; ix++) {
                int idx = (iz * gh + iy) * gw + ix;
                int pos[3] = {x0 + ix, y, z};
                uint8_t c[4];
                volume_get_at(volume, NULL, pos, c);
                if (c[3]) {
                    memcpy(colors + idx * 4, c, 4);
                    memcpy(out_c + idx * 4, c, 4);
                }
            }
        }
    }

    for (iz = 0; iz < gd; iz++) {
        z = z0 + iz;
        for (iy = 0; iy < gh; iy++) {
            y = y0 + iy;
            for (ix = 0; ix < gw; ix++) {
                float dx, dy, dz, d2, nd, k, falloff, wsum, t;
                float rsum, gsum, bsum;
                int idx, krad_x, krad_y, krad_z;
                const uint8_t *c0;
                uint8_t *dst;

                x = x0 + ix;
                idx = (iz * gh + iy) * gw + ix;
                c0 = colors + idx * 4;
                if (!c0[3])
                    continue;

                dx = (float)(x - cx);
                dy = (float)(y - cy);
                dz = (float)(z - cz);
                d2 = (dx * dx) / rx2 + (dy * dy) / ry2 + (dz * dz) / rz2;
                nd = sqrtf(d2);
                k = (1.f - nd) * rmin;
                if (dithering > 0.f) {
                    float n = uniform_noise((float)x, (float)y, (float)z);
                    k += (n * 2.f - 1.f) * dithering;
                }
                if (k < 0.f)
                    continue;

                falloff = smooth_fast_exp(-2.f * max(d2, 0.f));
                t = clamp(strength * falloff, 0.f, 1.f);

                wsum = 0.f;
                rsum = gsum = bsum = 0.f;
                krad_x = (int)ceilf(2.f * sx);
                krad_y = (int)ceilf(2.f * sy);
                krad_z = (int)ceilf(2.f * sz);
                for (nz = -krad_z; nz <= krad_z; nz++) {
                    int jz = iz + nz;
                    if (jz < 0 || jz >= gd) continue;
                    for (ny = -krad_y; ny <= krad_y; ny++) {
                        int jy = iy + ny;
                        if (jy < 0 || jy >= gh) continue;
                        for (nx = -krad_x; nx <= krad_x; nx++) {
                            int jx = ix + nx;
                            const uint8_t *nc;
                            float wx, wy, wz, w;
                            if (jx < 0 || jx >= gw) continue;
                            nc = colors + ((jz * gh + jy) * gw + jx) * 4;
                            if (!nc[3]) continue;
                            wx = (float)nx;
                            wy = (float)ny;
                            wz = (float)nz;
                            w = smooth_fast_exp(
                                -(wx * wx * inv_2sx2 +
                                  wy * wy * inv_2sy2 +
                                  wz * wz * inv_2sz2));
                            wsum += w;
                            rsum += w * (float)nc[0];
                            gsum += w * (float)nc[1];
                            bsum += w * (float)nc[2];
                        }
                    }
                }
                if (wsum <= 0.f)
                    continue;

                dst = out_c + idx * 4;
                dst[0] = (uint8_t)roundf(
                    mix((float)c0[0], rsum / wsum, t));
                dst[1] = (uint8_t)roundf(
                    mix((float)c0[1], gsum / wsum, t));
                dst[2] = (uint8_t)roundf(
                    mix((float)c0[2], bsum / wsum, t));
                dst[3] = c0[3];
                smooth_apply_paint_noise(dst, x, y, z,
                                         noise_enabled, noise_intensity,
                                         noise_saturation, noise_coverage);
            }
        }
    }

    for (iz = 0; iz < gd; iz++) {
        z = z0 + iz;
        for (iy = 0; iy < gh; iy++) {
            y = y0 + iy;
            for (ix = 0; ix < gw; ix++) {
                int idx = (iz * gh + iy) * gw + ix;
                const uint8_t *src = colors + idx * 4;
                const uint8_t *dst = out_c + idx * 4;
                int pos[3];
                if (!src[3])
                    continue;
                if (src[0] == dst[0] && src[1] == dst[1] &&
                    src[2] == dst[2])
                    continue;
                pos[0] = x0 + ix;
                pos[1] = y;
                pos[2] = z;
                volume_set_at(volume, NULL, pos, dst);
                changed++;
            }
        }
    }

done:
    free(colors);
    free(out_c);
    return changed;
}

static bool check_can_skip(tool_smooth_t *sm, const cursor_t *curs,
                           int painter_mode)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (volume &&
        pressed == sm->last_op.pressed &&
        sm->last_op.volume_key == volume_get_key(volume) &&
        sm->last_op.mode == sm->mode &&
        sm->last_op.painter_mode == painter_mode &&
        sm->last_op.radius_x == goxel.smooth_radius_x &&
        sm->last_op.radius_y == goxel.smooth_radius_y &&
        sm->last_op.radius_z == goxel.smooth_radius_z &&
        sm->last_op.strength == sm->strength &&
        sm->last_op.add_noise == sm->add_noise &&
        sm->last_op.noise == sm->noise &&
        sm->last_op.noise_scale == sm->noise_scale &&
        sm->last_op.lock_noise == sm->lock_noise &&
        sm->last_op.empty_column == sm->empty_column &&
        sm->last_op.paint_dithering == sm->paint_dithering &&
        sm->last_op.paint_noise_enabled == sm->paint_noise_enabled &&
        sm->last_op.paint_noise_intensity == sm->paint_noise_intensity &&
        sm->last_op.paint_noise_saturation == sm->paint_noise_saturation &&
        sm->last_op.paint_noise_coverage == sm->paint_noise_coverage &&
        vec3_equal(curs->pos, sm->last_op.pos)) {
        return true;
    }
    sm->last_op.pressed = pressed;
    sm->last_op.mode = sm->mode;
    sm->last_op.painter_mode = painter_mode;
    sm->last_op.radius_x = goxel.smooth_radius_x;
    sm->last_op.radius_y = goxel.smooth_radius_y;
    sm->last_op.radius_z = goxel.smooth_radius_z;
    sm->last_op.strength = sm->strength;
    sm->last_op.add_noise = sm->add_noise;
    sm->last_op.noise = sm->noise;
    sm->last_op.noise_scale = sm->noise_scale;
    sm->last_op.lock_noise = sm->lock_noise;
    sm->last_op.empty_column = sm->empty_column;
    sm->last_op.paint_dithering = sm->paint_dithering;
    sm->last_op.paint_noise_enabled = sm->paint_noise_enabled;
    sm->last_op.paint_noise_intensity = sm->paint_noise_intensity;
    sm->last_op.paint_noise_saturation = sm->paint_noise_saturation;
    sm->last_op.paint_noise_coverage = sm->paint_noise_coverage;
    vec3_copy(curs->pos, sm->last_op.pos);
    if (volume)
        sm->last_op.volume_key = volume_get_key(volume);
    return false;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_smooth_t *sm = USER_GET(user, 0);
    const painter_t *painter = USER_GET(user, 1);
    cursor_t *curs = gest->cursor;
    float r_x = goxel.smooth_radius_x;
    float r_y = goxel.smooth_radius_y;
    float r_z = goxel.smooth_radius_z;
    float spacing;
    float pos[3];
    int nb, i;
    const bool paint_colors = (painter->mode == MODE_PAINT);

    ensure_defaults(sm);

    if (gest->state == GESTURE_BEGIN) {
        image_history_push(goxel.image);
        if (!sm->volume_orig)
            sm->volume_orig = volume_new();
        volume_set(sm->volume_orig, goxel.image->active_layer->volume);
        if (!goxel.tool_volume)
            goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, sm->volume_orig);
        vec3_copy(curs->pos, sm->last_pos);

        if (sm->add_noise && !paint_colors) {
            if (sm->lock_noise) {
                /* Keep the same field across strokes. */
                perlin2_init_seed(SMOOTH_NOISE_SEED_FIXED);
            } else {
                /* Fresh field for this stroke; stable for the whole drag. */
                sm->stroke_seed = sm->stroke_seed * 1664525u + 1013904223u;
                if (!sm->stroke_seed)
                    sm->stroke_seed = 1u;
                perlin2_init_seed(sm->stroke_seed);
            }
        }
    }

    if (gest->state == GESTURE_UPDATE &&
        check_can_skip(sm, curs, painter->mode))
        return 0;

    if (sm->mode == SMOOTH_MODE_3D)
        spacing = max(0.7f, min(r_x, min(r_y, r_z)) * 0.5f);
    else
        spacing = max(0.7f, min(r_x, r_y) * 0.5f);
    nb = (int)ceil(vec3_dist(curs->pos, sm->last_pos) / spacing);
    nb = max(nb, 1);

    if (!goxel.tool_volume) {
        goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, sm->volume_orig
                   ? sm->volume_orig
                   : goxel.image->active_layer->volume);
    }

    {
        int flips = 0;
        for (i = 0; i < nb; i++) {
            vec3_mix(sm->last_pos, curs->pos, (i + 1.0f) / nb, pos);
            if (sm->mode == SMOOTH_MODE_3D) {
                if (paint_colors) {
                    flips += smooth_dab_3d_color(
                        goxel.tool_volume,
                        (int)floorf(pos[0]), (int)floorf(pos[1]),
                        (int)floorf(pos[2]),
                        r_x, r_y, r_z, sm->strength,
                        sm->paint_dithering,
                        sm->paint_noise_enabled,
                        sm->paint_noise_intensity,
                        sm->paint_noise_saturation,
                        sm->paint_noise_coverage);
                } else {
                    flips += smooth_dab_3d(
                        goxel.tool_volume,
                        (int)floorf(pos[0]), (int)floorf(pos[1]),
                        (int)floorf(pos[2]),
                        r_x, r_y, r_z, sm->strength,
                        sm->add_noise ? sm->noise : 0.f, sm->noise_scale);
                }
            } else if (paint_colors) {
                smooth_dab_2d_color(
                    goxel.tool_volume,
                    (int)floorf(pos[0]), (int)floorf(pos[1]),
                    r_x, r_y, sm->strength,
                    sm->paint_dithering,
                    sm->paint_noise_enabled,
                    sm->paint_noise_intensity,
                    sm->paint_noise_saturation,
                    sm->paint_noise_coverage);
            } else {
                smooth_dab_2d(goxel.tool_volume,
                              (int)floorf(pos[0]), (int)floorf(pos[1]),
                              r_x, r_y, sm->strength,
                              sm->add_noise ? sm->noise : 0.f, sm->noise_scale,
                              sm->empty_column);
            }
        }
        if (sm->mode == SMOOTH_MODE_3D) {
            goxel_set_help_text(
                paint_colors ? "3D colour smooth: %d voxels changed"
                             : "3D smooth: %d voxels changed",
                flips);
        }
    }

    sm->last_op.volume_key = volume_get_key(goxel.tool_volume);
    vec3_copy(curs->pos, sm->last_pos);

    if (gest->state == GESTURE_END) {
        volume_set(goxel.image->active_layer->volume, goxel.tool_volume);
        volume_set(sm->volume_orig, goxel.tool_volume);
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
    }
    return 0;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    tool_smooth_t *sm = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;
    float box[4][4];
    float r_z;

    ensure_defaults(sm);
    if (gest->state == GESTURE_END || !curs->snaped) {
        if (!(curs->flags & CURSOR_PRESSED)) {
            volume_delete(goxel.tool_volume);
            goxel.tool_volume = NULL;
        }
        return 0;
    }

    r_z = (sm->mode == SMOOTH_MODE_3D) ? goxel.smooth_radius_z : 0.5f;
    get_brush_box(curs->pos, goxel.smooth_radius_x, goxel.smooth_radius_y,
                  r_z, box);
    render_box(&goxel.rend, box, NULL, EFFECT_WIREFRAME);
    return 0;
}

static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    tool_smooth_t *sm = (tool_smooth_t *)tool;
    cursor_t *curs = &goxel.cursor;
    const bool paint_colors = (painter->mode == MODE_PAINT);

    (void)viewport;
    ensure_defaults(sm);
    if (paint_colors) {
        if (sm->mode == SMOOTH_MODE_3D) {
            goxel_set_help_text(
                "Drag to blend colours of solid voxels inside the brush "
                "(shape unchanged).");
        } else {
            goxel_set_help_text(
                "Drag to blend colours of column tops "
                "(heights unchanged).");
        }
    } else if (sm->mode == SMOOTH_MODE_3D) {
        goxel_set_help_text(
            "Drag to smooth voxels inside the 3D brush "
            "(local solid/air majority).");
    } else {
        goxel_set_help_text(
            "Drag to smooth terrain heights "
            "(Gaussian average of column tops).");
    }

    curs->snap_mask |= SNAP_ROUNDED;
    curs->snap_offset = -0.5;

    if (!sm->gestures.drag.type) {
        sm->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
        sm->gestures.hover = (gesture3d_t) {
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
    }

    gesture3d(&sm->gestures.drag, curs, USER_PASS(sm, painter));
    gesture3d(&sm->gestures.hover, curs, USER_PASS(sm, painter));
    return tool->state;
}

static int gui(tool_t *tool)
{
    tool_smooth_t *sm = (tool_smooth_t *)tool;
    float strength, noise, noise_scale, dither;
    const bool paint_colors = (goxel.painter.mode == MODE_PAINT);

    ensure_defaults(sm);

    gui_combo("Mode", &sm->mode, (const char*[]) {
              "2D (top surface)", "3D (volume)"}, 2);
    gui_tooltip_if_hovered(
        paint_colors
            ? "2D: blend colours of column tops. "
              "3D: blend colours of solids inside the brush ellipsoid. "
              "Occupancy is never changed in Paint mode."
            : "2D: smooth absolute column tops (terrain). "
              "3D: morphologically smooth voxels inside the brush ellipsoid "
              "(tunnel floors, etc.)");

    if (sm->mode == SMOOTH_MODE_3D) {
        tool_gui_radius_xyz_values(&goxel.smooth_radius_x,
                                   &goxel.smooth_radius_y,
                                   &goxel.smooth_radius_z);
    } else {
        tool_gui_radius_xy_values(&goxel.smooth_radius_x,
                                  &goxel.smooth_radius_y);
    }

    strength = sm->strength;
    if (gui_input_float("Strength", &strength, 0.05f, 0.05f, 1.f, "%.2f"))
        sm->strength = clamp(strength, 0.05f, 1.f);
    gui_tooltip_if_hovered(
        paint_colors
            ? "How far each dab blends voxel RGB toward the local Gaussian "
              "average"
            : (sm->mode == SMOOTH_MODE_3D
                   ? "How strongly each dab flips voxels toward the local majority"
                   : "How far each dab moves column tops toward the local Gaussian "
                     "average"));

    if (sm->mode == SMOOTH_MODE_2D && !paint_colors) {
        gui_combo("Empty cols", &sm->empty_column, (const char*[]) {
                  "Create within", "Hard cut"}, 2);
        gui_tooltip_if_hovered(
            "Create within: empty columns fill from the floor up toward the "
            "neighbour average. Hard cut: empty columns stay empty, but solid "
            "tops can smooth down toward the floor");
    }

    if (paint_colors) {
        dither = sm->paint_dithering;
        if (gui_input_float("Dithering", &dither, 0.1f, 0.f, 16.f, "%.1f"))
            sm->paint_dithering = clamp(dither, 0.f, 16.f);
        gui_tooltip_if_hovered(
            "0 = none; higher scatters which voxels get colour-smoothed "
            "(same idea as brush dithering)");

        if (gui_section_begin("Noise", GUI_SECTION_COLLAPSABLE)) {
            gui_checkbox("Enable", &sm->paint_noise_enabled, NULL);
            if (sm->paint_noise_enabled) {
                int intensity = sm->paint_noise_intensity;
                int saturation = sm->paint_noise_saturation;
                int coverage = sm->paint_noise_coverage;
                if (gui_input_int("Intensity", &intensity, 0, 100))
                    sm->paint_noise_intensity = clamp(intensity, 0, 100);
                if (gui_input_int("Saturation", &saturation, 0, 100))
                    sm->paint_noise_saturation = clamp(saturation, 0, 100);
                if (gui_input_int("Coverage", &coverage, 0, 100))
                    sm->paint_noise_coverage = clamp(coverage, 0, 100);
                if (gui_button("Reset", 0, 0)) {
                    sm->paint_noise_intensity = 20;
                    sm->paint_noise_saturation = 10;
                    sm->paint_noise_coverage = 100;
                }
            }
        }
        gui_section_end();
    } else {
        gui_checkbox("Add noise", &sm->add_noise,
                     sm->mode == SMOOTH_MODE_3D
                         ? "Enable Perlin modulation of morphological strength"
                         : "Enable Perlin noise on top of Gaussian smoothing");
        if (sm->add_noise) {
            noise = sm->noise;
            if (gui_input_float("Noise", &noise, 0.05f, 0.f, 1.f, "%.2f"))
                sm->noise = clamp(noise, 0.f, 1.f);
            gui_tooltip_if_hovered(
                sm->mode == SMOOTH_MODE_3D
                    ? "Perlin amount: varies morphological strength in patches"
                    : "Perlin amount: warps the kernel, varies strength in patches, "
                      "and adds brush-sized height detail");

            noise_scale = sm->noise_scale;
            if (gui_input_float("Noise scale", &noise_scale, 0.05f, 0.05f, 8.f,
                                "%.2f"))
                sm->noise_scale = clamp(noise_scale, 0.05f, 8.f);
            gui_tooltip_if_hovered(
                "Perlin frequency vs brush size. 1 = about one feature per diameter; "
                "higher = finer detail, lower = broader undulation");

            gui_checkbox("Lock noise", &sm->lock_noise,
                         "Keep the same Perlin seed across strokes. When off, each "
                         "stroke gets a fresh seed.");
        }
    }

    return 0;
}

TOOL_REGISTER(TOOL_SMOOTH, smooth, tool_smooth_t,
              .name = "Smooth",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .has_snap = true,
)
