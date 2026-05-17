/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
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

enum {
    DRAG_RESIZE,
    DRAG_MOVE,
};

static int g_drag_mode = 0;
static int original_drag_mode = 0;

typedef struct {
    bool    active;
    int     start_aabb[2][3];
    int     last_delta[3];
    float   start_center[3];
    int     size[3];
    uint8_t *buffer;
} alt_drag_t;

static alt_drag_t g_alt_drag = {};
static bool g_was_box_editing = false;

typedef struct {
    tool_t  tool;

    int     snap_face;
    float   start_pos[3];
    int     move_distance;
    bool    across_layers;

    struct {
        gesture3d_t hover;
        gesture3d_t drag;
    } gestures;

} tool_selection_t;

static void volume_translate_in_aabb(volume_t *volume, int axis, int sign,
                                   const int aabb[2][3], int distance)
{
    int pos[3];
    int dst_pos[3];
    int volume_pos[3];
    int size[3];
    uint8_t *buffer;
    size_t buffer_offset;
    int i;
    int delta;

    if (distance == 0)
        return;

    size[0] = aabb[1][0] - aabb[0][0];
    size[1] = aabb[1][1] - aabb[0][1];
    size[2] = aabb[1][2] - aabb[0][2];

    if (size[0] <= 0 || size[1] <= 0 || size[2] <= 0)
        return;

    buffer = calloc(4 * (size_t)size[0] * size[1] * size[2], 1);
    if (!buffer)
        return;

    delta = distance * sign;

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
                memcpy(dst_pos, pos, sizeof(pos));
                dst_pos[axis] += delta;
                if (dst_pos[axis] < 0 || dst_pos[axis] >= size[axis])
                    continue;

                memcpy(volume_pos, pos, sizeof(pos));
                for (i = 0; i < 3; i++)
                    volume_pos[i] += aabb[0][i];

                buffer_offset = 4 * ((size_t)dst_pos[2] * size[0] * size[1] +
                                     dst_pos[1] * size[0] + dst_pos[0]);

                volume_get_at(volume, NULL, volume_pos, &buffer[buffer_offset]);
            }
        }
    }

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
                memcpy(volume_pos, pos, sizeof(pos));
                for (i = 0; i < 3; i++)
                    volume_pos[i] += aabb[0][i];

                buffer_offset = 4 * ((size_t)pos[2] * size[0] * size[1] +
                                     pos[1] * size[0] + pos[0]);

                volume_set_at(volume, NULL, volume_pos, &buffer[buffer_offset]);
            }
        }
    }

    free(buffer);
    volume_remove_empty_tiles(volume, false);
}

static void volume_clear_aabb(volume_t *volume, const int aabb[2][3])
{
    int pos[3];
    uint8_t empty[4] = {0, 0, 0, 0};

    for (pos[0] = aabb[0][0]; pos[0] < aabb[1][0]; pos[0]++)
        for (pos[1] = aabb[0][1]; pos[1] < aabb[1][1]; pos[1]++)
            for (pos[2] = aabb[0][2]; pos[2] < aabb[1][2]; pos[2]++)
                volume_set_at(volume, NULL, pos, empty);
}

static void aabb_shift(const int aabb[2][3], const int delta[3], int out[2][3])
{
    int i;

    for (i = 0; i < 3; i++) {
        out[0][i] = aabb[0][i] + delta[i];
        out[1][i] = aabb[1][i] + delta[i];
    }
}

static void alt_drag_begin(const int aabb[2][3], volume_t *volume)
{
    int pos[3];
    int volume_pos[3];
    int i;
    size_t buffer_offset;

    g_alt_drag.size[0] = aabb[1][0] - aabb[0][0];
    g_alt_drag.size[1] = aabb[1][1] - aabb[0][1];
    g_alt_drag.size[2] = aabb[1][2] - aabb[0][2];
    if (g_alt_drag.size[0] <= 0 || g_alt_drag.size[1] <= 0 || g_alt_drag.size[2] <= 0)
        return;

    free(g_alt_drag.buffer);
    g_alt_drag.buffer = calloc(
            4 * (size_t)g_alt_drag.size[0] * g_alt_drag.size[1] * g_alt_drag.size[2],
            1);
    if (!g_alt_drag.buffer)
        return;

    memcpy(g_alt_drag.start_aabb, aabb, sizeof(g_alt_drag.start_aabb));
    vec3_copy(goxel.selection[3], g_alt_drag.start_center);
    memset(g_alt_drag.last_delta, 0, sizeof(g_alt_drag.last_delta));
    g_alt_drag.active = true;

    for (pos[0] = 0; pos[0] < g_alt_drag.size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < g_alt_drag.size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < g_alt_drag.size[2]; pos[2]++) {
                memcpy(volume_pos, pos, sizeof(pos));
                for (i = 0; i < 3; i++)
                    volume_pos[i] += aabb[0][i];

                buffer_offset = 4 * ((size_t)pos[2] * g_alt_drag.size[0] *
                                             g_alt_drag.size[1] +
                                     pos[1] * g_alt_drag.size[0] + pos[0]);

                volume_get_at(volume, NULL, volume_pos,
                              &g_alt_drag.buffer[buffer_offset]);
            }
        }
    }
}

