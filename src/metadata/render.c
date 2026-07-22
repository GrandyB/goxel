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

#include <string.h>

static void render_2d_point(renderer_t *rend, const image_t *img,
                            const custom_object_t *obj, const uint8_t rcolor[4])
{
    int z0, z1;
    float a[3], b[3];
    uint8_t color[4];

    image_z_range(img, &z0, &z1);
    memcpy(color, rcolor, 4);
    color[3] = 255;
    a[0] = obj->p0[0] + 0.5f;
    a[1] = obj->p0[1] + 0.5f;
    a[2] = (float)z0;
    b[0] = a[0];
    b[1] = a[1];
    b[2] = (float)(z1 + 1);
    /* Approximate thickness with a few parallel lines. */
    render_line(rend, a, b, color, 0);
    a[0] += 0.1f; b[0] += 0.1f;
    render_line(rend, a, b, color, 0);
    a[0] -= 0.2f; b[0] -= 0.2f;
    render_line(rend, a, b, color, 0);
}

static void render_3d_point(renderer_t *rend, const custom_object_t *obj,
                            const uint8_t color[4])
{
    float box[4][4];
    float pos[3] = {obj->p0[0] + 0.5f, obj->p0[1] + 0.5f, obj->p0[2] + 0.5f};
    /* Same style as selection / move origin markers. */
    bbox_from_extents(box, pos, 0.5f, 0.5f, 0.5f);
    render_box(rend, box, color, EFFECT_STRIP | EFFECT_WIREFRAME);
}

static void render_2d_zone(renderer_t *rend, const image_t *img,
                           const custom_object_t *obj, const uint8_t rcolor[4])
{
    int z0, z1, x0, y0, x1, y1, i;
    float a[3], b[3], box[4][4], face_plane[4][4];
    uint8_t color[4];
    /* Side faces only (skip top/bottom = 2, 3). */
    const int sides[4] = {0, 1, 4, 5};

    image_z_range(img, &z0, &z1);
    x0 = obj->p0[0] < obj->p1[0] ? obj->p0[0] : obj->p1[0];
    y0 = obj->p0[1] < obj->p1[1] ? obj->p0[1] : obj->p1[1];
    x1 = obj->p0[0] > obj->p1[0] ? obj->p0[0] : obj->p1[0];
    y1 = obj->p0[1] > obj->p1[1] ? obj->p0[1] : obj->p1[1];
    memcpy(color, rcolor, 4);

    /* Translucent square on each vertical side. */
    custom_object_get_box(img, obj, box);
    color[3] = (uint8_t)(0.1f * 255);
    for (i = 0; i < 4; i++) {
        mat4_mul(box, FACES_MATS[sides[i]], face_plane);
        mat4_iscale(face_plane, 2, 2, 1);
        mat4_itranslate(face_plane, 0, 0, 0.001);
        render_rect_fill(rend, face_plane, color);
    }

    /* Four vertical edges. */
    color[3] = 255;
    a[0] = x0; a[1] = y0; a[2] = z0;
    b[0] = x0; b[1] = y0; b[2] = z1 + 1;
    render_line(rend, a, b, color, 0);
    a[0] = x1 + 1; a[1] = y0; b[0] = x1 + 1; b[1] = y0;
    render_line(rend, a, b, color, 0);
    a[0] = x1 + 1; a[1] = y1 + 1; b[0] = x1 + 1; b[1] = y1 + 1;
    render_line(rend, a, b, color, 0);
    a[0] = x0; a[1] = y1 + 1; b[0] = x0; b[1] = y1 + 1;
    render_line(rend, a, b, color, 0);
}

static void render_3d_zone(renderer_t *rend, const image_t *img,
                           const custom_object_t *obj, const uint8_t color[4])
{
    float box[4][4];
    (void)img;
    custom_object_get_box(img, obj, box);
    render_box(rend, box, color, EFFECT_STRIP | EFFECT_WIREFRAME);
}

void custom_objects_render(renderer_t *rend, const image_t *img)
{
    custom_object_t *obj;
    uint8_t color[4];

    if (!rend || !img || !img->custom_objects_show) return;
    DL_FOREACH(img->custom_objects, obj) {
        if (!custom_object_effectively_visible(obj)) continue;
        if (!custom_object_is_spatial(obj->type)) continue;
        custom_object_effective_color(obj, color);
        switch (obj->type) {
        case CUSTOM_OBJ_POINT_2D: render_2d_point(rend, img, obj, color); break;
        case CUSTOM_OBJ_POINT_3D: render_3d_point(rend, obj, color); break;
        case CUSTOM_OBJ_ZONE_2D:  render_2d_zone(rend, img, obj, color); break;
        case CUSTOM_OBJ_ZONE_3D:  render_3d_zone(rend, img, obj, color); break;
        default: break;
        }
    }
}
