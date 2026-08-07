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

#ifndef GUI_HAS_ROTATION_BAR
#   define GUI_HAS_ROTATION_BAR 0
#endif

#ifndef GUI_HAS_HELP
#   define GUI_HAS_HELP 1
#endif

#ifndef GUI_HAS_MENU
#   define GUI_HAS_MENU 1
#endif

#ifndef YOCTO
#   define YOCTO 1
#endif

// Note: duplicated from gui.cpp!  To remove.
static const float ITEM_HEIGHT = 18;
static const float ICON_HEIGHT = 32;
static const float TOP_BAR_HEIGHT = ICON_HEIGHT + 10;
static const float MENU_BAR_HEIGHT = ITEM_HEIGHT + 2;

void gui_menu(void);
void gui_tools_panel(void);
void gui_top_bar(void);
void gui_snap_bar(void);
void gui_map_colors_bar(void);
void gui_layers_panel(void);
void gui_layers_panel_with_scroll();
void gui_view_panel(void);
void gui_material_panel(void);
void gui_cameras_panel(void);
void gui_image_panel(void);
void gui_render_panel(void);
void gui_debug_panel(void);
void gui_export_panel(void);
bool gui_rotation_bar(void);

static struct {
    const char *name;
    int icon;
    void (*fn)(void);
    bool detached;
} PANELS[] = {
    [PANEL_TOOLS]       = {"Tools", ICON_TOOLS, gui_tools_panel, true},
    [PANEL_PALETTE]     = {"Palette", ICON_PALETTE, NULL},
    [PANEL_LAYERS]      = {"Layers", ICON_LAYERS, gui_layers_panel},
    [PANEL_VIEW]        = {"View", ICON_VIEW, gui_view_panel},
    [PANEL_MATERIAL]    = {"Material", ICON_MATERIAL, gui_material_panel},
    [PANEL_CAMERAS]     = {"Cameras", ICON_CAMERA, gui_cameras_panel},
    [PANEL_IMAGE]       = {"Image", ICON_IMAGE, gui_image_panel},
#if YOCTO
    [PANEL_RENDER]      = {"Render", ICON_RENDER, gui_render_panel},
#endif
    [PANEL_EXPORT]      = {"Export", ICON_EXPORT, gui_export_panel},
#if DEBUG
    [PANEL_DEBUG]       = {"Debug", ICON_DEBUG, gui_debug_panel},
#endif
};

void gui_panel_show_detached(int panel)
{
    if (panel <= PANEL_NULL || panel >= (int)ARRAY_SIZE(PANELS))
        return;
    if (!PANELS[panel].fn)
        return;
    if (goxel.gui.current_panel == panel)
        goxel.gui.current_panel = 0;
    PANELS[panel].detached = true;
}

bool gui_panel_is_detached(int panel)
{
    if (panel <= PANEL_NULL || panel >= (int)ARRAY_SIZE(PANELS))
        return false;
    return PANELS[panel].fn && PANELS[panel].detached;
}

void gui_panel_toggle_detached(int panel)
{
    if (panel <= PANEL_NULL || panel >= (int)ARRAY_SIZE(PANELS))
        return;
    if (!PANELS[panel].fn)
        return;
    if (PANELS[panel].detached) {
        PANELS[panel].detached = false;
        return;
    }
    gui_panel_show_detached(panel);
}

void gui_palette_window_toggle(void)
{
    if (goxel.gui.palette_win_open) {
        goxel.gui.palette_win_open = false;
        goxel.gui.palette_win_collapsed = false;
    } else {
        goxel.gui.palette_win_open = true;
        goxel.gui.palette_win_expand_once = true;
    }
}

void gui_layers_panel_toggle(void)
{
    goxel.gui.layers_panel_open = !goxel.gui.layers_panel_open;
}

static void gui_filter_window(void *arg, filter_t *filter)
{
    float width;

    (void)arg;
    if (!filter->is_open)
        return;

    width = filter->panel_width ? filter->panel_width : goxel.gui.panel_width;
    gui_window_begin(filter->name, 0, 0, width, 0,
                     GUI_WINDOW_MOVABLE | GUI_WINDOW_CENTER);

    if (gui_panel_header(filter->name)) {
        if (filter->on_close) {
            filter->on_close(filter);
        }
        filter->is_open = false;
    }
    filter->gui_fn(filter);

    gui_window_end();
}