static void alt_drag_apply(volume_t *volume, const float box[4][4])
{
    int pos[3];
    int volume_pos[3];
    int delta[3];
    int shifted_aabb[2][3];
    int i;
    size_t buffer_offset;

    if (!g_alt_drag.active || !g_alt_drag.buffer)
        return;

    for (i = 0; i < 3; i++)
        delta[i] = (int)round(box[3][i] - g_alt_drag.start_center[i]);

    if (    delta[0] == g_alt_drag.last_delta[0] &&
            delta[1] == g_alt_drag.last_delta[1] &&
            delta[2] == g_alt_drag.last_delta[2])
        return;

    volume_clear_aabb(volume, g_alt_drag.start_aabb);
    if (    g_alt_drag.last_delta[0] || g_alt_drag.last_delta[1] ||
            g_alt_drag.last_delta[2]) {
        aabb_shift(g_alt_drag.start_aabb, g_alt_drag.last_delta, shifted_aabb);
        volume_clear_aabb(volume, shifted_aabb);
    }

    for (pos[0] = 0; pos[0] < g_alt_drag.size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < g_alt_drag.size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < g_alt_drag.size[2]; pos[2]++) {
                memcpy(volume_pos, pos, sizeof(pos));
                for (i = 0; i < 3; i++)
                    volume_pos[i] += g_alt_drag.start_aabb[0][i] + delta[i];

                buffer_offset = 4 * ((size_t)pos[2] * g_alt_drag.size[0] *
                                             g_alt_drag.size[1] +
                                     pos[1] * g_alt_drag.size[0] + pos[0]);

                volume_set_at(volume, NULL, volume_pos,
                              &g_alt_drag.buffer[buffer_offset]);
            }
        }
    }

    memcpy(g_alt_drag.last_delta, delta, sizeof(delta));
    volume_remove_empty_tiles(volume, false);
}

static void alt_drag_end(void)
{
    if (!g_alt_drag.active)
        return;

    if (    g_alt_drag.last_delta[0] || g_alt_drag.last_delta[1] ||
            g_alt_drag.last_delta[2])
        image_history_push(goxel.image);

    free(g_alt_drag.buffer);
    memset(&g_alt_drag, 0, sizeof(g_alt_drag));
}

static bool move_axis_buttons(int *out_axis, int *sign)
{
    char buf[8];
    bool ret = false;
    int axis;
    const char *AXIS_NAMES[] = {"X", "Y", "Z"};

    *out_axis = 0;
    *sign = 1;

    for (axis = 0; axis < 3; axis++) {
        gui_row_begin(2);

        snprintf(buf, sizeof(buf), "-%s", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0)) {
            *out_axis = axis;
            *sign = -1;
            ret = true;
        }

        snprintf(buf, sizeof(buf), "+%s", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0)) {
            *out_axis = axis;
            *sign = 1;
            ret = true;
        }

        gui_row_end();
    }

    return ret;
}

