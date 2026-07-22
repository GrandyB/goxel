/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#include "goxel.h"
#include "metadata.h"
#include "metadata/internal.h"

#include <math.h>
#include <string.h>

static bool g_editor_active = false;
static custom_object_t *g_list_selected = NULL;
static custom_object_t *g_solo_obj = NULL;

typedef struct {
    int state; /* 0 idle, 1 hover, 2 drag */
    custom_object_t *obj;
    int face;
    int mode; /* 0 move, 1 resize */
    int start_p0[3], start_p1[3];
    float start_box[4][4];
    float drag_plane[4][4];
    float box[4][4];
} edit_data_t;

static edit_data_t g_edit = {};

static void edit_reset(void)
{
    g_edit.state = 0;
    g_edit.obj = NULL;
}

void custom_objects_clear_edit_refs(const custom_object_t *obj)
{
    if (!obj) return;
    if (g_edit.obj == obj)
        edit_reset();
    if (g_list_selected == obj)
        g_list_selected = NULL;
    if (g_solo_obj == obj)
        g_solo_obj = NULL;
}

void custom_objects_on_list_freed(custom_object_t *list)
{
    custom_object_t *obj;
    DL_FOREACH(list, obj)
        custom_objects_clear_edit_refs(obj);
}

void custom_objects_set_editor_active(bool active)
{
    g_editor_active = active;
    if (!active) {
        edit_reset();
        g_list_selected = NULL;
        g_solo_obj = NULL;
    }
}

void custom_objects_set_list_selected(custom_object_t *obj)
{
    g_list_selected = obj;
}

custom_object_t *custom_objects_get_list_selected(void)
{
    return g_list_selected;
}

void custom_objects_toggle_solo(custom_object_t *obj)
{
    if (g_solo_obj == obj)
        g_solo_obj = NULL;
    else
        g_solo_obj = obj;
}

custom_object_t *custom_objects_get_solo(void)
{
    return g_solo_obj;
}

static bool face_allowed(const custom_object_t *obj, int face)
{
    /* Skip top/bottom (faces with Z normals: FACES_NORMALS 2 and 3) for 2D. */
    if (obj->type == CUSTOM_OBJ_POINT_2D || obj->type == CUSTOM_OBJ_ZONE_2D) {
        if (face == 2 || face == 3) return false;
    }
    /* Points: no resize faces needed; all allowed for move except 2D Z. */
    return face >= 0;
}

static float box_volume_approx(const float box[4][4])
{
    return fabsf(box[0][0] * box[1][1] * box[2][2]) * 8.f;
}

static void render_face_gizmo(const float box[4][4], int face)
{
    uint8_t color[4] = {255, 0, 0, 16};
    float face_plane[4][4], a[3], b[3], dir[3];
    mat4_mul(box, FACES_MATS[face], face_plane);
    mat4_iscale(face_plane, 2, 2, 1);
    mat4_itranslate(face_plane, 0, 0, 0.001);
    render_rect_fill(&goxel.rend, face_plane, color);
    vec3_normalize(face_plane[2], dir);
    vec3_copy(face_plane[3], a);
    vec3_addk(a, dir, 3, b);
    color[3] = 255;
    render_line(&goxel.rend, a, b, color, EFFECT_ARROW);
}

static void apply_move(custom_object_t *obj, int dx, int dy, int dz)
{
    if (obj->type == CUSTOM_OBJ_POINT_2D) {
        obj->p0[0] += dx;
        obj->p0[1] += dy;
    } else if (obj->type == CUSTOM_OBJ_POINT_3D) {
        obj->p0[0] += dx;
        obj->p0[1] += dy;
        obj->p0[2] += dz;
    } else if (obj->type == CUSTOM_OBJ_ZONE_2D) {
        obj->p0[0] += dx;
        obj->p0[1] += dy;
        obj->p1[0] += dx;
        obj->p1[1] += dy;
    } else {
        obj->p0[0] += dx;
        obj->p0[1] += dy;
        obj->p0[2] += dz;
        obj->p1[0] += dx;
        obj->p1[1] += dy;
        obj->p1[2] += dz;
    }
}

static void box_to_object_coords(custom_object_t *obj, const float box[4][4],
                                 const image_t *img)
{
    int aabb[2][3];
    int z0, z1;
    bbox_to_aabb(box, aabb);
    image_z_range(img, &z0, &z1);

    if (obj->type == CUSTOM_OBJ_POINT_2D) {
        obj->p0[0] = aabb[0][0];
        obj->p0[1] = aabb[0][1];
    } else if (obj->type == CUSTOM_OBJ_POINT_3D) {
        obj->p0[0] = aabb[0][0];
        obj->p0[1] = aabb[0][1];
        obj->p0[2] = aabb[0][2];
    } else if (obj->type == CUSTOM_OBJ_ZONE_2D) {
        obj->p0[0] = aabb[0][0];
        obj->p0[1] = aabb[0][1];
        obj->p1[0] = aabb[1][0] - 1;
        obj->p1[1] = aabb[1][1] - 1;
        obj->p0[2] = z0;
        obj->p1[2] = z1;
        normalize_corners(obj->p0, obj->p1);
        obj->p0[2] = z0;
        obj->p1[2] = z1;
    } else {
        obj->p0[0] = aabb[0][0];
        obj->p0[1] = aabb[0][1];
        obj->p0[2] = aabb[0][2];
        obj->p1[0] = aabb[1][0] - 1;
        obj->p1[1] = aabb[1][1] - 1;
        obj->p1[2] = aabb[1][2] - 1;
        normalize_corners(obj->p0, obj->p1);
    }
}

