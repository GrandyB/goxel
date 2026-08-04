/* Goxel 3D voxels editor
 *
 * Terrain heightfield Gaussian smooth brush.
 */

#include "goxel.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    tool_t tool;

    volume_t *volume_orig;
    float last_pos[3];
    float strength; /* 0..1 how far each dab moves toward the local average. */
    bool add_noise; /* Master switch for Perlin; hides noise fields when off. */
    float noise;    /* 0..1 Perlin warp / strength mod / residual detail. */
    float noise_scale; /* Spatial frequency multiplier (1 = ~1 cell per diameter). */
    /* When true, keep a fixed Perlin seed across strokes. */
    bool lock_noise;
    unsigned stroke_seed;
    bool defaults_inited;

    struct {
        float    pos[3];
        bool     pressed;
        uint64_t volume_key;
        float    radius_x, radius_y;
        float    strength;
        bool     add_noise;
        float    noise;
        float    noise_scale;
        bool     lock_noise;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;
} tool_smooth_t;

/* ---- 2D Perlin (world-space; seed may refresh per stroke) ---- */

static unsigned char g_smooth_perm[512];
static unsigned g_smooth_perm_seed = 0;
static bool g_smooth_perm_inited = false;

#define SMOOTH_NOISE_SEED_FIXED 0x51a007u

static void smooth_noise_init_seed(unsigned seed)
{
    int i, j, k;
    unsigned char p[256];
    unsigned s = seed ? seed : 1u;

    if (g_smooth_perm_inited && g_smooth_perm_seed == seed)
        return;

    for (i = 0; i < 256; i++)
        p[i] = (unsigned char)i;
    for (i = 255; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        j = (int)(s % (unsigned)(i + 1));
        k = p[i];
        p[i] = p[j];
        p[j] = (unsigned char)k;
    }
    for (i = 0; i < 256; i++) {
        g_smooth_perm[i] = p[i];
        g_smooth_perm[i + 256] = p[i];
    }
    g_smooth_perm_seed = seed;
    g_smooth_perm_inited = true;
}

static void smooth_noise_init(void)
{
    if (!g_smooth_perm_inited)
        smooth_noise_init_seed(SMOOTH_NOISE_SEED_FIXED);
}

static float smooth_fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float smooth_grad2(int h, float x, float y)
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

/* Classic Perlin in ~[-1, 1]. */
static float smooth_perlin2(float x, float y)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    int xi = x0 & 255;
    int yi = y0 & 255;
    float u = smooth_fade(fx);
    float v = smooth_fade(fy);
    int aa = g_smooth_perm[g_smooth_perm[xi] + yi];
    int ab = g_smooth_perm[g_smooth_perm[xi] + yi + 1];
    int ba = g_smooth_perm[g_smooth_perm[xi + 1] + yi];
    int bb = g_smooth_perm[g_smooth_perm[xi + 1] + yi + 1];
    float x1 = smooth_grad2(aa, fx, fy) +
               (smooth_grad2(ba, fx - 1.0f, fy) - smooth_grad2(aa, fx, fy)) * u;
    float x2 = smooth_grad2(ab, fx, fy - 1.0f) +
               (smooth_grad2(bb, fx - 1.0f, fy - 1.0f) -
                smooth_grad2(ab, fx, fy - 1.0f)) * u;
    return x1 + (x2 - x1) * v;
}

static void ensure_defaults(tool_smooth_t *sm)
{
    if (sm->defaults_inited) return;
    sm->strength = 0.85f;
    sm->add_noise = true;
    sm->noise = 0.7f;
    sm->noise_scale = 2.f;
    sm->lock_noise = false;
    sm->stroke_seed = SMOOTH_NOISE_SEED_FIXED;
    sm->defaults_inited = true;
    smooth_noise_init_seed(SMOOTH_NOISE_SEED_FIXED);
}

