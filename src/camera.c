/* Goxel 3D voxels editor
 *
 * copyright (c) 2016 Guillaume Chereau <guillaume@noctua-software.com>
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
#include "xxhash.h"

camera_t *camera_new(const char *name)
{
    camera_t *cam = calloc(1, sizeof(*cam));
    if (name)
        strncpy(cam->name, name, sizeof(cam->name) - 1);
    mat4_set_identity(cam->mat);
    cam->dist = 96;
    cam->aspect = 1;
    cam->fly_speed = 8.f;
    cam->player_speed = 2.5f;
    cam->fovy = 40.;
    cam->fovy_fpv = 100.;
    cam->mode = CAMERA_MODE_ORBIT;
    cam->standing_height = 2.7f;
    cam->crouch_height = 1.7f;
    cam->smoothing = 0.f;
    vec3_set(cam->player_vel, 0, 0, 0);
    mat4_itranslate(cam->mat, 0, 0, cam->dist);
    camera_turntable(cam, M_PI / 4, M_PI / 4);
    return cam;
}

void camera_delete(camera_t *cam)
{
    if (!cam) return;
    if (--cam->ref > 0) return;
    free(cam);
}

camera_t *camera_copy(const camera_t *other)
{
    camera_t *cam = malloc(sizeof(*cam));
    *cam = *other;
    cam->ref = 1;
    cam->next = cam->prev = NULL;
    return cam;
}

void camera_set(camera_t *cam, const camera_t *other)
{
    cam->ortho = other->ortho;
    cam->dist = other->dist;
    cam->mode = other->mode;
    cam->fly_speed = other->fly_speed;
    cam->player_speed = other->player_speed;
    cam->fovy = other->fovy;
    cam->fovy_fpv = other->fovy_fpv;
    cam->prev_dist = other->prev_dist;
    cam->prev_ortho = other->prev_ortho;
    cam->standing_height = other->standing_height;
    cam->crouch_height = other->crouch_height;
    cam->smoothing = other->smoothing;
    vec3_copy(other->player_vel, cam->player_vel);
    mat4_copy(other->mat, cam->mat);
}

static void compute_clip_tiles(
        const float view_mat[4][4], const volume_t *volume, float *near_, float *far_)
{
    int bpos[3];
    float p[3];
    float n = FLT_MAX, f = 256;
    const int margin = 8 * BLOCK_SIZE;
    volume_iterator_t iter;

    if (volume) {
        iter = volume_get_iterator(volume, VOLUME_ITER_TILES);
        while (volume_iter(&iter, bpos)) {
            vec3_set(p, bpos[0], bpos[1], bpos[2]);
            mat4_mul_vec3(view_mat, p, p);
            if (p[2] < 0) {
                n = min(n, -p[2] - margin);
                f = max(f, -p[2] + margin);
            }
        }
    }
    if (n >= f) n = 1;
    n = max(n, 0.1);
    *near_ = n;
    *far_ = f;
}

static void compute_clip(const float view_mat[4][4], float *near_, float *far_)
{
    int bpos[3];
    float p[3];
    float n = FLT_MAX, f = 256;
    int i;
    const int margin = 8 * BLOCK_SIZE;
    float vertices[8][3];
    const volume_t *volume = goxel_get_layers_volume(goxel.image);
    volume_iterator_t iter;

    if (!box_is_null(goxel.image->box)) {
        box_get_vertices(goxel.image->box, vertices);
        for (i = 0; i < 8; i++) {
            mat4_mul_vec3(view_mat, vertices[i], p);
            if (p[2] < 0) {
                n = min(n, -p[2] - margin);
                f = max(f, -p[2] + margin);
            }
        }
    }

    // If wrap preview is enabled, extend clipping for the 8 wrapped instances
    // around the image box (same offsets as the render path).
    if (goxel.wrap_view && !box_is_null(goxel.image->box)) {
        float half[3], full[3];
        float off[8][3];
        box_get_size(goxel.image->box, half);
        full[0] = half[0] * 2.0f;
        full[1] = half[1] * 2.0f;
        full[2] = half[2] * 2.0f;

        off[0][0] = -full[0]; off[0][1] =  0;       off[0][2] = 0;
        off[1][0] =  full[0]; off[1][1] =  0;       off[1][2] = 0;
        off[2][0] =  0;       off[2][1] = -full[1]; off[2][2] = 0;
        off[3][0] =  0;       off[3][1] =  full[1]; off[3][2] = 0;
        off[4][0] = -full[0]; off[4][1] = -full[1]; off[4][2] = 0;
        off[5][0] = -full[0]; off[5][1] =  full[1]; off[5][2] = 0;
        off[6][0] =  full[0]; off[6][1] = -full[1]; off[6][2] = 0;
        off[7][0] =  full[0]; off[7][1] =  full[1]; off[7][2] = 0;

        box_get_vertices(goxel.image->box, vertices);
        for (int oi = 0; oi < 8; oi++) {
            for (i = 0; i < 8; i++) {
                vec3_add(vertices[i], off[oi], p);
                mat4_mul_vec3(view_mat, p, p);
                if (p[2] < 0) {
                    n = min(n, -p[2] - margin);
                    f = max(f, -p[2] + margin);
                }
            }
        }
    }

    iter = volume_get_iterator(volume, VOLUME_ITER_TILES);
    while (volume_iter(&iter, bpos)) {
        vec3_set(p, bpos[0], bpos[1], bpos[2]);
        mat4_mul_vec3(view_mat, p, p);
        if (p[2] < 0) {
            n = min(n, -p[2] - margin);
            f = max(f, -p[2] + margin);
        }
    }
    if (n >= f) n = 1;
    n = max(n, 0.1);
    *near_ = n;
    *far_ = f;
}

void camera_update(camera_t *camera)
{
    float size;
    float clip_near, clip_far;

    mat4_invert(camera->mat, camera->view_mat);
    compute_clip(camera->view_mat, &clip_near, &clip_far);
    if (camera->ortho) {
        size = camera->dist;
        mat4_ortho(camera->proj_mat,
                -size, +size,
                -size / camera->aspect, +size / camera->aspect,
                clip_near, clip_far);
    } else {
        mat4_perspective(camera->proj_mat,
                camera_is_firstperson(camera) ? camera->fovy_fpv : camera->fovy,
                camera->aspect, clip_near, clip_far);
    }
}

void camera_update_for_volume(camera_t *camera, const volume_t *vol)
{
    float size;
    float clip_near, clip_far;

    mat4_invert(camera->mat, camera->view_mat);
    compute_clip_tiles(camera->view_mat, vol, &clip_near, &clip_far);
    if (camera->ortho) {
        size = camera->dist;
        mat4_ortho(camera->proj_mat,
                -size, +size,
                -size / camera->aspect, +size / camera->aspect,
                clip_near, clip_far);
    } else {
        mat4_perspective(camera->proj_mat,
                camera_is_firstperson(camera) ? camera->fovy_fpv : camera->fovy,
                camera->aspect, clip_near, clip_far);
    }
}

// Get the raytracing ray of the camera at a given screen position.
void camera_get_ray(const camera_t *camera, const float win[2],
                    const float viewport[4], float o[3], float d[3])
{
    float o1[3], o2[3], p[3];
    vec3_set(p, win[0], win[1], 0);
    unproject(p, camera->view_mat, camera->proj_mat, viewport, o1);
    vec3_set(p, win[0], win[1], 1);
    unproject(p, camera->view_mat, camera->proj_mat, viewport, o2);
    vec3_copy(o1, o);
    vec3_sub(o2, o1, d);
    vec3_normalize(d, d);
}

// Adjust the camera settings so that the rotation works for a given
// position.
void camera_set_target(camera_t *cam, const float pos[3])
{
    float world_to_mat[4][4], p[3];
    mat4_invert(cam->mat, world_to_mat);
    mat4_mul_vec3(world_to_mat, pos, p);
    cam->dist = -p[2];
}

/*
 * Function: camera_fit_box
 * Move a camera so that a given box is entirely visible.
 */
