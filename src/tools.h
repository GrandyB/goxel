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

#ifndef TOOLS_H
#define TOOLS_H

#include "shape.h"
#include "volume_utils.h"

enum {
    TOOL_NONE = 0,
    TOOL_BRUSH,
    TOOL_SHAPE,
    //TOOL_LINE,
    TOOL_LASER,
    TOOL_SET_PLANE,
    TOOL_MOVE,
    TOOL_PICK_COLOR,
    TOOL_SELECTION,
    TOOL_PROCEDURAL,
    TOOL_EXTRUDE,
    TOOL_FUZZY_SELECT,
    TOOL_RECT_SELECT,
    TOOL_PLACER,
    TOOL_FILL,
    TOOL_CLONE_STAMP,
    TOOL_CURSOR,
    TOOL_SMOOTH,

    TOOL_COUNT
};

enum {
    // Tools flags.
    TOOL_REQUIRE_CAN_EDIT = 1 << 0, // Set to tools that can edit the layer.
    TOOL_REQUIRE_CAN_MOVE = 1 << 1, // Set to tools that can move the layer.
    TOOL_ALLOW_PICK_COLOR = 1 << 2, // Ctrl switches to pick color tool.
    TOOL_SHOW_MASK        = 1 << 3,
};

// Tools
typedef struct tool tool_t;
struct tool {
    int id;
    const char *action_id;
    int action_idx;
    int (*iter_fn)(tool_t *tool, const painter_t *painter,
                   const float viewport[4]);
    int (*gui_fn)(tool_t *tool);
    void (*on_open)(tool_t *tool);  /* Entering the tool (after switch). */
    void (*on_close)(tool_t *tool); /* Leaving the tool (before switch). */
    const char *default_shortcut;
    int state; // XXX: to be removed I guess.
    int flags;
    const char *name;
    bool has_snap;
};

#define TOOL_REGISTER(id_, name_, klass_, ...) \
    static klass_ GOX_tool_##id_ = {\
            .tool = { \
                .action_idx = ACTION_tool_set_##name_, \
                .id = id_, .action_id = "tool_set_" #name_, __VA_ARGS__ \
            } \
        }; \
    static void GOX_register_tool_##tool_(void) __attribute__((constructor)); \
    static void GOX_register_tool_##tool_(void) { \
        tool_register_(&GOX_tool_##id_.tool); \
    }

void tool_register_(tool_t *tool);
const tool_t *tool_get(int id);

int tool_iter(tool_t *tool, const painter_t *painter, const float viewport[4]);
int tool_gui(tool_t *tool);

/* Wireframe boxes: Cursor-tool gizmos, plus layers-panel hover bbox for any
 * tool. Call from the 3D view render path so they stay visible over UI. */
void tool_cursor_render(void);
/* Cursor-tool layer name labels (always or Alt, per tool panel), plus
 * layers-panel hover / apostrophe pick / arrow-key flash labels; call
 * during the gui frame. */
void tool_cursor_render_labels(void);
/* Clear panel-hover solo at the start of each GUI frame. */
void tool_cursor_on_gui_frame(void);
/* Solo this layer's bounding box while the mouse is over its panel row
 * (any tool). */
void tool_cursor_set_panel_hover(layer_t *layer);
/* Hold-to-preview for select-layer-under-cursor: bbox + name at label_pos
 * (world hit under the cursor). Pass NULL layer to clear. */
void tool_cursor_set_pick_preview(layer_t *layer, const float label_pos[3]);
/* Show a visible layer's wireframe bbox + centred name for duration_sec
 * (e.g. arrow-key layer switch). No-op if hidden or empty. */
void tool_cursor_flash_layer_bbox(layer_t *layer, double duration_sec);
/* Drop in-progress cursor drag / viewport hover (e.g. panel selection change). */
void tool_cursor_clear_edit(void);

/* Drop idle tool_volume preview (brush/shape/etc.). Keep mid-stroke previews. */
void tool_clear_preview(void);

/* Brush / shape / laser / fill / etc. - show "recent map colors" in snap bar. */
bool tool_uses_map_recent_colors(const tool_t *tool);

int tool_gui_mask_mode(void);
int tool_gui_shape(const shape_t **shape);
int tool_gui_radius(void);
int tool_gui_radius_xy(void);
int tool_gui_radius_xy_values(float *radius_x, float *radius_y);
int tool_gui_radius_xyz_values(float *radius_x, float *radius_y,
                               float *radius_z);
int tool_gui_smoothness(void);
int tool_gui_inherit(void);
int tool_gui_noise(int section_flags);
int tool_gui_color(bool always_show_opacity);
int tool_gui_color_default_collapsed(bool always_show_opacity);
int tool_gui_brush_source(const char *tabsheet_id);
int tool_gui_symmetry(void);
int tool_gui_drag_mode(int *mode);

#endif // TOOLS_H
