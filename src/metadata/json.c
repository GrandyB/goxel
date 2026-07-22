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
#include "utils/json.h"
#include "../../ext_src/json/json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CHILD_TYPE_FIRST CUSTOM_OBJ_POINT_2D

static void normalize_type_key(const char *src, char *dst, size_t dst_len)
{
    size_t i, j = 0;
    if (!src || !dst || dst_len == 0) return;
    for (i = 0; src[i] && j + 1 < dst_len; i++) {
        if (src[i] == ' ' || src[i] == '_' || src[i] == '-')
            continue;
        dst[j++] = (char)tolower((unsigned char)src[i]);
    }
    dst[j] = '\0';
}

static bool type_from_json_string(const char *s, custom_object_type_t *out)
{
    char key[64];
    if (!s || !out) return false;
    normalize_type_key(s, key, sizeof(key));
    if (strcmp(key, "point2d") == 0 || strcmp(key, "2dpoint") == 0) {
        *out = CUSTOM_OBJ_POINT_2D;
        return true;
    }
    if (strcmp(key, "point3d") == 0 || strcmp(key, "3dpoint") == 0) {
        *out = CUSTOM_OBJ_POINT_3D;
        return true;
    }
    if (strcmp(key, "zone2d") == 0 || strcmp(key, "2dzone") == 0) {
        *out = CUSTOM_OBJ_ZONE_2D;
        return true;
    }
    if (strcmp(key, "zone3d") == 0 || strcmp(key, "3dzone") == 0) {
        *out = CUSTOM_OBJ_ZONE_3D;
        return true;
    }
    if (strcmp(key, "float") == 0) {
        *out = CUSTOM_OBJ_FLOAT;
        return true;
    }
    if (strcmp(key, "text") == 0) {
        *out = CUSTOM_OBJ_TEXT;
        return true;
    }
    if (strcmp(key, "color") == 0 || strcmp(key, "colour") == 0) {
        *out = CUSTOM_OBJ_COLOR;
        return true;
    }
    if (strcmp(key, "enum") == 0) {
        *out = CUSTOM_OBJ_ENUM;
        return true;
    }
    if (strcmp(key, "group") == 0) {
        *out = CUSTOM_OBJ_GROUP;
        return true;
    }
    return false;
}

static bool apply_template_default(image_t *img, custom_object_t *obj,
                                   json_value *jv)
{
    unsigned int i;
    int z0, z1;

    if (!jv || !obj) return false;

    switch (obj->type) {
    case CUSTOM_OBJ_TEXT:
        if (jv->type != json_string) return false;
        snprintf(obj->text_value, sizeof(obj->text_value),
                 "%s", jv->u.string.ptr);
        return true;
    case CUSTOM_OBJ_FLOAT:
        if (jv->type == json_double)
            obj->fvalue = (float)jv->u.dbl;
        else if (jv->type == json_integer)
            obj->fvalue = (float)jv->u.integer;
        else
            return false;
        return true;
    case CUSTOM_OBJ_COLOR:
        return json_read_u8_rgba(jv, obj->color);
    case CUSTOM_OBJ_ENUM:
        if (jv->type == json_string) {
            for (i = 0; i < (unsigned int)obj->enum_option_count; i++) {
                if (strcmp(obj->enum_options[i], jv->u.string.ptr) == 0) {
                    obj->enum_index = (int)i;
                    return true;
                }
            }
            LOG_W("Unknown enum default \"%s\" for %s",
                  jv->u.string.ptr, obj->name);
            return false;
        }
        if (jv->type == json_integer &&
            jv->u.integer >= 0 &&
            jv->u.integer < obj->enum_option_count) {
            obj->enum_index = (int)jv->u.integer;
            return true;
        }
        return false;
    case CUSTOM_OBJ_POINT_2D:
    case CUSTOM_OBJ_POINT_3D:
        if (jv->type != json_array) return false;
        {
            int display[3] = {0, 0, 0};
            if (jv->u.array.length >= 1) {
                json_value *e = jv->u.array.values[0];
                if (e->type == json_integer)
                    display[0] = (int)e->u.integer;
                else if (e->type == json_double)
                    display[0] = (int)e->u.dbl;
            }
            if (jv->u.array.length >= 2) {
                json_value *e = jv->u.array.values[1];
                if (e->type == json_integer)
                    display[1] = (int)e->u.integer;
                else if (e->type == json_double)
                    display[1] = (int)e->u.dbl;
            }
            if (obj->type == CUSTOM_OBJ_POINT_3D && jv->u.array.length >= 3) {
                json_value *e = jv->u.array.values[2];
                if (e->type == json_integer)
                    display[2] = (int)e->u.integer;
                else if (e->type == json_double)
                    display[2] = (int)e->u.dbl;
            }
            custom_object_display_to_world(img, display, obj->p0);
            if (obj->type == CUSTOM_OBJ_POINT_2D) {
                image_z_range(img, &z0, &z1);
                obj->p0[2] = z0;
            }
        }
        return jv->u.array.length >=
               (obj->type == CUSTOM_OBJ_POINT_3D ? 3u : 2u);
    default:
        return false;
    }
}