static void get_brush_box(const float p[3], float r_x, float r_y, float out[4][4])
{
    float box[4][4];
    bbox_from_extents(box, p, r_x, r_y, 0.5f);
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

static void set_column_top(volume_t *volume, volume_accessor_t *acc,
                           int x, int y, int old_z, int new_z)
{
    int pos[3];
    uint8_t color[4];
    uint8_t empty[4] = {0, 0, 0, 0};
    int z;

    if (new_z == old_z || old_z == INT_MIN)
        return;

    pos[0] = x;
    pos[1] = y;
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
 * Gaussian-smooth column tops under an elliptical brush centred on (cx, cy).
 * Neighbourhood and falloff both scale with brush radii.
 * noise (0..1): world-space Perlin warps the kernel, modulates blend strength,
 * and reintroduces coherent residual detail.
 * noise_scale: multiplies Perlin frequency (1 = ~one cell per brush diameter).
 */
static void smooth_dab(volume_t *volume, int cx, int cy,
                       float r_x, float r_y, float strength, float noise,
                       float noise_scale)
{
    int margin, gw, gh, x0, y0;
    int *heights = NULL;
    int *out_h = NULL;
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
    if (!heights || !out_h)
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
            int h0, krad_x, krad_y;

            x = x0 + ix;
            h0 = heights[iy * gw + ix];
            out_h[iy * gw + ix] = h0;
            if (h0 == INT_MIN)
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
                float nx_w = smooth_perlin2(px, py);
                float ny_w = smooth_perlin2(px + 19.7f, py + 7.3f);
                float ns = smooth_perlin2(px + 41.1f, py + 23.9f);
                float nr = smooth_perlin2(px + 3.1f, py + 61.4f);

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
            krad_x = (int)ceilf(2.f * sx + fabsf(kox));
            krad_y = (int)ceilf(2.f * sy + fabsf(koy));
            for (ny = -krad_y; ny <= krad_y; ny++) {
                int jy = iy + ny;
                if (jy < 0 || jy >= gh) continue;
                for (nx = -krad_x; nx <= krad_x; nx++) {
                    int jx = ix + nx;
                    int hn;
                    float wx, wy, w;
                    if (jx < 0 || jx >= gw) continue;
                    hn = heights[jy * gw + jx];
                    if (hn == INT_MIN) continue;
                    wx = (float)nx - kox;
                    wy = (float)ny - koy;
                    w = smooth_fast_exp(
                        -(wx * wx * inv_2sx2 + wy * wy * inv_2sy2));
                    wsum += w;
                    hsum += w * (float)hn;
                }
            }
            if (wsum <= 0.f)
                continue;

            avg = hsum / wsum;
            t = strength * falloff * n_str;
            blended = (float)h0 + (avg - (float)h0) * t;
            /* Residual scales with brush size and Noise. */
            if (namp > 0.f)
                blended += n_res * namp * falloff * res_amp;
            out_h[iy * gw + ix] = (int)roundf(blended);
            if (out_h[iy * gw + ix] < z_clamp_lo)
                out_h[iy * gw + ix] = z_clamp_lo;
            if (out_h[iy * gw + ix] > z_clamp_hi)
                out_h[iy * gw + ix] = z_clamp_hi;
        }
    }

    for (iy = 0; iy < gh; iy++) {
        y = y0 + iy;
        for (ix = 0; ix < gw; ix++) {
            x = x0 + ix;
            if (out_h[iy * gw + ix] == heights[iy * gw + ix])
                continue;
            set_column_top(volume, &acc, x, y,
                           heights[iy * gw + ix], out_h[iy * gw + ix]);
        }
    }

done:
    free(heights);
    free(out_h);
}

