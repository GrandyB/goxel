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

#include <stdlib.h>
#include <string.h>

#define CUST_FORMAT_V2 2
#define CUST_FORMAT_V3 3
#define CUST_FORMAT_V4 4
#define CUST_FORMAT_V5 5
#define CUST_DEFAULT_CHILD_NONE 255

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

static int object_serialized_size_v4(const custom_object_t *obj)
{
    return object_serialized_size(obj) + 4 + 1;
}

static int object_serialized_size_v5(const custom_object_t *obj)
{
    return object_serialized_size_v4(obj) + 1;
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
    buf[w++] = 1; /* per-item visibility is session-only */
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

static int write_object_v4(uint8_t *buf, int cap, int w, const custom_object_t *obj,
                           int32_t group_index)
{
    uint8_t child_type = CUST_DEFAULT_CHILD_NONE;
    w = write_object(buf, cap, w, obj);
    if (w + 5 > cap) return w;
    memcpy(buf + w, &group_index, 4);
    w += 4;
    if (obj->type == CUSTOM_OBJ_GROUP &&
        obj->default_child_type <= CUSTOM_OBJ_GROUP)
        child_type = (uint8_t)obj->default_child_type;
    buf[w++] = child_type;
    return w;
}

static int write_object_v5(uint8_t *buf, int cap, int w, const custom_object_t *obj,
                           int32_t group_index)
{
    w = write_object_v4(buf, cap, w, obj, group_index);
    if (w + 1 > cap) return w;
    buf[w++] = (obj->type == CUSTOM_OBJ_GROUP && obj->lock_child_types_to_default) ? 1 : 0;
    return w;
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
    (*pos)++; /* visible byte ignored — session-only */
    obj->visible = true;
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
            /* Colour value lives in base color[4]; no extra payload. */
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
        cap += object_serialized_size_v5(obj);
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

    buf[w++] = CUST_FORMAT_V5;
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
        w = write_object_v5(buf, cap, w, objs[i], group_index);
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
    if (version == CUST_FORMAT_V2 || version == CUST_FORMAT_V3 ||
        version == CUST_FORMAT_V4 || version == CUST_FORMAT_V5) {
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
        if (version == CUST_FORMAT_V5) {
            if (!read_object_v2(data, len, &pos, obj)) {
                free(obj);
                count = i;
                break;
            }
            if (pos + 6 > len) {
                free(obj);
                count = i;
                break;
            }
            memcpy(&group_indices[i], data + pos, 4);
            pos += 4;
            if (obj->type == CUSTOM_OBJ_GROUP &&
                data[pos] != CUST_DEFAULT_CHILD_NONE &&
                data[pos] <= CUSTOM_OBJ_GROUP)
                obj->default_child_type =
                    (custom_object_type_t)data[pos];
            else if (obj->type == CUSTOM_OBJ_GROUP)
                obj->default_child_type = CUSTOM_OBJ_POINT_3D;
            pos += 1;
            if (obj->type == CUSTOM_OBJ_GROUP)
                obj->lock_child_types_to_default = data[pos] != 0;
            pos += 1;
        } else if (version == CUST_FORMAT_V4) {
            if (!read_object_v2(data, len, &pos, obj)) {
                free(obj);
                count = i;
                break;
            }
            if (pos + 5 > len) {
                free(obj);
                count = i;
                break;
            }
            memcpy(&group_indices[i], data + pos, 4);
            pos += 4;
            if (obj->type == CUSTOM_OBJ_GROUP &&
                data[pos] != CUST_DEFAULT_CHILD_NONE &&
                data[pos] <= CUSTOM_OBJ_GROUP)
                obj->default_child_type =
                    (custom_object_type_t)data[pos];
            else if (obj->type == CUSTOM_OBJ_GROUP)
                obj->default_child_type = CUSTOM_OBJ_POINT_3D;
            pos += 1;
        } else if (version == CUST_FORMAT_V3) {
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
            if (obj->type == CUSTOM_OBJ_GROUP)
                obj->default_child_type = CUSTOM_OBJ_POINT_3D;
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
        if ((version == CUST_FORMAT_V3 || version == CUST_FORMAT_V4 ||
             version == CUST_FORMAT_V5) &&
            j >= 0 && j < count && loaded[j])
            loaded[i]->group = loaded[j];
        if (loaded[i]->type == CUSTOM_OBJ_GROUP && version < CUST_FORMAT_V3)
            loaded[i]->default_child_type = CUSTOM_OBJ_POINT_3D;
        DL_APPEND(img->custom_objects, loaded[i]);
    }

    free(loaded);
    free(group_indices);
}
