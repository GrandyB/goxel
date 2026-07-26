/* Clone stamp volume apply — independent of volume_op. */

#include "goxel.h"
#include "utils/clone_stamp_op.h"
#include "utils/noise.h"

static void shape_box_setup(const float box[4][4], float mat[4][4],
                            float size[3])
{
    box_get_size(box, size);
    mat4_copy(box, mat);
    mat4_iscale(mat, 1 / size[0], 1 / size[1], 1 / size[2]);
    mat4_invert(mat, mat);
}

/*
 * Grow tangential brush axes so soft AA / dither samples outside the hard
 * silhouette are visited.  Do not grow box[0] (Diameter Z / face normal) —
 * expanding it paints extra layers of thickness.
 */
static void grow_box_for_aa(const float box[4][4], float shape_sm,
                            float out[4][4])
{
    int i;

    mat4_copy(box, out);
    if (shape_sm <= 0.f) return;
    for (i = 1; i < 3; i++) { /* axes 1,2 = tangential; skip 0 = depth */
        float n = vec3_norm(out[i]);
        if (n > 1e-6f)
            vec3_mul(out[i], (n + shape_sm) / n, out[i]);
    }
}

/*
 * Brush-shaped coverage in [0,1].  AA/dither soften the tangential
 * footprint only; hard depth from the ungrown box is enforced separately.
 */
static float shape_coverage(const shape_t *shape, const float mat[4][4],
                            const float size[3], float smoothness,
                            float dithering, const int vp[3])
{
    float p[3], k, v;
    float shape_sm = smoothness + dithering;

    vec3_set(p, vp[0] + 0.5f, vp[1] + 0.5f, vp[2] + 0.5f);
    mat4_mul_vec3(mat, p, p);
    k = shape->func(p, size, shape_sm);
    if (dithering > 0.f) {
        float n = uniform_noise((float)vp[0], (float)vp[1], (float)vp[2]);
        k += (n * 2.f - 1.f) * dithering;
    }
    if (smoothness) {
        v = clamp(k / smoothness, -1.0f, 1.0f) / 2.0f + 0.5f;
    } else {
        v = (k >= 0.f) ? 1.f : 0.f;
    }
    return v;
}

/* World axis of box[0] (Diameter Z / face normal) — used for hard depth. */
static int box_depth_axis(const float box[4][4])
{
    float ax = fabsf(box[0][0]), ay = fabsf(box[0][1]), az = fabsf(box[0][2]);
    if (ax >= ay && ax >= az) return 0;
    if (ay >= az) return 1;
    return 2;
}

/* True if voxel is inside the hard (ungrown) brush box along Diameter Z. */
static bool voxel_in_hard_depth(const int aabb[2][3], const float box[4][4],
                                const int vp[3])
{
    int a = box_depth_axis(box);
    return vp[a] >= aabb[0][a] && vp[a] < aabb[1][a];
}

static int sample_lowest_on_axis(const volume_t *sample, int axis)
{
    float box[4][4];
    int bbox[2][3];

    mat4_copy(goxel.image->box, box);
    if (!box_is_null(box))
        return (int)floor(box[3][axis] - box[axis][axis]);
    if (volume_get_bbox(sample, bbox, true))
        return bbox[0][axis];
    return 0;
}

static int sample_highest_on_axis(const volume_t *sample, int axis)
{
    float box[4][4];
    int bbox[2][3];

    mat4_copy(goxel.image->box, box);
    if (!box_is_null(box))
        return (int)floor(box[3][axis] + box[axis][axis]) - 1;
    if (volume_get_bbox(sample, bbox, true))
        return bbox[1][axis] - 1;
    return 255;
}

static const clone_stamp_sample_t *sample_opts_or_default(
        const clone_stamp_sample_t *opts, clone_stamp_sample_t *tmp)
{
    if (opts) return opts;
    tmp->take_uppermost = true;
    tmp->depth = 0;
    tmp->source_face = -1;
    tmp->target_face = -1;
    return tmp;
}

static bool face_ok(int face)
{
    return face >= 0 && face < 6;
}

/*
 * Stable face UV for wall remapping (not raw FACES_MATS).
 *
 * FACES_MATS swaps U/V on some sides (e.g. -X vs +X), so U→U mapping
 * only worked in one direction.  Here vertical walls always use V = +Z
 * (world up) and U = cross(V, N), so left/right/forward faces share the
 * same semantic axes.
 */
static void face_uvn(int face, int u[3], int v[3], int n[3],
                     int *u_axis, int *v_axis, int *n_axis)
{
    int i;

    for (i = 0; i < 3; i++)
        n[i] = FACES_NORMALS[face][i];
    *n_axis = n[0] ? 0 : (n[1] ? 1 : 2);

    if (*n_axis != 2) {
        /* Vertical wall: V = +Z, U = V × N = (-n_y, n_x, 0). */
        v[0] = 0;
        v[1] = 0;
        v[2] = 1;
        u[0] = -n[1];
        u[1] = n[0];
        u[2] = 0;
    } else {
        /* Top / bottom: U = +Y, V = N × U. */
        u[0] = 0;
        u[1] = 1;
        u[2] = 0;
        v[0] = -n[2]; /* n×u: (-n_z, 0, 0) for n=(0,0,±1) */
        v[1] = 0;
        v[2] = 0;
    }

    *u_axis = u[0] ? 0 : (u[1] ? 1 : 2);
    *v_axis = v[0] ? 0 : (v[1] ? 1 : 2);
}

