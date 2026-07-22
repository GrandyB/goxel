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
#include "utils/color.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool custom_object_name_exists(void *user, const char *name)
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
    if (custom_objects_get_solo())
        return obj == custom_objects_get_solo();
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

bool is_descendant_of(const custom_object_t *obj,
                      const custom_object_t *ancestor)
{
    const custom_object_t *g;
    if (!obj || !ancestor) return false;
    for (g = obj; g; g = g->group) {
        if (g == ancestor) return true;
    }
    return false;
}

custom_object_t *last_in_group_subtree(custom_object_t *list,
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

void custom_object_world_to_display(const image_t *img, const int world[3],
                                    int display[3])
{
    int origin[3];
    image_bottom_left(img, origin);
    display[0] = world[0] - origin[0];
    display[1] = world[1] - origin[1];
    display[2] = world[2] - origin[2];
}

void custom_object_display_to_world(const image_t *img, const int display[3],
                                    int world[3])
{
    int origin[3];
    image_bottom_left(img, origin);
    world[0] = display[0] + origin[0];
    world[1] = display[1] + origin[1];
    world[2] = display[2] + origin[2];
}

void normalize_corners(int a[3], int b[3])
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

void zone_corners(const custom_object_t *obj, int min[3], int max[3])
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
    if (*list)
        custom_objects_on_list_freed(*list);
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

void init_enum_defaults(custom_object_t *obj)
{
    obj->enum_index = 0;
    obj->enum_option_count = 3;
    snprintf(obj->enum_options[0], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 1");
    snprintf(obj->enum_options[1], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 2");
    snprintf(obj->enum_options[2], CUSTOM_OBJ_ENUM_OPTION_LEN, "Option 3");
}

void init_value_fields(custom_object_t *obj)
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

void init_object_coords(image_t *img, custom_object_t *obj)
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

void random_object_color(uint8_t color[4])
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
    if (type == CUSTOM_OBJ_GROUP)
        obj->default_child_type = CUSTOM_OBJ_POINT_3D;
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
                custom_objects_clear_edit_refs(other);
                DL_DELETE(img->custom_objects, other);
                free(other);
            }
        }
    }
    custom_objects_clear_edit_refs(obj);
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
