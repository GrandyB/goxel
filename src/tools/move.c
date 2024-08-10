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
    DRAG_MOVE = 0,
    DRAG_RESIZE,
};

typedef struct {
    tool_t tool;
} tool_move_t;

static void update_view(void)
{
    float transf[4][4];
    float origin_box[4][4] = MAT4_IDENTITY;
    bool first;
    uint8_t color[4] = {255, 0, 0, 255};


    layer_t *layer = goxel.image->active_layer;

    if (layer_is_volume(layer)) {
        goxel.tool_drag_mode = DRAG_MOVE;
    }
    if (box_edit(SNAP_LAYER_OUT, goxel.tool_drag_mode, transf, &first)) {
        if (first) image_history_push(goxel.image);
        do_move_layer(layer, transf, VEC(0, 0, 0), false);
    }

    if (layer_is_volume(layer)) {
        vec3_copy(layer->mat[3], origin_box[3]);
        mat4_iscale(origin_box, 0.1, 0.1, 0.1);
        render_box(&goxel.rend, origin_box, color,
                EFFECT_NO_DEPTH_TEST | EFFECT_NO_SHADING);
    }
}

static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    update_view();
    return 0;
}

static void center_origin(layer_t *layer)
{
    int bbox[2][3];
    float pos[3];
    float translation[4][4] = MAT4_IDENTITY;

    volume_get_bbox(layer->volume, bbox, true);
    pos[0] = round((bbox[0][0] + bbox[1][0] - 1) / 2.0);
    pos[1] = round((bbox[0][1] + bbox[1][1] - 1) / 2.0);
    pos[2] = round((bbox[0][2] + bbox[1][2] - 1) / 2.0);

    vec3_sub(pos, layer->mat[3], translation[3]);
    do_move_layer(layer, translation, NULL, true);
}

int degX = 0, degY = 0, degZ = 0;
static int gui(tool_t *tool)
{
    layer_t *layer;
    float mat[4][4] = MAT4_IDENTITY, v;
    int i;
    int x, y, z;
    bool only_origin = false;

    update_view();

    layer = goxel.image->active_layer;
    if (layer->shape) {
        tool_gui_drag_mode(&goxel.tool_drag_mode);
    } else {
        goxel.tool_drag_mode = DRAG_MOVE;
    }

    x = (int)round(layer->mat[3][0]);
    y = (int)round(layer->mat[3][1]);
    z = (int)round(layer->mat[3][2]);

    gui_group_begin("Position");
    if (gui_input_int("X", &x, 0, 0))
        mat[3][0] = x - (int)round(layer->mat[3][0]);
    if (gui_input_int("Y", &y, 0, 0))
        mat[3][1] = y - (int)round(layer->mat[3][1]);
    if (gui_input_int("Z", &z, 0, 0))
        mat[3][2] = z - (int)round(layer->mat[3][2]);
    gui_group_end();

    gui_group_begin("Rotation");

    gui_row_begin(2);
    if (gui_button("-X", 0, 0))
        mat4_irotate(mat, -M_PI / 2, 1, 0, 0);
    if (gui_button("+X", 0, 0))
        mat4_irotate(mat, +M_PI / 2, 1, 0, 0);
    gui_row_end();

    gui_row_begin(2);
    if (gui_button("-Y", 0, 0))
        mat4_irotate(mat, -M_PI / 2, 0, 1, 0);
    if (gui_button("+Y", 0, 0))
        mat4_irotate(mat, +M_PI / 2, 0, 1, 0);
    gui_row_end();

    gui_row_begin(2);
    if (gui_button("-Z", 0, 0))
        mat4_irotate(mat, -M_PI / 2, 0, 0, 1);
    if (gui_button("+Z", 0, 0))
        mat4_irotate(mat, +M_PI / 2, 0, 0, 1);
    gui_row_end();

    gui_group_end();

    if (layer->image && gui_input_int("Scale", &i, 0, 0)) {
        v = pow(2, i);
        mat4_iscale(mat, v, v, v);
    }

    gui_group_begin("Flip");
    gui_row_begin(3);
    if (gui_button("X", -1, 0)) mat4_iscale(mat, -1,  1,  1);
    if (gui_button("Y", -1, 0)) mat4_iscale(mat,  1, -1,  1);
    if (gui_button("Z", -1, 0)) mat4_iscale(mat,  1,  1, -1);
    gui_row_end();
    gui_group_end();

    gui_group_begin("Scale");
    gui_row_begin(3);
    if (gui_button("x2", -1, 0)) mat4_iscale(mat, 2, 2, 2);
    if (gui_button("x0.5", -1, 0)) mat4_iscale(mat, 0.5, 0.5, 0.5);
    gui_row_end();
    gui_group_end();

    if (layer_is_volume(layer)) {
        if (gui_section_begin("Origin", GUI_SECTION_COLLAPSABLE_CLOSED)) {
            if (gui_input_int("X", &x, 0, 0)) {
                mat[3][0] = x - (int)round(layer->mat[3][0]);
                only_origin = true;
            }
            if (gui_input_int("Y", &y, 0, 0)) {
                mat[3][1] = y - (int)round(layer->mat[3][1]);
                only_origin = true;
            }
            if (gui_input_int("Z", &z, 0, 0)) {
                mat[3][2] = z - (int)round(layer->mat[3][2]);
                only_origin = true;
            }
            if (gui_button("Center", -1, 0)) {
                center_origin(layer);
            }
        } gui_section_end();
    }

    

    if (gui_section_begin("Rotation (Destructive)", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        gui_input_int("Degrees X", &degX, -360, 360);
        gui_input_int("Degrees Y", &degY, -360, 360);
        gui_input_int("Degrees Z", &degZ, -360, 360);
        
        if (gui_button("Apply", -1, 0)) {
            if (degX)
                mat4_irotate(mat, degX * DD2R, 1, 0, 0);
            if (degY)
                mat4_irotate(mat, degY * DD2R, 0, 1, 0);
            if (degZ)
                mat4_irotate(mat, degZ * DD2R, 0, 0, 1);
        }
    } gui_section_end();

    if (memcmp(&mat, &mat4_identity, sizeof(mat))) {
        image_history_push(goxel.image);
        do_move_layer(layer, mat, NULL, only_origin);
    }

    return 0;
}

TOOL_REGISTER(TOOL_MOVE, move, tool_move_t,
              .name = "Move",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_MOVE,
              .default_shortcut = "M",
)
