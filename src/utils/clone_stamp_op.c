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
 * Grow horizontal brush axes so soft AA / dither samples outside the hard
 * XY silhouette are visited.  Do not grow world-Z: after get_box3's
 * box_swap_axis(2,0,1), axis 0 is vertical — expanding it paints extra
 * layers when Diameter Z is 1.
 */
static void grow_box_for_aa(const float box[4][4], float shape_sm,
                            float out[4][4])
{
    int i;

    mat4_copy(box, out);
    if (shape_sm <= 0.f) return;
    for (i = 1; i < 3; i++) { /* axes 1,2 = horizontal; skip 0 = Z */
        float n = vec3_norm(out[i]);
        if (n > 1e-6f)
            vec3_mul(out[i], (n + shape_sm) / n, out[i]);
    }
}

/*
 * Brush-shaped coverage in [0,1].  AA/dither soften the XY footprint only:
 * hard Z from the ungrown box is enforced separately so Diameter Z stays
 * exact.
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

/* True if voxel Z is inside the hard (ungrown) brush box AABB. */
static bool voxel_in_hard_z(const int aabb[2][3], const int vp[3])
{
    return vp[2] >= aabb[0][2] && vp[2] < aabb[1][2];
}

static int sample_lowest_z(const volume_t *sample)
{
    float box[4][4];
    int bbox[2][3];

    mat4_copy(goxel.image->box, box);
    if (!box_is_null(box))
        return (int)floor(box[3][2] - box[2][2]);
    if (volume_get_bbox(sample, bbox, true))
        return bbox[0][2];
    return 0;
}

/* Top of the map / volume — used when scanning for the absolute uppermost. */
static int sample_highest_z(const volume_t *sample)
{
    float box[4][4];
    int bbox[2][3];

    mat4_copy(goxel.image->box, box);
    if (!box_is_null(box))
        return (int)floor(box[3][2] + box[2][2]) - 1;
    if (volume_get_bbox(sample, bbox, true))
        return bbox[1][2] - 1;
    return 255;
}

static const clone_stamp_sample_t *sample_opts_or_default(
        const clone_stamp_sample_t *opts, clone_stamp_sample_t *tmp)
{
    if (opts) return opts;
    tmp->take_uppermost = true;
    tmp->depth = 0;
    return tmp;
}

/*
 * take_uppermost: absolute top solid in the column (map top → bottom), so as
 * the source footprint moves onto taller terrain those higher blocks are used.
 * Otherwise: uppermost solid in [source_z ± depth].
 */
static bool find_sample_voxel(const volume_t *sample, volume_accessor_t *acc,
                              int x, int y, int source_z, int lowest_z,
                              int highest_z, const clone_stamp_sample_t *opts,
                              uint8_t out_color[4], int out_pos[3])
{
    int z, z_hi, z_lo, pos[3];
    uint8_t color[4];

    pos[0] = x;
    pos[1] = y;

    if (opts->take_uppermost) {
        z_hi = highest_z;
        z_lo = lowest_z;
    } else {
        int depth = max(0, opts->depth);
        z_hi = source_z + depth;
        z_lo = source_z - depth;
        if (z_lo < lowest_z) z_lo = lowest_z;
        if (z_hi > highest_z) z_hi = highest_z;
    }

    for (z = z_hi; z >= z_lo; z--) {
        pos[2] = z;
        volume_get_at(sample, acc, pos, color);
        if (color[3]) {
            memcpy(out_color, color, 4);
            if (out_pos) {
                out_pos[0] = x;
                out_pos[1] = y;
                out_pos[2] = z;
            }
            return true;
        }
    }
    memset(out_color, 0, 4);
    return false;
}

static bool column_in_brush(const shape_t *shape, const float mat[4][4],
                            const float size[3], float smoothness,
                            const int aabb[2][3], int x, int y)
{
    int z, vp[3];

    vp[0] = x;
    vp[1] = y;
    for (z = aabb[0][2]; z < aabb[1][2]; z++) {
        vp[2] = z;
        if (shape_coverage(shape, mat, size, smoothness, 0.f, vp) > 0.f)
            return true;
    }
    return false;
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
    int vp[3], off[2], source_z, lowest_z, highest_z, hard_aabb[2][3];
    uint8_t dest_c[4], sample_c[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape);
    opac = clamp(opacity, 0.f, 1.f);
    if (opac <= 0.f) return;

    shape_box_setup(box, mat, size);
    grow_box_for_aa(box, smoothness + dithering, iter_box);
    box_get_aabb(box, hard_aabb);

    off[0] = (int)floor(source[0]) - (int)floor(target[0]);
    off[1] = (int)floor(source[1]) - (int)floor(target[1]);
    source_z = (int)floor(source[2]);
    lowest_z = sample_lowest_z(sample);
    highest_z = sample_highest_z(sample);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);
    iter = volume_get_box_iterator(dest, iter_box, VOLUME_ITER_SKIP_EMPTY);

    while (volume_iter(&iter, vp)) {
        /* Keep Diameter Z exact — AA/dither must not bleed into other layers. */
        if (!voxel_in_hard_z(hard_aabb, vp)) continue;

        v = shape_coverage(shape, mat, size, smoothness, dithering, vp);
        if (v <= 0.f) continue;

        volume_get_at(dest, &dest_acc, vp, dest_c);
        if (!dest_c[3]) continue;

        if (!find_sample_voxel(sample, &sample_acc,
                               vp[0] + off[0], vp[1] + off[1],
                               source_z, lowest_z, highest_z, o,
                               sample_c, NULL))
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
    int aabb[2][3], x, y, source_z, lowest_z, highest_z, sample_pos[3];
    uint8_t sample_c[4], dest_c[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape && marker_color);
    shape_box_setup(box, mat, size);
    box_get_aabb(box, aabb);

    source_z = (int)floor(source[2]);
    lowest_z = sample_lowest_z(sample);
    highest_z = sample_highest_z(sample);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);

    for (y = aabb[0][1]; y < aabb[1][1]; y++)
    for (x = aabb[0][0]; x < aabb[1][0]; x++) {
        if (!column_in_brush(shape, mat, size, smoothness, aabb, x, y))
            continue;
        if (!find_sample_voxel(sample, &sample_acc, x, y, source_z, lowest_z,
                               highest_z, o, sample_c, sample_pos))
            continue;

        volume_get_at(dest, &dest_acc, sample_pos, dest_c);
        /* Write solid markers (typically into a sparse overlay volume). */
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