/*
 * take_uppermost: absolute outermost solid along depth_axis (n_sign > 0
 * scans high→low).  Otherwise: outermost solid in [source_d ± depth].
 */
static bool find_sample_voxel(const volume_t *sample, volume_accessor_t *acc,
                              int pos[3], int depth_axis, int n_sign,
                              int source_d, int lowest, int highest,
                              const clone_stamp_sample_t *opts,
                              uint8_t out_color[4], int out_pos[3])
{
    int d, d_hi, d_lo;
    uint8_t color[4];

    if (opts->take_uppermost) {
        d_hi = highest;
        d_lo = lowest;
    } else {
        int depth = max(0, opts->depth);
        d_hi = source_d + depth;
        d_lo = source_d - depth;
        if (d_lo < lowest) d_lo = lowest;
        if (d_hi > highest) d_hi = highest;
    }

    if (n_sign >= 0) {
        for (d = d_hi; d >= d_lo; d--) {
            pos[depth_axis] = d;
            volume_get_at(sample, acc, pos, color);
            if (color[3]) {
                memcpy(out_color, color, 4);
                if (out_pos) memcpy(out_pos, pos, sizeof(int) * 3);
                return true;
            }
        }
    } else {
        for (d = d_lo; d <= d_hi; d++) {
            pos[depth_axis] = d;
            volume_get_at(sample, acc, pos, color);
            if (color[3]) {
                memcpy(out_color, color, 4);
                if (out_pos) memcpy(out_pos, pos, sizeof(int) * 3);
                return true;
            }
        }
    }
    memset(out_color, 0, 4);
    return false;
}

static bool column_in_brush(const shape_t *shape, const float mat[4][4],
                            const float size[3], float smoothness,
                            const int aabb[2][3],
                            int a0, int a1, int a_depth,
                            int c0, int c1)
{
    int d, vp[3];

    vp[a0] = c0;
    vp[a1] = c1;
    for (d = aabb[0][a_depth]; d < aabb[1][a_depth]; d++) {
        vp[a_depth] = d;
        if (shape_coverage(shape, mat, size, smoothness, 0.f, vp) > 0.f)
            return true;
    }
    return false;
}

/* Map dest voxel → source column coords using face tangents (or classic XY). */
static void source_column_for_dest(const float target[3], const float source[3],
                                   const int vp[3],
                                   const clone_stamp_sample_t *o,
                                   int out_col[3], int *depth_axis,
                                   int *n_sign, int *source_d)
{
    if (face_ok(o->source_face) && face_ok(o->target_face)) {
        int su[3], sv[3], sn[3], tu[3], tv[3], tn[3];
        int su_a, sv_a, sn_a, tu_a, tv_a, tn_a;
        int rel_u, rel_v;
        int i;

        face_uvn(o->source_face, su, sv, sn, &su_a, &sv_a, &sn_a);
        face_uvn(o->target_face, tu, tv, tn, &tu_a, &tv_a, &tn_a);

        rel_u = tu[tu_a] * (vp[tu_a] - (int)floor(target[tu_a]));
        rel_v = tv[tv_a] * (vp[tv_a] - (int)floor(target[tv_a]));

        for (i = 0; i < 3; i++)
            out_col[i] = (int)floor(source[i]);
        out_col[su_a] = (int)floor(source[su_a]) + su[su_a] * rel_u;
        out_col[sv_a] = (int)floor(source[sv_a]) + sv[sv_a] * rel_v;

        *depth_axis = sn_a;
        *n_sign = sn[sn_a];
        *source_d = (int)floor(source[sn_a]);
        (void)tn;
        (void)tn_a;
    } else {
        out_col[0] = vp[0] + ((int)floor(source[0]) - (int)floor(target[0]));
        out_col[1] = vp[1] + ((int)floor(source[1]) - (int)floor(target[1]));
        out_col[2] = 0;
        *depth_axis = 2;
        *n_sign = 1;
        *source_d = (int)floor(source[2]);
    }
}

