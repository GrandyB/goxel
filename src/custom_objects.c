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
#include "custom_objects.h"
#include "utils/json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static bool g_editor_active = false;

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

static bool custom_object_name_exists(void *user, const char *name)
{
    image_t *img = user;
    custom_object_t *obj;
    DL_FOREACH(img->custom_objects, obj) {
        if (strncmp(obj->name, name, sizeof(obj->name)) == 0)
            return true;
    }
    return false;
}

const char *custom_object_type_name(custom_object_type_t type)
{
    switch (type) {
    case CUSTOM_OBJ_POINT_2D: return "2D Point";
    case CUSTOM_OBJ_POINT_3D: return "3D Point";
    case CUSTOM_OBJ_ZONE_2D:  return "2D Zone";
    case CUSTOM_OBJ_ZONE_3D:  return "3D Zone";
    case CUSTOM_OBJ_FLOAT:    return "Float";
    case CUSTOM_OBJ_TEXT:     return "Text";
    case CUSTOM_OBJ_COLOR:    return "Colour";
    case CUSTOM_OBJ_ENUM:     return "Enum";
    case CUSTOM_OBJ_GROUP:    return "Group";
    default: return "Unknown";
    }
}

bool custom_object_is_group(custom_object_type_t type)
{
    return type == CUSTOM_OBJ_GROUP;
}

int custom_object_depth(const custom_object_t *obj)
{
    int depth = 0;
    const custom_object_t *g;
    if (!obj) return 0;
    for (g = obj->group; g; g = g->group)
        depth++;
    return depth;
}

bool custom_object_effectively_visible(const custom_object_t *obj)
{
    const custom_object_t *g;
    if (!obj || !obj->visible) return false;
    for (g = obj->group; g; g = g->group) {
        if (!g->visible) return false;
    }
    return true;
}

void custom_object_effective_color(const custom_object_t *obj, uint8_t color[4])
{
    if (!obj) return;
    if (obj->group)
        memcpy(color, obj->group->color, 4);
    else
        memcpy(color, obj->color, 4);
}

static bool is_descendant_of(const custom_object_t *obj,
                             const custom_object_t *ancestor)
{
    const custom_object_t *g;
    if (!obj || !ancestor) return false;
    for (g = obj; g; g = g->group) {
        if (g == ancestor) return true;
    }
    return false;
}

static custom_object_t *last_in_group_subtree(custom_object_t *list,
                                              const custom_object_t *group)
{
    custom_object_t *obj, *last = (custom_object_t *)group;
    DL_FOREACH(list, obj) {
        if (obj == group || is_descendant_of(obj, group))
            last = obj;
    }
    return last;
}

bool custom_object_is_spatial(custom_object_type_t type)
{
    return type <= CUSTOM_OBJ_ZONE_3D;
}

static void image_z_range(const image_t *img, int *z0, int *z1)
{
    int start[3], dims[3];
    float box[4][4];
    if (box_is_null(img->box)) {
        *z0 = 0;
        *z1 = 31;
        return;
    }
    mat4_copy(img->box, box);
    box_get_start_pos(box, start);
    box_get_dimensions(box, dims);
    *z0 = start[2];
    *z1 = start[2] + dims[2] - 1;
    if (*z1 < *z0) *z1 = *z0;
}

static void image_center(const image_t *img, int out[3])
{
    int start[3], dims[3];
    float box[4][4];
    if (box_is_null(img->box)) {
        out[0] = out[1] = 0;
        out[2] = 16;
        return;
    }
    mat4_copy(img->box, box);
    box_get_start_pos(box, start);
    box_get_dimensions(box, dims);
    out[0] = start[0] + dims[0] / 2;
    out[1] = start[1] + dims[1] / 2;
    out[2] = start[2] + dims[2] / 2;
}

static void normalize_corners(int a[3], int b[3])
{
    int i, t;
    for (i = 0; i < 3; i++) {
        if (a[i] > b[i]) {
            t = a[i];
            a[i] = b[i];
            b[i] = t;
        }
    }
}

