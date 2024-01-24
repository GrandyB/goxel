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
#include "file_format.h"

static file_format_t *g_current = NULL;

typedef struct {
    tool_t tool;

    volume_t *volume_orig; // Original volume.
    volume_t *volume;      // Volume containing only the tool path.

    float mat[4][4]; // centre pos of the doodad
    float box[4][4]; // the bounding box of the doodad
    float offset[4][4]; // relative offset of position
    float origin[3]; // relative offset of origin
    float last_curs_pos[3];

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

} tool_placer_t;

static bool check_can_skip(tool_placer_t *placer, const cursor_t *curs)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (    pressed == placer->last_op.pressed &&
            placer->last_op.volume_key == volume_get_key(volume) &&
            vec3_equal(curs->pos, placer->last_op.pos)) {
        return true;
    }
    placer->last_op.pressed = pressed;
    placer->last_op.volume_key = volume_get_key(volume);
    vec3_copy(curs->pos, placer->last_op.pos);
    return false;
}

static void center_origin(tool_placer_t *placer)
{
    int bbox[2][3];
    float pos[3];
    float translation[4][4] = MAT4_IDENTITY;

    volume_get_bbox(placer->volume, bbox, true);
    pos[0] = round((bbox[0][0] + bbox[1][0] - 1) / 2.0);
    pos[1] = round((bbox[0][1] + bbox[1][1] - 1) / 2.0);
    pos[2] = 0; //round((bbox[0][2] + bbox[1][2] - 1) / 2.0);

    vec3_copy(pos, placer->origin);
    vec3_sub(pos, placer->mat[3], translation[3]);
    //vec3_add(translation[3], placer->offset[3], translation[3]);
    do_move(placer->volume, placer->box, placer->mat, translation, NULL, true, true);
}

static int on_drag(gesture3d_t *gest, void *user)
{
    if (gest->state == GESTURE_END) {
        image_history_push(goxel.image);
        if (goxel.image->active_layer->volume && goxel.tool_volume) {
            volume_set(goxel.image->active_layer->volume, goxel.tool_volume);
        }
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
    }
    return 0;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    volume_t *volume = volume_copy(goxel.image->active_layer->volume);
    tool_placer_t *placer = USER_GET(user, 0);
    //const painter_t *painter = USER_GET(user, 1);
    cursor_t *curs = gest->cursor;
    //float box[4][4] = MAT4_IDENTITY;
    //float transform[4][4] = MAT4_IDENTITY;
    //bool shift = curs->flags & CURSOR_SHIFT;

    if (gest->state == GESTURE_END || !curs->snaped) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        return 0;
    }

    if (goxel.tool_volume && check_can_skip(placer, curs))
        return 0;

    //if (shift)
    //    render_line(&goxel.rend, placer->start_pos, curs->pos, NULL, 0);

    //if (goxel.tool_volume && check_can_skip(placer, curs, painter->mode))
    //    return 0;

    //get_box3(curs->pos, NULL, curs->normal, goxel.radius_x, goxel.radius_y, goxel.radius_z, NULL, box);
    //mat4_mul()
    //mat4_itranslate(placer->volume)
    //volume_move(placer->volume, curs->pos);
    //mat4_translate(*painter->box, curs->pos[0], curs->pos[1], curs->pos[2], *painter->box);
    //mat4_mul_vec3(*painter->box, curs->pos, painter->box);
    // debug_log_44_matrix(transform);
    // transform[3][0] = curs->pos[0];
    // transform[3][1] = curs->pos[1];
    // transform[3][2] = curs->pos[2];
    
    if (placer->volume) {
        float prev_transform[4][4] = MAT4_IDENTITY;
        mat4_copy(placer->mat, prev_transform);

        // Each frame, we find the difference in cursor positions
        float diff[3];
        vec3_sub(curs->pos, placer->mat[3], diff);
        //vec3_set(placer->mat[3], diff[0], diff[1], diff[2]);
        //vec3_set(placer->mat[3], curs->pos[0], curs->pos[1], curs->pos[2]);
        //vec3_sub(placer->mat[3], placer->origin, placer->mat[3]);

        // This difference is then loaded into a transform
        float transform[4][4] = MAT4_IDENTITY;
        //vec3_sub(placer->mat[3], placer->last_curs_pos, diff);
        vec3_set(transform[3], diff[0], diff[1], diff[2]);
        vec3_add(transform[3], placer->offset[3], transform[3]);

        // LOG_D("transform:");
        // debug_log_44_matrix(transform);
        // LOG_D("---");
        
        // Placer's position is set to be the cursor's position
        //vec3_set(placer->mat[3], placer->last_curs_pos[0], placer->last_curs_pos[1], placer->last_curs_pos[2]);

        // ...and that transform applied to the doodad in the placer->volume (mat/box gets updated too)
        do_move(placer->volume, placer->box, placer->mat, transform, placer->offset[3], true, false);

        // LOG_D("placer->mat:");
        // debug_log_44_matrix(placer->mat);
        // LOG_D("---");

        //volume_move(placer->volume, transform);
        volume_merge(volume, placer->volume, MODE_OVER, NULL);
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, volume);
        //volume_op(goxel.tool_volume, painter, box);
    }

    placer->last_op.volume_key = volume_get_key(volume);
    vec3_copy(curs->pos, placer->last_curs_pos);
    return 0;
}


