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
#include "filters/metadata_gui.h"

#include <stdlib.h>
#include <string.h>

static const custom_object_type_t PICKER_TYPES[] = {
    CUSTOM_OBJ_POINT_2D,
    CUSTOM_OBJ_POINT_3D,
    CUSTOM_OBJ_ZONE_2D,
    CUSTOM_OBJ_ZONE_3D,
    CUSTOM_OBJ_FLOAT,
    CUSTOM_OBJ_TEXT,
    CUSTOM_OBJ_COLOR,
    CUSTOM_OBJ_GROUP,
};
#define PICKER_TYPES_COUNT ((int)(sizeof(PICKER_TYPES) / sizeof(PICKER_TYPES[0])))
#define PICKER_CHILD_COUNT ((int)CUSTOM_OBJ_COLOR + 1)

static int type_to_picker_index(custom_object_type_t type)
{
    int i;
    for (i = 0; i < PICKER_TYPES_COUNT; i++) {
        if (PICKER_TYPES[i] == type)
            return i;
    }
    return 0;
}

static custom_object_t *g_pending_delete = NULL;
static custom_object_t *g_pending_dup = NULL;
static custom_object_t *g_pending_add_to_group = NULL;
static bool g_pending_add = false;

static void ensure_list_current_valid(image_t *img)
{
    custom_object_t *obj, *current = custom_objects_get_list_selected();
    bool found = false;
    if (!current) return;
    DL_FOREACH(img->custom_objects, obj) {
        if (obj == current) {
            found = true;
            break;
        }
    }
    if (!found) custom_objects_set_list_selected(NULL);
}

static bool type_has_list_color(custom_object_type_t type)
{
    return type != CUSTOM_OBJ_TEXT &&
           type != CUSTOM_OBJ_FLOAT &&
           type != CUSTOM_OBJ_ENUM;
}

static bool type_has_list_visibility(custom_object_type_t type)
{
    return custom_object_is_spatial(type) || custom_object_is_group(type);
}

static bool render_list_item(void *item, int idx, bool current)
{
    custom_object_t *obj = item;
    bool visible = obj->visible;
    bool selected = current;
    bool solo_active = custom_objects_get_solo() == obj;
    bool solo_press = false;
    bool ret = false;
    bool press = false;
    bool is_group = custom_object_is_group(obj->type);
    bool in_group = obj->group != NULL;
    bool show_color = !in_group && type_has_list_color(obj->type);
    bool show_visibility = type_has_list_visibility(obj->type);
    bool show_solo = show_visibility;
    char id[32];
    float icon_h = gui_icon_height(true);
    float spacing = gui_style_item_spacing_x();
    /* Room for optional group-add + duplicate + delete on the same row. */
    float trailing = (2 + (is_group ? 1 : 0)) * icon_h + (2 + (is_group ? 1 : 0)) * spacing;

    snprintf(id, sizeof(id), "%d", idx);
    gui_push_id(id);

    if (show_color) {
        if (gui_color_small_no_label(id, obj->color))
            ret = true;
        gui_same_line();
    } else {
        /* Same width as ColorEdit4; one dummy avoids extra ItemSpacing. */
        gui_spacing_f(gui_frame_height());
    }

    if (gui_condensed_layer_item_trailing(idx, 0, NULL,
                                          show_visibility ? &visible : NULL,
                                          &selected,
                                          obj->name, sizeof(obj->name),
                                          trailing, true, solo_active,
                                          show_solo ? &solo_press : NULL,
                                          !show_visibility, !show_solo, true))
        ret = true;
    if (show_solo && solo_press)
        custom_objects_toggle_solo(obj);
    if (selected != current)
        custom_objects_set_list_selected(selected ? obj : NULL);
    if (show_visibility && visible != obj->visible)
        obj->visible = visible;

    gui_same_line();
    if (is_group) {
        press = false;
        if (gui_condensed_selectable_icon("##Add child", &press, ICON_ADD))
            g_pending_add_to_group = obj;
        gui_same_line();
    }
    press = false;
    if (gui_condensed_selectable_icon("##dup", &press, ICON_COPY))
        g_pending_dup = obj;
    gui_same_line();
    press = false;
    if (gui_condensed_selectable_icon("##del", &press, ICON_REMOVE))
        g_pending_delete = obj;

    gui_pop_id();
    return ret;
}