void custom_object_get_box(const image_t *img, const custom_object_t *obj,
                           float box[4][4])
{
    int aabb[2][3];
    int z0, z1;
    int x0, y0, x1, y1, zz0, zz1;

    if (!custom_object_is_spatial(obj->type)) {
        mat4_copy(mat4_zero, box);
        return;
    }

    image_z_range(img, &z0, &z1);

    switch (obj->type) {
    case CUSTOM_OBJ_POINT_2D:
        aabb[0][0] = obj->p0[0];
        aabb[0][1] = obj->p0[1];
        aabb[0][2] = z0;
        aabb[1][0] = obj->p0[0] + 1;
        aabb[1][1] = obj->p0[1] + 1;
        aabb[1][2] = z1 + 1;
        break;
    case CUSTOM_OBJ_POINT_3D:
        aabb[0][0] = obj->p0[0];
        aabb[0][1] = obj->p0[1];
        aabb[0][2] = obj->p0[2];
        aabb[1][0] = obj->p0[0] + 1;
        aabb[1][1] = obj->p0[1] + 1;
        aabb[1][2] = obj->p0[2] + 1;
        break;
    case CUSTOM_OBJ_ZONE_2D:
        x0 = obj->p0[0] < obj->p1[0] ? obj->p0[0] : obj->p1[0];
        y0 = obj->p0[1] < obj->p1[1] ? obj->p0[1] : obj->p1[1];
        x1 = obj->p0[0] > obj->p1[0] ? obj->p0[0] : obj->p1[0];
        y1 = obj->p0[1] > obj->p1[1] ? obj->p0[1] : obj->p1[1];
        aabb[0][0] = x0;
        aabb[0][1] = y0;
        aabb[0][2] = z0;
        aabb[1][0] = x1 + 1;
        aabb[1][1] = y1 + 1;
        aabb[1][2] = z1 + 1;
        break;
    case CUSTOM_OBJ_ZONE_3D:
    default:
        x0 = obj->p0[0] < obj->p1[0] ? obj->p0[0] : obj->p1[0];
        y0 = obj->p0[1] < obj->p1[1] ? obj->p0[1] : obj->p1[1];
        zz0 = obj->p0[2] < obj->p1[2] ? obj->p0[2] : obj->p1[2];
        x1 = obj->p0[0] > obj->p1[0] ? obj->p0[0] : obj->p1[0];
        y1 = obj->p0[1] > obj->p1[1] ? obj->p0[1] : obj->p1[1];
        zz1 = obj->p0[2] > obj->p1[2] ? obj->p0[2] : obj->p1[2];
        aabb[0][0] = x0;
        aabb[0][1] = y0;
        aabb[0][2] = zz0;
        aabb[1][0] = x1 + 1;
        aabb[1][1] = y1 + 1;
        aabb[1][2] = zz1 + 1;
        break;
    }
    bbox_from_aabb(box, aabb);
}

void custom_objects_free_list(custom_object_t **list)
{
    custom_object_t *obj, *tmp;
    if (!list) return;
    if (*list && g_edit.obj) {
        DL_FOREACH(*list, obj) {
            if (obj == g_edit.obj) {
                edit_reset();
                break;
            }
        }
    }
    DL_FOREACH_SAFE(*list, obj, tmp) {
        DL_DELETE(*list, obj);
        free(obj);
    }
    *list = NULL;
}

void custom_objects_copy_list(custom_object_t **dst, const custom_object_t *src)
{
    const custom_object_t *obj;
    custom_object_t *copy, **copies = NULL;
    int count = 0, i, j;

    custom_objects_free_list(dst);
    DL_COUNT(src, obj, count);
    if (count <= 0) return;

    copies = calloc(count, sizeof(*copies));
    if (!copies) return;

    i = 0;
    DL_FOREACH(src, obj) {
        copy = calloc(1, sizeof(*copy));
        *copy = *obj;
        copy->next = copy->prev = NULL;
        copy->group = NULL;
        copies[i++] = copy;
        DL_APPEND(*dst, copy);
    }

    i = 0;
    DL_FOREACH(src, obj) {
        if (obj->group) {
            const custom_object_t *g;
            j = 0;
            DL_FOREACH(src, g) {
                if (g == obj->group) {
                    copies[i]->group = copies[j];
                    break;
                }
                j++;
            }
        }
        i++;
    }
    free(copies);
}

