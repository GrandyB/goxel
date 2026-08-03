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

#include "../../ext_src/stb/stb_ds.h"

#ifndef YOCTO
#   define YOCTO 1
#endif

int gui_settings_popup(void *data);
int gui_about_popup(void *data);

static void import_image_reference(void)
{
    const char *path;
    const char *filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", NULL};
    path = sys_open_file_dialog("Open", NULL, filters, "png, jpeg, bmp");
    if (!path) return;
    goxel_import_image_reference(path);
}

static void import_image_volume(void)
{
    const char *path;
    const char *filters[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", NULL};
    path = sys_open_file_dialog("Open", NULL, filters, "png, jpeg, bmp");
    if (!path) return;
    goxel_import_image_volume(path);
}

static void import_hmap_cmap(void) {
    const char *path;
    // sys_open_file_dialog returns a pointer to a shared static buffer that is
    // overwritten by the next call, so copy the first result before reusing it.
    char hmap_path[1024];
    const char *cmap_path;
    const char *filters[] = {"*.bmp", NULL};
    path = sys_open_file_dialog("Choose heightmap image", NULL, filters, "bmp");
    if (!path) return;
    snprintf(hmap_path, sizeof(hmap_path), "%s", path);
    cmap_path = sys_open_file_dialog("Choose colormap image", NULL, filters, "bmp");
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

/* Toggle the filter window, opening it with the given layer scope. An open
 * filter clicked from the other menu only switches scope. */
static void filter_menu_item(filter_t *filter, bool current_only)
{
    const action_t *action;
    if (!gui_menu_item(0, filter->name, true)) return;
    if (filter->is_open && filter->current_only != current_only) {
        filter->current_only = current_only;
        return;
    }
    filter->current_only = current_only;
    action = action_get_by_name(filter->action_id);
    assert(action);
    action_exec(action);
}

static void on_filter(void *user, filter_t *filter)
{
    filter_menu_item(filter, false);
}

/* Same as on_filter, but the filter defaults to the active layer only. */
static void on_layer_filter(void *user, filter_t *filter)
{
    filter_menu_item(filter, true);
}

/* Filter shown as an on/off toggle, for filters living in the View menu. */
static void on_filter_toggle(void *user, filter_t *filter)
{
    const action_t *action;
    if (gui_menu_toggle(0, filter->name, filter->is_open, true)) {
        action = action_get_by_name(filter->action_id);
        assert(action);
        action_exec(action);
    }
}

void gui_menu(void)
{
    int i;
    if (gui_menu_begin("File", true)) {
        if (gui_menu_begin("New", true)) {
            gui_menu_item(ACTION_reset, "New (32x32x32)", true);
            gui_menu_item(ACTION_reset_512, "New (512x512x64)", true);
            gui_menu_end();
        }
        gui_menu_item(ACTION_save, "Save",
                image_get_key(goxel.image) != goxel.image->saved_key);
        gui_menu_item(ACTION_save_as, "Save as", true);
        gui_menu_item(ACTION_open, "Open", true);
        if (gui_menu_begin("Open Recent", true)) {
            for (i = 0; i < arrlen(goxel.recent_files); i++) {
                if (gui_menu_item(0, goxel.recent_files[i], true)) {
                    goxel_open_file(goxel.recent_files[i]);
                }
            }
            gui_menu_end();
        };
        if (gui_menu_begin("Import...", true)) {
            if (gui_menu_item(0, "image reference", true))
                import_image_reference();
            if (gui_menu_item(0, "image volume", true))
                import_image_volume();
            if (gui_menu_item(0, "hmap + cmap", true))
                import_hmap_cmap();
            file_format_iter("r", NULL, import_menu_callback);
            gui_menu_end();
        }
        if (gui_menu_begin("Export As..", true)) {
            file_format_iter("w", NULL, export_menu_callback);
            gui_menu_end();
        }
        if (gui_menu_item(0, "Export window...", true))
            gui_panel_show_detached(PANEL_EXPORT);
#if YOCTO
        if (gui_menu_item(0, "Render", true))
            gui_panel_show_detached(PANEL_RENDER);
#endif
        gui_menu_item(ACTION_quit, "Quit", true);
        gui_menu_end();
    }
    if (gui_menu_begin("Edit", true)) {
        gui_menu_item(ACTION_layer_clear, "Clear", true);
        gui_menu_item(ACTION_undo, "Undo", true);
        gui_menu_item(ACTION_redo, "Redo", true);
        gui_menu_item(ACTION_copy, "Copy", true);
        gui_menu_item(ACTION_paste, "Paste", true);
        filters_iter_menu("edit", NULL, NULL, on_filter);
        if (gui_menu_item(0, "Settings", true))
            gui_open_popup("Settings", GUI_POPUP_FULL | GUI_POPUP_RESIZE,
                           NULL, gui_settings_popup);
        gui_menu_end();
    }
    if (gui_menu_begin("View", true)) {
        gui_menu_checkbox_column(true);
        gui_menu_toggle(ACTION_view_toggle_tools, "Tools",
                        gui_panel_is_detached(PANEL_TOOLS), true);
        if (gui_menu_toggle(0, "Layers", goxel.gui.layers_panel_open, true))
            gui_layers_panel_toggle();
        if (gui_menu_toggle(0, "Palette",
                            goxel.gui.palette_win_open, true))
            gui_palette_window_toggle();
        if (gui_menu_toggle(0, "Cameras",
                            gui_panel_is_detached(PANEL_CAMERAS), true))
            gui_panel_toggle_detached(PANEL_CAMERAS);
        if (gui_menu_toggle(0, "Materials",
                            gui_panel_is_detached(PANEL_MATERIAL), true))
            gui_panel_toggle_detached(PANEL_MATERIAL);
        filters_iter_menu("view", NULL, NULL, on_filter_toggle);
        gui_separator();
        if (gui_menu_toggle(0, "View settings",
                            gui_panel_is_detached(PANEL_VIEW), true))
            gui_panel_toggle_detached(PANEL_VIEW);
        gui_separator();
        gui_menu_item(ACTION_view_left, "Left", true);
        gui_menu_item(ACTION_view_right, "Right", true);
        gui_menu_item(ACTION_view_front, "Front", true);
        gui_menu_item(ACTION_view_top, "Top", true);
        gui_menu_toggle(ACTION_view_toggle_ortho, "Toggle ortho",
                        goxel.image->active_camera &&
                        goxel.image->active_camera->ortho, true);
        gui_menu_item(ACTION_view_default, "Default", true);
        gui_separator();
        gui_menu_toggle(ACTION_view_toggle_ui, "Show UI",
                        goxel.gui.ui_visible, true);
        gui_separator();
        if (gui_menu_toggle(0, "Image box", !goxel.hide_box, true)) {
            goxel.hide_box = !goxel.hide_box;
            settings_save();
        }
        if (gui_menu_toggle(0, "Wrap preview", goxel.wrap_view, true))
            goxel_wrap_view_set(!goxel.wrap_view);
        gui_separator();
        if (gui_menu_toggle(0, "View cube", goxel.gui.view_cube_open, true))
            goxel.gui.view_cube_open = !goxel.gui.view_cube_open;
        if (gui_menu_toggle(0, "Camera presets",
                            goxel.gui.camera_presets_open, true))
            goxel.gui.camera_presets_open = !goxel.gui.camera_presets_open;
        gui_menu_end();
    }
    if (gui_menu_begin("Image", true)) {
        if (gui_menu_item(0, "Image size...", true))
            gui_panel_show_detached(PANEL_IMAGE);
        gui_menu_item(ACTION_img_auto_resize, "Auto resize (all/original)", true);
        gui_menu_item(ACTION_img_auto_resize_reset,
                      "Crop to visible & reset origin", true);
        gui_menu_item(ACTION_img_crop_to_box, "Crop to image box",
                      !box_is_null(goxel.image->box));
        gui_separator();
        filters_iter_menu("image", NULL, NULL, on_filter);
        gui_separator();
        if (gui_menu_item(0, "Rotate 90deg clockwise", true))
            goxel_rotate_90(+1, false);
        if (gui_menu_item(0, "Rotate 90deg anti-clockwise", true))
            goxel_rotate_90(-1, false);
        gui_menu_end();
    }
    if (goxel.image && goxel.image->active_layer &&
        gui_menu_begin("Layer", true)) {
        filters_iter_menu("adjustments", NULL, NULL, on_layer_filter);
        gui_separator();
        filters_iter_menu("image", NULL, NULL, on_layer_filter);
        gui_separator();
        if (gui_menu_item(0, "Rotate 90deg clockwise", true))
            goxel_rotate_90(+1, true);
        if (gui_menu_item(0, "Rotate 90deg anti-clockwise", true))
            goxel_rotate_90(-1, true);
        gui_menu_end();
    }
    if (gui_menu_begin("Effects", true)) {
        if (gui_menu_begin("Generate", true)) {
            filters_iter_menu("effects", "generate", NULL, on_filter);
            gui_menu_end();
        }
        if (gui_menu_begin("Plan", true)) {
            filters_iter_menu("effects", "plan", NULL, on_filter);
            gui_menu_end();
        }
        if (gui_menu_begin("Lighting", true)) {
            filters_iter_menu("effects", "lighting", NULL, on_filter);
            gui_menu_end();
        }
        if (gui_menu_begin("Palette", true)) {
            filters_iter_menu("effects", "palette", NULL, on_filter);
            gui_menu_end();
        }
        if (gui_menu_begin("Utilities", true)) {
            filters_iter_menu("effects", "utilities", NULL, on_filter);
            gui_menu_end();
        }
        gui_menu_end();
    }
    if (gui_menu_begin("Help", true)) {
        if (gui_menu_item(0, "About", true))
            gui_open_popup("About", GUI_POPUP_RESIZE, NULL, gui_about_popup);
        gui_menu_end();
    }
}