static bool check_can_skip(tool_smooth_t *sm, const cursor_t *curs)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (volume &&
        pressed == sm->last_op.pressed &&
        sm->last_op.volume_key == volume_get_key(volume) &&
        sm->last_op.radius_x == goxel.smooth_radius_x &&
        sm->last_op.radius_y == goxel.smooth_radius_y &&
        sm->last_op.strength == sm->strength &&
        sm->last_op.add_noise == sm->add_noise &&
        sm->last_op.noise == sm->noise &&
        sm->last_op.noise_scale == sm->noise_scale &&
        sm->last_op.lock_noise == sm->lock_noise &&
        vec3_equal(curs->pos, sm->last_op.pos)) {
        return true;
    }
    sm->last_op.pressed = pressed;
    sm->last_op.radius_x = goxel.smooth_radius_x;
    sm->last_op.radius_y = goxel.smooth_radius_y;
    sm->last_op.strength = sm->strength;
    sm->last_op.add_noise = sm->add_noise;
    sm->last_op.noise = sm->noise;
    sm->last_op.noise_scale = sm->noise_scale;
    sm->last_op.lock_noise = sm->lock_noise;
    vec3_copy(curs->pos, sm->last_op.pos);
    if (volume)
        sm->last_op.volume_key = volume_get_key(volume);
    return false;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_smooth_t *sm = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;
    float r_x = goxel.smooth_radius_x;
    float r_y = goxel.smooth_radius_y;
    float spacing;
    float pos[3];
    int nb, i;

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

        if (sm->add_noise) {
            if (sm->lock_noise) {
                /* Keep the same field across strokes. */
                smooth_noise_init_seed(SMOOTH_NOISE_SEED_FIXED);
            } else {
                /* Fresh field for this stroke; stable for the whole drag. */
                sm->stroke_seed = sm->stroke_seed * 1664525u + 1013904223u;
                if (!sm->stroke_seed)
                    sm->stroke_seed = 1u;
                smooth_noise_init_seed(sm->stroke_seed);
            }
        }
    }

    if (gest->state == GESTURE_UPDATE && check_can_skip(sm, curs))
        return 0;

    spacing = max(0.7f, min(r_x, r_y) * 0.5f);
    nb = (int)ceil(vec3_dist(curs->pos, sm->last_pos) / spacing);
    nb = max(nb, 1);

    if (!goxel.tool_volume) {
        goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, sm->volume_orig
                   ? sm->volume_orig
                   : goxel.image->active_layer->volume);
    }

    for (i = 0; i < nb; i++) {
        vec3_mix(sm->last_pos, curs->pos, (i + 1.0f) / nb, pos);
        smooth_dab(goxel.tool_volume, (int)floorf(pos[0]), (int)floorf(pos[1]),
                   r_x, r_y, sm->strength,
                   sm->add_noise ? sm->noise : 0.f, sm->noise_scale);
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

    (void)sm;
    if (gest->state == GESTURE_END || !curs->snaped) {
        if (!(curs->flags & CURSOR_PRESSED)) {
            volume_delete(goxel.tool_volume);
            goxel.tool_volume = NULL;
        }
        return 0;
    }

    get_brush_box(curs->pos, goxel.smooth_radius_x, goxel.smooth_radius_y, box);
    render_box(&goxel.rend, box, NULL, EFFECT_WIREFRAME);
    return 0;
}

static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    tool_smooth_t *sm = (tool_smooth_t *)tool;
    cursor_t *curs = &goxel.cursor;

    (void)painter;
    (void)viewport;
    ensure_defaults(sm);
    goxel_set_help_text(
        "Drag to smooth terrain heights (Gaussian average of column tops).");

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
    float strength, noise, noise_scale;

    ensure_defaults(sm);
    tool_gui_radius_xy_values(&goxel.smooth_radius_x, &goxel.smooth_radius_y);

    strength = sm->strength;
    if (gui_input_float("Strength", &strength, 0.05f, 0.05f, 1.f, "%.2f"))
        sm->strength = clamp(strength, 0.05f, 1.f);
    gui_tooltip_if_hovered(
        "How far each dab moves column tops toward the local Gaussian average");

    gui_checkbox("Add noise", &sm->add_noise,
                 "Enable Perlin noise on top of Gaussian smoothing");
    if (sm->add_noise) {
        noise = sm->noise;
        if (gui_input_float("Noise", &noise, 0.05f, 0.f, 1.f, "%.2f"))
            sm->noise = clamp(noise, 0.f, 1.f);
        gui_tooltip_if_hovered(
            "Perlin amount: warps the kernel, varies strength in patches, and "
            "adds brush-sized height detail");

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

    return 0;
}

TOOL_REGISTER(TOOL_SMOOTH, smooth, tool_smooth_t,
              .name = "Smooth",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .has_snap = true,
)