static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    goxel_set_help_text("Click to brush - there are hotkeys for changing modes etc! TIP: Holding shift will toggle line mode.");
    tool_placer_t *placer = (tool_placer_t*)tool;
    cursor_t *curs = &goxel.cursor;
    // XXX: for the moment we force rounded positions for the placer tool
    // to make things easier.
    curs->snap_mask |= SNAP_ROUNDED;

    if (!placer->volume_orig)
        placer->volume_orig = volume_copy(goxel.image->active_layer->volume);
    if (!placer->volume)
        placer->volume = volume_new();

    if (!placer->gestures.drag.type) {
        placer->gestures.hover = (gesture3d_t) {
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
        placer->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
    }

    if (curs->snaped && placer->volume) {
        // render the bounding box
        render_box(&goxel.rend, placer->box, NULL, EFFECT_STRIP | EFFECT_WIREFRAME);

        // render the origin dot
        float origin_box[4][4] = MAT4_IDENTITY;
        uint8_t color[4] = {255, 0, 0, 255};
        vec3_copy(placer->mat[3], origin_box[3]);
        mat4_iscale(origin_box, 0.1, 0.1, 0.1);
        render_box(&goxel.rend, origin_box, color,
                EFFECT_NO_DEPTH_TEST | EFFECT_NO_SHADING);
    }

    curs->snap_offset = 0;

    gesture3d(&placer->gestures.hover, curs, USER_PASS(placer, painter));
    gesture3d(&placer->gestures.drag, curs, USER_PASS(placer, painter));

    return tool->state;
}

static const char *make_label(const file_format_t *f, char *buf, int len)
{
    const char *ext = f->exts[0] + 1;
    snprintf(buf, len, "%s (%s)", f->name, ext);
    return buf;
}

static void on_format(void *user, file_format_t *f)
{
    char label[128];
    make_label(f, label, sizeof(label));
    if (gui_combo_item(label, f == g_current)) {
        g_current = f;
    }
}

static int gui(tool_t *tool)
{
    tool_placer_t *placer = (tool_placer_t*)tool;
    float mat[4][4] = MAT4_IDENTITY;
    bool only_origin = false;
    
    int origin_x, origin_y, origin_z, offset_x, offset_y, offset_z;

    // Browse files
    
    char label[128];
    gui_text("Import as");
    if (!g_current) g_current = file_formats; // First one.

    make_label(g_current, label, sizeof(label));
    if (gui_combo_begin("Import as", label)) {
        file_format_iter("v", NULL, on_format);
        gui_combo_end();
    }

    if (g_current->import_gui)
        g_current->import_gui(g_current);
    if (gui_button("Import", 1, 0)) {
        if (!placer->volume)
            placer->volume = volume_new();
        goxel_import_file_to_volume(NULL, g_current->name, placer->volume);
        int bbox[2][3];
        volume_get_bbox(placer->volume, bbox, true);
        bbox_from_aabb(placer->box, bbox);
        mat4_copy(mat, placer->mat);
        center_origin(placer);
        
        // float origin[3];
        // center_origin_translation(placer->volume, origin);
        // LOG_D("Origin translation: %f / %f / %f", origin[0], origin[1], origin[2]);
        // //vec3_sub(pos, layer->mat[3], translation[3]);
        // //mat4_translate(mat, -origin[0], -origin[1], -origin[2], mat);
        // //volume_move(placer->volume, ot);

        // float m[4][4] = MAT4_IDENTITY;
        // vec3_add(m[3], origin, m[3]);
        // mat4_itranslate(m, +origin[0], +origin[1], +origin[2]);
        // mat4_imul(m, mat);
        // mat4_itranslate(m, -origin[0], -origin[1], -origin[2]);
        // mat4_copy(m, mat);

    }
    //volume_iterator_t iter = {0};
    //volume_set_at(placer->tool_volume, &iter, pos,
    //                  (uint8_t[]){c[0], c[1], c[2], 255});
    if (gui_button("Reset", 1, 0)) {
        volume_delete(placer->volume);
        placer->volume = volume_new();
    }

    tool_gui_snap();
    
    if (placer->volume) {
        origin_x = (int)round(placer->mat[3][0]);
        origin_y = (int)round(placer->mat[3][1]);
        origin_z = (int)round(placer->mat[3][2]);

        offset_x = (int)round(placer->offset[3][0]);
        offset_y = (int)round(placer->offset[3][1]);
        offset_z = (int)round(placer->offset[3][2]);

        gui_group_begin("Offset");
        if (gui_input_int("X", &offset_x, 0, 0)) {
            placer->offset[3][0] = offset_x;
        }
        if (gui_input_int("Y", &offset_y, 0, 0)) {
            placer->offset[3][1] = offset_y;
        }
        if (gui_input_int("Z", &offset_z, 0, 0)) {
            placer->offset[3][2] = offset_z;
        }
        if (gui_button("Reset", -1, 0)) {
            placer->offset[3][0] = 0;
            placer->offset[3][1] = 0;
            placer->offset[3][2] = 0;
        }
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

        gui_group_begin("Origin");
        if (gui_input_int("X", &origin_x, 0, 0)) {
            mat[3][0] = origin_x - (int)round(placer->mat[3][0]);
            only_origin = true;
        }
        if (gui_input_int("Y", &origin_y, 0, 0)) {
            mat[3][1] = origin_y - (int)round(placer->mat[3][1]);
            only_origin = true;
        }
        if (gui_input_int("Z", &origin_z, 0, 0)) {
            mat[3][2] = origin_z - (int)round(placer->mat[3][2]);
            only_origin = true;
        }
        if (gui_button("Center", -1, 0)) {
            center_origin(placer);
        }
        gui_group_end();
    }

    if (placer->volume) {
        //float transform[4][4] = MAT4_IDENTITY;
        do_move(placer->volume, placer->box, placer->mat, mat,
                    placer->origin, true, only_origin);
    }
    return 0;
}

TOOL_REGISTER(TOOL_PLACER, placer, tool_placer_t,
              .name = "Placer",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .default_shortcut = "P"
)
