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

#ifndef GUI_CUSTOM_TOPBAR

static int gui_mode_select(void)
{
    bool v;
    char label[64];
    const action_t *action = NULL;
    int i;
    const struct {
        int mode;
        const char *label;
        int action;
        int icon;
    } values[] = {
        {MODE_OVER,     "Add",     ACTION_set_mode_add,    ICON_MODE_ADD},
        {MODE_SUB,      "Sub",     ACTION_set_mode_sub,    ICON_MODE_SUB},
        {MODE_PAINT,    "Paint",   ACTION_set_mode_paint,  ICON_MODE_PAINT},
    };
    // XXX: almost the same as in tools_panel.
    gui_group_begin(NULL);
    gui_row_begin(0);
    for (i = 0; i < ARRAY_SIZE(values); i++) {
        v = goxel.painter.mode == values[i].mode;
        action = action_get(values[i].action, true);
        sprintf(label, "%s (%s)", values[i].label, action->shortcut);
        if (gui_selectable_icon(label, &v, values[i].icon)) {
            action_exec(action);
        }
    }
    gui_row_end();
    gui_group_end();
    return 0;
}

// A copy of that within tools.c
static bool inline_snap_button(const char *label, int s)
{
    bool v = goxel.snap_mask & s;
    if (gui_condensed_selectable(label, &v, NULL, strlen(label) * 10)) {
        set_flag(&goxel.snap_mask, s, v);
        return true;
    }
    return false;
}
// Condensed inline version of tools.c
static int tool_gui_snap_inline(void)
{
    gui_group_begin(NULL);
    gui_row_begin(0);
    gui_text("Snap on: ");
    gui_same_line();
    inline_snap_button("Volume", SNAP_VOLUME);
    inline_snap_button("Plane", SNAP_PLANE);
    if (!box_is_null(goxel.selection)) {
        inline_snap_button("Sel In", SNAP_SELECTION_IN);
        inline_snap_button("Sel out", SNAP_SELECTION_OUT);
    }
    inline_snap_button("Image box", SNAP_IMAGE_BOX);
    gui_row_end();
    gui_group_end();
    return 0;
}

void gui_top_bar(void)
{
    gui_row_begin(0); {
        gui_group_begin(NULL); {
            gui_row_begin(0); {
                gui_action_button(ACTION_undo, NULL, 0);
                gui_action_button(ACTION_redo, NULL, 0);
            } gui_row_end();
        } gui_group_end();
        gui_row_begin(0); {
            gui_action_button(ACTION_layer_clear, NULL, 0);
            gui_mode_select();
            gui_color("##color", goxel.painter.color);
        } gui_row_end();
    } gui_row_end();
}

void gui_snap_bar(void) {
    gui_row_begin(0); {
        if (goxel.tool->has_snap) {
            tool_gui_snap_inline();
        }
    } gui_row_end();
}

#endif // GUI_CUSTOM_TOPBAR
