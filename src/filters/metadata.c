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

typedef struct {
    filter_t filter;
} filter_metadata_t;

#define TEMPLATE_MAX 64

typedef struct {
    char paths[TEMPLATE_MAX][1024];
    char names[TEMPLATE_MAX][256];
    int count;
    int selected;
} template_popup_t;

static template_popup_t g_template_popup;
static bool g_open_template_popup = false;

static void on_open(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(true);
}

static void on_close(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(false);
    custom_objects_set_list_selected(NULL);
}

static int on_template_file(const char *dirpath, const char *name, void *user)
{
    template_popup_t *popup = user;
    char display[256];
    size_t len;

    if (!str_endswith(name, ".json")) return 0;
    if (popup->count >= TEMPLATE_MAX) return 0;
    snprintf(popup->paths[popup->count], sizeof(popup->paths[0]),
             "%s/%s", dirpath, name);
    snprintf(display, sizeof(display), "%s", name);
    len = strlen(display);
    if (len > 5 && strcasecmp(display + len - 5, ".json") == 0)
        display[len - 5] = '\0';
    snprintf(popup->names[popup->count], sizeof(popup->names[0]), "%s", display);
    popup->count++;
    return 0;
}

static void refresh_template_list(template_popup_t *popup)
{
    char dir[1024];

    popup->count = 0;
    popup->selected = 0;
    if (!sys_get_user_dir()) return;
    snprintf(dir, sizeof(dir), "%s/metadata-templates", sys_get_user_dir());
    sys_make_dir(dir);
    sys_list_dir(dir, on_template_file, popup);
}

static int template_popup_gui(void *data)
{
    template_popup_t *popup = &g_template_popup;
    image_t *img = goxel.image;
    const char *names[TEMPLATE_MAX];
    int i, has_items, ret = 0;

    (void)data;
    has_items = popup->count > 0;
    if (has_items) {
        if (popup->selected < 0) popup->selected = 0;
        if (popup->selected >= popup->count)
            popup->selected = popup->count - 1;
        for (i = 0; i < popup->count; i++)
            names[i] = popup->names[i];
        gui_combo("Template", &popup->selected, names, popup->count);
    } else {
        gui_text("No templates found.");
    }

    if (img && img->custom_objects)
        gui_text("Replaces all current items.");

    gui_row_begin(0);
    if (has_items && gui_button("Load template", 0, 0)) {
        image_history_push(img);
        custom_objects_load_template_json(popup->paths[popup->selected], img);
        custom_objects_set_list_selected(NULL);
        ret = 1;
    } else if (gui_button("Cancel", 0, 0)) {
        ret = 2;
    }
    gui_row_end();
    return ret;
}

static void on_mouse(filter_t *filter_, const float viewport[4])
{
    (void)filter_;
    custom_objects_edit_iter(viewport);
}

static int gui(filter_t *filter_)
{
    image_t *img = goxel.image;
    (void)filter_;

    if (!img) return 0;

    gui_text_wrapped(
        "Create and manage metadata about the map.\n"
        "Use the 'Group' type to collect multiple items under one parent.\n"
        "Use the 'S' button to temporarily solo the visibility to the chosen item.");

    if (gui_button("Load template", 1.0, 0))
        g_open_template_popup = true;

    if (g_open_template_popup) {
        g_open_template_popup = false;
        refresh_template_list(&g_template_popup);
        gui_open_popup_sized("Load template", 300, 150, 0,
                             NULL, template_popup_gui);
    }

    gui_checkbox("Show/hide all", &img->custom_objects_show,
                 "Enable/disable the showing of any of the map metadata items "
                 "in the viewport");

    metadata_gui_panel(img);
    return 0;
}

FILTER_REGISTER(customobjects, filter_metadata_t,
                .name = "Metadata",
                .on_open = on_open,
                .on_close = on_close,
                .override_mouse = true,
                .mouse_fn = on_mouse,
                .panel_width = 380,
                .gui_fn = gui, )
