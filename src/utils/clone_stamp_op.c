/* Clone stamp volume apply — independent of volume_op. */

#include "goxel.h"
#include "utils/clone_stamp_op.h"

static void shape_box_setup(const float box[4][4], float mat[4][4],
                            float size[3])
{
    box_get_size(box, size);
    mat4_copy(box, mat);
    mat4_iscale(mat, 1 / size[0], 1 / size[1], 1 / size[2]);
    mat4_invert(mat, mat);
}

static float shape_coverage(const shape_t *shape, const float mat[4][4],
                            const float size[3], float smoothness,
                            const int vp[3])
{
    float p[3], k, v;

    vec3_set(p, vp[0] + 0.5f, vp[1] + 0.5f, vp[2] + 0.5f);
    mat4_mul_vec3(mat, p, p);
    k = shape->func(p, size, smoothness);
    if (smoothness) {
        v = clamp(k / smoothness, -1.0f, 1.0f) / 2.0f + 0.5f;
    } else {
        v = (k >= 0.f) ? 1.f : 0.f;
    }
    return v;
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

static const clone_stamp_sample_t *sample_opts_or_default(
        const clone_stamp_sample_t *opts, clone_stamp_sample_t *tmp)
{
    if (opts) return opts;
    tmp->take_uppermost = true;
    tmp->depth = 0;
    return tmp;
}

/*
 * Resolve the voxel copied from in column (x, y).
 * Returns false if no solid in range.  Writes colour and position when found.
 */
static bool find_sample_voxel(const volume_t *sample, volume_accessor_t *acc,
                              int x, int y, int source_z, int lowest_z,
                              const clone_stamp_sample_t *opts,
                              uint8_t out_color[4], int out_pos[3])
{
    int z, z_hi, z_lo, pos[3];
    uint8_t color[4];

    pos[0] = x;
    pos[1] = y;

    if (opts->take_uppermost) {
        z_hi = source_z;
        z_lo = lowest_z;
    } else {
        int depth = max(0, opts->depth);
        z_hi = source_z + depth;
        z_lo = source_z - depth;
        if (z_lo < lowest_z) z_lo = lowest_z;
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

/* True if column (x,y) intersects the brush shape somewhere in the box AABB. */
static bool column_in_brush(const shape_t *shape, const float mat[4][4],
                            const float size[3], float smoothness,
                            const int aabb[2][3], int x, int y)
{
    int z, vp[3];

    vp[0] = x;
    vp[1] = y;
    for (z = aabb[0][2]; z < aabb[1][2]; z++) {
        vp[2] = z;
        if (shape_coverage(shape, mat, size, smoothness, vp) > 0.f)
            return true;
    }
    return false;
}

void clone_stamp_apply(volume_t *dest, const volume_t *sample,
                       const float target[3], const float source[3],
                       const float box[4][4], const shape_t *shape,
                       float smoothness,
                       const clone_stamp_sample_t *opts)
{
    volume_iterator_t iter;
    volume_accessor_t dest_acc, sample_acc;
    float mat[4][4], size[3], v;
    int vp[3], off[2], source_z, lowest_z;
    uint8_t dest_c[4], sample_c[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape);
    shape_box_setup(box, mat, size);

    off[0] = (int)floor(source[0]) - (int)floor(target[0]);
    off[1] = (int)floor(source[1]) - (int)floor(target[1]);
    source_z = (int)floor(source[2]);
    lowest_z = sample_lowest_z(sample);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);
    iter = volume_get_box_iterator(dest, box, VOLUME_ITER_SKIP_EMPTY);

    while (volume_iter(&iter, vp)) {
        v = shape_coverage(shape, mat, size, smoothness, vp);
        if (v <= 0.f) continue;

        volume_get_at(dest, &dest_acc, vp, dest_c);
        if (!dest_c[3]) continue;

        if (!find_sample_voxel(sample, &sample_acc,
                               vp[0] + off[0], vp[1] + off[1],
                               source_z, lowest_z, o, sample_c, NULL))
            continue;

        sample_c[3] = (uint8_t)(sample_c[3] * v);
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
    int aabb[2][3], x, y, source_z, lowest_z, sample_pos[3];
    uint8_t sample_c[4], dest_c[4], paint[4], out[4];
    clone_stamp_sample_t opts_tmp;
    const clone_stamp_sample_t *o = sample_opts_or_default(opts, &opts_tmp);

    assert(dest && sample && shape && marker_color);
    shape_box_setup(box, mat, size);
    box_get_aabb(box, aabb);

    source_z = (int)floor(source[2]);
    lowest_z = sample_lowest_z(sample);

    dest_acc = volume_get_accessor(dest);
    sample_acc = volume_get_accessor(sample);

    /* One highlight per XY column in the brush — on the exact sampled block. */
    for (y = aabb[0][1]; y < aabb[1][1]; y++)
    for (x = aabb[0][0]; x < aabb[1][0]; x++) {
        if (!column_in_brush(shape, mat, size, smoothness, aabb, x, y))
            continue;
        if (!find_sample_voxel(sample, &sample_acc, x, y, source_z, lowest_z,
                               o, sample_c, sample_pos))
            continue;

        volume_get_at(dest, &dest_acc, sample_pos, dest_c);
        if (!dest_c[3]) continue;

        memcpy(paint, marker_color, 4);
        voxel_combine(dest_c, paint, MODE_PAINT, out);
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
        v = shape_coverage(shape, mat, size, smoothness, vp);
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
