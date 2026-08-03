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

    struct {
        float    pos[3];
        bool     pressed;
        uint64_t volume_key;
        float    radius_x, radius_y;
        float    strength;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;
} tool_smooth_t;

static void ensure_defaults(tool_smooth_t *sm)
{
    if (sm->strength <= 0.f)
        sm->strength = 0.4f;
}

static void get_brush_box(const float p[3], float r_x, float r_y, float out[4][4])
{
    float box[4][4];
    bbox_from_extents(box, p, r_x, r_y, 0.5f);
    box_swap_axis(box, 2, 0, 1, box);
    mat4_copy(box, out);
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
 */
static void smooth_dab(volume_t *volume, int cx, int cy,
                       float r_x, float r_y, float strength)
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
    volume_accessor_t acc;

    if (r_x < 0.5f) r_x = 0.5f;
    if (r_y < 0.5f) r_y = 0.5f;
    strength = clamp(strength, 0.f, 1.f);
    if (strength <= 0.f)
        return;

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
            falloff = expf(-2.f * d2);

            wsum = 0.f;
            hsum = 0.f;
            krad_x = (int)ceilf(2.f * sx);
            krad_y = (int)ceilf(2.f * sy);
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
                    wx = (float)nx;
                    wy = (float)ny;
                    w = expf(-(wx * wx * inv_2sx2 + wy * wy * inv_2sy2));
                    wsum += w;
                    hsum += w * (float)hn;
                }
            }
            if (wsum <= 0.f)
                continue;

            avg = hsum / wsum;
            t = strength * falloff;
            out_h[iy * gw + ix] = (int)roundf((float)h0 + (avg - (float)h0) * t);
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

/* Reflect dab centres for painter XY symmetry (Z ignored for heightfield). */
static void smooth_dab_symmetry(volume_t *volume, float pos[3],
                                float r_x, float r_y, float strength,
                                int symmetry, const float sym_o[3])
{
    int i;
    float p[3];
    int cx, cy;

    vec3_copy(pos, p);
    cx = (int)floorf(p[0]);
    cy = (int)floorf(p[1]);
    smooth_dab(volume, cx, cy, r_x, r_y, strength);

    /* Mutate symmetry like volume_op so each axis combo runs once. */
    for (i = 0; i < 2; i++) {
        float q[3];
        if (!(symmetry & (1 << i))) continue;
        symmetry &= ~(1 << i);
        vec3_copy(p, q);
        q[i] = 2.f * sym_o[i] - q[i];
        smooth_dab_symmetry(volume, q, r_x, r_y, strength, symmetry, sym_o);
    }
}

static bool check_can_skip(tool_smooth_t *sm, const cursor_t *curs)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (volume &&
        pressed == sm->last_op.pressed &&
        sm->last_op.volume_key == volume_get_key(volume) &&
        sm->last_op.radius_x == goxel.radius_x &&
        sm->last_op.radius_y == goxel.radius_y &&
        sm->last_op.strength == sm->strength &&
        vec3_equal(curs->pos, sm->last_op.pos)) {
        return true;
    }
    sm->last_op.pressed = pressed;
    sm->last_op.radius_x = goxel.radius_x;
    sm->last_op.radius_y = goxel.radius_y;
    sm->last_op.strength = sm->strength;
    vec3_copy(curs->pos, sm->last_op.pos);
    if (volume)
        sm->last_op.volume_key = volume_get_key(volume);
    return false;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_smooth_t *sm = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;
    float r_x = goxel.radius_x;
    float r_y = goxel.radius_y;
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
        smooth_dab_symmetry(goxel.tool_volume, pos, r_x, r_y, sm->strength,
                            goxel.painter.symmetry,
                            goxel.painter.symmetry_origin);
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

    get_brush_box(curs->pos, goxel.radius_x, goxel.radius_y, box);
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
    float strength;

    ensure_defaults(sm);
    tool_gui_radius_xy();

    strength = sm->strength;
    if (gui_input_float("Strength", &strength, 0.05f, 0.05f, 1.f, "%.2f"))
        sm->strength = clamp(strength, 0.05f, 1.f);
    gui_tooltip_if_hovered(
        "How far each dab moves column tops toward the local Gaussian average");

    tool_gui_symmetry();
    return 0;
}

TOOL_REGISTER(TOOL_SMOOTH, smooth, tool_smooth_t,
              .name = "Smooth",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .has_snap = true,
)
