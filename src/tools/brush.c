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
    volume_t *delta;       // Stamps applied this frame only (for incremental preview).
    bool inherit; // Tell the painter to use the colour beneath to guide the colour

    // Gesture start and last pos (should we put it in the 3d gesture?)
    float start_pos[3];
    float last_pos[3];
    // Cache of the last operation (hover/drag skip).
    // XXX: could we remove this?
    struct     {
        float      pos[3];
        bool       pressed;
        int        mode;
        uint64_t   volume_key;
        float      radius_x, radius_y, radius_z;
    } last_op;
    /* Active layer before this stroke; used to add map-color history on commit. */
    uint64_t   layer_key_at_stroke_start;

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
            brush->last_op.radius_x == goxel.radius_x &&
            brush->last_op.radius_y == goxel.radius_y &&
            brush->last_op.radius_z == goxel.radius_z &&
            vec3_equal(curs->pos, brush->last_op.pos)) {
        return true;
    }
    brush->last_op.pressed = pressed;
    brush->last_op.mode = mode;
    brush->last_op.volume_key = volume_get_key(volume);
    brush->last_op.radius_x = goxel.radius_x;
    brush->last_op.radius_y = goxel.radius_y;
    brush->last_op.radius_z = goxel.radius_z;
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
        if (goxel.brush_origin_at_base)
            box[3][2] += r_z;
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
    if (goxel.brush_origin_at_base)
        box[3][2] += r_z;
    mat4_copy(box, out);
}


