/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "utils/color.h"
#include "xxhash.h"

#include <limits.h>

#define N TILE_SIZE

/*
 * Merge / painter-op LRU-ish caches (utils/cache.c). Values are entry counts
 * (cost 1 each). Larger sizes reduce thrashing with many layers/tiles; each
 * miss does heavy work. Override at compile time if needed, e.g.
 * -DVOLUME_MERGE_CACHE_SIZE=4096
 */
#ifndef VOLUME_OP_CACHE_SIZE
#   define VOLUME_OP_CACHE_SIZE 128
#endif
#ifndef VOLUME_TILE_MERGE_CACHE_SIZE
#   define VOLUME_TILE_MERGE_CACHE_SIZE 8192
#endif
#ifndef VOLUME_MERGE_CACHE_SIZE
#   define VOLUME_MERGE_CACHE_SIZE 2048
#endif

// Used for the cache.
static int volume_del(void *data_)
{
    volume_t *volume = data_;
    volume_delete(volume);
    return 0;
}

int volume_select(const volume_t *volume,
                const int start_pos[3],
                int (*cond)(void *user, const volume_t *volume,
                            const int base_pos[3],
                            const int new_pos[3],
                            volume_accessor_t *volume_accessor),
                void *user, volume_t *selection)
{
    int i, a;
    int pos[3], p[3];
    bool keep = true;
    volume_iterator_t iter;
    volume_accessor_t volume_accessor, selection_accessor;
    volume_clear(selection);

    volume_accessor = volume_get_accessor(volume);
    selection_accessor = volume_get_accessor(selection);

    if (!volume_get_alpha_at(volume, &volume_accessor, start_pos))
        return 0;
    volume_set_at(selection, &selection_accessor, start_pos,
                (uint8_t[]){255, 255, 255, 255});

    // XXX: Very inefficient algorithm!
    // Iter and test all the neighbors of the selection until there is
    // no more possible changes.
    while (keep) {
        keep = false;
        iter = volume_get_iterator(selection, VOLUME_ITER_VOXELS);
        while (volume_iter(&iter, pos)) {
            // Shouldn't be needed if the iter function did filter the voxels.
            if (!volume_get_alpha_at(selection, &selection_accessor, pos))
                continue;

            for (i = 0; i < 6; i++) {
                p[0] = pos[0] + FACES_NORMALS[i][0];
                p[1] = pos[1] + FACES_NORMALS[i][1];
                p[2] = pos[2] + FACES_NORMALS[i][2];
                if (volume_get_alpha_at(selection, &selection_accessor, p))
                    continue; // Already done.
                if (!volume_get_alpha_at(volume, &volume_accessor, p))
                    continue; // No voxel here.
                a = cond(user, volume, pos, p, &volume_accessor);
                if (a) {
                    volume_set_at(selection, &selection_accessor, p,
                                (uint8_t[]){255, 255, 255, a});
                    keep = true;
                }
            }
        }
    }
    return 0;
}

static inline int noise_tex_coord(int w);
void apply_noise_if_applicable(const painter_t *painter, float global_p[3],
                               uint8_t col[4]);

// XXX: need to redo this function from scratch.  Even the API is a bit
// stupid.
void volume_extrude(volume_t *volume,
                  const float plane[4][4],
                  const float box[4][4],
                  const painter_t *painter,
                  const volume_t *inherit_from)
{
    float proj[4][4];
    float n[3], pos[3], p[3], global_p[3];
    volume_iterator_t iter;
    int vpos[3];
    uint8_t value[4];
    int axis = -1;

    vec3_normalize(plane[2], n);
    vec3_copy(plane[3], pos);

    // Extrude faces are axis-aligned; pick the dominant normal axis.
    if (fabs(n[0]) > 0.5) axis = 0;
    else if (fabs(n[1]) > 0.5) axis = 1;
    else if (fabs(n[2]) > 0.5) axis = 2;

    // Generate the projection into the plane.
    // XXX: *very* ugly code, fix this!
    mat4_set_identity(proj);

    if (fabs(plane[2][0]) > 0.1) {
        proj[0][0] = 0;
        proj[3][0] = pos[0];
    }
    if (fabs(plane[2][1]) > 0.1) {
        proj[1][1] = 0;
        proj[3][1] = pos[1];
    }
    if (fabs(plane[2][2]) > 0.1) {
        proj[2][2] = 0;
        proj[3][2] = pos[2];
    }

    volume_accessor_t accessor_get = volume_get_accessor(volume);
    volume_accessor_t accessor_set = volume_get_accessor(volume);
    volume_accessor_t inherit_acc = {0};
    if (inherit_from)
        inherit_acc = volume_get_accessor(inherit_from);
    iter = volume_get_box_iterator(volume, box, 0);
    while (volume_iter(&iter, vpos)) {
        vec3_set(p, vpos[0], vpos[1], vpos[2]);
        if (!bbox_contains_vec(box, p)) {
            memset(value, 0, 4);
        } else {
            mat4_mul_vec3(proj, p, p);
            int pi[3] = {floor(p[0]), floor(p[1]), floor(p[2])};
            volume_get_at(volume, &accessor_get, pi, value);
            // Inherit colours from blocks met along the extrusion column.
            if (inherit_from && value[3] && axis >= 0 &&
                    (pi[0] != vpos[0] || pi[1] != vpos[1] || pi[2] != vpos[2])) {
                int dist = abs(vpos[axis] - pi[axis]);
                int step = (vpos[axis] > pi[axis]) ? 1 : -1;
                int check[3] = {pi[0], pi[1], pi[2]};
                uint8_t met[4];
                int s;
                for (s = 1; s <= dist; s++) {
                    check[axis] = pi[axis] + s * step;
                    volume_get_at(inherit_from, &inherit_acc, check, met);
                    if (met[3])
                        memcpy(value, met, 4);
                }
            }
            // Only vary newly extruded voxels, not the source face.
            if (painter && value[3] &&
                    (pi[0] != vpos[0] || pi[1] != vpos[1] || pi[2] != vpos[2])) {
                vec3_set(global_p,
                         (float)noise_tex_coord(vpos[0]),
                         (float)noise_tex_coord(vpos[1]),
                         (float)noise_tex_coord(vpos[2]));
                apply_noise_if_applicable(painter, global_p, value);
            }
        }
        volume_set_at(volume, &accessor_set, vpos, value);
    }

}

