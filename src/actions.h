/* Goxel 3D voxels editor
 *
 * copyright (c) 2020 Guillaume Chereau <guillaume@noctua-software.com>
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

/*
 * This file contains the list of all the actions, in the form of an
 * enum of values ACTION_<name>.
 */

#ifndef ACTIONS_H
#define ACTIONS_H

#define X(name) ACTION_##name

enum {
    ACTION_NULL = 0,

    X(layer_clear),
    X(img_new_layer),
    X(img_del_layer),
    X(img_move_layer_up),
    X(img_move_layer_down),
    X(img_duplicate_layer),
    X(img_clone_layer),
    X(img_unclone_layer),
    X(img_select_parent_layer),
    X(img_merge_layer_down),
    X(img_merge_visible_layers),
    X(img_new_camera),
    X(img_del_camera),
    X(img_move_camera_up),
    X(img_move_camera_down),
    X(toggle_first_person_camera),
    X(img_image_layer_to_volume),
    X(img_new_shape_layer),
    X(img_new_material),
    X(img_del_material),
    X(img_auto_resize),
    X(img_auto_resize_reset),

    X(cut_as_new_layer),
    X(reset_selection),
    X(fill_selection),
    X(add_selection),
    X(sub_selection),
    X(copy),
    X(past),
    X(view_left),
    X(view_right),
    X(view_top),
    X(view_toggle_ortho),
    X(view_default),
    X(view_front),
    X(quit),
    X(undo),
    X(redo),
    X(toggle_mode),
    X(set_mode_add),
    X(set_mode_sub),
    X(set_mode_paint),
    X(export_render_buf_to_photos),
    X(open),
    X(save_as),
    X(save),
    X(reset),
    X(reset_512),

    X(move_plane_up),
    X(move_plane_down),
    X(toggle_plane_visible),

    X(tool_size_increase),
    X(tool_size_decrease),

    X(tool_set_brush),
    X(tool_set_laser),
    X(tool_set_shape),
    X(tool_set_pick_color),
    X(tool_set_extrude),
    X(tool_set_plane),
    X(tool_set_selection),
    X(tool_set_fuzzy_select),
    X(tool_set_rect_select),
    X(tool_set_line),
    X(tool_set_move),
    X(tool_set_placer),

    X(export_to_photos),

    X(select_layer_under_cursor),

    ACTION_COUNT
};

#undef X

#endif // ACTIONS_H