void camera_fit_box(camera_t *cam, const float box[4][4])
{
    float size[3], dist;
    if (box_is_null(box)) {
        cam->dist = 128;
        cam->aspect = 1;
        return;
    }
    box_get_size(box, size);
    // XXX: not the proper way to compute the distance.
    dist = max3(size[0], size[1], size[2]) * 8;
    mat4_mul_vec3(box, VEC(0, 0, 0), cam->mat[3]);
    mat4_itranslate(cam->mat, 0, 0, dist);
    cam->dist = dist;
}

/* Corner offset from lookat into camera local axes (look = -Z). */
static void camera_frame_local_offset(const camera_t *cam,
                                      const float lookat[3],
                                      const float world[3],
                                      float out[3])
{
    float d[3];
    vec3_sub(world, lookat, d);
    out[0] = d[0] * cam->mat[0][0] + d[1] * cam->mat[0][1] +
             d[2] * cam->mat[0][2];
    out[1] = d[0] * cam->mat[1][0] + d[1] * cam->mat[1][1] +
             d[2] * cam->mat[1][2];
    out[2] = d[0] * cam->mat[2][0] + d[1] * cam->mat[2][1] +
             d[2] * cam->mat[2][2];
}

/*
 * Function: camera_frame_box
 * Orbit around a box at a distance that fits all 8 corners in the current
 * frustum (keeps orientation). Uses per-axis projected extents so flat /
 * elongated boxes are not over-pulled like a bounding sphere. Under
 * perspective, recentres on the projected AABB so foreshortening does not
 * leave the subject sitting low in the view. No-op if box is null.
 */