static void get_box(const float p0[3], const float p1[3], const float n[3],
                     float r, const float plane[4][4], float out[4][4])
{
    float rot[4][4], box[4][4];
    float v[3];
    if (p1 == NULL) {
        bbox_from_extents(box, p0, r, r, r);
        box_swap_axis(box, 2, 0, 1, box);
        mat4_copy(box, out);
        return;
    }
    if (r == 0) {
        bbox_from_points(box, p0, p1);
        bbox_grow(box, 0.5, 0.5, 0.5, box);
        // Apply the plane rotation.
        mat4_copy(plane, rot);
        vec4_set(rot[3], 0, 0, 0, 1);
        mat4_imul(box, rot);
        mat4_copy(box, out);
        return;
    }

    // Create a box for a line:
    int i;
    const float AXES[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    mat4_set_identity(box);
    vec3_mix(p0, p1, 0.5, box[3]);
    vec3_sub(p1, box[3], box[2]);
    for (i = 0; i < 3; i++) {
        vec3_cross(box[2], AXES[i], box[0]);
        if (vec3_norm2(box[0]) > 0) break;
    }
    if (i == 3) {
        mat4_copy(box, out);
        return;
    }
    vec3_normalize(box[0], v);
    vec3_mul(v, r, box[0]);
    vec3_cross(box[2], box[0], v);
    vec3_normalize(v, v);
    vec3_mul(v, r, box[1]);
    mat4_copy(box, out);
}

static int on_hover(gesture3d_t *gest, void *user)
{
    float box[4][4];
    cursor_t *curs = gest->cursor;
    uint8_t box_color[4] = {255, 255, 0, 255};

    goxel_set_help_text(
            "Click and drag to set selection; hold Shift to move the box; "
            "hold Alt to move voxels inside the box with it");
    get_box(curs->pos, curs->pos, curs->normal, 0, goxel.plane, box);
    render_box(&goxel.rend, box, box_color, EFFECT_WIREFRAME);
    return 0;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_selection_t *tool = user;
    cursor_t *curs = gest->cursor;

    if (gest->state == GESTURE_BEGIN)
        vec3_copy(curs->pos, tool->start_pos);
    curs->snap_mask &= ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT);
    goxel_set_help_text("Drag.");
    get_box(tool->start_pos, curs->pos, curs->normal,
            0, goxel.plane, goxel.selection);
    return 0;
}

// XXX: this is very close to tool_shape_iter.
static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    float transf[4][4];
    bool first = false;
    int box_active;
    bool alt_move_contents;
    layer_t *layer = goxel.image->active_layer;
    volume_t *volume = layer ? layer->volume : NULL;
    int aabb[2][3];

    tool_selection_t *selection = (tool_selection_t*)tool;
    cursor_t *curs = &goxel.cursor;
    curs->snap_mask |= SNAP_ROUNDED;
    curs->snap_mask &= ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT);
    curs->snap_offset = 0.5;
    curs->snap_mask |= SNAP_SELECTION_OUT;

    if (curs->flags & (CURSOR_SHIFT | CURSOR_LEFT_ALT))
        g_drag_mode = DRAG_MOVE;
    else
        g_drag_mode = original_drag_mode;

    alt_move_contents = (curs->flags & CURSOR_LEFT_ALT) && !box_is_null(goxel.selection);

    if (g_alt_drag.active && !alt_move_contents)
        alt_drag_end();

    if (!selection->gestures.drag.type) {
        selection->gestures.hover = (gesture3d_t) {
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
        selection->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
    }

    box_active = box_edit(SNAP_SELECTION_OUT, g_drag_mode == DRAG_RESIZE ? 1 : 0,
                          transf, &first);

    if (g_was_box_editing && !box_active)
        alt_drag_end();

    if (box_active) {
        if (alt_move_contents && g_drag_mode == DRAG_MOVE && volume) {
            if (first) {
                bbox_to_aabb(goxel.selection, aabb);
                alt_drag_begin(aabb, volume);
            }
            mat4_mul(transf, goxel.selection, goxel.selection);
            alt_drag_apply(volume, goxel.selection);
        } else {
            mat4_mul(transf, goxel.selection, goxel.selection);
        }
        g_was_box_editing = true;
        return 0;
    }

    g_was_box_editing = false;

    if (gesture3d(&selection->gestures.drag, curs, selection)) goto end;
    if (gesture3d(&selection->gestures.hover, curs, selection)) goto end;

end:
    return tool->state;
}

static float get_magnitude(float box[4][4], int axis_index)
{
    return box[0][axis_index] + box[1][axis_index] + box[2][axis_index];
}

