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
    volume_t *volume_orig;
    volume_t *volume;
    float  box[4][4]; // Selection bbox; fixed for the drag.
    int    snap_face;
    int    last_delta;
    bool   inherit_passed_through;
    bool   inherit_all_layers; // false (default) = confine inherit to active layer
    bool   use_color;
    float  color_threshold; // Max RGB channel delta vs cursor (when use_color)
    uint8_t start_color[4]; // Colour under cursor at gesture begin
    struct {
        gesture3d_t drag;
    } gestures;
} tool_extrude_t;

typedef struct {
    int snap_face;
    bool use_color;
    float color_threshold;
    const uint8_t *start_color;
} extrude_select_t;

static int select_cond(void *user, const volume_t *volume,
                       const int base_pos[3],
                       const int new_pos[3],
                       volume_accessor_t *volume_accessor)
{
    extrude_select_t *ctx = user;
    int snap_face = ctx->snap_face;
    int p[3], n[3];
    uint8_t c[4];
    int diff;

    // Only consider voxel in the snap plane.
    memcpy(n, FACES_NORMALS[snap_face], sizeof(n));
    p[0] = new_pos[0] - base_pos[0];
    p[1] = new_pos[1] - base_pos[1];
    p[2] = new_pos[2] - base_pos[2];
    if (p[0] * n[0] + p[1] * n[1] + p[2] * n[2]) return 0;

    // Also ignore if the face is not visible.
    p[0] = new_pos[0] + FACES_NORMALS[snap_face][0];
    p[1] = new_pos[1] + FACES_NORMALS[snap_face][1];
    p[2] = new_pos[2] + FACES_NORMALS[snap_face][2];
    if (volume_get_alpha_at(volume, volume_accessor, p)) return 0;

    // Optional colour filter vs the voxel under the cursor at gesture start.
    if (ctx->use_color) {
        volume_get_at(volume, volume_accessor, new_pos, c);
        if (!c[3] || !ctx->start_color[3]) return 0;
        diff = max3(abs(c[0] - ctx->start_color[0]),
                    abs(c[1] - ctx->start_color[1]),
                    abs(c[2] - ctx->start_color[2]));
        if (diff > ctx->color_threshold) return 0;
    }

    return 255;
}

// Get the face index from the normal.
// XXX: used in a few other places!
static int get_face(const float n[3])
{
    int f;
    const int *n2;
    for (f = 0; f < 6; f++) {
        n2 = FACES_NORMALS[f];
        if (vec3_dot(n, VEC(n2[0], n2[1], n2[2])) > 0.5)
            return f;
    }
    return -1;
}

