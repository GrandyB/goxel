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

static bool render_camera_item(void *item, int idx, bool is_current)
{
    camera_t *cam = item;
    return gui_layer_item(idx, 0, NULL, NULL, &is_current, cam->name,
                          sizeof(cam->name));
}

void gui_cameras_panel(void)
{
    camera_t *cam;
    float rot[3][3], e1[3], e2[3], eul[3], pitch, yaw, v;

    gui_list(&(gui_list_t) {
        .items = (void**)&goxel.image->cameras,
        .current = (void**)&goxel.image->active_camera,
        .render = render_camera_item,
    });

    gui_row_begin(0);
    gui_action_button(ACTION_img_new_camera, NULL, 0);
    gui_action_button(ACTION_img_del_camera, NULL, 0);
    gui_action_button(ACTION_img_move_camera_up, NULL, 0);
    gui_action_button(ACTION_img_move_camera_down, NULL, 0);
    gui_row_end();

    if (!goxel.image->cameras) image_add_camera(goxel.image, NULL);

    cam = goxel.image->active_camera;
        gui_group_begin(NULL);
    if(gui_checkbox("First Person", &cam->fpv, NULL)) {
        post_toggle_fpv(cam);
    };

    if (cam->fpv) {
        // Change camera speed
        gui_input_float("Speed", &cam->speed, 0.5, 0, 30.0, NULL);

        // Manual X/Y/Z editing
        float xyz[4][4], x, y, z;
        // camera x/y/z position is cam->mat[3][0]/[3][1]/[3][2]
        mat4_copy(cam->mat, xyz);

        x = xyz[3][0];
        if(gui_input_float("X", &x, 1, 0, 0, "%.0f")) {
            xyz[3][0] = x;
            mat4_copy(xyz, cam->mat);
        };
        y = xyz[3][1];
        if(gui_input_float("Y", &y, 1, 0, 0, "%.0f")) {
            LOG_D("Changing y: %f", y);
            xyz[3][1] = y;
            mat4_copy(xyz, cam->mat);
        };
        z = xyz[3][2];
        if(gui_input_float("Z", &z, 1, 0, 0, "%.0f")) {
            LOG_D("Changing z: %f", z);
            xyz[3][2] = z;
            mat4_copy(xyz, cam->mat);
        };
    }
    gui_group_end();
    // Change camera fov
    gui_input_float("FOV", (cam->fpv) ? &cam->fovy_fpv : &cam->fovy, 1.0, 10.0, 150.0, NULL);

    if (!cam->fpv) {
        gui_input_float("dist", &cam->dist, 10.0, 0, 0, NULL);

        /*
        gui_group_begin("Offset");
        gui_input_float("x", &cam->ofs[0], 1.0, 0, 0, NULL);
        gui_input_float("y", &cam->ofs[1], 1.0, 0, 0, NULL);
        gui_input_float("z", &cam->ofs[2], 1.0, 0, 0, NULL);
        gui_group_end();
        gui_quat("Rotation", cam->rot);
        */

        gui_checkbox("Ortho", &cam->ortho, NULL);
    }

    gui_group_begin("Set");
    gui_row_begin(2);
    gui_action_button(ACTION_view_left, "left", 1.0);
    gui_action_button(ACTION_view_right, "right", 1.0);
    gui_row_end();
    gui_row_begin(2);
    gui_action_button(ACTION_view_front, "front", 1.0);
    gui_action_button(ACTION_view_top, "top", 1.0);
    gui_row_end();
    gui_action_button(ACTION_view_default, "default", 1.0);
    gui_group_end();

    // Allow to edit euler angles (Should this be a generic widget?)
    gui_group_begin(NULL);
    mat4_to_mat3(cam->mat, rot);
    mat3_to_eul2(rot, EULER_ORDER_XYZ, e1, e2);
    if (fabs(e1[1]) < fabs(e2[1]))
        vec3_copy(e1, eul);
    else
        vec3_copy(e2, eul);

    pitch = round(eul[0] * DR2D);
    if (pitch < 0) pitch += 360;
    v = pitch;
    if (gui_input_float("Pitch", &v, 1, -90, 90, "%.0f")) {
        v = (v - pitch) * DD2R;
        camera_turntable(cam, 0, v);
    }

    yaw = round(eul[2] * DR2D);
    v = yaw;
    if (gui_input_float("Yaw", &v, 1, -180, 180, "%.0f")) {
        v = (v - yaw) * DD2R;
        camera_turntable(cam, v, 0);
    }
    gui_group_end();
}