static void init_enum_defaults(custom_object_t *obj)
{
    obj->enum_index = 0;
    obj->enum_option_count = 3;
    snprintf(obj->enum_options[0], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 1");
    snprintf(obj->enum_options[1], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 2");
    snprintf(obj->enum_options[2], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 3");
}

static void init_value_fields(custom_object_t *obj)
{
    switch (obj->type) {
    case CUSTOM_OBJ_FLOAT:
        obj->fvalue = 0.f;
        break;
    case CUSTOM_OBJ_TEXT:
        obj->text_value[0] = '\0';
        break;
    case CUSTOM_OBJ_ENUM:
        init_enum_defaults(obj);
        break;
    default:
        break;
    }
}

static void init_object_coords(image_t *img, custom_object_t *obj)
{
    int c[3], z0, z1;
    image_center(img, c);
    image_z_range(img, &z0, &z1);
    obj->p0[0] = c[0];
    obj->p0[1] = c[1];
    obj->p0[2] = c[2];
    obj->p1[0] = c[0] + 3;
    obj->p1[1] = c[1] + 3;
    obj->p1[2] = c[2] + 3;
    if (obj->type == CUSTOM_OBJ_ZONE_2D) {
        obj->p0[2] = z0;
        obj->p1[2] = z1;
    }
    if (obj->type == CUSTOM_OBJ_POINT_2D)
        obj->p0[2] = z0;
}

static void hsv_to_rgb_u8(float h, float s, float v, uint8_t rgb[3])
{
    float r, g, b, f, p, q, t;
    int i;

    h = fmodf(h, 1.f);
    if (h < 0.f) h += 1.f;
    s = clamp(s, 0.f, 1.f);
    v = clamp(v, 0.f, 1.f);
    i = (int)(h * 6.f);
    f = h * 6.f - (float)i;
    p = v * (1.f - s);
    q = v * (1.f - f * s);
    t = v * (1.f - (1.f - f) * s);
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (uint8_t)clamp(r * 255.f, 0.f, 255.f);
    rgb[1] = (uint8_t)clamp(g * 255.f, 0.f, 255.f);
    rgb[2] = (uint8_t)clamp(b * 255.f, 0.f, 255.f);
}

static void random_object_color(uint8_t color[4])
{
    float h = random_int(0, 359) / 360.f;
    hsv_to_rgb_u8(h, 0.75f, 1.f, color);
    color[3] = 255;
}

custom_object_t *custom_object_add(image_t *img, custom_object_type_t type)
{
    return custom_object_add_to_group(img, NULL, type);
}

custom_object_t *custom_object_add_to_group(image_t *img, custom_object_t *group,
                                           custom_object_type_t type)
{
    custom_object_t *obj, *after;
    if (!img) return NULL;
    if (group && group->type != CUSTOM_OBJ_GROUP) return NULL;
    if (group && type == CUSTOM_OBJ_GROUP) return NULL;
    obj = calloc(1, sizeof(*obj));
    if (!obj) return NULL;
    obj->ref = 1;
    obj->type = type;
    obj->visible = true;
    obj->group = group;
    if (group)
        memcpy(obj->color, group->color, 4);
    else
        random_object_color(obj->color);
    make_uniq_name(obj->name, sizeof(obj->name),
                   group ? group->name :
                   (type == CUSTOM_OBJ_GROUP ? "Group" : "Object"),
                   img, custom_object_name_exists);
    if (custom_object_is_spatial(type))
        init_object_coords(img, obj);
    else if (type != CUSTOM_OBJ_GROUP)
        init_value_fields(obj);
    if (group) {
        after = last_in_group_subtree(img->custom_objects, group);
        if (after)
            DL_APPEND_ELEM(img->custom_objects, after, obj);
        else
            DL_APPEND(img->custom_objects, obj);
    } else {
        DL_APPEND(img->custom_objects, obj);
    }
    return obj;
}

custom_object_t *custom_object_duplicate(image_t *img, custom_object_t *src)
{
    custom_object_t *obj;
    if (!img || !src) return NULL;
    obj = calloc(1, sizeof(*obj));
    if (!obj) return NULL;
    *obj = *src;
    obj->ref = 1;
    obj->next = obj->prev = NULL;
    make_uniq_name(obj->name, sizeof(obj->name), src->name, img,
                   custom_object_name_exists);
    DL_APPEND_ELEM(img->custom_objects, src, obj);
    return obj;
}

void custom_object_delete(image_t *img, custom_object_t *obj)
{
    custom_object_t *other, *tmp;
    if (!img || !obj) return;
    if (obj->type == CUSTOM_OBJ_GROUP) {
        DL_FOREACH_SAFE(img->custom_objects, other, tmp) {
            if (other != obj && is_descendant_of(other, obj)) {
                if (g_edit.obj == other)
                    edit_reset();
                DL_DELETE(img->custom_objects, other);
                free(other);
            }
        }
    }
    if (g_edit.obj == obj)
        edit_reset();
    DL_DELETE(img->custom_objects, obj);
    free(obj);
}

void custom_object_set_type(image_t *img, custom_object_t *obj,
                            custom_object_type_t type)
{
    int z0, z1, c[3];
    custom_object_type_t old;
    bool old_spatial, new_spatial;
    if (!obj || obj->type == type) return;
    if (obj->group && type == CUSTOM_OBJ_GROUP) return;
    old = obj->type;
    old_spatial = custom_object_is_spatial(old);
    new_spatial = custom_object_is_spatial(type);
    obj->type = type;

    if (type == CUSTOM_OBJ_GROUP)
        return;

    if (!old_spatial && new_spatial) {
        init_object_coords(img, obj);
        return;
    }
    if (old_spatial && !new_spatial) {
        init_value_fields(obj);
        return;
    }
    if (!old_spatial && !new_spatial) {
        init_value_fields(obj);
        return;
    }
    if (old == CUSTOM_OBJ_GROUP && new_spatial) {
        init_object_coords(img, obj);
        return;
    }
    if (old == CUSTOM_OBJ_GROUP && !new_spatial) {
        init_value_fields(obj);
        return;
    }

    image_z_range(img, &z0, &z1);
    image_center(img, c);

    if ((old == CUSTOM_OBJ_POINT_2D || old == CUSTOM_OBJ_POINT_3D) &&
        (type == CUSTOM_OBJ_ZONE_2D || type == CUSTOM_OBJ_ZONE_3D)) {
        obj->p1[0] = obj->p0[0] + 3;
        obj->p1[1] = obj->p0[1] + 3;
        obj->p1[2] = obj->p0[2] + 3;
    }
    if ((old == CUSTOM_OBJ_ZONE_2D || old == CUSTOM_OBJ_ZONE_3D) &&
        (type == CUSTOM_OBJ_POINT_2D || type == CUSTOM_OBJ_POINT_3D)) {
        obj->p0[0] = (obj->p0[0] + obj->p1[0]) / 2;
        obj->p0[1] = (obj->p0[1] + obj->p1[1]) / 2;
        obj->p0[2] = (obj->p0[2] + obj->p1[2]) / 2;
    }
    if (type == CUSTOM_OBJ_POINT_2D || type == CUSTOM_OBJ_ZONE_2D) {
        if (type == CUSTOM_OBJ_POINT_2D)
            obj->p0[2] = z0;
        else {
            obj->p0[2] = z0;
            obj->p1[2] = z1;
        }
    } else if (old == CUSTOM_OBJ_POINT_2D || old == CUSTOM_OBJ_ZONE_2D) {
        if (type == CUSTOM_OBJ_POINT_3D)
            obj->p0[2] = c[2];
        else if (obj->p0[2] == obj->p1[2]) {
            obj->p0[2] = c[2] - 2;
            obj->p1[2] = c[2] + 2;
        }
    }
}

/* ---------- Serialization ---------- */

#define CUST_FORMAT_V2 2
#define CUST_FORMAT_V3 3

static int object_serialized_size(const custom_object_t *obj)
{
    int namelen = (int)strlen(obj->name);
    int textlen;
    int i, size;

    if (namelen > 127) namelen = 127;
    size = 1 + namelen + 1 + 4 + 1 + 12 + 12;
    switch (obj->type) {
    case CUSTOM_OBJ_FLOAT:
        size += 4;
        break;
    case CUSTOM_OBJ_TEXT:
        textlen = (int)strlen(obj->text_value);
        if (textlen > (int)sizeof(obj->text_value) - 1)
            textlen = (int)sizeof(obj->text_value) - 1;
        size += 2 + textlen;
        break;
    case CUSTOM_OBJ_ENUM:
        size += 4 + 4;
        for (i = 0; i < obj->enum_option_count; i++) {
            int olen = (int)strlen(obj->enum_options[i]);
            if (olen > CUSTOM_OBJ_ENUM_OPTION_LEN - 1)
                olen = CUSTOM_OBJ_ENUM_OPTION_LEN - 1;
            size += 1 + olen;
        }
        break;
    default:
        break;
    }
    return size;
}

static int object_serialized_size_v3(const custom_object_t *obj)
{
    return object_serialized_size(obj) + 4;
}

static int write_object(uint8_t *buf, int cap, int w, const custom_object_t *obj)
{
    uint8_t namelen = (uint8_t)strlen(obj->name);
    int textlen, i, olen;
    int32_t n;

    if (namelen > 127) namelen = 127;
    if (w + object_serialized_size(obj) > cap) return w;

    buf[w++] = namelen;
    memcpy(buf + w, obj->name, namelen);
    w += namelen;
    buf[w++] = (uint8_t)obj->type;
    memcpy(buf + w, obj->color, 4);
    w += 4;
    buf[w++] = obj->visible ? 1 : 0;
    memcpy(buf + w, obj->p0, 12);
    w += 12;
    memcpy(buf + w, obj->p1, 12);
    w += 12;

    switch (obj->type) {
    case CUSTOM_OBJ_FLOAT:
        memcpy(buf + w, &obj->fvalue, 4);
        w += 4;
        break;
    case CUSTOM_OBJ_TEXT:
        textlen = (int)strlen(obj->text_value);
        if (textlen > (int)sizeof(obj->text_value) - 1)
            textlen = (int)sizeof(obj->text_value) - 1;
        n = textlen;
        memcpy(buf + w, &n, 2);
        w += 2;
        if (textlen > 0) {
            memcpy(buf + w, obj->text_value, textlen);
            w += textlen;
        }
        break;
    case CUSTOM_OBJ_ENUM:
        n = obj->enum_index;
        memcpy(buf + w, &n, 4);
        w += 4;
        n = obj->enum_option_count;
        memcpy(buf + w, &n, 4);
        w += 4;
        for (i = 0; i < obj->enum_option_count; i++) {
            olen = (int)strlen(obj->enum_options[i]);
            if (olen > CUSTOM_OBJ_ENUM_OPTION_LEN - 1)
                olen = CUSTOM_OBJ_ENUM_OPTION_LEN - 1;
            buf[w++] = (uint8_t)olen;
            if (olen > 0) {
                memcpy(buf + w, obj->enum_options[i], olen);
                w += olen;
            }
        }
        break;
    default:
        break;
    }
    return w;
}

static int write_object_v3(uint8_t *buf, int cap, int w, const custom_object_t *obj,
                           int32_t group_index)
{
    w = write_object(buf, cap, w, obj);
    if (w + 4 > cap) return w;
    memcpy(buf + w, &group_index, 4);
    return w + 4;
}

static bool read_object_base(const uint8_t *data, int len, int *pos,
                             custom_object_t *obj, bool clamp_spatial_only)
{
    uint8_t namelen;

    if (*pos + 1 > len) return false;
    namelen = data[(*pos)++];
    if (*pos + namelen + 1 + 4 + 1 + 24 > len) return false;
    if (namelen >= sizeof(obj->name)) namelen = sizeof(obj->name) - 1;
    memcpy(obj->name, data + *pos, namelen);
    obj->name[namelen] = '\0';
    *pos += namelen;
    obj->type = (custom_object_type_t)data[(*pos)++];
    if (clamp_spatial_only && obj->type > CUSTOM_OBJ_ZONE_3D)
        obj->type = CUSTOM_OBJ_POINT_3D;
    memcpy(obj->color, data + *pos, 4);
    *pos += 4;
    obj->visible = data[(*pos)++] != 0;
    memcpy(obj->p0, data + *pos, 12);
    *pos += 12;
    memcpy(obj->p1, data + *pos, 12);
    *pos += 12;
    return true;
}

static bool read_object_legacy(const uint8_t *data, int len, int *pos,
                               custom_object_t *obj)
{
    return read_object_base(data, len, pos, obj, true);
}

static bool read_object_v2(const uint8_t *data, int len, int *pos,
                           custom_object_t *obj)
{
    int16_t textlen;
    int32_t n;
    int i, olen;

    if (!read_object_base(data, len, pos, obj, false)) return false;
    if (!custom_object_is_spatial(obj->type)) {
        switch (obj->type) {
        case CUSTOM_OBJ_FLOAT:
            if (*pos + 4 > len) return false;
            memcpy(&obj->fvalue, data + *pos, 4);
            *pos += 4;
            break;
        case CUSTOM_OBJ_TEXT:
            if (*pos + 2 > len) return false;
            memcpy(&textlen, data + *pos, 2);
            *pos += 2;
            if (textlen < 0) textlen = 0;
            if (textlen >= (int)sizeof(obj->text_value))
                textlen = (int)sizeof(obj->text_value) - 1;
            if (*pos + textlen > len) return false;
            if (textlen > 0) {
                memcpy(obj->text_value, data + *pos, textlen);
                *pos += textlen;
            }
            obj->text_value[textlen] = '\0';
            break;
        case CUSTOM_OBJ_COLOR:
            /* Legacy v2: skip separate rgba payload; value is color[4]. */
            if (*pos + 16 <= len)
                *pos += 16;
            break;
        case CUSTOM_OBJ_ENUM:
            if (*pos + 8 > len) return false;
            memcpy(&n, data + *pos, 4);
            *pos += 4;
            obj->enum_index = n;
            memcpy(&n, data + *pos, 4);
            *pos += 4;
            obj->enum_option_count = n;
            if (obj->enum_option_count < 0)
                obj->enum_option_count = 0;
            if (obj->enum_option_count > CUSTOM_OBJ_ENUM_OPTIONS_MAX)
                obj->enum_option_count = CUSTOM_OBJ_ENUM_OPTIONS_MAX;
            for (i = 0; i < obj->enum_option_count; i++) {
                if (*pos + 1 > len) return false;
                olen = data[(*pos)++];
                if (olen >= CUSTOM_OBJ_ENUM_OPTION_LEN)
                    olen = CUSTOM_OBJ_ENUM_OPTION_LEN - 1;
                if (*pos + olen > len) return false;
                if (olen > 0) {
                    memcpy(obj->enum_options[i], data + *pos, olen);
                    *pos += olen;
                }
                obj->enum_options[i][olen] = '\0';
            }
            if (obj->enum_index < 0 ||
                obj->enum_index >= obj->enum_option_count)
                obj->enum_index = 0;
            break;
        default:
            break;
        }
    }
    return true;
}

uint8_t *custom_objects_serialize(const image_t *img, int *out_len)
{
    custom_object_t *obj, **objs = NULL;
    int count = 0, cap, w = 0, i, j;
    uint8_t *buf;
    int32_t n, group_index;

    if (!img || !out_len) return NULL;
    DL_COUNT(img->custom_objects, obj, count);
    cap = 6;
    DL_FOREACH(img->custom_objects, obj)
        cap += object_serialized_size_v3(obj);
    cap += 16;
    buf = calloc(1, cap);
    if (!buf) return NULL;

    if (count > 0) {
        objs = calloc(count, sizeof(*objs));
        if (!objs) {
            free(buf);
            return NULL;
        }
        i = 0;
        DL_FOREACH(img->custom_objects, obj)
            objs[i++] = obj;
    }

    buf[w++] = CUST_FORMAT_V3;
    buf[w++] = img->custom_objects_show ? 1 : 0;
    n = count;
    memcpy(buf + w, &n, 4);
    w += 4;

    for (i = 0; i < count; i++) {
        group_index = -1;
        if (objs[i]->group) {
            for (j = 0; j < count; j++) {
                if (objs[j] == objs[i]->group) {
                    group_index = j;
                    break;
                }
            }
        }
        w = write_object_v3(buf, cap, w, objs[i], group_index);
    }

    free(objs);
    *out_len = w;
    return buf;
}

void custom_objects_deserialize(image_t *img, const uint8_t *data, int len)
{
    int pos = 0, i, j, count;
    int32_t n;
    int version;
    custom_object_t **loaded = NULL;
    int32_t *group_indices = NULL;

    if (!img || !data || len < 5) return;
    custom_objects_free_list(&img->custom_objects);

    version = data[0];
    if (version == CUST_FORMAT_V2 || version == CUST_FORMAT_V3) {
        if (len < 6) return;
        pos = 1;
        img->custom_objects_show = data[pos++] != 0;
    } else {
        img->custom_objects_show = data[pos++] != 0;
    }
    memcpy(&n, data + pos, 4);
    pos += 4;
    count = n;
    if (count < 0) count = 0;

    if (count > 0) {
        loaded = calloc(count, sizeof(*loaded));
        group_indices = calloc(count, sizeof(*group_indices));
        if (!loaded || !group_indices) {
            free(loaded);
            free(group_indices);
            return;
        }
    }

    for (i = 0; i < count; i++) {
        custom_object_t *obj = calloc(1, sizeof(*obj));
        obj->ref = 1;
        if (version == CUST_FORMAT_V3) {
            if (!read_object_v2(data, len, &pos, obj)) {
                free(obj);
                count = i;
                break;
            }
            if (pos + 4 > len) {
                free(obj);
                count = i;
                break;
            }
            memcpy(&group_indices[i], data + pos, 4);
            pos += 4;
        } else if (version == CUST_FORMAT_V2) {
            if (!read_object_v2(data, len, &pos, obj)) {
                free(obj);
                count = i;
                break;
            }
            group_indices[i] = -1;
        } else {
            if (!read_object_legacy(data, len, &pos, obj)) {
                free(obj);
                count = i;
                break;
            }
            group_indices[i] = -1;
        }
        loaded[i] = obj;
    }

    for (i = 0; i < count; i++) {
        if (!loaded[i]) continue;
        j = group_indices[i];
        if (version == CUST_FORMAT_V3 && j >= 0 && j < count && loaded[j])
            loaded[i]->group = loaded[j];
        DL_APPEND(img->custom_objects, loaded[i]);
    }

    free(loaded);
    free(group_indices);
}

void custom_objects_export_log(const image_t *img)
{
    custom_object_t *obj;
    if (!img) return;
    LOG_I("=== Custom objects export ===");
    DL_FOREACH(img->custom_objects, obj) {
        switch (obj->type) {
        case CUSTOM_OBJ_POINT_2D:
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X pos=(%d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  obj->p0[0], obj->p0[1]);
            break;
        case CUSTOM_OBJ_POINT_3D:
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X pos=(%d, %d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  obj->p0[0], obj->p0[1], obj->p0[2]);
            break;
        case CUSTOM_OBJ_ZONE_2D:
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X "
                  "min=(%d, %d) max=(%d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  obj->p0[0], obj->p0[1], obj->p1[0], obj->p1[1]);
            break;
        case CUSTOM_OBJ_ZONE_3D:
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X "
                  "min=(%d, %d, %d) max=(%d, %d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  obj->p0[0], obj->p0[1], obj->p0[2],
                  obj->p1[0], obj->p1[1], obj->p1[2]);
            break;
        case CUSTOM_OBJ_FLOAT:
            LOG_I("%s [%s] value=%g", obj->name,
                  custom_object_type_name(obj->type), obj->fvalue);
            break;
        case CUSTOM_OBJ_TEXT:
            LOG_I("%s [%s] value=\"%s\"", obj->name,
                  custom_object_type_name(obj->type), obj->text_value);
            break;
        case CUSTOM_OBJ_COLOR:
            LOG_I("%s [%s] value=#%02X%02X%02X%02X",
                  obj->name, custom_object_type_name(obj->type),
                  obj->color[0], obj->color[1], obj->color[2], obj->color[3]);
            break;
        case CUSTOM_OBJ_ENUM:
            if (obj->enum_option_count > 0 &&
                obj->enum_index >= 0 &&
                obj->enum_index < obj->enum_option_count)
                LOG_I("%s [%s] value=\"%s\" (index %d)",
                      obj->name, custom_object_type_name(obj->type),
                      obj->enum_options[obj->enum_index], obj->enum_index);
            else
                LOG_I("%s [%s] value=(none)", obj->name,
                      custom_object_type_name(obj->type));
            break;
        case CUSTOM_OBJ_GROUP:
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2]);
            break;
        }
    }
    LOG_I("=== End custom objects ===");
}

