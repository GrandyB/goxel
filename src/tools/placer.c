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
#include "utlist.h"

static file_format_t *g_current = NULL;

typedef struct {
    tool_t tool;

    volume_t *volume_orig; // Original layer volume.
    volume_t *imported_volume; // Volume containing only the imported volume (current/may have been rotated)
    volume_t *imported_volume_orig; // Volume containing the volume as it was when first imported

    float mat[4][4]; // position of the doodad; after everything else is applied
    float box[4][4]; // the bounding box of the doodad
    float offset[3]; // relative offset of position
    float center[3]; // bottom centre of the model, calculated once on import
    float origin[3]; // relative offset of origin from the center
    float last_curs_pos[3];
    float rot[4][4]; // current rotation applied to the doodad; modified by the GUI, applied to a copy of imported_volume_orig, which is then set back to imported_volume

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


static void update_bbox(tool_placer_t *placer) {
    if (!box_is_null(placer->box)) {
        int bbox[2][3];
        volume_get_bbox(placer->imported_volume, bbox, true);
        bbox_from_aabb(placer->box, bbox);
    }
}

/*
 * @brief move_to controls if/how we move the volume/bbox/origin around on the screen.
 *
 * @param placer tool_placer_t.
 * @param curs cursor_t.
 * @param translation float[4][4] a rotation or change to origin/offset provided by the UI.
 */
static void move_to(tool_placer_t *placer, float curs_pos[3]) {
    if (!placer->imported_volume) return;
    if (curs_pos && vec3_equal(curs_pos, placer->last_curs_pos)) return;

    // Apply translation from the UI if any
    if (curs_pos) {
        float corrected_curs_pos[3] = {
            curs_pos[0]-0.5, curs_pos[1]-0.5, curs_pos[2]-0.5
        };
        
        // Destination = curs + offset + center
        float destination[4][4] = MAT4_IDENTITY;
        vec3_add(destination[3], corrected_curs_pos, destination[3]);
        vec3_add(destination[3], placer->offset, destination[3]);
        vec3_sub(destination[3], placer->center, destination[3]);
        
        //LOG_D("    curs_pos: (%f,%f,%f)", corrected_curs_pos[0], corrected_curs_pos[1], corrected_curs_pos[2]);

        float trans[4][4] = MAT4_IDENTITY;
        vec3_sub(destination[3], placer->mat[3], trans[3]);
        // Move by this amount
        volume_move(placer->imported_volume, trans);
        mat4_copy(destination, placer->mat);
        //debug_log_44_matrix("trans", trans);
    }

    // Update bounding box if there is one
    update_bbox(placer);
}

static void apply_rotation(tool_placer_t *placer, float translation[4][4]) {
        float m[4][4] = MAT4_IDENTITY;
        float origin[3];

        // Add the translation to the long-running state
        mat4_imul(placer->rot, translation);

        // Origin = position + center (relative offset) + origin (relative offset)
        vec3_set(origin,
            floor(placer->mat[3][0]) + 0.5,
            floor(placer->mat[3][1]) + 0.5,
            floor(placer->mat[3][2]) + 0.5);
        vec3_add(origin, placer->center, origin);
        vec3_add(origin, placer->origin, origin);

        // Apply rotation to the volume - scoot to 0,0,0 rotate and then scoot back out
        // (same mechanic as do_move)
        mat4_itranslate(m, +origin[0], +origin[1], +origin[2]);
        mat4_imul(m, placer->rot);
        mat4_itranslate(m, -origin[0], -origin[1], -origin[2]);
        volume_t *copy = volume_copy(placer->imported_volume_orig);
        volume_move(copy, placer->mat);
        volume_move(copy, m);
        volume_delete(placer->imported_volume);
        placer->imported_volume = copy;

        // Apply the rotation to the origin - scoot to 0,0,0 rotate and then scoot back out
        mat4_copy(mat4_identity, m);
        mat4_itranslate(m, +placer->origin[0], +placer->origin[1], +placer->origin[2]);
        mat4_imul(m, placer->rot);
        mat4_itranslate(m, -placer->origin[0], -placer->origin[1], -placer->origin[2]);

        float imat[4][4];
        mat4_invert(m, imat);
        float or[4][4] = MAT4_IDENTITY;
        vec3_to_mat4(placer->origin, or);
        mat4_mul(m, or, or);
        vec3_copy(or[3], placer->origin);
}

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

/**
 * @brief Find the bottom centre point of the placer->imported_volume.
 */
static void center_origin(tool_placer_t *placer)
{
    int bbox[2][3];
    float pos[3];

    volume_get_bbox(placer->imported_volume, bbox, true);
    //bbox_from_aabb(placer->box, bbox);
    pos[0] = round((bbox[0][0] + bbox[1][0] - 1) / 2.0); // centre x
    pos[1] = round((bbox[0][1] + bbox[1][1] - 1) / 2.0); // centre y
    pos[2] = bbox[0][2]; // bottom level z

    LOG_D("center_origin:");
    //LOG_D("    Cursor: %f / %f / %f", goxel.cursor.pos[0], goxel.cursor.pos[1], goxel.cursor.pos[2]);
    //LOG_D("    BBox: (%i,%i,%i) (%i,%i,%i)", bbox[0][0], bbox[0][1], bbox[0][2], bbox[1][0], bbox[1][1], bbox[1][2]);
    //debug_log_vec3_float("    found center:", pos);
    vec3_copy(pos, placer->center);
    vec3_set(placer->origin, 0, 0, 0);
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
    cursor_t *curs = gest->cursor;

    if (gest->state == GESTURE_END || !curs->snaped) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        return 0;
    }

    if (goxel.tool_volume && check_can_skip(placer, curs))
        return 0;
    
    if (placer->imported_volume && curs->snaped) {
        move_to(placer, curs->pos);

        volume_merge(volume, placer->imported_volume, MODE_OVER, NULL);
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, volume);
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
    if (!placer->imported_volume) {
        placer->imported_volume = volume_new();
        placer->imported_volume_orig = volume_new();
    }

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

    if (curs->snaped && placer->imported_volume) {
        // render the bounding box
        render_box(&goxel.rend, placer->box, NULL, EFFECT_STRIP | EFFECT_WIREFRAME);

        // render the origin dot
        float origin_box[4][4] = MAT4_IDENTITY;
        uint8_t color[4] = {255, 0, 0, 255};
        float corrected_curs_pos[3] = {
            curs->pos[0]-0.5, curs->pos[1]-0.5, curs->pos[2]-0.5
        };
        vec3_copy(corrected_curs_pos, origin_box[3]);
        vec3_add(origin_box[3], placer->offset, origin_box[3]);
        vec3_add(origin_box[3], placer->origin, origin_box[3]);
        mat4_iscale(origin_box, 0.1, 0.1, 0.1);
        render_box(&goxel.rend, origin_box, color,
                EFFECT_NO_DEPTH_TEST | EFFECT_NO_SHADING);
    }

    curs->snap_offset = +0.5;

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

static void reset(tool_placer_t* placer) {
    volume_delete(placer->imported_volume);
    placer->imported_volume = volume_new();
    placer->imported_volume_orig = volume_new();
    mat4_copy(mat4_identity, placer->mat);
    mat4_copy(mat4_identity, placer->box);
    vec3_set(placer->offset, 0, 0, 0);
    vec3_set(placer->origin, 0, 0, 0);
    vec3_set(placer->last_curs_pos, 0, 0, 0);
    mat4_copy(mat4_identity, placer->rot);
}
typedef struct past_import past_import_t;
struct past_import {
    past_import_t *next, *prev; // For the global list of past files.
    const char *path;
    const char *file_name;
    const file_format_t *format;
};
past_import_t *past_files = NULL;

static void on_file_import(const char *path, const char *file_name, const file_format_t *format) {
    past_import_t *past;
    past = calloc(1, sizeof(*past));
    *past = (past_import_t) {
        .path = path,
        .file_name = file_name,
        .format = format,
    };

    past_import_t *f, *tmp;
    DL_FOREACH_SAFE(past_files, f, tmp) {
        // If there is an entry for this name, remove it
        if (strcmp(f->file_name, file_name) != 0) {
            continue;
        }
        LOG_D("Delete %s", f->file_name);
        DL_DELETE(past_files, f);
    }

    DL_APPEND(past_files, past);
}

static void post_import(tool_placer_t *placer) {
    placer->imported_volume_orig = volume_copy(placer->imported_volume);
    center_origin(placer);
}

static void placer_acquire_selection() {
    tool_placer_t *placer = (tool_placer_t*) goxel.tool;
    reset(placer);
    volume_t* copy;
    painter_t painter;
    const float (*box)[4][4] = &goxel.selection;

    copy = volume_copy(goxel.image->active_layer->volume);

    // Use the mask (from fuzzy select) if there
    if (!volume_is_empty(goxel.mask)) {
        volume_merge(copy, goxel.mask, MODE_INTERSECT, NULL);
    } else {
        // Otherwise, use the selection box
        painter = (painter_t) {
            .shape = &shape_cube,
            .mode = MODE_INTERSECT,
            .color = {255, 255, 255, 255},
        };
        volume_op(copy, &painter, *box);
    }

    placer->imported_volume = copy;
    post_import(placer);
}

float zero(float num) {
    return (num == -0.0f) ? 0.0f : num;
}

static int gui(tool_t *tool)
{
    tool_placer_t *placer = (tool_placer_t*)tool;
    float rotation[4][4] = MAT4_IDENTITY;
    float rot_degs[3];
    bool reset_rotation = false;
    int origin_x, origin_y, origin_z, offset_x, offset_y, offset_z;
    mat4_to_eul_degxyz(placer->rot, rot_degs);
    
    if (!box_is_null(goxel.selection)) {
        if(gui_section_begin("Selection", true)) {
            if(gui_button("Copy to placer", -1, 0)) {
                // Just copy
                placer_acquire_selection();
            }
            if(gui_button("Move via placer", -1, 0)) {
                // Copy to placer, wipe the selection and voxels within
                placer_acquire_selection();
                action_exec(action_get(ACTION_tool_set_selection, true));
                action_exec(action_get(ACTION_layer_clear, true));
                action_exec(action_get(ACTION_tool_set_placer, true));
                action_exec(action_get(ACTION_reset_selection, true));
            }
        }
        gui_section_end();
    }

    // Browse files
    char label[128];
    gui_text("Import as");
    if (!g_current) g_current = file_formats_import_to_volume; // First one.

    make_label(g_current, label, sizeof(label));
    if (gui_combo_begin("Import as", label)) {
        file_format_iter("v", NULL, on_format);
        gui_combo_end();
    }

    if (g_current->import_gui)
        g_current->import_gui(g_current);

    if (gui_button("Import", 1, 0)) {
        reset(placer);
        goxel_import_file_to_volume(NULL, g_current->name, placer->imported_volume, on_file_import);
        post_import(placer);
    }
    
    if (gui_button("Reset", 1, 0)) {
        reset(placer);
    }

    tool_gui_snap();
    
    if (placer->imported_volume) {
        origin_x = (int)round(placer->origin[0]);
        origin_y = (int)round(placer->origin[1]);
        origin_z = (int)round(placer->origin[2]);

        offset_x = (int)round(placer->offset[0]);
        offset_y = (int)round(placer->offset[1]);
        offset_z = (int)round(placer->offset[2]);

        if (gui_section_begin("Rotation", true)) {
            gui_group_begin(NULL);
            gui_row_begin(2);
            if (gui_button("-X", 0, 0))
                mat4_irotate(rotation, -M_PI / 8, 1, 0, 0);
            if (gui_button("+X", 0, 0))
                mat4_irotate(rotation, +M_PI / 8, 1, 0, 0);
            gui_row_end();

            gui_row_begin(2);
            if (gui_button("-Y", 0, 0))
                mat4_irotate(rotation, -M_PI / 8, 0, 1, 0);
            if (gui_button("+Y", 0, 0))
                mat4_irotate(rotation, +M_PI / 8, 0, 1, 0);
            gui_row_end();

            gui_row_begin(2);
            if (gui_button("-Z", 0, 0))
                mat4_irotate(rotation, -M_PI / 8, 0, 0, 1);
            if (gui_button("+Z", 0, 0))
                mat4_irotate(rotation, +M_PI / 8, 0, 0, 1);
            gui_row_end();
            
            gui_row_begin(1);
            char *msg;
            asprintf(&msg, "Rotation: %.1f / %.1f / %.1f", zero(rot_degs[0]), zero(rot_degs[1]), zero(rot_degs[2]));
            gui_text(msg);
            gui_row_end();
            if (gui_button("Reset", 0, 0)) {
                reset_rotation = true;
            }
            gui_group_end();
        }
        gui_section_end();

        if (gui_section_begin("Offset", GUI_SECTION_COLLAPSABLE_CLOSED)) {
            gui_group_begin(NULL);
            if (gui_input_int("X", &offset_x, 0, 0)) {
                placer->offset[0] = offset_x;
            }
            if (gui_input_int("Y", &offset_y, 0, 0)) {
                placer->offset[1] = offset_y;
            }
            if (gui_input_int("Z", &offset_z, 0, 0)) {
                placer->offset[2] = offset_z;
            }
            if (gui_button("Reset", -1, 0)) {
                placer->offset[0] = 0;
                placer->offset[1] = 0;
                placer->offset[2] = 0;
            }
            gui_group_end();
        }
        gui_section_end();

        if (gui_section_begin("Origin", GUI_SECTION_COLLAPSABLE_CLOSED)) {
            gui_group_begin(NULL);
            if (gui_input_int("X", &origin_x, 0, 0)) {
                placer->origin[0] = origin_x;
            }
            if (gui_input_int("Y", &origin_y, 0, 0)) {
                placer->origin[1] = origin_y;
            }
            if (gui_input_int("Z", &origin_z, 0, 0)) {
                placer->origin[2] = origin_z;
            }
            if (gui_button("Center", -1, 0)) {
                vec3_set(placer->origin, 0, 0, 0);
            }
            gui_group_end();
        }
        gui_section_end();
    }

    if (gui_section_begin("History", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        const past_import_t *i;
        DL_FOREACH_REVERSE(past_files, i) {
            if(gui_button(i->file_name, 0, 0)) {
                reset(placer);
                i->format->import_volume_func(i->format, placer->imported_volume, i->path);
                post_import(placer);
            }
        }
    } gui_section_end();

    if (reset_rotation || (placer->imported_volume && !mat4_equal(rotation, mat4_identity))) {
        if (reset_rotation) mat4_set_identity(placer->rot);
        apply_rotation(placer, rotation);
    }
    
    return 0;
}

TOOL_REGISTER(TOOL_PLACER, placer, tool_placer_t,
              .name = "Placer",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .default_shortcut = "P",
              .has_snap = true,
)

ACTION_REGISTER(ACTION_placer_acquire_selection,
    .help = "Placer - acquire selection",
    .flags = ACTION_CAN_EDIT_SHORTCUT,
    .cfunc = placer_acquire_selection
)