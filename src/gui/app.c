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

#define INITIAL_FILTER_OFFSET 10
#define RELATIVE_FILTER_OFFSET 40

// Note: duplicated from gui.cpp!  To remove.
static const float ITEM_HEIGHT = 18;
static const float ICON_HEIGHT = 32;
static const float TOP_BAR_HEIGHT = ICON_HEIGHT + 10;

void gui_menu(void);
void gui_tools_panel(void);
void gui_top_bar(void);
void gui_snap_bar(void);
void gui_palette_panel(void);
void gui_layers_panel(void);
void gui_layers_panel_with_scroll();
void gui_view_panel(void);
void gui_material_panel(void);
void gui_light_panel(void);
void gui_cameras_panel(void);
void gui_image_panel(void);
void gui_render_panel(void);
void gui_debug_panel(void);
void gui_export_panel(void);
bool gui_rotation_bar(void);

enum {
    PANEL_NULL,
    PANEL_TOOLS,
    PANEL_PALETTE,
    PANEL_LAYERS,
    PANEL_VIEW,
    PANEL_MATERIAL,
    PANEL_LIGHT,
    PANEL_CAMERAS,
    PANEL_IMAGE,
    PANEL_RENDER,
    PANEL_EXPORT,
    PANEL_DEBUG,
};

static struct {
    const char *name;
    int icon;
    void (*fn)(void);
    bool detached;
} PANELS[] = {
    [PANEL_TOOLS]       = {"Tools", ICON_TOOLS, gui_tools_panel},
    [PANEL_PALETTE]     = {"Palette", ICON_PALETTE, gui_palette_panel},
    [PANEL_LAYERS]      = {"Layers", ICON_LAYERS, gui_layers_panel},
    [PANEL_VIEW]        = {"View", ICON_VIEW, gui_view_panel},
    [PANEL_MATERIAL]    = {"Material", ICON_MATERIAL, gui_material_panel},
    [PANEL_LIGHT]       = {"Light", ICON_LIGHT, gui_light_panel},
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

typedef struct filter_layout_state filter_layout_state_t;

struct filter_layout_state {
    int next_x;
    int next_y;
};

static void on_click(void) {
    if (DEFINED(GUI_SOUND))
        sound_play("click", 1.0, 1.0);
}

static void render_left_panel(void)
{
    int i;
    bool selected;

    for (i = 1; i < (int)ARRAY_SIZE(PANELS); i++) {
        selected = (goxel.gui.current_panel == i);
        if (gui_tab(PANELS[i].name, PANELS[i].icon, &selected)) {
            on_click();
            goxel.gui.current_panel = selected ? i : 0;
        }
    }
}

static void gui_filter_window(void *arg, filter_t *filter)
{
    filter_layout_state_t *state = arg;

    if (filter->is_open) {
        float width = filter->panel_width ? filter->panel_width : goxel.gui.panel_width;
        gui_window_begin(filter->name, state->next_x, state->next_y, width, 0, GUI_WINDOW_MOVABLE);

        if (gui_panel_header(filter->name)) {
            if (filter->on_close) {
                filter->on_close(filter);
            }
            filter->is_open = false;
        }
        filter->gui_fn(filter);

        gui_window_end();
    }

    state->next_x += RELATIVE_FILTER_OFFSET;
    state->next_y += RELATIVE_FILTER_OFFSET;
}

void gui_app(void)
{
    float x = 0, y = 0;
    const char *name;
    int i;
    int flags;
    filter_layout_state_t filter_layout_state;

    goxel.show_export_viewport = false;

    if (GUI_HAS_MENU) {
        if (gui_menu_bar_begin()) {
            gui_menu();

            // Add the Help test in the top menu.
            gui_menu_begin("      ", false);
            gui_menu_begin(goxel.hint_text ?: "", false);
            gui_menu_begin("      ", false);
            gui_menu_begin(goxel.help_text ?: "", false);
            goxel_set_help_text(NULL);
            goxel_set_hint_text(NULL);
            gui_menu_bar_end();
        }
        y = ITEM_HEIGHT + 2;
    }

    gui_window_begin("Top Bar", x, y, 0, TOP_BAR_HEIGHT, 0);
    gui_top_bar();
    gui_window_end();

    if (goxel.tool->has_snap) {
        gui_window_begin("Snap Bar", 280, y, 0, 32.0f, 0);
        gui_snap_bar();
        gui_window_end();
    }

    y += ICON_HEIGHT + 28;
    gui_window_begin("Left Bar", x, y, 0, 0, 0);
    render_left_panel();
    gui_window_end();

    if (goxel.gui.current_panel) {
        x += ICON_HEIGHT + 28;
        name = PANELS[goxel.gui.current_panel].name;
        flags = gui_window_begin(
            name, x, y, goxel.gui.panel_width, 0, GUI_WINDOW_MOVABLE);
        if (gui_panel_header(name))
            goxel.gui.current_panel = 0;
        else
            PANELS[goxel.gui.current_panel].fn();
        gui_window_end();

        if (flags & GUI_WINDOW_MOVED) {
            PANELS[goxel.gui.current_panel].detached = true;
            goxel.gui.current_panel = 0;
        }
    }

    for (i = 0; i < ARRAY_SIZE(PANELS); i++) {
        if (!PANELS[i].detached) continue;
        name = PANELS[i].name;
        gui_window_begin(name, 0, 0, goxel.gui.panel_width, 0, GUI_WINDOW_MOVABLE);
        if (gui_panel_header(name)) {
            PANELS[i].detached = false;
        }
        PANELS[i].fn();
        gui_window_end();
    }
    
    gui_window_begin("Right Bar", (goxel.screen_size[0] - goxel.gui.panel_width - 5), ICON_HEIGHT, goxel.gui.panel_width, (goxel.screen_size[1] - ICON_HEIGHT), 0);
    gui_panel_header("Layers");
    gui_layers_panel_with_scroll();
    gui_window_end();

    filter_layout_state.next_x = x + goxel.gui.panel_width +
                                    INITIAL_FILTER_OFFSET;
    filter_layout_state.next_y = y;
    filters_iter_all(&filter_layout_state, gui_filter_window);

    goxel.pathtrace = goxel.pathtracer.status &&
        (goxel.gui.current_panel == PANEL_RENDER ||
         PANELS[PANEL_RENDER].detached);
}