void clone_stamp_apply(volume_t *dest, const volume_t *sample,
                       const float target[3], const float source[3],
                       const float box[4][4], const shape_t *shape,
                       float smoothness, float dithering, float opacity,
                       const clone_stamp_sample_t *opts)
{
    volume_iterator_t iter;
    volume_accessor_t dest_acc, sample_acc;
    float mat[4][4], size[3], iter_box[4][4], v, opac;
    int vp[3], col[3], depth_axis, n_sign, source_d, lowest, highest;
    int hard_aabb[2][3];
    uint8_t dest_c[4], sample_c[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape);
    opac = clamp(opacity, 0.f, 1.f);
    if (opac <= 0.f) return;

    shape_box_setup(box, mat, size);
    grow_box_for_aa(box, smoothness + dithering, iter_box);
    box_get_aabb(box, hard_aabb);

    /* Bound range along inherit axis (source face normal or world Z). */
    if (face_ok(o->source_face)) {
        const int *fn = FACES_NORMALS[o->source_face];
        depth_axis = fn[0] ? 0 : (fn[1] ? 1 : 2);
    } else {
        depth_axis = 2;
    }
    lowest = sample_lowest_on_axis(sample, depth_axis);
    highest = sample_highest_on_axis(sample, depth_axis);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);
    iter = volume_get_box_iterator(dest, iter_box, VOLUME_ITER_SKIP_EMPTY);

    while (volume_iter(&iter, vp)) {
        /* Keep Diameter Z exact — AA/dither must not bleed into other layers. */
        if (!voxel_in_hard_depth(hard_aabb, box, vp)) continue;

        v = shape_coverage(shape, mat, size, smoothness, dithering, vp);
        if (v <= 0.f) continue;

        volume_get_at(dest, &dest_acc, vp, dest_c);
        if (!dest_c[3]) continue;

        source_column_for_dest(target, source, vp, o, col, &depth_axis,
                               &n_sign, &source_d);
        if (!find_sample_voxel(sample, &sample_acc, col, depth_axis, n_sign,
                               source_d, lowest, highest, o, sample_c, NULL))
            continue;

        sample_c[3] = (uint8_t)(sample_c[3] * v * opac);
        if (!sample_c[3]) continue;

        voxel_combine(dest_c, sample_c, MODE_PAINT, out);
        if (!vec4_equal(dest_c, out))
            volume_set_at(dest, &dest_acc, vp, out);
    }
}

void clone_stamp_preview_source(volume_t *dest, const volume_t *sample,
                                const float source[3],
                                const float box[4][4], const shape_t *shape,
                                float smoothness,
                                const clone_stamp_sample_t *opts,
                                const uint8_t marker_color[4])
{
    volume_accessor_t dest_acc, sample_acc;
    float mat[4][4], size[3];
    int aabb[2][3], c0, c1, a0, a1, a_depth, n_sign, source_d;
    int lowest, highest, col[3], sample_pos[3], i;
    uint8_t sample_c[4], dest_c[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape && marker_color);
    shape_box_setup(box, mat, size);
    box_get_aabb(box, aabb);

    if (face_ok(o->source_face)) {
        int su[3], sv[3], sn[3], su_a, sv_a, sn_a;
        face_uvn(o->source_face, su, sv, sn, &su_a, &sv_a, &sn_a);
        a0 = su_a;
        a1 = sv_a;
        a_depth = sn_a;
        n_sign = sn[sn_a];
        source_d = (int)floor(source[sn_a]);
    } else {
        a0 = 0;
        a1 = 1;
        a_depth = 2;
        n_sign = 1;
        source_d = (int)floor(source[2]);
    }
    lowest = sample_lowest_on_axis(sample, a_depth);
    highest = sample_highest_on_axis(sample, a_depth);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);

    for (c1 = aabb[0][a1]; c1 < aabb[1][a1]; c1++)
    for (c0 = aabb[0][a0]; c0 < aabb[1][a0]; c0++) {
        if (!column_in_brush(shape, mat, size, smoothness, aabb,
                             a0, a1, a_depth, c0, c1))
            continue;

        for (i = 0; i < 3; i++)
            col[i] = (int)floor(source[i]);
        col[a0] = c0;
        col[a1] = c1;

        if (!find_sample_voxel(sample, &sample_acc, col, a_depth, n_sign,
                               source_d, lowest, highest, o, sample_c,
                               sample_pos))
            continue;

        volume_get_at(dest, &dest_acc, sample_pos, dest_c);
        memcpy(out, marker_color, 4);
        if (!vec4_equal(dest_c, out))
            volume_set_at(dest, &dest_acc, sample_pos, out);
    }
}

void clone_stamp_highlight(volume_t *dest, const float center[3],
                           const float box[4][4], const shape_t *shape,
                           float smoothness, const uint8_t color[4])
{
    volume_iterator_t iter;
    volume_accessor_t acc;
    float mat[4][4], size[3], v;
    int vp[3];
    uint8_t dest_c[4], paint[4], out[4];

    assert(dest && shape && color);
    (void)center;
    shape_box_setup(box, mat, size);

    acc = volume_get_accessor(dest);
    iter = volume_get_box_iterator(dest, box, VOLUME_ITER_SKIP_EMPTY);

    while (volume_iter(&iter, vp)) {
        v = shape_coverage(shape, mat, size, smoothness, 0.f, vp);
        if (v <= 0.f) continue;

        volume_get_at(dest, &acc, vp, dest_c);
        if (!dest_c[3]) continue;

        memcpy(paint, color, 4);
        paint[3] = (uint8_t)(color[3] * v);
        if (!paint[3]) continue;

        voxel_combine(dest_c, paint, MODE_PAINT, out);
        if (!vec4_equal(dest_c, out))
            volume_set_at(dest, &acc, vp, out);
    }
}