void camera_frame_box(camera_t *cam, const float box[4][4])
{
    float center[3], lookat[3], verts[8][3], dist = 1.f;
    float half_fovy, tan_x, tan_y, aspect;
    int i, pass;

    if (!cam || box_is_null(box)) return;

    mat4_mul_vec3(box, VEC(0, 0, 0), center);
    vec3_copy(center, lookat);
    box_get_vertices(box, verts);

    aspect = max(cam->aspect, 0.01f);
    half_fovy = cam->fovy * 0.5f * (float)(M_PI / 180.0);
    if (half_fovy < 1e-3f) half_fovy = 1e-3f;
    tan_y = tanf(half_fovy);
    tan_x = tan_y * aspect;

    /* Pass 0: fit about the geometric centre, then pan lookat so the
     * projected AABB mid sits at the view centre. Pass 1: refit dist. */
    for (pass = 0; pass < 2; pass++) {
        dist = 1.f;
        for (i = 0; i < 8; i++) {
            float o[3], need;
            camera_frame_local_offset(cam, lookat, verts[i], o);
            if (cam->ortho) {
                need = max(fabsf(o[0]), fabsf(o[1]) * aspect);
            } else {
                need = max(o[2] + fabsf(o[0]) / tan_x,
                           o[2] + fabsf(o[1]) / tan_y);
            }
            if (need > dist) dist = need;
        }

        if (cam->ortho || pass == 1) break;

        {
            float min_nx = 1e9f, max_nx = -1e9f;
            float min_ny = 1e9f, max_ny = -1e9f;
            float mid_x, mid_y, shift_x, shift_y;

            for (i = 0; i < 8; i++) {
                float o[3], depth, nx, ny;
                camera_frame_local_offset(cam, lookat, verts[i], o);
                depth = dist - o[2];
                if (depth < 1e-3f) depth = 1e-3f;
                nx = o[0] / (depth * tan_x);
                ny = o[1] / (depth * tan_y);
                if (nx < min_nx) min_nx = nx;
                if (nx > max_nx) max_nx = nx;
                if (ny < min_ny) min_ny = ny;
                if (ny > max_ny) max_ny = ny;
            }
            mid_x = 0.5f * (min_nx + max_nx);
            mid_y = 0.5f * (min_ny + max_ny);
            shift_x = mid_x * dist * tan_x;
            shift_y = mid_y * dist * tan_y;
            lookat[0] = center[0] + shift_x * cam->mat[0][0] +
                        shift_y * cam->mat[1][0];
            lookat[1] = center[1] + shift_x * cam->mat[0][1] +
                        shift_y * cam->mat[1][1];
            lookat[2] = center[2] + shift_x * cam->mat[0][2] +
                        shift_y * cam->mat[1][2];
        }
    }

    /* Relative pad + a few voxels so small props keep breathing room
     * without pushing map-scale frames far out. */
    dist = dist * 1.12f + 2.5f;
    if (dist < 1.f) dist = 1.f;

    vec3_copy(lookat, cam->mat[3]);
    mat4_itranslate(cam->mat, 0, 0, dist);
    cam->dist = dist;
}

