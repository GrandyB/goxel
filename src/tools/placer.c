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
    float transform[4][4] = MAT4_IDENTITY;
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

    float diff[3];
    vec3_sub(curs->pos, placer->last_curs_pos, diff);

    vec3_set(transform[3], diff[0], diff[1], diff[2]);

    volume_move(placer->volume, transform);
    volume_merge(volume, placer->volume, MODE_OVER, NULL);
    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, volume);
    //volume_op(goxel.tool_volume, painter, box);

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

    curs->snap_offset = goxel.snap_offset * goxel.radius_x +
        ((painter->mode == MODE_OVER) ? 0.5 : -0.5);
    // curs->snap_offset = goxel.snap_offset * goxel.tool_radius +
    //     ((painter->mode == MODE_OVER) ? 0.5 : -0.5);

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
    }
    //volume_iterator_t iter = {0};
    //volume_set_at(placer->tool_volume, &iter, pos,
    //                  (uint8_t[]){c[0], c[1], c[2], 255});
    if (gui_button("Reset", 1, 0)) {
        volume_delete(placer->volume);
        placer->volume = volume_new();
    }

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

    tool_gui_snap();
    return 0;
}

TOOL_REGISTER(TOOL_PLACER, placer, tool_placer_t,
              .name = "Placer",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .default_shortcut = "P"
)