static void render_metadata_list(image_t *img)
{
    custom_object_t *obj;
    bool is_current;
    int i;

    gui_group_begin(NULL);
    i = 0;
    DL_FOREACH(img->custom_objects, obj) {
        is_current = custom_objects_get_list_selected() == obj;
        render_list_item(obj, i, is_current);
        i++;
    }
    gui_group_end();
}

/* Edit one int vec; show_z false hides Z (2D objects). Returns true if changed. */
static bool gui_coord_vec(const char *label, int v[3], bool show_z)
{
    int x = v[0], y = v[1], z = v[2];
    bool changed = false;

    gui_group_begin(label);
    if (gui_input_int("X", &x, 0, 0)) changed = true;
    if (gui_input_int("Y", &y, 0, 0)) changed = true;
    if (show_z && gui_input_int("Z", &z, 0, 0)) changed = true;
    gui_group_end();

    if (changed) {
        v[0] = x;
        v[1] = y;
        if (show_z) v[2] = z;
    }
    return changed;
}

static void gui_object_coords(image_t *img, custom_object_t *obj)
{
    int p0[3], p1[3];
    bool is_2d = (obj->type == CUSTOM_OBJ_POINT_2D ||
                  obj->type == CUSTOM_OBJ_ZONE_2D);
    bool is_zone = (obj->type == CUSTOM_OBJ_ZONE_2D ||
                    obj->type == CUSTOM_OBJ_ZONE_3D);
    bool changed = false;

    if (!custom_object_is_spatial(obj->type)) return;

    if (is_zone) {
        int w0[3], w1[3];
        /* Keep Min <= Max in the UI. */
        w0[0] = obj->p0[0] < obj->p1[0] ? obj->p0[0] : obj->p1[0];
        w0[1] = obj->p0[1] < obj->p1[1] ? obj->p0[1] : obj->p1[1];
        w0[2] = obj->p0[2] < obj->p1[2] ? obj->p0[2] : obj->p1[2];
        w1[0] = obj->p0[0] > obj->p1[0] ? obj->p0[0] : obj->p1[0];
        w1[1] = obj->p0[1] > obj->p1[1] ? obj->p0[1] : obj->p1[1];
        w1[2] = obj->p0[2] > obj->p1[2] ? obj->p0[2] : obj->p1[2];
        custom_object_world_to_display(img, w0, p0);
        custom_object_world_to_display(img, w1, p1);

        if (gui_coord_vec("Min", p0, !is_2d)) changed = true;
        if (gui_coord_vec("Max", p1, !is_2d)) changed = true;
    } else {
        custom_object_world_to_display(img, obj->p0, p0);
        if (gui_coord_vec("Position", p0, !is_2d)) changed = true;
    }

    if (!changed) return;

    image_history_push(img);
    if (is_zone) {
        int w0[3], w1[3];
        custom_object_display_to_world(img, p0, w0);
        custom_object_display_to_world(img, p1, w1);
        memcpy(obj->p0, w0, sizeof(obj->p0));
        memcpy(obj->p1, w1, sizeof(obj->p1));
    } else {
        custom_object_display_to_world(img, p0, obj->p0);
    }
}

