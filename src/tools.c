/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
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

static const tool_t *g_tools[TOOL_COUNT] = {};

static void a_tool_set(void *data)
{
    if (goxel.tool_volume) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
    }
    goxel.tool = (tool_t*)data;
}

void tool_register_(tool_t *tool)
{
    action_t action;
    action = (action_t) {
        .id = tool->action_id,
        .default_shortcut = tool->default_shortcut,
        .help = "set tool",
        .cfunc_data = a_tool_set,
        .data = (void*)tool,
        .flags = ACTION_CAN_EDIT_SHORTCUT,
    };
    action_register(&action, tool->action_idx);
    g_tools[tool->id] = tool;
}

const tool_t *tool_get(int id)
{
    return g_tools[id];
}

static int pick_color_gesture(gesture3d_t *gest, void *user)
{
    cursor_t *curs = &goxel.cursor;
    const volume_t *volume = goxel_get_layers_volume(goxel.image);
    int pi[3] = {floor(curs->pos[0]),
                 floor(curs->pos[1]),
                 floor(curs->pos[2])};
    uint8_t color[4];
    curs->snap_mask = SNAP_VOLUME;
    curs->snap_offset = -0.5;

    goxel_set_help_text("Click on a voxel to pick the color");
    if (!curs->snaped) return 0;
    volume_get_at(volume, NULL, pi, color);
    color[3] = 255;
    goxel_set_help_text("pick: %d %d %d", color[0], color[1], color[2]);
    if (curs->flags & CURSOR_PRESSED) vec4_copy(color, goxel.painter.color);
    return 0;
}

static gesture3d_t g_pick_color_gesture = {
    .type = GESTURE_HOVER,
    .callback = pick_color_gesture,
    .buttons = CURSOR_CTRL,
};

int tool_iter(tool_t *tool, const painter_t *painter, const float viewport[4])
{
    assert(tool);
    if (    (tool->flags & TOOL_REQUIRE_CAN_EDIT) &&
            !image_layer_can_edit(goxel.image, goxel.image->active_layer)) {
        goxel_set_help_text("Cannot edit this layer");
        return 0;
    }
    tool->state = tool->iter_fn(tool, painter, viewport);

    if (tool->flags & TOOL_ALLOW_PICK_COLOR)
        gesture3d(&g_pick_color_gesture, &goxel.cursor, NULL);

    return tool->state;
}

int tool_gui(tool_t *tool)
{
    if (!tool->gui_fn) return 0;
    return tool->gui_fn(tool);
}


static bool snap_button(const char *label, int s)
{
    bool v = goxel.snap_mask & s;
    if (gui_selectable(label, &v, NULL, -1)) {
        set_flag(&goxel.snap_mask, s, v);
        return true;
    }
    return false;
}

int tool_gui_snap(void)
{
    float v;
    if (gui_section_begin("Snap on", true)) {
        gui_group_begin(NULL);
        gui_row_begin(2);
        snap_button("Volume", SNAP_VOLUME);
        snap_button("Plane", SNAP_PLANE);
        gui_row_end();
        if (!box_is_null(goxel.selection)) {
            gui_row_begin(2);
            snap_button("Sel In", SNAP_SELECTION_IN);
            snap_button("Sel out", SNAP_SELECTION_OUT);
            gui_row_end();
        }
        if (!box_is_null(goxel.image->box)) {
            snap_button("Image box", SNAP_IMAGE_BOX);
        }
        gui_group_end();

        v = goxel.snap_offset;
        if (gui_input_float("Offset", &v, 0.1, -1, +1, "%.1f"))
            goxel.snap_offset = clamp(v, -1, +1);
    }
    gui_section_end();
    return 0;
}

static bool mask_mode_button(const char *label, int s)
{
    bool v = goxel.mask_mode == s;
    if (gui_selectable(label, &v, NULL, 0)) {
        goxel.mask_mode = s;
        return true;
    }
    return false;
}

int tool_gui_mask_mode(void)
{
    gui_text("Mask");
    gui_group_begin(NULL);
    gui_row_begin(3);
    mask_mode_button("Set", MODE_REPLACE);
    mask_mode_button("Add", MODE_OVER);
    mask_mode_button("Sub", MODE_SUB);
    gui_row_end();
    gui_group_end();
    return 0;
}

int tool_gui_shape(const shape_t **shape)
{
    struct {
        const char  *name;
        shape_t     *shape;
        int         icon;
    } shapes[] = {
        {"Sphere", &shape_sphere, ICON_SHAPE_SPHERE},
        {"Cube", &shape_cube, ICON_SHAPE_CUBE},
        {"Cylinder", &shape_cylinder, ICON_SHAPE_CYLINDER},
    };
    gui_icon_info_t grid[64] = {};
    shape = shape ?: &goxel.painter.shape;
    int i, ret = 0;
    int current;
    const int nb = ARRAY_SIZE(shapes);

    if (gui_section_begin("Shape", true)) {
        for (i = 0; i < nb; i++) {
            grid[i] = (gui_icon_info_t) {
                .label = shapes[i].name,
                .icon = shapes[i].icon,
            };
            if (*shape == shapes[i].shape) current = i;
        }
        if (gui_icons_grid(nb, grid, &current)) {
            *shape = shapes[current].shape;
            ret = 1;
        }
    }
    gui_section_end();
    return ret;
}