/*
 * Function: camera_get_key
 * Return a value that is guarantied to change when the camera change.
 */
uint32_t camera_get_key(const camera_t *cam)
{
    uint32_t key = 0;
    key = XXH32(&cam->name, sizeof(cam->name), key);
    key = XXH32(&cam->ortho, sizeof(cam->ortho), key);
    key = XXH32(&cam->dist, sizeof(cam->dist), key);
    key = XXH32(&cam->mat, sizeof(cam->mat), key);    
    key = XXH32(&cam->mode, sizeof(cam->mode), key);
    key = XXH32(&cam->fovy, sizeof(cam->fovy), key);
    key = XXH32(&cam->fovy_fpv, sizeof(cam->fovy_fpv), key);
    key = XXH32(&cam->standing_height, sizeof(cam->standing_height), key);
    key = XXH32(&cam->crouch_height, sizeof(cam->crouch_height), key);
    key = XXH32(&cam->smoothing, sizeof(cam->smoothing), key);
    return key;
}

void camera_turntable(camera_t *camera, float rz, float rx)
{
    float center[3], mat[4][4] = MAT4_IDENTITY;

    mat4_mul_vec3(camera->mat, VEC(0, 0, -camera->dist), center);   // center (target) = 'dist' units away from camera in current direction
    mat4_itranslate(mat, center[0], center[1], center[2]);          // move camera to the target
    mat4_irotate(mat, rz, 0, 0, 1);                                 // rotate the camera vertically around the center point
    mat4_itranslate(mat, -center[0], -center[1], -center[2]);
    mat4_imul(mat, camera->mat);
    mat4_copy(mat, camera->mat);

    mat4_itranslate(camera->mat, 0, 0, -camera->dist);
    mat4_irotate(camera->mat, rx, 1, 0, 0);
    mat4_itranslate(camera->mat, 0, 0, camera->dist);
}

void camera_turntable_around_point(
        camera_t *camera, float rz, float rx, const float pivot[3])
{
    float mat[4][4] = MAT4_IDENTITY;
    float axis[3];

    /* Yaw around world Z through the pivot (works off view-axis). */
    mat4_itranslate(mat, pivot[0], pivot[1], pivot[2]);
    mat4_irotate(mat, rz, 0, 0, 1);
    mat4_itranslate(mat, -pivot[0], -pivot[1], -pivot[2]);
    mat4_imul(mat, camera->mat);
    mat4_copy(mat, camera->mat);

    /* Pitch around camera right through the same pivot - not local-Z
     * translate-by-dist, which only orbits a point on the view axis. */
    vec3_normalize(camera->mat[0], axis);
    mat4_set_identity(mat);
    mat4_itranslate(mat, pivot[0], pivot[1], pivot[2]);
    mat4_irotate(mat, rx, axis[0], axis[1], axis[2]);
    mat4_itranslate(mat, -pivot[0], -pivot[1], -pivot[2]);
    mat4_imul(mat, camera->mat);
    mat4_copy(mat, camera->mat);

    /* dist is view-axis depth of the pivot (same as zoom / set_target). */
    camera_set_target(camera, pivot);
}

/* First person move
 * rz: up is +ve, down is -ve.
 * ry: forward is +ve, backwards is -ve.
 * rx - right is +ve, left is -ve.
 */
void camera_move(camera_t *cam, float rx, float ry, float rz, float speed)
{
    float mat[4][4];
    mat4_copy(cam->mat, mat);

    float multiplier = speed / 20;

    mat4_itranslate(mat, 0, 0, ry*multiplier);
    mat4_itranslate(mat, rx*multiplier, 0, 0);

    // in mat[4][4], camera x/y/z position is [3][0]/[3][1]/[3][2]
    // z is just up/down in world space
    mat[3][2] += rz*multiplier;

    mat4_copy(mat, cam->mat);
}

