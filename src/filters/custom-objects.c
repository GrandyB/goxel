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

typedef struct {
    filter_t filter;
} filter_custom_objects_t;

static const char *TYPE_NAMES[] = {
    "2D Point",
    "3D Point",
    "2D Zone",
    "3D Zone",
};

/* List highlight only (gizmos are hover-based, not list selection). */
static custom_object_t *g_list_current = NULL;
static custom_object_t *g_pending_delete = NULL;
static custom_object_t *g_pending_dup = NULL;
static bool g_pending_add = false;

static void on_open(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(true);
}

static void on_close(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(false);
    g_list_current = NULL;
}

static void on_mouse(filter_t *filter_, const float viewport[4])
{
    (void)filter_;
    custom_objects_edit_iter(viewport);
}

static void ensure_list_current_valid(image_t *img)
{
    custom_object_t *obj;
    bool found = false;
    if (!g_list_current) return;
    DL_FOREACH(img->custom_objects, obj) {
        if (obj == g_list_current) {
            found = true;
            break;
        }
    }
    if (!found) g_list_current = NULL;
}

static bool render_list_item(void *item, int idx, bool current)
{
    custom_object_t *obj = item;
    bool visible = obj->visible;
    bool selected = current;
    bool ret = false;
    bool press = false;
    char id[32];
    float icon_h = gui_icon_height(true);
    float spacing = gui_style_item_spacing_x();
    /* Reserve room for duplicate + delete on the same row. */
    float trailing = 2 * icon_h + 2 * spacing;

    snprintf(id, sizeof(id), "%d", idx);
    gui_push_id(id);

    gui_color_small_no_label(id, obj->color);
    gui_same_line();
    if (gui_condensed_layer_item_trailing(idx, 0, NULL, &visible, &selected,
                                          obj->name, sizeof(obj->name),
                                          trailing))
        ret = true;
    if (visible != obj->visible)
        obj->visible = visible;

    gui_same_line();
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

    memcpy(p0, obj->p0, sizeof(p0));
    memcpy(p1, obj->p1, sizeof(p1));

    if (is_zone) {
        /* Keep Min <= Max in the UI. */
        p0[0] = obj->p0[0] < obj->p1[0] ? obj->p0[0] : obj->p1[0];
        p0[1] = obj->p0[1] < obj->p1[1] ? obj->p0[1] : obj->p1[1];
        p0[2] = obj->p0[2] < obj->p1[2] ? obj->p0[2] : obj->p1[2];
        p1[0] = obj->p0[0] > obj->p1[0] ? obj->p0[0] : obj->p1[0];
        p1[1] = obj->p0[1] > obj->p1[1] ? obj->p0[1] : obj->p1[1];
        p1[2] = obj->p0[2] > obj->p1[2] ? obj->p0[2] : obj->p1[2];

        if (gui_coord_vec("Min", p0, !is_2d)) changed = true;
        if (gui_coord_vec("Max", p1, !is_2d)) changed = true;
    } else {
        if (gui_coord_vec("Position", p0, !is_2d)) changed = true;
    }

    if (!changed) return;

    image_history_push(img);
    memcpy(obj->p0, p0, sizeof(obj->p0));
    if (is_zone)
        memcpy(obj->p1, p1, sizeof(obj->p1));
}

static int gui(filter_t *filter_)
{
    image_t *img = goxel.image;
    custom_object_t *obj;
    int type;
    (void)filter_;

    if (!img) return 0;

    ensure_list_current_valid(img);

    gui_checkbox("Show", &img->custom_objects_show, NULL);

    if (gui_button("+ Add new", 1.0, ICON_ADD))
        g_pending_add = true;

    g_pending_delete = NULL;
    g_pending_dup = NULL;
    gui_list(&(gui_list_t){
        .items = (void **)&img->custom_objects,
        .current = (void **)&g_list_current,
        .render = render_list_item,
    });

    if (g_pending_add) {
        g_pending_add = false;
        image_history_push(img);
        g_list_current = custom_object_add(img, CUSTOM_OBJ_POINT_3D);
    }

    if (g_pending_dup) {
        custom_object_t *src = g_pending_dup;
        g_pending_dup = NULL;
        image_history_push(img);
        g_list_current = custom_object_duplicate(img, src);
    }

    if (g_pending_delete) {
        custom_object_t *del = g_pending_delete;
        g_pending_delete = NULL;
        if (g_list_current == del)
            g_list_current = NULL;
        image_history_push(img);
        custom_object_delete(img, del);
    }

    obj = g_list_current;
    if (obj) {
        type = (int)obj->type;
        gui_text("Type");
        if (gui_combo("##obj_type", &type, TYPE_NAMES, 4)) {
            if (type >= 0 && type <= (int)CUSTOM_OBJ_ZONE_3D &&
                type != (int)obj->type) {
                image_history_push(img);
                custom_object_set_type(img, obj, (custom_object_type_t)type);
            }
        }
        gui_object_coords(img, obj);
    }

    if (gui_button("Export", 1.0, 0))
        custom_objects_export_log(img);

    return 0;
}

FILTER_REGISTER(customobjects, filter_custom_objects_t,
                .name = "Custom objects",
                .on_open = on_open,
                .on_close = on_close,
                .override_mouse = true,
                .mouse_fn = on_mouse,
                .panel_width = 380,
                .gui_fn = gui, )