static void zone_corners(const custom_object_t *obj, int min[3], int max[3])
{
    int i;
    for (i = 0; i < 3; i++) {
        if (obj->p0[i] < obj->p1[i]) {
            min[i] = obj->p0[i];
            max[i] = obj->p1[i];
        } else {
            min[i] = obj->p1[i];
            max[i] = obj->p0[i];
        }
    }
}

static json_value *export_object_value(const image_t *img,
                                       const custom_object_t *obj)
{
    custom_object_t *child;
    int min[3], max[3];
    int color[4];

    if (obj->type == CUSTOM_OBJ_GROUP) {
        json_value *array = json_array_new(0);
        DL_FOREACH(img->custom_objects, child) {
            if (child->group == obj)
                json_array_push(array, export_object_value(img, child));
        }
        return array;
    }

    switch (obj->type) {
    case CUSTOM_OBJ_POINT_2D:
        return json_int_array_new(obj->p0, 2);
    case CUSTOM_OBJ_POINT_3D:
        return json_int_array_new(obj->p0, 3);
    case CUSTOM_OBJ_ZONE_2D:
        zone_corners(obj, min, max);
        {
            json_value *zone = json_object_new(2);
            json_object_push(zone, "min", json_int_array_new(min, 2));
            json_object_push(zone, "max", json_int_array_new(max, 2));
            return zone;
        }
    case CUSTOM_OBJ_ZONE_3D:
        zone_corners(obj, min, max);
        {
            json_value *zone = json_object_new(2);
            json_object_push(zone, "min", json_int_array_new(min, 3));
            json_object_push(zone, "max", json_int_array_new(max, 3));
            return zone;
        }
    case CUSTOM_OBJ_FLOAT:
        {
            double v = round((double)obj->fvalue * 1e6) / 1e6;
            return json_double_new(v);
        }
    case CUSTOM_OBJ_TEXT:
        return json_string_new(obj->text_value);
    case CUSTOM_OBJ_COLOR:
        color[0] = obj->color[0];
        color[1] = obj->color[1];
        color[2] = obj->color[2];
        color[3] = obj->color[3];
        return json_int_array_new(color, 4);
    case CUSTOM_OBJ_ENUM:
        if (obj->enum_option_count > 0 &&
            obj->enum_index >= 0 &&
            obj->enum_index < obj->enum_option_count)
            return json_string_new(obj->enum_options[obj->enum_index]);
        return json_string_new("");
    default:
        return json_null_new();
    }
}