static int on_drag(gesture3d_t *gest, void *user)
{
    tool_brush_t *brush = USER_GET(user, 0);
    painter_t painter = *(painter_t*)USER_GET(user, 1);
    const shape_t *shape = painter.shape;
    float box[4][4];
    cursor_t *curs = gest->cursor;
    bool shift = curs->flags & CURSOR_SHIFT;
    float r_x = goxel.radius_x;
    float r_y = goxel.radius_y;
    float r_z = goxel.radius_z;
    int nb, i;
    float pos[3];
    bool alt = curs->flags & CURSOR_LEFT_ALT;
    int merge_mode;

    float target[3];
    vec3_copy(curs->pos, target);
    if (alt) {
        target[2] = brush->start_pos[2];
    }

    if (gest->state == GESTURE_BEGIN) {
        brush->layer_key_at_stroke_start =
            volume_get_key(goxel.image->active_layer->volume);
        volume_set(brush->volume_orig, goxel.image->active_layer->volume);
        brush->last_op.mode = 0; // Discard last op.
        vec3_copy(target, brush->last_pos);
        image_history_push(goxel.image);
        volume_clear(brush->volume);
        if (!brush->delta) brush->delta = volume_new();
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, brush->volume_orig);
    }

    painter = *(painter_t*)USER_GET(user, 1);
    shape = painter.shape;
    if (    (gest->state == GESTURE_UPDATE) &&
            (check_can_skip(brush, curs, painter.mode))) {
        return 0;
    }

    merge_mode = painter.mode;
    // MODE_PAINT of soft/partial coverage is not idempotent: merging each
    // frame's stamps re-applies falloff onto already-painted voxels and
    // hardens AA edges vs one merge of the accumulated mask (commit path).
    bool rebuild_preview = (merge_mode == MODE_PAINT &&
                            (painter.smoothness > 0 || painter.color[3] < 255));
    painter.mode = MODE_MAX;

    if (!brush->delta) brush->delta = volume_new();
    volume_clear(brush->delta);

    // Shift+click: cylinder from previous stroke end to current target.
    if (gest->state == GESTURE_BEGIN && shift) {
        painter.shape = &shape_cylinder;
        get_box3(brush->start_pos, target, curs->normal, r_x, r_y, r_z, NULL, box);
        volume_op(brush->delta, &painter, box);
        painter.shape = shape;
    }

    // Stamp along the segment so fast motion does not leave gaps.
    // Step ~ brush radius (min axis): small brushes stay 1-voxel dense;
    // large brushes avoid a volume_op per voxel of travel.
    float spacing = max(0.7f, min3(r_x, r_y, r_z));
    nb = ceil(vec3_dist(curs->pos, brush->last_pos) / spacing);
    nb = max(nb, 1);
    if (!alt) {
        for (i = 0; i < nb; i++) {
            vec3_mix(brush->last_pos, curs->pos, (i + 1.0) / nb, pos);
            get_box3(pos, NULL, curs->normal, r_x, r_y, r_z, NULL, box);
            volume_op(brush->delta, &painter, box);
        }
    }

    // Keep full stroke mask via cheap tile merge.
    volume_merge_from(brush->volume, brush->delta, MODE_MAX, NULL);

    if (gest->state == GESTURE_END || rebuild_preview) {
        // Authoritative result: one merge of the full mask onto the layer.
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, brush->volume_orig);
        volume_merge_from(goxel.tool_volume, brush->volume, merge_mode, NULL);
    } else {
        // Incremental preview: merge only this frame's stamps onto the layer copy.
        // Re-seed if hover END (or anything else) wiped tool_volume mid-stroke.
        if (!goxel.tool_volume) {
            goxel.tool_volume = volume_new();
            volume_set(goxel.tool_volume, brush->volume_orig);
            volume_merge_from(goxel.tool_volume, brush->volume, merge_mode, NULL);
        } else {
            volume_merge_from(goxel.tool_volume, brush->delta, merge_mode, NULL);
        }
    }
    vec3_copy(target, brush->start_pos);
    brush->last_op.volume_key = volume_get_key(goxel.tool_volume);

    if (gest->state == GESTURE_END) {
        volume_set(goxel.image->active_layer->volume, goxel.tool_volume);
        if (volume_get_key(goxel.image->active_layer->volume) !=
            brush->layer_key_at_stroke_start) {
            int m = goxel.painter.mode;
            if (m == MODE_OVER || m == MODE_PAINT)
                image_recent_color_push_from_painter(goxel.image, &goxel.painter);
        }
        volume_set(brush->volume_orig, goxel.tool_volume);
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
    }
    vec3_copy(target, brush->last_pos);
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
    bool alt = curs->flags & CURSOR_LEFT_ALT;

    if (gest->state == GESTURE_END || !curs->snaped) {
        // Drag runs before hover; on press hover ENDs after drag BEGIN and
        // must not destroy the stroke preview. Drag END clears tool_volume.
        if (!(curs->flags & CURSOR_PRESSED)) {
            volume_delete(goxel.tool_volume);
            goxel.tool_volume = NULL;
        }
        return 0;
    }

    if (shift) {
        float target[3];
        vec3_copy(curs->pos, target);
        if (alt) {
            target[2] = brush->start_pos[2];
        }

        float diff[3];
        vec3_sub(brush->start_pos, target, diff);
        goxel_set_help_text("Line drawing mode - distance: [%.0f/%.0f/%.0f] (%0.1f)", diff[0], diff[1], diff[2], sqrtf(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]));
        render_line(&goxel.rend, brush->start_pos, target, NULL, 0);
    }

    if (goxel.tool_volume && check_can_skip(brush, curs, painter->mode))
        return 0;

    get_box3(curs->pos, NULL, curs->normal, goxel.radius_x, goxel.radius_y, goxel.radius_z, NULL, box);

    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, volume);
    volume_op(goxel.tool_volume, painter, box);

    brush->last_op.volume_key = volume_get_key(goxel.tool_volume);

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

    // Drag before hover so release commits the stroke before hover restarts
    // and rebuilds tool_volume for the idle preview.
    gesture3d(&brush->gestures.drag, curs, USER_PASS(brush, painter));
    gesture3d(&brush->gestures.hover, curs, USER_PASS(brush, painter));

    return tool->state;
}


static int gui(tool_t *tool)
{
    tool_gui_radius();
    gui_checkbox("Origin at base", &goxel.brush_origin_at_base,
                 "Lowest Z of the shape is at the cursor (Z-up), not the center");
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
              .default_shortcut = "B",
              .has_snap = true,
)