void custom_objects_edit_iter(const float viewport[4])
{
    image_t *img = goxel.image;
    custom_object_t *obj, *best = NULL;
    cursor_t *curs = &goxel.cursor;
    float hit[3], n[3], box[4][4];
    float best_vol = INFINITY;
    int face = -1, best_face = -1;
    bool pressed = curs->flags & CURSOR_PRESSED;
    bool shift = curs->flags & CURSOR_SHIFT;
    camera_t *cam;
    float opos[3], onorm[3], wpos[3];

    if (!g_editor_active || !img || !img->custom_objects_show) {
        g_edit.state = 0;
        g_edit.obj = NULL;
        return;
    }

    cam = img->active_camera;
    if (!cam) return;

    /* Continue drag. */
    if (g_edit.state == 2 && g_edit.obj) {
        float pos[3], d[3], ofs[3], nrm[3], face_plane[4][4];
        float new_box[4][4];

        wpos[0] = curs->xy[0];
        wpos[1] = curs->xy[1];
        wpos[2] = 0;
        camera_get_ray(cam, wpos, viewport, opos, onorm);
        if (!plane_line_intersection(g_edit.drag_plane, opos, onorm, pos))
            goto drag_end_check;
        /* pos is in plane local; convert to world. */
        {
            float local[3] = {pos[0], pos[1], 0};
            mat4_mul_vec3(g_edit.drag_plane, local, pos);
        }
        pos[0] = roundf(pos[0]);
        pos[1] = roundf(pos[1]);
        pos[2] = roundf(pos[2]);

        mat4_mul(g_edit.start_box, FACES_MATS[g_edit.face], face_plane);
        vec3_normalize(face_plane[2], nrm);

        if (g_edit.mode == 1 &&
            (g_edit.obj->type == CUSTOM_OBJ_ZONE_2D ||
             g_edit.obj->type == CUSTOM_OBJ_ZONE_3D)) {
            box_move_face(g_edit.start_box, g_edit.face, pos, new_box);
            if (box_get_volume(new_box) == 0) goto drag_end_check;
            if (g_edit.obj->type == CUSTOM_OBJ_ZONE_2D) {
                /* Keep full image height. */
                int aabb[2][3], z0, z1;
                bbox_to_aabb(new_box, aabb);
                image_z_range(img, &z0, &z1);
                aabb[0][2] = z0;
                aabb[1][2] = z1 + 1;
                bbox_from_aabb(new_box, aabb);
            }
            box_to_object_coords(g_edit.obj, new_box, img);
        } else {
            vec3_add(g_edit.start_box[3], face_plane[2], d);
            vec3_sub(pos, d, ofs);
            vec3_project(ofs, nrm, ofs);
            /* Reset from start each frame to avoid drift. */
            memcpy(g_edit.obj->p0, g_edit.start_p0, sizeof(g_edit.start_p0));
            memcpy(g_edit.obj->p1, g_edit.start_p1, sizeof(g_edit.start_p1));
            apply_move(g_edit.obj,
                       (int)roundf(ofs[0]),
                       (int)roundf(ofs[1]),
                       (int)roundf(ofs[2]));
        }
        custom_object_get_box(img, g_edit.obj, box);
        render_face_gizmo(box, g_edit.face);
        goxel_set_help_text(g_edit.mode ? "Drag to resize" : "Drag to move");

drag_end_check:
        if (!pressed) {
            g_edit.state = 0;
            g_edit.obj = NULL;
        }
        return;
    }

    /* Hit-test: prefer smallest volume (points over large zones).
     * Walk newest-first so clones / later objects win ties (on top). */
    DL_FOREACH_REVERSE(img->custom_objects, obj) {
        float vol;
        if (!custom_object_effectively_visible(obj)) continue;
        if (!custom_object_is_spatial(obj->type)) continue;
        custom_object_get_box(img, obj, box);
        if (!box_unproject(cam, viewport, curs->xy, box, false, hit, n, &face))
            continue;
        if (!face_allowed(obj, face)) continue;
        vol = box_volume_approx(box);
        if (obj->type == CUSTOM_OBJ_POINT_2D || obj->type == CUSTOM_OBJ_POINT_3D)
            vol *= 0.01f; /* Prefer points. */
        if (vol < best_vol) {
            best_vol = vol;
            best = obj;
            best_face = face;
            mat4_copy(box, g_edit.box);
            vec3_copy(hit, curs->pos);
            vec3_copy(n, curs->normal);
        }
    }

    if (!best) {
        g_edit.state = 0;
        g_edit.obj = NULL;
        return;
    }

    g_edit.obj = best;
    g_edit.face = best_face;
    g_edit.state = 1;
    render_face_gizmo(g_edit.box, best_face);

    if (best->type == CUSTOM_OBJ_ZONE_2D || best->type == CUSTOM_OBJ_ZONE_3D)
        goxel_set_help_text(shift ? "Drag to move (Shift)" : "Drag to resize");
    else
        goxel_set_help_text("Drag to move");

    if (pressed) {
        float face_plane[4][4], v[3];
        g_list_selected = best;
        /* Zones: default resize; Shift to move. Points: always move. */
        g_edit.mode = (!shift &&
            (best->type == CUSTOM_OBJ_ZONE_2D ||
             best->type == CUSTOM_OBJ_ZONE_3D)) ? 1 : 0;
        image_history_push(img);
        g_edit.obj = best;
        g_edit.face = best_face;
        g_edit.state = 2;
        memcpy(g_edit.start_p0, best->p0, sizeof(best->p0));
        memcpy(g_edit.start_p1, best->p1, sizeof(best->p1));
        mat4_copy(g_edit.box, g_edit.start_box);
        mat4_mul(g_edit.box, FACES_MATS[best_face], face_plane);
        vec3_normalize(face_plane[0], v);
        plane_from_vectors(g_edit.drag_plane, curs->pos, curs->normal, v);
    }
}