static json_value *export_named_entry(const custom_object_t *obj,
                                      json_value *value)
{
    json_value *entry = json_object_new(1);
    json_object_push(entry, obj->name, value);
    return entry;
}

bool custom_objects_export_json(const image_t *img, const char *path)
{
    custom_object_t *obj;
    json_value *root, *metadata, *entry;
    json_serialize_opts opts = {
        .mode = json_serialize_mode_multiline,
        .opts = 0,
        .indent_size = 2,
    };
    size_t len;
    char *buf;
    FILE *f;
    bool ok = false;

    if (!img || !path) return false;

    root = json_object_new(1);
    metadata = json_array_new(0);
    json_object_push(root, "metadata", metadata);

    DL_FOREACH(img->custom_objects, obj) {
        if (obj->group) continue;
        entry = export_named_entry(obj, export_object_value(img, obj));
        json_array_push(metadata, entry);
    }

    len = json_measure_ex(root, opts);
    buf = malloc(len);
    if (!buf) goto done;
    json_serialize_ex(buf, root, opts);
    len = strlen(buf);

    f = fopen(path, "wb");
    if (!f) {
        LOG_E("Failed to write metadata export: %s", path);
        goto done;
    }
    if (fwrite(buf, 1, len, f) != len) {
        LOG_E("Failed to write metadata export: %s", path);
        fclose(f);
        goto done;
    }
    fclose(f);
    LOG_I("Exported metadata to %s", path);
    ok = true;

done:
    free(buf);
    json_builder_free(root);
    return ok;
}