static void volume_fill(
        volume_t *volume,
        const float box[4][4],
        void (*get_color)(const int pos[3], uint8_t out[4], void *user_data),
        void *user_data)
{
    int pos[3];
    uint8_t color[4];
    volume_iterator_t iter;
    volume_accessor_t accessor;

    volume_clear(volume);
    accessor = volume_get_accessor(volume);
    iter = volume_get_box_iterator(volume, box, 0);
    while (volume_iter(&iter, pos)) {
        get_color(pos, color, user_data);
        volume_set_at(volume, &accessor, pos, color);
    }
}

static void volume_move_get_color(const int pos[3], uint8_t c[4], void *user)
{
    float p[3] = {pos[0], pos[1], pos[2]};
    volume_t *volume = USER_GET(user, 0);
    float (*mat)[4][4] = USER_GET(user, 1);
    mat4_mul_vec3(*mat, p, p);
    int pi[3] = {round(p[0]), round(p[1]), round(p[2])};
    volume_get_at(volume, NULL, pi, c);
}

void volume_move(volume_t *volume, const float mat[4][4])
{
    float box[4][4];
    volume_t *src_volume = volume_copy(volume);
    float imat[4][4];

    mat4_invert(mat, imat); // Invert transformation matrix
    volume_get_box(volume, true, box); // Get bbox
    if (box_is_null(box)) return;
    mat4_mul(mat, box, box); // Apply transformation to bbox
    volume_fill(volume, box, volume_move_get_color,
                USER_PASS(src_volume, &imat)); // Fill volume with transformed data
    volume_delete(src_volume); // Delete copy
    volume_remove_empty_tiles(volume, false);
}

void volume_blit(volume_t *volume, const uint8_t *data,
               int x, int y, int z, int w, int h, int d,
               volume_iterator_t *iter)
{
    volume_iterator_t default_iter = {0};
    int pos[3];
    if (!iter) iter = &default_iter;
    for (pos[2] = z; pos[2] < z + d; pos[2]++)
    for (pos[1] = y; pos[1] < y + h; pos[1]++)
    for (pos[0] = x; pos[0] < x + w; pos[0]++) {
        volume_set_at(volume, iter, pos, data);
        data += 4;
    }
    volume_remove_empty_tiles(volume, false);
}

void volume_write_aabb_from_buffer(volume_t *volume, const uint8_t *buffer,
                                   const int aabb[2][3])
{
    int size[3];
    int pos[3];
    int volume_pos[3];
    size_t buffer_offset;
    volume_iterator_t iter = {0};
    const uint8_t empty[4] = {0, 0, 0, 0};

    size[0] = aabb[1][0] - aabb[0][0];
    size[1] = aabb[1][1] - aabb[0][1];
    size[2] = aabb[1][2] - aabb[0][2];

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++)
    for (pos[1] = 0; pos[1] < size[1]; pos[1]++)
    for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
        buffer_offset = 4 * ((size_t)pos[2] * size[0] * size[1] +
                             pos[1] * size[0] + pos[0]);
        volume_pos[0] = aabb[0][0] + pos[0];
        volume_pos[1] = aabb[0][1] + pos[1];
        volume_pos[2] = aabb[0][2] + pos[2];
        if (buffer[buffer_offset + 3]) {
            volume_set_at(volume, &iter, volume_pos, &buffer[buffer_offset]);
        } else if (volume_get_alpha_at(volume, &iter, volume_pos)) {
            volume_set_at(volume, &iter, volume_pos, empty);
        }
    }
    volume_remove_empty_tiles(volume, false);
}

void volume_shift_alpha(volume_t *volume, int v)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t value[4];

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, value);
        value[3] = clamp(value[3] + v, 0, 255);
        volume_set_at(volume, NULL, pos, value);
    }
}

// Multiply two colors together.
static void color_mul(const uint8_t a[4], const uint8_t b[4],
                      uint8_t out[4])
{
    out[0] = (int)a[0] * b[0] / 255;
    out[1] = (int)a[1] * b[1] / 255;
    out[2] = (int)a[2] * b[2] / 255;
    out[3] = (int)a[3] * b[3] / 255;
}