// XXX: this code is just too ugly.  Needs a lot of cleanup.
// The problem is that we should use some generic functions to handle
// box resize, since we do it a lot, and the code is never very clear.
static int on_drag(gesture3d_t *gest, void *user)
{
    tool_extrude_t *tool = (tool_extrude_t*)user;
    volume_t *volume = goxel.image->active_layer->volume;
    volume_t *tmp_volume;
    cursor_t *curs = gest->cursor;
    float face_plane[4][4];
    float n[3], pos[3], v[3], box[4][4];
    int pi[3];
    float delta;
    extrude_select_t select_ctx;
    const volume_t *inherit_from;

    if (gest->state == GESTURE_BEGIN) {
        tool->snap_face = get_face(curs->normal);

        tmp_volume = volume_new();
        tool->volume = volume_copy(volume);
        pi[0] = floor(curs->pos[0]);
        pi[1] = floor(curs->pos[1]);
        pi[2] = floor(curs->pos[2]);
        select_ctx = (extrude_select_t) {
            .snap_face = tool->snap_face,
            .use_color = tool->use_color,
            .color_threshold = tool->color_threshold,
            .start_color = tool->start_color,
        };
        if (tool->use_color)
            volume_get_at(volume, NULL, pi, tool->start_color);
        volume_select(volume, pi, select_cond, &select_ctx, tmp_volume);
        volume_merge(tool->volume, tmp_volume, MODE_MULT_ALPHA, NULL);
        volume_delete(tmp_volume);

        volume_set(tool->volume_orig, volume);
        image_history_push(goxel.image);

        // XXX: to remove: this is duplicated from selection tool.
        volume_get_box(tool->volume, true, tool->box);
        mat4_mul(tool->box, FACES_MATS[tool->snap_face], face_plane);
        vec3_normalize(face_plane[0], v);
        plane_from_vectors(goxel.tool_plane, curs->pos, curs->normal, v);
        tool->last_delta = 0;
    }

    // Selection is fixed for the drag; reuse the cached bbox.
    mat4_copy(tool->box, box);

    // XXX: have some generic way to resize boxes, since we use it all the
    // time!
    mat4_mul(box, FACES_MATS[tool->snap_face], face_plane);
    vec3_normalize(face_plane[2], n);
    // XXX: Is there a better way to compute the delta??
    vec3_sub(curs->pos, goxel.tool_plane[3], v);
    vec3_project(v, n, v);
    delta = vec3_dot(n, v);
    // render_box(&goxel.rend, &box, NULL, EFFECT_WIREFRAME);

    // Skip if we didn't move.
    if (round(delta) == tool->last_delta) goto end;
    tool->last_delta = round(delta);

    vec3_sub(curs->pos, goxel.tool_plane[3], v);
    vec3_project(v, n, v);
    vec3_add(goxel.tool_plane[3], v, pos);
    pos[0] = round(pos[0]);
    pos[1] = round(pos[1]);
    pos[2] = round(pos[2]);

    volume_set(volume, tool->volume_orig);
    tmp_volume = volume_copy(tool->volume);

    if (delta >= 1) {
        vec3_iaddk(face_plane[3], n, -0.5);
        box_move_face(box, tool->snap_face, pos, box);
        inherit_from = NULL;
        if (tool->inherit_passed_through) {
            // Active layer is back at volume_orig.
            inherit_from = tool->inherit_all_layers
                    ? goxel_get_layers_volume(goxel.image)
                    : tool->volume_orig;
        }
        volume_extrude(tmp_volume, face_plane, box, &goxel.painter,
                       inherit_from);
        volume_merge_from(volume, tmp_volume, MODE_OVER, NULL);
    }
    if (delta < 0.5) {
        box_move_face(box, FACES_OPPOSITES[tool->snap_face], pos, box);
        vec3_imul(face_plane[2], -1.0);
        vec3_iaddk(face_plane[3], n, -0.5);
        volume_extrude(tmp_volume, face_plane, box, NULL, NULL);
        volume_merge_from(volume, tmp_volume, MODE_SUB, NULL);
    }
    volume_delete(tmp_volume);

end:
    if (gest->state == GESTURE_END) {
        volume_delete(tool->volume);
        mat4_copy(plane_null, goxel.tool_plane);
    }
    return 0;
}

static int iter(tool_t *tool_, const painter_t *painter,
                const float viewport[4])
{
    tool_extrude_t *tool = (tool_extrude_t*)tool_;
    cursor_t *curs = &goxel.cursor;
    curs->snap_offset = -0.5;
    curs->snap_mask &= ~SNAP_ROUNDED;

    if (!tool->volume_orig) tool->volume_orig = volume_new();
    if (!tool->gestures.drag.type) {
        tool->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
    }
    gesture3d(&tool->gestures.drag, curs, tool);
    return 0;
}

static int gui(tool_t *tool_)
{
    tool_extrude_t *tool = (tool_extrude_t*)tool_;
    goxel_set_help_text("Drag any surface to extrude it.");
    /* Skip the default label column so these checkboxes sit on the left. */
    gui_label_size_push(0);
    gui_checkbox("Inherit passed through colors",
                 &tool->inherit_passed_through,
                 "Extruded voxels keep their source colour until the column "
                 "meets blocks (e.g. on another layer), then use those "
                 "colours from there onwards");
    gui_label_size_pop();
    if (tool->inherit_passed_through) {
        bool confine = !tool->inherit_all_layers;
        if (gui_checkbox("Confine to layer", &confine,
                         "When enabled, only look for passed-through colours on "
                         "the current layer; when disabled, use all layers")) {
            tool->inherit_all_layers = !confine;
        }
    }
    gui_label_size_push(0);
    gui_checkbox("Use color", &tool->use_color,
                 "Only select face voxels similar to the colour under the "
                 "cursor at gesture start");
    gui_label_size_pop();
    if (tool->use_color) {
        gui_input_float("Threshold", &tool->color_threshold, 1.f, 0.f, 255.f,
                        "%.0f");
        gui_tooltip_if_hovered(
                "Max RGB channel difference vs the cursor colour "
                "(0 = exact match only)");
    }
    return 0;
}

TOOL_REGISTER(TOOL_EXTRUDE, extrude, tool_extrude_t,
              .name = "Extrude",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .default_shortcut = "F",
              .has_snap = true,
)