static bool parse_template_entry(json_value *entry, image_t *img)
{
    json_value *jv;
    custom_object_type_t type;
    custom_object_t *obj;
    const char *name, *type_str;
    uint8_t color[4];
    bool has_color = false;
    custom_object_type_t child_type = DEFAULT_CHILD_TYPE_FIRST;
    bool has_child_type = false;
    bool lock_child_types_to_default = false;
    char options[CUSTOM_OBJ_ENUM_OPTIONS_MAX][CUSTOM_OBJ_ENUM_OPTION_LEN];
    int option_count = 0;
    unsigned int i;

    if (!entry || entry->type != json_object) return false;
    jv = json_obj_get(entry, "name");
    if (!jv || jv->type != json_string || !jv->u.string.ptr[0])
        return false;
    name = jv->u.string.ptr;
    jv = json_obj_get(entry, "type");
    if (!jv || jv->type != json_string) return false;
    type_str = jv->u.string.ptr;
    if (!type_from_json_string(type_str, &type)) {
        LOG_W("Unknown metadata template type: %s", type_str);
        return false;
    }
    jv = json_obj_get(entry, "color");
    if (jv && json_read_u8_rgba(jv, color))
        has_color = true;
    jv = json_obj_get(entry, "default_child_type");
    if (jv && jv->type == json_string &&
        type_from_json_string(jv->u.string.ptr, &child_type))
        has_child_type = true;
    jv = json_obj_get(entry, "lock_child_types_to_default");
    if (jv && jv->type == json_boolean)
        lock_child_types_to_default = jv->u.boolean;
    jv = json_obj_get(entry, "options");
    if (jv && jv->type == json_array) {
        for (i = 0; i < jv->u.array.length &&
             option_count < CUSTOM_OBJ_ENUM_OPTIONS_MAX; i++) {
            json_value *opt = jv->u.array.values[i];
            if (opt->type != json_string) continue;
            snprintf(options[option_count], CUSTOM_OBJ_ENUM_OPTION_LEN,
                     "%s", opt->u.string.ptr);
            option_count++;
        }
    }

    obj = custom_object_add_to_group(img, NULL, type);
    if (!obj) return false;

    snprintf(obj->name, sizeof(obj->name), "%s", name);
    if (has_color)
        memcpy(obj->color, color, 4);
    if (type == CUSTOM_OBJ_GROUP) {
        obj->default_child_type = has_child_type ? child_type :
                                  DEFAULT_CHILD_TYPE_FIRST;
        obj->lock_child_types_to_default = lock_child_types_to_default;
    }
    if (type == CUSTOM_OBJ_ENUM && option_count > 0) {
        obj->enum_option_count = option_count;
        obj->enum_index = 0;
        for (i = 0; i < (unsigned int)option_count; i++)
            snprintf(obj->enum_options[i], CUSTOM_OBJ_ENUM_OPTION_LEN,
                     "%s", options[i]);
    }

    jv = json_obj_get(entry, "default");
    if (jv && !apply_template_default(img, obj, jv))
        LOG_W("Invalid default value for %s", name);
    return true;
}