// XXX: cleanup this: in fact we might not need that many modes!
void voxel_combine(const uint8_t a[4], const uint8_t b[4], int mode,
                   uint8_t out[4])
{
    int i, aa = a[3], ba = b[3];
    uint8_t ret[4];
    memcpy(ret, a, 4);
    if (mode == MODE_PAINT) {
        ret[0] = mix(a[0], b[0], ba / 255.);
        ret[1] = mix(a[1], b[1], ba / 255.);
        ret[2] = mix(a[2], b[2], ba / 255.);
    }
    else if (mode == MODE_OVER) {
        if (255 * ba + aa * (255 - ba)) {
            for (i = 0; i < 3; i++) {
                ret[i] = (255 * b[i] * ba + a[i] * aa * (255 - ba)) /
                         (255 * ba + aa * (255 - ba));
            }
        }
        ret[3] = ba + aa * (255 - ba) / 255;
    }
    else if (mode == MODE_SUB) {
        ret[3] = max(0, aa - ba);
    }
    else if (mode == MODE_MAX) {
        ret[0] = b[0];
        ret[1] = b[1];
        ret[2] = b[2];
        ret[3] = max(a[3], b[3]);
    } else if (mode == MODE_SUB_CLAMP) {
        ret[0] = a[0];
        ret[1] = a[1];
        ret[2] = a[2];
        ret[3] = min(aa, 255 - ba);
    } else if (mode == MODE_MULT_ALPHA) {
        ret[0] = ret[0] * ba / 255;
        ret[1] = ret[1] * ba / 255;
        ret[2] = ret[2] * ba / 255;
        ret[3] = ret[3] * ba / 255;
    } else if (mode == MODE_INTERSECT) {
        ret[3] = min(aa, ba);
    } else if (mode == MODE_INTERSECT_FILL) {
        ret[3] = min(aa, ba);
        if (ret[3]) {
            ret[0] = b[0];
            ret[1] = b[1];
            ret[2] = b[2];
        }
    } else {
        assert(false);
    }
    memcpy(out, ret, 4);
}

/* Context for color_inherit lookups within one volume_op / surface stamp.
 * Caller fetches goxel_get_layers_volume once; do not call it per voxel. */
typedef struct color_beneath_ctx {
    const volume_t *volume;
    volume_iterator_t iter;
    int lowest_z;
    /* Column memo: first solid at/below empty_top is at found_z with color. */
    bool memo_valid;
    int memo_x, memo_y;
    int memo_empty_top;
    int memo_found_z;
    uint8_t memo_color[4];
} color_beneath_ctx_t;

static void color_beneath_ctx_init(color_beneath_ctx_t *ctx)
{
    float box[4][4];

    memset(ctx, 0, sizeof(*ctx));
    ctx->volume = goxel_get_layers_volume(goxel.image);
    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(ctx->volume, true, box);
    ctx->lowest_z = (int)(box[3][2] - box[2][2]);
}

/** Beginning at start_pos, burrow downwards until a block is found. */
static void get_color_beneath(color_beneath_ctx_t *ctx, const int start_pos[3],
                              uint8_t *out)
{
    uint8_t color[4] = {0};
    int z, pos[3];
    int start_z = start_pos[2];

    pos[0] = start_pos[0];
    pos[1] = start_pos[1];

    if (ctx->memo_valid &&
            ctx->memo_x == pos[0] && ctx->memo_y == pos[1] &&
            start_z >= ctx->memo_found_z && start_z <= ctx->memo_empty_top) {
        memcpy(out, ctx->memo_color, 4);
        return;
    }

    /* Extend an existing column memo upward when possible. */
    if (ctx->memo_valid &&
            ctx->memo_x == pos[0] && ctx->memo_y == pos[1] &&
            start_z > ctx->memo_empty_top) {
        for (z = start_z; z > ctx->memo_empty_top; z--) {
            pos[2] = z;
            volume_get_at(ctx->volume, &ctx->iter, pos, color);
            if (color[3] != 0) {
                ctx->memo_empty_top = start_z;
                ctx->memo_found_z = z;
                memcpy(ctx->memo_color, color, 4);
                memcpy(out, color, 4);
                return;
            }
        }
        ctx->memo_empty_top = start_z;
        memcpy(out, ctx->memo_color, 4);
        return;
    }

    for (z = start_z; z >= ctx->lowest_z; z--) {
        pos[2] = z;
        volume_get_at(ctx->volume, &ctx->iter, pos, color);
        if (color[3] != 0)
            break;
    }

    ctx->memo_valid = true;
    ctx->memo_x = pos[0];
    ctx->memo_y = pos[1];
    ctx->memo_empty_top = start_z;
    ctx->memo_found_z = z;
    memcpy(ctx->memo_color, color, 4);
    memcpy(out, color, 4);
}

static inline int noise_tex_coord(int w)
{
    int m = w % NOISE_TEXTURE_SIZE;
    if (m < 0)
        m += NOISE_TEXTURE_SIZE;
    return m;
}

static inline int wrap_tex_coord(int v, int size)
{
    int m;
    if (size <= 0) return 0;
    m = v % size;
    if (m < 0) m += size;
    return m;
}

static bool brush_sample_texture_color(const int vp[3], uint8_t out[4])
{
    const brush_texture_t *tex = goxel_brush_texture_current();
    int x, y, idx, bpp;
    if (!tex || !tex->pixels || tex->w <= 0 || tex->h <= 0)
        return false;
    x = wrap_tex_coord(vp[0], tex->w);
    y = wrap_tex_coord(vp[1], tex->h);
    bpp = tex->bpp > 0 ? tex->bpp : 4;
    idx = (y * tex->w + x) * bpp;
    out[0] = tex->pixels[idx + 0];
    out[1] = tex->pixels[idx + 1];
    out[2] = tex->pixels[idx + 2];
    out[3] = (bpp >= 4) ? tex->pixels[idx + 3] : 255;
    srgb8_adjust_hsl(out, goxel.brush_texture_hue,
                     goxel.brush_texture_saturation,
                     goxel.brush_texture_lightness);
    return true;
}

