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


typedef struct {
    tool_t tool;

    volume_t *volume_orig; // Original volume.
    volume_t *volume;      // Volume containing only the tool path.
    bool inherit; // Tell the painter to use the colour beneath to guide the colour

    // Gesture start and last pos (should we put it in the 3d gesture?)
    float start_pos[3];
    float last_pos[3];
    // Cache of the last operation.
    // XXX: could we remove this?
    struct     {
        float      pos[3];
        bool       pressed;
        int        mode;
        uint64_t   volume_key;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;

} tool_brush_t;

static bool check_can_skip(tool_brush_t *brush, const cursor_t *curs,
                           int mode)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (    pressed == brush->last_op.pressed &&
            mode == brush->last_op.mode &&
            brush->last_op.volume_key == volume_get_key(volume) &&
            vec3_equal(curs->pos, brush->last_op.pos)) {
        return true;
    }
    brush->last_op.pressed = pressed;
    brush->last_op.mode = mode;
    brush->last_op.volume_key = volume_get_key(volume);
    vec3_copy(curs->pos, brush->last_op.pos);
    return false;
}

// XXX: same as in brush.c.
static void get_box3(const float p0[3], const float p1[3], const float n[3],
                    float r_x, float r_y, float r_z, const float plane[4][4], float out[4][4])
{
    float rot[4][4], box[4][4];
    float v[3];

    if (p1 == NULL) {
        bbox_from_extents(box, p0, r_x, r_y, r_z);
        box_swap_axis(box, 2, 0, 1, box);
        mat4_copy(box, out);
        return;
    }
    // Used to just check radius == 0
    if (r_x == 0 || r_y == 0 || r_z == 0) {
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
    vec3_mul3(v, r_x, r_y, r_z, box[0]);
    vec3_cross(box[2], box[0], v);
    vec3_normalize(v, v);
    vec3_mul3(v, r_x, r_y, r_z, box[1]);
    mat4_copy(box, out);
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_brush_t *brush = USER_GET(user, 0);
    painter_t painter = *(painter_t*)USER_GET(user, 1);
    float box[4][4];
    cursor_t *curs = gest->cursor;
    bool shift = curs->flags & CURSOR_SHIFT;
    float r_x = goxel.radius_x;
    float r_y = goxel.radius_y;
    float r_z = goxel.radius_z;
    int nb, i;
    float pos[3];

    if (gest->state == GESTURE_BEGIN) {
        volume_set(brush->volume_orig, goxel.image->active_layer->volume);
        brush->last_op.mode = 0; // Discard last op.
        vec3_copy(curs->pos, brush->last_pos);
        image_history_push(goxel.image);
        volume_clear(brush->volume);

        if (shift) {
            painter.shape = &shape_cylinder;
            painter.mode = MODE_MAX;
            // Why was this a thing?
            //vec4_set(painter.color, 255, 255, 255, 255);
            get_box3(brush->start_pos, curs->pos, curs->normal, r_x, r_y, r_z, NULL, box);
            volume_op(brush->volume, &painter, box);
        }
    }

    painter = *(painter_t*)USER_GET(user, 1);
    if (    (gest->state == GESTURE_UPDATE) &&
            (check_can_skip(brush, curs, painter.mode))) {
        return 0;
    }

    painter.mode = MODE_MAX;
    // Why was this a thing?
    //vec4_set(painter.color, 255, 255, 255, 255);

    // Render several times if the space between the current pos
    // and the last pos is larger than the size of the tool shape.
    // Base on radius x?
    nb = ceil(vec3_dist(curs->pos, brush->last_pos) / 1); //(2 * goxel.radius_x));
    nb = max(nb, 1);
    for (i = 0; i < nb; i++) {
        vec3_mix(brush->last_pos, curs->pos, (i + 1.0) / nb, pos);
        get_box3(pos, NULL, curs->normal, r_x, r_y, r_z, NULL, box);
        volume_op(brush->volume, &painter, box);
    }

    painter = *(painter_t*)USER_GET(user, 1);
    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, brush->volume_orig);
    // layer_t *toolvollayer = layer_new("toolvolume");
    // toolvollayer->volume = volume_copy(goxel.tool_volume);
    // layer_t *brushvolume = layer_new("brushvolume");
    // brushvolume->volume = volume_copy(brush->volume);
    // DL_APPEND(goxel.image->layers, toolvollayer);
    // DL_APPEND(goxel.image->layers, brushvolume);
    volume_merge(goxel.tool_volume, brush->volume, painter.mode, NULL);
        //painter.color_blend == COLOR_INHERITED ? NULL : painter.color);
    vec3_copy(curs->pos, brush->start_pos);
    brush->last_op.volume_key = volume_get_key(goxel.tool_volume);

    if (gest->state == GESTURE_END) {
        volume_set(goxel.image->active_layer->volume, goxel.tool_volume);
        volume_set(brush->volume_orig, goxel.tool_volume);
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
    }
    vec3_copy(curs->pos, brush->last_pos);
    return 0;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    volume_t *volume = goxel.image->active_layer->volume;
    tool_brush_t *brush = USER_GET(user, 0);
    const painter_t *painter = USER_GET(user, 1);
    cursor_t *curs = gest->cursor;
    float box[4][4];
    bool shift = curs->flags & CURSOR_SHIFT;

    if (gest->state == GESTURE_END || !curs->snaped) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        return 0;
    }

    if (shift)
        render_line(&goxel.rend, brush->start_pos, curs->pos, NULL, 0);

    if (goxel.tool_volume && check_can_skip(brush, curs, painter->mode))
        return 0;

    get_box3(curs->pos, NULL, curs->normal, goxel.radius_x, goxel.radius_y, goxel.radius_z, NULL, box);

    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, volume);
    volume_op(goxel.tool_volume, painter, box);

    brush->last_op.volume_key = volume_get_key(volume);

    return 0;
}


static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    goxel_set_help_text("Click to brush - there are hotkeys for changing modes etc! TIP: Holding shift will toggle line mode.");
    tool_brush_t *brush = (tool_brush_t*)tool;
    cursor_t *curs = &goxel.cursor;
    // XXX: for the moment we force rounded positions for the brush tool
    // to make things easier.
    curs->snap_mask |= SNAP_ROUNDED;

    if (!brush->volume_orig)
        brush->volume_orig = volume_copy(goxel.image->active_layer->volume);
    if (!brush->volume)
        brush->volume = volume_new();

    if (!brush->gestures.drag.type) {
        brush->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
        brush->gestures.hover = (gesture3d_t) {
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
    }

    curs->snap_offset = goxel.snap_offset * goxel.radius_x +
        ((painter->mode == MODE_OVER) ? 0.5 : -0.5);
    // curs->snap_offset = goxel.snap_offset * goxel.tool_radius +
    //     ((painter->mode == MODE_OVER) ? 0.5 : -0.5);

    gesture3d(&brush->gestures.hover, curs, USER_PASS(brush, painter));
    gesture3d(&brush->gestures.drag, curs, USER_PASS(brush, painter));

    return tool->state;
}


static int gui(tool_t *tool)
{
    tool_gui_radius();
    tool_gui_smoothness();
    tool_gui_color();
    gui_section_end();

    tool_gui_snap();
    tool_gui_shape(NULL);
    tool_gui_symmetry();
    return 0;
}

TOOL_REGISTER(TOOL_BRUSH, brush, tool_brush_t,
              .name = "Brush",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT | TOOL_ALLOW_PICK_COLOR,
              .default_shortcut = "B"
)