int tool_gui_radius(void)
{
    gui_text("Size");
    gui_group_begin(NULL);
    int x = goxel.radius_x * 2;
    if (gui_input_int("Diameter X", &x, 1, 128)) {
        x = clamp(x, 1, 128);
        goxel.radius_x = x / 2.0;
    }
    int y = goxel.radius_y * 2;
    if (gui_input_int("Diameter Y", &y, 1, 128)) {
        y = clamp(y, 1, 128);
        goxel.radius_y = y / 2.0;
    }
    int z = goxel.radius_z * 2;
    if (gui_input_int("Diameter Z", &z, 1, 128)) {
        z = clamp(z, 1, 128);
        goxel.radius_z = z / 2.0;
    }
    gui_group_end();
    return 0;
}

int tool_gui_smoothness(void)
{
    bool s;
    s = goxel.painter.smoothness;
    if (gui_checkbox("Antialiased", &s, NULL)) {
        goxel.painter.smoothness = s ? 1 : 0;
    }
    return 0;
}

int tool_gui_noise(void)
{
    if (gui_section_begin("Noise", true)) {
        bool noise_enabled = goxel.painter.noise_enabled;
        if (gui_checkbox("Enable", &noise_enabled, NULL)) {
            goxel.painter.noise_enabled = noise_enabled ? 1 : 0;
        }
        if (noise_enabled) {
            int intensity = goxel.painter.noise_intensity;
            if (gui_input_int("Intensity", &intensity, 0, 100)) {
                intensity = clamp(intensity, 0, 100);
                goxel.painter.noise_intensity = intensity;
            }
            int saturation = goxel.painter.noise_saturation;
            if (gui_input_int("Saturation", &saturation, 0, 100)) {
                saturation = clamp(saturation, 0, 100);
                goxel.painter.noise_saturation = saturation;
            }
            int coverage = goxel.painter.noise_coverage;
            if (gui_input_int("Coverage", &coverage, 0, 100)) {
                coverage = clamp(coverage, 0, 100);
                goxel.painter.noise_coverage = coverage;
            }
            if (gui_button("Reset", 0, 0)) {
                goxel.painter.noise_intensity = 20;
                goxel.painter.noise_saturation = 5;
                goxel.painter.noise_coverage = 100;
            }
        }
    }
    gui_section_end();
    return 0;
}

static bool color_blend_button(const char *label, int s)
{
    bool v = goxel.painter.color_blend == s;
    if (gui_selectable(label, &v, NULL, -1)) {
        goxel.painter.color_blend = s;
        return true;
    }
    return false;
}

int tool_gui_color(void)
{
    if (gui_section_begin("Color", true)) {
        gui_color_inline("", goxel.painter.color);
        if (goxel.painter.mode == MODE_PAINT) {
            gui_color_opacity(goxel.painter.color);
        }
        gui_text("Blend mode");
        gui_group_begin(NULL);
        gui_row_begin(4);
        color_blend_button("User", COLOR_USER);
        color_blend_button("Inherit", COLOR_INHERITED);
        color_blend_button("Add", COLOR_ADD_INHERITED);
        color_blend_button("Mid", COLOR_MIDPOINT_INHERITED);
        gui_row_end();
        gui_group_end();
    }
    
    tool_gui_noise();
    return 0;
}

int tool_gui_symmetry(void)
{
    int i;
    bool v;
    const char *labels_u[] = {"X", "Y", "Z"};
    const char *labels_l[] = {"x", "y", "z"};
    if (gui_section_begin("Symmetry", true)) {
        gui_group_begin("##Axis");
        gui_row_begin(3);
        for (i = 0; i < 3; i++) {
            v = (goxel.painter.symmetry >> i) & 0x1;
            if (gui_selectable(labels_u[i], &v, NULL, 0))
                set_flag(&goxel.painter.symmetry, 1 << i, v);
        }
        gui_row_end();
        gui_group_end();
        for (i = 0; i < 3; i++) {
            gui_input_float(labels_l[i], &goxel.painter.symmetry_origin[i],
                             0.5, -FLT_MAX, +FLT_MAX, "%.1f");
        }
    }
    gui_section_end();
    return 0;
}

int tool_gui_drag_mode(int *mode)
{
    int ret = 0;
    bool b;

    gui_group_begin("Drag mode");
    gui_row_begin(2);
    b = *mode == 0;
    if (gui_selectable("Move", &b, NULL, 0)) {
        *mode = 0;
        ret = 1;
    }
    b = *mode == 1;
    if (gui_selectable("Resize", &b, NULL, 0)) {
        *mode = 1;
        ret = 1;
    }
    gui_row_end();
    gui_group_end();
    return ret;
}