void apply_noise_if_applicable(const painter_t* painter, float global_p[3], uint8_t col[4]) {
    if (painter->noise_enabled != 0 && painter->noise_intensity != 0 && painter->noise_coverage != 0) {
        float noise_value = uniform_noise(global_p[0], global_p[1], global_p[2]);
        int noise_col[3];
        noise_col[0] = col[0];
        noise_col[1] = col[1];
        noise_col[2] = col[2];
        if (noise_value < (float)painter->noise_coverage / 100.0f) {
            blend_with_noise_alpha(noise_col, noise_value, (float)painter->noise_intensity, (float)painter->noise_saturation, noise_col);
            col[0] = noise_col[0];
            col[1] = noise_col[1];
            col[2] = noise_col[2];
        }

        // // Apply coverage: skip voxels outside the noise coverage range
        // if (noise_value > (float)painter->noise_coverage / 100.0f) {
        //     //LOG_D("Skipped");
        // } else {
        //     // Adjust noise intensity and saturation
        //     float noise_factor = (float)painter->noise_intensity / 100.0f * noise_value;
        //     //LOG_D("Noise factor: %f", noise_factor);
        //     col[0] = (uint8_t)clamp(col[0] + noise_factor * painter->noise_saturation, 0.0f, 255.0f);
        //     col[1] = (uint8_t)clamp(col[1] + noise_factor * painter->noise_saturation, 0.0f, 255.0f);
        //     col[2] = (uint8_t)clamp(col[2] + noise_factor * painter->noise_saturation, 0.0f, 255.0f);
        //     //col[3] = (uint8_t)clamp(col[3] * (1.0f - noise_factor), 0.0f, 255.0f);
        // }
    }
}

static bool brush_surface_voxel_is_solid(const uint8_t color[4])
{
    return color[3] != 0;
}

static bool brush_surface_pos_in_aabb(const int pos[3], const int start_pos[3],
                                      const int dimensions[3])
{
    return pos[0] >= start_pos[0] && pos[0] < start_pos[0] + dimensions[0] &&
           pos[1] >= start_pos[1] && pos[1] < start_pos[1] + dimensions[1] &&
           pos[2] >= start_pos[2] && pos[2] < start_pos[2] + dimensions[2];
}

static bool brush_surface_column_membership(const painter_t *painter,
                                            const float center[3],
                                            float radius_x, float radius_y,
                                            int x, int y, float *out_alpha)
{
    float dx = (x + 0.5f) - center[0];
    float dy = (y + 0.5f) - center[1];
    float k;
    float v;

    /* 2D cross sections of the shape SDFs in src/shape.c, so the signed
     * distance (and therefore the AA band width) is in voxels like volume_op. */
    if (painter->shape == &shape_cube) {
        float ratio = INFINITY;
        k = INFINITY;
        if (dx != 0) {
            ratio = radius_x / fabsf(dx);
            k = radius_x - fabsf(dx);
        }
        if (dy != 0 && radius_y / fabsf(dy) < ratio) {
            k = radius_y - fabsf(dy);
        }
    } else {
        float d = sqrtf(dx * dx + dy * dy);
        if (d == 0) {
            k = max(radius_x, radius_y);
        } else {
            /* Ellipse radius along (dx, dy), matching sphere_func. */
            float ex = radius_y * dx / d;
            float ey = radius_x * dy / d;
            float r = radius_x * radius_y / sqrtf(ex * ex + ey * ey);
            k = r - d;
        }
    }

    if (painter->dithering > 0) {
        k += (uniform_noise((float)x, (float)y, center[2]) * 2.f - 1.f) *
             painter->dithering;
    }
    if (painter->smoothness > 0) {
        v = clamp(k / painter->smoothness, -1.0f, 1.0f) / 2.0f + 0.5f;
    } else {
        v = (k >= 0.f) ? 1.f : 0.f;
    }
    if (v <= 0.f) return false;

    *out_alpha = v;
    return true;
}

static bool brush_surface_is_exposed(const volume_t *src,
                                     volume_iterator_t *src_iter,
                                     const int pos[3],
                                     const int start_pos[3],
                                     const int dimensions[3])
{
    static const int offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1},
    };
    uint8_t cur[4], neigh[4];
    int i, npos[3];

    volume_get_at(src, src_iter, pos, cur);
    if (!brush_surface_voxel_is_solid(cur))
        return false;

    for (i = 0; i < 6; i++) {
        npos[0] = pos[0] + offsets[i][0];
        npos[1] = pos[1] + offsets[i][1];
        npos[2] = pos[2] + offsets[i][2];
        if (!brush_surface_pos_in_aabb(npos, start_pos, dimensions))
            continue;
        volume_get_at(src, src_iter, npos, neigh);
        if (!brush_surface_voxel_is_solid(neigh))
            return true;
    }
    return false;
}