/* ---------- Rendering ---------- */

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

/* ---------- Edit gizmos ---------- */

void custom_objects_set_editor_active(bool active)
{
    g_editor_active = active;
    if (!active)
        edit_reset();
}

static bool unproject_on_box(const float viewport[4], const float pos[2],
                             const float box[4][4], bool inside,
                             float out[3], float normal[3], int *face)
{
    int f;
    float wpos[3] = {pos[0], pos[1], 0};
    float opos[3], onorm[3];
    float plane[4][4];
    camera_t *cam = goxel.image->active_camera;

    if (!cam || box_is_null(box)) return false;
    camera_get_ray(cam, wpos, viewport, opos, onorm);
    for (f = 0; f < 6; f++) {
        mat4_copy(box, plane);
        mat4_imul(plane, FACES_MATS[f]);
        if (!inside && vec3_dot(plane[2], onorm) >= 0)
            continue;
        if (inside && vec3_dot(plane[2], onorm) <= 0)
            continue;
        if (!plane_line_intersection(plane, opos, onorm, out))
            continue;
        if (!(out[0] >= -1 && out[0] < 1 && out[1] >= -1 && out[1] < 1))
            continue;
        if (face) *face = f;
        mat4_mul_vec3(plane, out, out);
        vec3_normalize(plane[2], normal);
        if (inside) vec3_imul(normal, -1);
        return true;
    }
    return false;
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
        if (!unproject_on_box(viewport, curs->xy, box, false, hit, n, &face))
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