void gui_app(void)
{
    float x = 0, y = 0;
    const char *name;
    int i;
    /* Extra Tools width when body scrolled last frame (avoids content squeeze). */
    static bool tools_had_v_scrollbar = false;

    goxel.show_export_viewport = false;

    if (goxel.gui.current_panel == PANEL_PALETTE)
        goxel.gui.current_panel = 0;

    if (GUI_HAS_MENU) {
        if (gui_menu_bar_begin()) {
            gui_menu();

            // Add the Help test in the top menu.
            gui_menu_bar_text(goxel.hint_text);
            gui_menu_bar_text(goxel.help_text);
            goxel_set_help_text(NULL);
            goxel_set_hint_text(NULL);
            gui_menu_bar_panel_toggles();
            gui_menu_bar_end();
        }
        y = MENU_BAR_HEIGHT;
    }

    if (!goxel.gui.ui_visible) {
        goxel.pathtrace = false;
        return;
    }

    gui_window_begin("Top Bar", x, y, 0, TOP_BAR_HEIGHT, 0);
    gui_top_bar();
    gui_window_end();

    if (goxel.tool->has_snap) {
        gui_window_begin("Snap Bar", 280, y, 0, 32.0f, 0);
        gui_snap_bar();
        gui_window_end();
    }
    if (tool_uses_map_recent_colors(goxel.tool) && goxel.image) {
        float map_x = goxel.tool->has_snap ? 560.0f : 280.0f;
        /* w=0 with no widgets sizes the window to the full viewport; keep min. */
        float map_w = goxel.image->recent_color_count == 0 ? 8.0f : 0.0f;
        gui_window_begin("Map colors", map_x, y, map_w, 32.0f, 0);
        gui_map_colors_bar();
        gui_window_end();
    }

    y += ICON_HEIGHT + 28;

    for (i = 0; i < ARRAY_SIZE(PANELS); i++) {
        float panel_w;
        gui_window_ret_t win_ret;

        if (!PANELS[i].detached || !PANELS[i].fn) continue;
        name = PANELS[i].name;
        panel_w = goxel.gui.panel_width;
        /* Toolbox stays top-left; other panels open centred. */
        if (i == PANEL_TOOLS) {
            if (tools_had_v_scrollbar)
                panel_w += 20.0f;
            gui_window_begin(name, 0, y, panel_w, 0, GUI_WINDOW_MOVABLE);
        } else {
            gui_window_begin(name, 0, 0, panel_w, 0,
                             GUI_WINDOW_MOVABLE | GUI_WINDOW_CENTER);
        }
        if (gui_panel_header(name)) {
            PANELS[i].detached = false;
        } else {
            PANELS[i].fn();
        }
        win_ret = gui_window_end();
        if (i == PANEL_TOOLS)
            tools_had_v_scrollbar = win_ret.has_v_scrollbar;
    }
    
    if (goxel.gui.layers_panel_open) {
        float layers_w = goxel.gui.layers_panel_width;
        float layers_max_w = goxel.screen_size[0] * 0.5f;

        if (layers_w < GUI_PANEL_WIDTH_NORMAL)
            layers_w = goxel.gui.layers_panel_width = GUI_PANEL_WIDTH_NORMAL;
        if (layers_max_w < GUI_PANEL_WIDTH_NORMAL)
            layers_max_w = GUI_PANEL_WIDTH_NORMAL;

        gui_window_begin("Right Bar",
                (goxel.screen_size[0] - layers_w), MENU_BAR_HEIGHT , // x, y
                layers_w, (goxel.screen_size[1] - MENU_BAR_HEIGHT), 0); // w, y, flags
        if (gui_panel_header("Layers"))
            goxel.gui.layers_panel_open = false;
        else
            gui_layers_panel_with_scroll();
        /* After content so the resize cursor wins over panel widgets. */
        gui_window_resize_left_edge(&goxel.gui.layers_panel_width,
                                    GUI_PANEL_WIDTH_NORMAL, layers_max_w);
        gui_window_end();
    }

    /* Open filters centred (same as other detachable panels). */
    filters_iter_all(NULL, gui_filter_window);

    placer_gui_history_floating();

    gui_palette_floating();

    goxel.pathtrace = goxel.pathtracer.status && PANELS[PANEL_RENDER].detached;
}