void volume_brush_surface_stamp(volume_t *dst, const volume_t *src,
                                const painter_t *painter,
                                const float center[3],
                                float radius_x, float radius_y,
                                int mode)
{
    int start_pos[3], dimensions[3];
    float box[4][4];
    float band;
    int min_x, max_x, min_y, max_y;
    int x, y, z;
    int pos[3];
    bool seen_solid;
    uint8_t src_voxel[4], dst_voxel[4], paint_voxel[4], new_voxel[4];
    volume_iterator_t src_iter = {0};
    volume_accessor_t dst_accessor;
    color_beneath_ctx_t inherit_ctx;

    if (!src || !dst) return;

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(src, true, box);
    box_get_start_pos(box, start_pos);
    box_get_dimensions(box, dimensions);
    if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] <= 0)
        return;

    // The AA / dither band straddles the nominal edge, so grow the footprint
    // like volume_op grows its iteration box; otherwise the outward half of
    // the feather is never visited and the edge only blurs inwards.
    band = painter->smoothness + painter->dithering;

    min_x = max(start_pos[0], (int)floorf(center[0] - radius_x - band));
    max_x = min(start_pos[0] + dimensions[0] - 1,
                (int)ceilf(center[0] + radius_x + band));
    min_y = max(start_pos[1], (int)floorf(center[1] - radius_y - band));
    max_y = min(start_pos[1] + dimensions[1] - 1,
                (int)ceilf(center[1] + radius_y + band));
    if (min_x > max_x || min_y > max_y)
        return;

    if (painter->color_inherit)
        color_beneath_ctx_init(&inherit_ctx);

    dst_accessor = volume_get_accessor(dst);
    for (x = min_x; x <= max_x; x++) {
        for (y = min_y; y <= max_y; y++) {
            float edge_alpha = 0.f;
            float global_p[3];
            if (!brush_surface_column_membership(painter, center, radius_x, radius_y,
                                                 x, y, &edge_alpha))
                continue;

            seen_solid = false;
            for (z = start_pos[2] + dimensions[2] - 1; z >= start_pos[2]; z--) {
                pos[0] = x; pos[1] = y; pos[2] = z;
                volume_get_at(src, &src_iter, pos, src_voxel);
                if (!brush_surface_voxel_is_solid(src_voxel)) {
                    if (seen_solid)
                        break;
                    continue;
                }
                seen_solid = true;
                if (!brush_surface_is_exposed(src, &src_iter, pos,
                                              start_pos, dimensions)) {
                    break;
                }
                memcpy(paint_voxel, painter->color, 4);
                if (goxel.tool && goxel.tool->id == TOOL_BRUSH &&
                        goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE &&
                        brush_sample_texture_color(pos, paint_voxel)) {
                    paint_voxel[3] = ((int)paint_voxel[3] * (int)painter->color[3]) / 255;
                } else if (goxel.tool && goxel.tool->id == TOOL_BRUSH &&
                           goxel.brush_source_mode == BRUSH_SOURCE_PALETTE &&
                           goxel_brush_palette_sample_at(pos, paint_voxel)) {
                    if (painter->mode == MODE_PAINT)
                        paint_voxel[3] = ((int)paint_voxel[3] *
                                          (int)painter->color[3]) / 255;
                } else if (painter->color_inherit) {
                    get_color_beneath(&inherit_ctx, pos, paint_voxel);
                }
                if (!(goxel.tool && goxel.tool->id == TOOL_BRUSH &&
                      (goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE ||
                       goxel.brush_source_mode == BRUSH_SOURCE_PALETTE))) {
                    vec3_set(global_p,
                             (float)noise_tex_coord(pos[0]),
                             (float)noise_tex_coord(pos[1]),
                             (float)noise_tex_coord(pos[2]));
                    apply_noise_if_applicable(painter, global_p, paint_voxel);
                }
                paint_voxel[3] = (uint8_t)((float)paint_voxel[3] * edge_alpha);
                if (paint_voxel[3] == 0)
                    continue;
                volume_get_at(dst, &dst_accessor, pos, dst_voxel);
                voxel_combine(dst_voxel, paint_voxel, mode, new_voxel);
                if (!vec4_equal(dst_voxel, new_voxel))
                    volume_set_at(dst, &dst_accessor, pos, new_voxel);
            }
        }
    }
}