bool custom_objects_load_template_json(const char *path, image_t *img)
{
    char *data;
    int size;
    json_value *root, *metadata;
    unsigned int i;
    bool ok = true;

    if (!path || !img) return false;
    data = read_file(path, &size);
    if (!data || size <= 0) {
        LOG_E("Failed to read metadata template: %s", path);
        free(data);
        return false;
    }
    root = json_parse(data, (size_t)size);
    free(data);
    if (!root) {
        LOG_E("Failed to parse metadata template: %s", path);
        return false;
    }
    metadata = json_obj_get(root, "metadata");
    if (!metadata || metadata->type != json_array) {
        LOG_E("Metadata template missing \"metadata\" array: %s", path);
        json_value_free(root);
        return false;
    }
    custom_objects_free_list(&img->custom_objects);
    for (i = 0; i < metadata->u.array.length; i++) {
        if (!parse_template_entry(metadata->u.array.values[i], img))
            ok = false;
    }
    json_value_free(root);
    if (!ok)
        LOG_W("Metadata template loaded with errors: %s", path);
    else
        LOG_I("Loaded metadata template: %s", path);
    return ok;
}

void custom_objects_export_log(const image_t *img)
{
    custom_object_t *obj;
    int display[3];
    if (!img) return;
    LOG_I("=== Metadata export ===");
    DL_FOREACH(img->custom_objects, obj) {
        switch (obj->type) {
        case CUSTOM_OBJ_POINT_2D:
            custom_object_world_to_display(img, obj->p0, display);
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X pos=(%d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  display[0], display[1]);
            break;
        case CUSTOM_OBJ_POINT_3D:
            custom_object_world_to_display(img, obj->p0, display);
            LOG_I("%s [%s] visible=%d color=#%02X%02X%02X pos=(%d, %d, %d)",
                  obj->name, custom_object_type_name(obj->type),
                  (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                  display[0], display[1], display[2]);
            break;
        case CUSTOM_OBJ_ZONE_2D:
            {
                int world_min[3], world_max[3], max_display[3];
                zone_corners(obj, world_min, world_max);
                custom_object_world_to_display(img, world_min, display);
                custom_object_world_to_display(img, world_max, max_display);
                LOG_I("%s [%s] visible=%d color=#%02X%02X%02X "
                      "min=(%d, %d) max=(%d, %d)",
                      obj->name, custom_object_type_name(obj->type),
                      (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                      display[0], display[1], max_display[0], max_display[1]);
            }
            break;
        case CUSTOM_OBJ_ZONE_3D:
            {
                int world_min[3], world_max[3], max_display[3];
                zone_corners(obj, world_min, world_max);
                custom_object_world_to_display(img, world_min, display);
                custom_object_world_to_display(img, world_max, max_display);
                LOG_I("%s [%s] visible=%d color=#%02X%02X%02X "
                      "min=(%d, %d, %d) max=(%d, %d, %d)",
                      obj->name, custom_object_type_name(obj->type),
                      (int)obj->visible, obj->color[0], obj->color[1], obj->color[2],
                      display[0], display[1], display[2],
                      max_display[0], max_display[1], max_display[2]);
            }
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

static json_value *export_object_value(const image_t *img,
                                       const custom_object_t *obj)
{
    custom_object_t *child;
    int world_min[3], world_max[3];
    int min[3], max[3];
    int display[3];
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
        custom_object_world_to_display(img, obj->p0, display);
        return json_int_array_new(display, 2);
    case CUSTOM_OBJ_POINT_3D:
        custom_object_world_to_display(img, obj->p0, display);
        return json_int_array_new(display, 3);
    case CUSTOM_OBJ_ZONE_2D:
        zone_corners(obj, world_min, world_max);
        custom_object_world_to_display(img, world_min, min);
        custom_object_world_to_display(img, world_max, max);
        {
            json_value *zone = json_object_new(2);
            json_object_push(zone, "min", json_int_array_new(min, 2));
            json_object_push(zone, "max", json_int_array_new(max, 2));
            return zone;
        }
    case CUSTOM_OBJ_ZONE_3D:
        zone_corners(obj, world_min, world_max);
        custom_object_world_to_display(img, world_min, min);
        custom_object_world_to_display(img, world_max, max);
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

bool custom_objects_export_json(const image_t *img, const char *path)
{
    custom_object_t *obj;
    json_value *root, *metadata;
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
    metadata = json_object_new(0);
    json_object_push(root, "metadata", metadata);

    DL_FOREACH(img->custom_objects, obj) {
        if (obj->group) continue;
        json_object_push(metadata, obj->name,
                         export_object_value(img, obj));
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
