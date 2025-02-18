/* Goxel 3D voxels editor
 *
 * copyright (c) 2019 Guillaume Chereau <guillaume@noctua-software.com>
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

#include "file_format.h"
#include "script.h"

int gui_settings_popup(void *data);
int gui_about_popup(void *data);
int gui_about_scripts_popup(void *data);

static void import_image_plane(void)
{
    const char *path;
    const char *filters[] = {"*.png", "*.jpg", "*.jpeg", NULL};
    path = sys_open_file_dialog("Open", NULL, filters, "png, jpeg");
    if (!path) return;
    goxel_import_image_plane(path);
}

static void import_hmap_cmap(void) {
    const char *hmap_path;
    const char *cmap_path;
    const char *filters[] = {"*.png", "*.bmp", "*.jpg", "*.jpeg", NULL};
    hmap_path = sys_open_file_dialog("Choose heightmap image", NULL, filters, "png, jpeg, bmp");
    if (!hmap_path) return;
    cmap_path = sys_open_file_dialog("Choose colormap image", NULL, filters, "png, jpeg, bmp");
    if (!cmap_path) return;
    LOG_I("Importing\nhmap: '%s'\ncmap: '%s'\n", hmap_path, cmap_path);
    goxel_import_hmap_cmap(hmap_path, cmap_path);
}

static file_format_t *g_import_format = NULL;

static int import_gui(void *data)
{
    g_import_format->import_gui(g_import_format);
    if (gui_button("OK", 0, 0)) {
        goxel_import_file(NULL, g_import_format->name);
        return 1;
    }
    return 0;
}

static void import_menu_callback(void *user, file_format_t *f)
{
    if (!gui_menu_item(0, f->name, true)) return;
    if (f->import_gui) {
        g_import_format = f;
        gui_open_popup("Import", 0, NULL, import_gui);
        return;
    }
    goxel_import_file(NULL, f->name);
}

static void export_menu_callback(void *user, file_format_t *f)
{
    if (gui_menu_item(0, f->name, true))
        goxel_export_to_file(NULL, f->name);
}

static void on_script(void *user, const char *name)
{
    if (gui_menu_item(0, name, true))
        script_execute(name);
}

static void on_filter(void *user, const filter_t *filter)
{
    const action_t *action;
    if (gui_menu_item(0, filter->name, true)) {
        action = action_get_by_name(filter->action_id);
        assert(action);
        action_exec(action);
    }
}

void gui_menu(void)
{
    if (gui_menu_begin("File", true)) {
        gui_menu_item(ACTION_reset, "New (32x32x32)", true);
        gui_menu_item(ACTION_reset_512, "New (512x512x64)", true);
        gui_menu_item(ACTION_save, "Save",
                image_get_key(goxel.image) != goxel.image->saved_key);
        gui_menu_item(ACTION_save_as, "Save as", true);
        gui_menu_item(ACTION_open, "Open", true);
        if (gui_menu_begin("Import...", true)) {
            if (gui_menu_item(0, "image plane", true))
                import_image_plane();
            if (gui_menu_item(0, "hmap + cmap", true))
                import_hmap_cmap();
            file_format_iter("r", NULL, import_menu_callback);
            gui_menu_end();
        }
        if (gui_menu_begin("Export As..", true)) {
            file_format_iter("w", NULL, export_menu_callback);
            gui_menu_end();
        }
        gui_menu_item(ACTION_quit, "Quit", true);
        gui_menu_end();
    }
    if (gui_menu_begin("Edit", true)) {
        gui_menu_item(ACTION_layer_clear, "Clear", true);
        gui_menu_item(ACTION_undo, "Undo", true);
        gui_menu_item(ACTION_redo, "Redo", true);
        gui_menu_item(ACTION_copy, "Copy", true);
        gui_menu_item(ACTION_past, "Paste", true);
        if (gui_menu_item(0, "Settings", true))
            gui_open_popup("Settings", GUI_POPUP_FULL | GUI_POPUP_RESIZE,
                           NULL, gui_settings_popup);
        gui_menu_end();
    }
    if (gui_menu_begin("View", true)) {
        gui_menu_item(ACTION_view_left, "Left", true);
        gui_menu_item(ACTION_view_right, "Right", true);
        gui_menu_item(ACTION_view_front, "Front", true);
        gui_menu_item(ACTION_view_top, "Top", true);
        gui_menu_item(ACTION_view_toggle_ortho, "Toggle ortho", true);
        gui_menu_item(ACTION_view_default, "Default", true);
        gui_menu_end();
    }
    if (gui_menu_begin("Filters", true)) { // Note: to translate.
        filters_iter_all(NULL, on_filter);
        gui_menu_end();
    }
    if (gui_menu_begin("Scripts", true)) {
        if (gui_menu_item(0, "About Scripts", true))
            gui_open_popup("Scripts", 0, NULL, gui_about_scripts_popup);
        script_iter_all(NULL, on_script);
        gui_menu_end();
    }
    if (gui_menu_begin("Help", true)) {
        if (gui_menu_item(0, "About", true))
            gui_open_popup("About", GUI_POPUP_RESIZE, NULL, gui_about_popup);
        gui_menu_end();
    }
}