void volume_op(volume_t *volume, const painter_t *painter, const float box[4][4])
{   
    // box[1][0] = 1/2 x size
    // box[2][1] = 1/2 y size
    // box[0][2] = 1/2 z size

    int i, vp[3];
    uint8_t value[4], new_value[4], c[4];
    volume_iterator_t iter;
    volume_accessor_t accessor;
    float size[3], p[3], global_p[3];
    float mat[4][4];
    float (*shape_func)(const float[3], const float[3], float smoothness);
    float k, v;
    int mode = painter->mode;
    bool use_box, skip_src_empty, skip_dst_empty;
    painter_t painter2;
    float box2[4][4];
    int aabb[2][3];
    volume_t *cached;
    static cache_t *cache = NULL;
    const float *sym_o = painter->symmetry_origin;

    // Check if the operation has been cached.
    if (!cache) cache = cache_create(VOLUME_OP_CACHE_SIZE);
    struct {
        uint64_t  id;
        float     box[4][4];
        painter_t painter;
        /* Texture brush state is outside painter_t; include it so cache
         * does not reuse stamps after switching texture / HSL. */
        int       brush_source_mode;
        int       brush_texture_index;
        float     brush_texture_hue;
        float     brush_texture_saturation;
        float     brush_texture_lightness;
        uint32_t  brush_palette_fp;
    } key;
    memset(&key, 0, sizeof(key));
    key.id = volume_get_key(volume);
    mat4_copy(box, key.box);
    key.painter = *painter;
    key.brush_source_mode = goxel.brush_source_mode;
    key.brush_texture_index = goxel.brush_texture_index;
    key.brush_texture_hue = goxel.brush_texture_hue;
    key.brush_texture_saturation = goxel.brush_texture_saturation;
    key.brush_texture_lightness = goxel.brush_texture_lightness;
    key.brush_palette_fp = goxel_brush_palette_fingerprint();
    cached = cache_get(cache, &key, sizeof(key));
    if (cached) {
        volume_set(volume, cached);
        return;
    }

    if (painter->symmetry) {
        painter2 = *painter;
        for (i = 0; i < 3; i++) {
            if (!(painter->symmetry & (1 << i))) continue;
            painter2.symmetry &= ~(1 << i);
            mat4_set_identity(box2);
            mat4_itranslate(box2, +sym_o[0], +sym_o[1], +sym_o[2]);
            if (i == 0) mat4_iscale(box2, -1,  1,  1);
            if (i == 1) mat4_iscale(box2,  1, -1,  1);
            if (i == 2) mat4_iscale(box2,  1,  1, -1);
            mat4_itranslate(box2, -sym_o[0], -sym_o[1], -sym_o[2]);
            mat4_imul(box2, box);
            volume_op(volume, &painter2, box2);
        }
    }

    shape_func = painter->shape->func;
    box_get_size(box, size);
    mat4_copy(box, mat);
    mat4_iscale(mat, 1 / size[0], 1 / size[1], 1 / size[2]);
    mat4_invert(mat, mat);
    use_box = painter->box && !box_is_null(*painter->box);
    skip_src_empty = mode == MODE_SUB ||
                     mode == MODE_SUB_CLAMP ||
                     mode == MODE_MULT_ALPHA;
    skip_dst_empty = mode == MODE_SUB ||
                     mode == MODE_SUB_CLAMP ||
                     mode == MODE_MULT_ALPHA ||
                     mode == MODE_INTERSECT ||
                     mode == MODE_INTERSECT_FILL;

    // for intersection start by deleting all the tiles that are not in
    // the box.
    if (mode == MODE_INTERSECT || mode == MODE_INTERSECT_FILL) {
        iter = volume_get_iterator(volume, VOLUME_ITER_TILES);
        while (volume_iter(&iter, vp)) {
            volume_get_tile_aabb(vp, aabb);
            if (box_intersect_aabb(box, aabb)) continue;
            volume_clear_tile(volume, &iter, vp);
        }
    }

    // Soft AA and dithering scatter extend outside the hard shape;
    // grow the iteration box so those voxels are visited.  Cubes need a
    // finite SDF band of the same width (they return ±inf outside it).
    float iter_box[4][4];
    float shape_sm = painter->smoothness + painter->dithering;
    mat4_copy(box, iter_box);
    if (shape_sm > 0) {
        for (i = 0; i < 3; i++) {
            float n = vec3_norm(iter_box[i]);
            if (n > 1e-6f)
                vec3_mul(iter_box[i], (n + shape_sm) / n, iter_box[i]);
        }
    }
    // INTERSECT keeps tiles that only partially overlap the shape box.
    // Iterating only the box would skip voxels in those tiles that lie
    // outside it, leaving a TILE_SIZE strip of extras (e.g. copy/cut into
    // placer). Visit every remaining voxel so outside ones are cleared.
    if (mode == MODE_INTERSECT || mode == MODE_INTERSECT_FILL) {
        iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS |
                (skip_dst_empty ? VOLUME_ITER_SKIP_EMPTY : 0));
    } else {
        iter = volume_get_box_iterator(volume, iter_box,
                skip_dst_empty ? VOLUME_ITER_SKIP_EMPTY : 0);
    }

    // XXX: for the moment we cannot use the same accessor for both
    // setting and getting!  Need to fix that!!
    accessor = volume_get_accessor(volume);

    color_beneath_ctx_t inherit_ctx;
    if (painter->color_inherit)
        color_beneath_ctx_init(&inherit_ctx);

    // For every tile in the volume, iterate
    while (volume_iter(&iter, vp)) {
        vec3_set(p, vp[0] + 0.5, vp[1] + 0.5, vp[2] + 0.5);
        vec3_set(global_p,
                 (float)noise_tex_coord(vp[0]),
                 (float)noise_tex_coord(vp[1]),
                 (float)noise_tex_coord(vp[2]));
        if (use_box && !bbox_contains_vec(*painter->box, p)) continue;
        mat4_mul_vec3(mat, p, p);
        k = shape_func(p, size, shape_sm);
        // Randomly displace the SDF boundary so edges dither/scatter.
        if (painter->dithering > 0) {
            float n = uniform_noise((float)vp[0], (float)vp[1], (float)vp[2]);
            k += (n * 2.f - 1.f) * painter->dithering;
        }
        if (painter->smoothness) {
            v = clamp(k / painter->smoothness, -1.0f, 1.0f) / 2.0f + 0.5f;
        } else {
            v = (k >= 0.f) ? 1.f : 0.f;
        }
        if (!v && skip_src_empty) continue;

        // Apply colours
        uint8_t col[4];
        memcpy(col, painter->color, 4);
        if (goxel.tool && goxel.tool->id == TOOL_BRUSH &&
                goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE &&
                brush_sample_texture_color(vp, col)) {
            // Apply shared brush opacity to sampled texture alpha.
            col[3] = ((int)col[3] * (int)painter->color[3]) / 255;
        } else if (goxel.tool && goxel.tool->id == TOOL_BRUSH &&
                   goxel.brush_source_mode == BRUSH_SOURCE_PALETTE &&
                   goxel_brush_palette_sample_at(vp, col)) {
            if (painter->mode == MODE_PAINT)
                col[3] = ((int)col[3] * (int)painter->color[3]) / 255;
        } else if (painter->color_inherit) {
            get_color_beneath(&inherit_ctx, vp, col);
        }

        // Texture / palette mode should not inherit hidden color-noise settings.
        if (!(goxel.tool && goxel.tool->id == TOOL_BRUSH &&
              (goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE ||
               goxel.brush_source_mode == BRUSH_SOURCE_PALETTE))) {
            apply_noise_if_applicable(painter, global_p, col);
        }
        
        memcpy(c, col, 4);

        c[3] *= v;
            //LOG_D("C: %i/%i/%i", c[0], c[1], c[2]);
        if (!c[3] && skip_src_empty) continue;
        // volume = tool volume, value = color at point in tool volume
        volume_get_at(volume, &accessor, vp, value);
        if (!value[3] && skip_dst_empty) continue;
            //LOG_D("Value: %i/%i/%i, C: %i/%i/%i", value[0], value[1], value[2], c[0], c[1], c[2]);
        voxel_combine(value, c, mode, new_value);
            //LOG_D("new_value: %i/%i/%i", new_value[0], new_value[1], new_value[2]);
        if (!vec4_equal(value, new_value)) {
            volume_set_at(volume, &accessor, vp, new_value);
        }
    }

    cache_add(cache, &key, sizeof(key), volume_copy(volume), 1, volume_del);
}