static void gui_object_values(image_t *img, custom_object_t *obj)
{
    float fvalue;
    int enum_index;
    const char *enum_names[CUSTOM_OBJ_ENUM_OPTIONS_MAX];
    int i;

    switch (obj->type) {
    case CUSTOM_OBJ_FLOAT:
        fvalue = obj->fvalue;
        if (gui_input_float("Value", &fvalue, 0.0001f, 0, 0, "%.4f")) {
            image_history_push(img);
            obj->fvalue = fvalue;
        }
        break;
    case CUSTOM_OBJ_TEXT:
        if (gui_input_text("Value", obj->text_value, sizeof(obj->text_value)))
            image_history_push(img);
        break;
    case CUSTOM_OBJ_ENUM:
        if (obj->enum_option_count <= 0) return;
        enum_index = obj->enum_index;
        if (enum_index < 0) enum_index = 0;
        if (enum_index >= obj->enum_option_count)
            enum_index = obj->enum_option_count - 1;
        for (i = 0; i < obj->enum_option_count; i++)
            enum_names[i] = obj->enum_options[i];
        if (gui_combo("Value", &enum_index, enum_names,
                      obj->enum_option_count)) {
            image_history_push(img);
            obj->enum_index = enum_index;
        }
        break;
    default:
        break;
    }
}

void metadata_gui_panel(image_t *img)
{
    custom_object_t *obj;
    int type;
    const char *type_names[PICKER_TYPES_COUNT];
    int i;

    if (!img) return;

    ensure_list_current_valid(img);

    for (i = 0; i < PICKER_TYPES_COUNT; i++)
        type_names[i] = custom_object_type_name(PICKER_TYPES[i]);

    if (gui_button("Add new", 1.0, ICON_ADD))
        g_pending_add = true;

    g_pending_delete = NULL;
    g_pending_dup = NULL;
    g_pending_add_to_group = NULL;
    render_metadata_list(img);

    if (g_pending_add) {
        g_pending_add = false;
        image_history_push(img);
        custom_objects_set_list_selected(
            custom_object_add(img, CUSTOM_OBJ_POINT_3D));
    }

    if (g_pending_add_to_group) {
        custom_object_t *group = g_pending_add_to_group;
        custom_object_type_t child_type = group->default_child_type;
        g_pending_add_to_group = NULL;
        if (child_type == CUSTOM_OBJ_GROUP)
            child_type = CUSTOM_OBJ_POINT_3D;
        image_history_push(img);
        custom_objects_set_list_selected(
            custom_object_add_to_group(img, group, child_type));
    }

    if (g_pending_dup) {
        custom_object_t *src = g_pending_dup;
        g_pending_dup = NULL;
        image_history_push(img);
        custom_objects_set_list_selected(
            custom_object_duplicate(img, src));
    }

    if (g_pending_delete) {
        custom_object_t *del = g_pending_delete;
        g_pending_delete = NULL;
        if (custom_objects_get_list_selected() == del)
            custom_objects_set_list_selected(NULL);
        image_history_push(img);
        custom_object_delete(img, del);
    }

    obj = custom_objects_get_list_selected();
    if (obj) {
        gui_text("Type");
        if (obj->type == CUSTOM_OBJ_ENUM) {
            gui_text("Enum");
        } else {
            bool type_locked = obj->group && obj->group->lock_child_types_to_default;
            if (type_locked) {
                gui_text("%s", custom_object_type_name(obj->type));
            } else {
                type = type_to_picker_index(obj->type);
                if (gui_combo("##obj_type", &type, type_names,
                              obj->group ? PICKER_CHILD_COUNT : PICKER_TYPES_COUNT)) {
                    custom_object_type_t new_type = PICKER_TYPES[type];
                    if (new_type != obj->type) {
                        image_history_push(img);
                        custom_object_set_type(img, obj, new_type);
                    }
                }
            }
        }
        if (custom_object_is_spatial(obj->type))
            gui_object_coords(img, obj);
        else if (obj->type != CUSTOM_OBJ_COLOR &&
                 obj->type != CUSTOM_OBJ_GROUP)
            gui_object_values(img, obj);
    }

    gui_dummy(0, (int)gui_style_item_spacing_y());

    if (gui_button("Export", 1.0, 0)) {
        const char *filters[] = {"*.json", NULL};
        const char *path;
        custom_objects_export_log(img);
        path = sys_get_save_path("metadata.json", filters, "json");
        if (path)
            custom_objects_export_json(img, path);
    }
}