static int gui(tool_t *tool)
{
    tool_selection_t *selection = (tool_selection_t *)tool;
    float x_mag, y_mag, z_mag;
    int x, y, z, w, h, d;
    float (*box)[4][4] = &goxel.selection;

    if(gui_button("Select entire layer", -1, 0)) {
        float box[4][4];
        volume_get_box(goxel.image->active_layer->volume, true, box);
        mat4_copy(box, goxel.selection);
    }

    if (box_is_null(*box)) return 0;

    gui_text("Drag mode");
    if(gui_combo("##drag_mode", &g_drag_mode,
              (const char*[]) {"Resize", "Move"}, 2)) {
        original_drag_mode = g_drag_mode;
    };

    gui_group_begin(NULL);
    if (gui_action_button(ACTION_reset_selection, "Reset", 1.0)) {
        gui_group_end();
        return 0;
    }
    gui_action_button(ACTION_fill_selection_box, "Fill", 1.0);
    gui_action_button(ACTION_layer_clear, "Clear", 1.0);
    gui_row_begin(2);
    gui_action_button(ACTION_add_selection, "Add", 0.5);
    gui_action_button(ACTION_sub_selection, "Sub", 1.0);
    gui_row_end();
    gui_action_button(ACTION_cut_as_new_layer, "Cut as new layer", 1.0);
    gui_group_end();

    // XXX: why not using gui_bbox here?
    x_mag = fabs(get_magnitude(*box, 0));
    y_mag = fabs(get_magnitude(*box, 1));
    z_mag = fabs(get_magnitude(*box, 2));
    w = round(x_mag * 2);
    h = round(y_mag * 2);
    d = round(z_mag * 2);
    x = round((*box)[3][0] - x_mag);
    y = round((*box)[3][1] - y_mag);
    z = round((*box)[3][2] - z_mag);

    gui_group_begin("Origin");
    gui_input_int("x", &x, 0, 0);
    gui_input_int("y", &y, 0, 0);
    gui_input_int("z", &z, 0, 0);
    gui_group_end();

    gui_group_begin("Size");
    gui_input_int("w", &w, 0, 0);
    gui_input_int("h", &h, 0, 0);
    gui_input_int("d", &d, 0, 0);
    w = max(1, w);
    h = max(1, h);
    d = max(1, d);
    gui_group_end();

    bbox_from_extents(*box,
            VEC(x + w / 2., y + h / 2., z + d / 2.),
            w / 2., h / 2., d / 2.);

    if (gui_section_begin("Move selection", GUI_SECTION_COLLAPSABLE)) {
        int axis, sign;
        int aabb[2][3];
        bool should_move;
        layer_t *layer;
        static bool move_defaults_applied;

        if (!move_defaults_applied) {
            selection->move_distance = 1;
            move_defaults_applied = true;
        }

        gui_input_int("Distance", &selection->move_distance, 0, 9999);

        gui_checkbox(
            "Across layers",
            &selection->across_layers,
            "If checked, voxels on all layers are moved.\n"
            "If unchecked, only voxels on the current layer are moved.");

        gui_group_begin(NULL);
        should_move = move_axis_buttons(&axis, &sign);
        gui_group_end();

        if (should_move) {
            int delta = selection->move_distance * sign;

            bbox_to_aabb(*box, aabb);
            DL_FOREACH(goxel.image->layers, layer) {
                if (!selection->across_layers &&
                    layer != goxel.image->active_layer)
                    continue;
                volume_translate_in_aabb(
                        layer->volume, axis, sign, aabb,
                        selection->move_distance);
            }
            (*box)[3][axis] += (float)delta;
            image_history_push(goxel.image);
        }
    }
    gui_section_end();

    if(gui_section_begin("Placer", true)) {
        if(gui_button("Copy to placer", -1, 0)) {
            // Copy to placer and switch to placer
            action_exec(action_get(ACTION_tool_set_placer, true));
            action_exec(action_get(ACTION_placer_acquire_selection, true));
        }
        if(gui_button("Move via placer", -1, 0)) {
            // Copy to placer, switch to placer and wipe the selection
            action_exec(action_get(ACTION_tool_set_placer, true));
            action_exec(action_get(ACTION_placer_acquire_selection, true));
            action_exec(action_get(ACTION_tool_set_selection, true));
            action_exec(action_get(ACTION_layer_clear, true));
            action_exec(action_get(ACTION_tool_set_placer, true));
            mat4_copy(mat4_zero, *box);
        }
    }
    gui_section_end();
    return 0;
}

TOOL_REGISTER(TOOL_SELECTION, selection, tool_selection_t,
              .name = "Selection",
              .iter_fn = iter,
              .gui_fn = gui,
              .default_shortcut = "R",
              .flags = TOOL_SHOW_MASK,
              .has_snap = true,
)