// XXX: remove this function!
void volume_get_box(const volume_t *volume, bool exact, float box[4][4])
{
    int bbox[2][3];
    volume_get_bbox(volume, bbox, exact);
    bbox_from_aabb(box, bbox);
}

void box_get_dimensions(float box[4][4], int dimensions[3]) {
    dimensions[0] = box[0][0] * 2; // volume width
    dimensions[1] = box[1][1] * 2; // volume height
    dimensions[2] = box[2][2] * 2; // volume depth
}

void box_get_start_pos(float box[4][4], int start_pos[3]) {
    start_pos[0] = box[3][0] - box[0][0];   // x starting position
    start_pos[1] = box[3][1] - box[1][1];   // y starting position
    start_pos[2] = box[3][2] - box[2][2];   // z starting position
}

void volume_get_dimensions(const volume_t *volume, int dimensions[3]) {
    float box[4][4];
    volume_get_box(volume, true, box);
    box_get_dimensions(box, dimensions);
}

void volume_get_start_pos(const volume_t *volume, int start_pos[3]) {
    float box[4][4];
    volume_get_box(volume, true, box);
    box_get_start_pos(box, start_pos);
}

void allocate_heights(int dimensions[3], int **heights) {
    *heights = (int *)malloc(dimensions[0] * dimensions[1] * sizeof(int));

    for (int i = 0; i < dimensions[0] * dimensions[1]; i++) {
        (*heights)[i] = -1;
    }
}
void volume_get_heights(const volume_t *volume, int* heights) {
    float box[4][4];
    int dimensions[3], start_pos[3];
    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);
    volume_get_heights_in_box(volume, dimensions, start_pos, heights);
}
void volume_get_heights_in_box(const volume_t *volume, int dimensions[3], int start_pos[3], int* heights)
{
    // We assume heights has already been instatiated using allocate_heights above
    int x, y, z, pos[3];
    uint8_t cur_block_color[4];
    volume_iterator_t iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    
    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            pos[0] = x + start_pos[0];
            pos[1] = y + start_pos[1];
            // Start from top and go down
            for (z = dimensions[2]; z >= 0; z--) {
                pos[2] = z + start_pos[2];
                // Get the block color at the position; if has alpha, is a block
                volume_get_at(volume, &iter, pos, cur_block_color);
                if (cur_block_color[3] != 0) {
                    heights[y * dimensions[0] + x] = z;
                    break;
                }
            }
        }
    }
}

// for brush, volume = tool_volume, other = brush volume
static void tile_merge(volume_t *volume, const volume_t *other, const int pos[3],
                        int mode, const uint8_t color[4])
{
    int p[3];
    int x, y, z;
    uint64_t id1, id2;
    volume_t *tile;
    uint8_t v1[4], v2[4];
    static cache_t *cache = NULL;
    volume_accessor_t a1, a2, a3;

    volume_get_tile_data(volume,  NULL, pos, &id1);
    volume_get_tile_data(other, NULL, pos, &id2);

    // XXX: cleanup this code!

    if (    (mode == MODE_OVER ||
             mode == MODE_MAX ||
             mode == MODE_SUB ||
             mode == MODE_SUB_CLAMP) && id2 == 0)
    {
        return;
    }

    if ((mode == MODE_OVER || mode == MODE_MAX) && id1 == 0 && !color) {
        volume_copy_tile(other, pos, volume, pos);
        return;
    }

    if ((mode == MODE_MULT_ALPHA) && id1 == 0) return;
    if ((mode == MODE_MULT_ALPHA) && id2 == 0) {
        // XXX: could just delete the tile.
    }

    // Check if the merge op has been cached.
    if (!cache) cache = cache_create(VOLUME_TILE_MERGE_CACHE_SIZE);
    struct {
        uint64_t id1;
        uint64_t id2;
        int      mode;
        uint8_t  color[4];
    } key = { id1, id2, mode };
    if (color) memcpy(key.color, color, 4);
    _Static_assert(sizeof(key) == 24, "");
    tile = cache_get(cache, &key, sizeof(key));
    if (tile) goto end;

    tile = volume_new();
    a1 = volume_get_accessor(volume);
    a2 = volume_get_accessor(other);
    a3 = volume_get_accessor(tile);

    for (z = 0; z < N; z++)
    for (y = 0; y < N; y++)
    for (x = 0; x < N; x++) {
        p[0] = pos[0] + x;
        p[1] = pos[1] + y;
        p[2] = pos[2] + z;
        //uint8_t ov1[4], ov2[4];
        volume_get_at(volume, &a1, p, v1);
        //volume_get_at(volume, &a1, p, ov1);
        volume_get_at(other, &a2, p, v2);
        //volume_get_at(other, &a2, p, ov2);
        // When a color is not given, v1 is blank and v2 is from the tool
        // When a color is given, v1 is blank, and v2 becomes the paint color * colour in tool
        if (color) color_mul(v2, color, v2);
        //if (!vec4_equal(v1, v2)) {
            // LOG_D("Pos: %i/%i/%i", pos[0], pos[1], pos[2]);
            // LOG_D("V1: %i/%i/%i, V2: %i/%i/%i", v1[0], v1[1], v1[2], v2[0], v2[1], v2[2]);
            // LOG_D("OV1: %i/%i/%i, OV2: %i/%i/%i", ov1[0], ov1[1], ov1[2], ov2[0], ov2[1], ov2[2]);
        //}
        voxel_combine(v1, v2, mode, v1);
        volume_set_at(tile, &a3, (int[]){x, y, z}, v1);
    }
    cache_add(cache, &key, sizeof(key), tile, 1, volume_del);

end:
    volume_copy_tile(tile, (int[]){0, 0, 0}, volume, pos);
    return;
}