void camera_move_flat(camera_t *cam, float rx, float ry, float speed)
{
    float mult = speed / 20.f;
    float sx = cam->mat[0][0];
    float sy = cam->mat[0][1];
    float slen = sqrtf(sx * sx + sy * sy);
    float fx, fy;

    /* Yaw basis from camera right (stays in XY with Z-yaw / X-pitch). Local
     * +Z flattened matches upright mat[2].xy = (sy, -sx). */
    if (slen > 1e-6f) {
        sx /= slen;
        sy /= slen;
    } else {
        sx = 1.f;
        sy = 0.f;
    }
    fx = sy;
    fy = -sx;

    cam->mat[3][0] += (fx * ry + sx * rx) * mult;
    cam->mat[3][1] += (fy * ry + sy * rx) * mult;
}

void camera_move_blend(camera_t *cam, float rx, float ry, float rz,
                       float speed, float flat_amt)
{
    float save[4][4];
    float pl[3], z0;

    if (flat_amt <= 0.f) {
        camera_move(cam, rx, ry, rz, speed);
        return;
    }
    if (flat_amt >= 1.f) {
        camera_move_flat(cam, rx, ry, speed);
        return;
    }

    z0 = cam->mat[3][2];
    mat4_copy(cam->mat, save);
    camera_move(cam, rx, ry, rz * (1.f - flat_amt), speed);
    vec3_copy(cam->mat[3], pl);
    mat4_copy(save, cam->mat);
    camera_move_flat(cam, rx, ry, speed);

    cam->mat[3][0] = mix(pl[0], cam->mat[3][0], flat_amt);
    cam->mat[3][1] = mix(pl[1], cam->mat[3][1], flat_amt);
    cam->mat[3][2] = mix(pl[2], z0, flat_amt);
}

bool camera_is_firstperson(const camera_t *cam)
{
    return cam->mode == CAMERA_MODE_FPV || cam->mode == CAMERA_MODE_PLAYER;
}

bool camera_is_player(const camera_t *cam)
{
    return cam->mode == CAMERA_MODE_PLAYER;
}

/* Forward dolly amount so a subject at `orbit_depth` keeps the same size when
 * FOV goes from orbit `fovy` to wider `fovy_fpv`. Fly→orbit applies the inverse. */
static float camera_fov_dolly_delta(float orbit_depth, float fovy_deg, float fovy_fpv_deg)
{
    float t0, t1;
    if (orbit_depth <= 0.f) return 0.f;
    t0 = tanf(fovy_deg * (float)(M_PI / 180.0) * 0.5f);
    t1 = tanf(fovy_fpv_deg * (float)(M_PI / 180.0) * 0.5f);
    if (t1 < 1e-6f) return 0.f;
    return orbit_depth * (1.f - t0 / t1);
}

void camera_set_mode(camera_t *cam, camera_mode_t m)
{
    camera_mode_t old = cam->mode;
    if (old == m)
        return;
    if (old == CAMERA_MODE_PLAYER && m != CAMERA_MODE_PLAYER)
        vec3_set(cam->player_vel, 0, 0, 0);
    if (m == CAMERA_MODE_PLAYER && old != CAMERA_MODE_PLAYER)
        vec3_set(cam->player_vel, 0, 0, 0);

    if (old == CAMERA_MODE_ORBIT &&
        (m == CAMERA_MODE_FPV || m == CAMERA_MODE_PLAYER)) {
        float depth = cam->dist;
        bool was_ortho = cam->ortho;
        float delta;
        cam->prev_dist = cam->dist;
        cam->prev_ortho = cam->ortho;
        cam->ortho = false;
        /* Wider fovy_fpv would look zoomed-out at the same eye; dolly forward. */
        if (!was_ortho) {
            delta = camera_fov_dolly_delta(depth, cam->fovy, cam->fovy_fpv);
            if (delta != 0.f)
                mat4_itranslate(cam->mat, 0, 0, -delta);
        }
        cam->dist = 0;
    } else if ((old == CAMERA_MODE_FPV || old == CAMERA_MODE_PLAYER) &&
               m == CAMERA_MODE_ORBIT) {
        float delta;
        cam->dist = cam->prev_dist;
        cam->ortho = cam->prev_ortho;
        /* Exact inverse of the orbit→FPV dolly (same delta, opposite sign). */
        if (!cam->ortho) {
            delta = camera_fov_dolly_delta(cam->prev_dist, cam->fovy, cam->fovy_fpv);
            if (delta != 0.f)
                mat4_itranslate(cam->mat, 0, 0, +delta);
        }
    }
    cam->mode = m;
}