void volume_merge(volume_t *volume, const volume_t *other, int mode,
                const uint8_t color[4])
{
    volume_t *cached;
    assert(volume && other);
    static cache_t *cache = NULL;
    volume_iterator_t iter;
    int bpos[3];
    uint64_t id1, id2;

    // Simple case for replace.
    if (mode == MODE_REPLACE) {
        volume_set(volume, other);
        return;
    }

    // Check if the merge op has been cached.
    if (!cache) cache = cache_create(VOLUME_MERGE_CACHE_SIZE);
    id1 = volume_get_key(volume);
    id2 = volume_get_key(other);
    struct {
        uint64_t id1;
        uint64_t id2;
        int      mode;
        uint8_t  color[4];
    } key = { id1, id2, mode };
    if (color) memcpy(key.color, color, 4);
    _Static_assert(sizeof(key) == 24, "");
    cached = cache_get(cache, &key, sizeof(key));
    if (cached) {
        volume_set(volume, cached);
        return;
    }

    iter = volume_get_union_iterator(volume, other, VOLUME_ITER_TILES);
    while (volume_iter(&iter, bpos)) {
        tile_merge(volume, other, bpos, mode, color);
    }

    cache_add(cache, &key, sizeof(key), volume_copy(volume), 1, volume_del);
}

void volume_merge_from(volume_t *volume, const volume_t *other, int mode,
                       const uint8_t color[4])
{
    volume_iterator_t iter;
    int bpos[3];

    assert(volume && other);
    if (mode == MODE_REPLACE) {
        volume_set(volume, other);
        return;
    }

    iter = volume_get_iterator(other, VOLUME_ITER_TILES);
    while (volume_iter(&iter, bpos)) {
        tile_merge(volume, other, bpos, mode, color);
    }
}

void volume_merge_sparse_from(volume_t *volume, const volume_t *other, int mode)
{
    volume_iterator_t iter;
    volume_accessor_t accessor;
    int pos[3];
    uint8_t src[4], dst[4], out[4];

    assert(volume && other);
    iter = volume_get_iterator(other,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    accessor = volume_get_accessor(volume);
    while (volume_iter(&iter, pos)) {
        volume_get_at(other, &iter, pos, src);
        if (!src[3]) continue;
        volume_get_at(volume, &accessor, pos, dst);
        voxel_combine(dst, src, mode, out);
        if (!vec4_equal(dst, out))
            volume_set_at(volume, &accessor, pos, out);
    }
}

void volume_crop(volume_t *volume, const float box[4][4])
{
    painter_t painter = {
        .mode = MODE_INTERSECT,
        .color = {255, 255, 255, 255},
        .shape = &shape_cube,
    };
    volume_op(volume, &painter, box);
}

/* Function: volume_crc32
 * Compute the crc32 of the volume data as an array of xyz rgba values.
 *
 * This is only used in the tests, to make sure that we can still open
 * old file formats.
 */
uint32_t volume_crc32(const volume_t *volume)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    uint32_t ret = 0;
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!v[3]) continue;
        ret = XXH32(pos, sizeof(pos), ret);
        ret = XXH32(v, sizeof(v), ret);
    }
    return ret;
}

bool layer_is_volume(const layer_t *layer)
{
    if (!layer) return false;
    return !layer->base_id && !layer->image && !layer->shape;
}

void do_move(volume_t *volume, float box[4][4], float mat[4][4], const float trans[4][4],
                    const float origin_[3], bool layer_is_volume, bool only_origin)
{
    /*
     * Note: for voxel volume layers, rotation and scale are only
     * applied to the voxels, without modifying the layer transformation
     * matrix.  For translation we modify the matrix (so that the origin
     * is moved) but we also modify the voxels because we want all the layer
     * volume to stay aligned.
     */

    float m[4][4] = MAT4_IDENTITY;
    float origin[3];

    if (mat4_equal(trans, mat4_identity)) return;

    vec3_copy(origin_ ?:mat[3], origin);

    // Make sure we always center on a grid point.
    origin[0] = floor(mat[3][0]) + 0.5;
    origin[1] = floor(mat[3][1]) + 0.5;
    origin[2] = floor(mat[3][2]) + 0.5;

    // Change referential to the volume origin.
    // XXX: maybe this should be done in volume_move directy??
    mat4_itranslate(m, +origin[0], +origin[1], +origin[2]);
    mat4_imul(m, trans);
    mat4_itranslate(m, -origin[0], -origin[1], -origin[2]);

    if (!layer_is_volume) {
        mat4_mul(m, mat, mat);
    } else {
        // Only apply translation to the layer->mat.
        vec3_add(mat[3], trans[3], mat[3]);

        if (!only_origin) {
            volume_move(volume, m);
            // Update bounding box if there is one
            if (!box_is_null(box)) {
                mat4_mul(m, box, box);
                box_get_bbox(box, box);
            }
        }
    }
}

void do_move_layer(layer_t *layer, const float mat[4][4],
                    const float origin_[3], bool only_origin) {
    bool is_volume = layer_is_volume(layer);
    /* Identity no-ops must not dirty clones: image_update rematerializes
     * via volume_move whenever base_volume_key is cleared. */
    if (mat4_equal(mat, mat4_identity)) return;
    do_move(layer->volume, layer->box, layer->mat, mat, origin_, is_volume, only_origin);
    if (!is_volume) {
        layer->base_volume_key = 0; // Mark it as dirty.
    }
}