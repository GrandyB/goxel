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
    /* Face normal locked at drag begin (block face alignment). */
    float stroke_normal[3];
    // Cache of the last operation (hover/drag skip).
    // XXX: could we remove this?
    struct     {
        float      pos[3];
        float      normal[3];
        bool       pressed;
        int        mode;
        uint64_t   volume_key;
        float      radius_x, radius_y, radius_z;
        bool       block_face_alignment;
        bool       origin_at_base;
        bool       surface_paint;
        int        brush_source_mode;
        int        brush_texture_index;
        float      brush_texture_hue;
        float      brush_texture_saturation;
        float      brush_texture_lightness;
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
            brush->last_op.block_face_alignment ==
                goxel.brush_block_face_alignment &&
            brush->last_op.origin_at_base == goxel.brush_origin_at_base &&
            brush->last_op.surface_paint == goxel.brush_surface_paint &&
            brush->last_op.brush_source_mode == goxel.brush_source_mode &&
            brush->last_op.brush_texture_index == goxel.brush_texture_index &&
            brush->last_op.brush_texture_hue == goxel.brush_texture_hue &&
            brush->last_op.brush_texture_saturation ==
                goxel.brush_texture_saturation &&
            brush->last_op.brush_texture_lightness ==
                goxel.brush_texture_lightness &&
            vec3_equal(curs->pos, brush->last_op.pos) &&
            /* Drag locks alignment at begin; only hover tracks live normal. */
            (!goxel.brush_block_face_alignment || pressed ||
             vec3_equal(curs->normal, brush->last_op.normal))) {
        return true;
    }
    brush->last_op.pressed = pressed;
    brush->last_op.mode = mode;
    brush->last_op.volume_key = volume_get_key(volume);
    brush->last_op.radius_x = goxel.radius_x;
    brush->last_op.radius_y = goxel.radius_y;
    brush->last_op.radius_z = goxel.radius_z;
    brush->last_op.block_face_alignment = goxel.brush_block_face_alignment;
    brush->last_op.origin_at_base = goxel.brush_origin_at_base;
    brush->last_op.surface_paint = goxel.brush_surface_paint;
    brush->last_op.brush_source_mode = goxel.brush_source_mode;
    brush->last_op.brush_texture_index = goxel.brush_texture_index;
    brush->last_op.brush_texture_hue = goxel.brush_texture_hue;
    brush->last_op.brush_texture_saturation = goxel.brush_texture_saturation;
    brush->last_op.brush_texture_lightness = goxel.brush_texture_lightness;
    vec3_copy(curs->pos, brush->last_op.pos);
    vec3_copy(curs->normal, brush->last_op.normal);
    return false;
}

/* Match extrude/box_edit: nearest cube face for an axis-aligned normal. */
static int brush_get_face(const float n[3])
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

// XXX: same as in brush.c.
static void get_box3(const float p0[3], const float p1[3], const float n[3],
                    float r_x, float r_y, float r_z, const float plane[4][4], float out[4][4])
{
    float rot[4][4], box[4][4];
    float v[3];
    int face;

    if (p1 == NULL) {
        // Block face alignment: Diameter Z along the snapped face normal
        // (e.g. wall +Y → world extents X/Z/Y for diameters X/Y/Z).
        // box[0]=Z, box[1]=X, box[2]=Y - same convention as box_swap_axis.
        if (goxel.brush_block_face_alignment && n &&
            (face = brush_get_face(n)) >= 0) {
            const float (*fm)[4] = FACES_MATS[face];
            mat4_set_identity(box);
            vec3_mul(fm[2], r_z, box[0]);
            vec3_mul(fm[0], r_x, box[1]);
            vec3_mul(fm[1], r_y, box[2]);
            vec3_copy(p0, box[3]);
            if (goxel.brush_origin_at_base)
                vec3_addk(box[3], fm[2], r_z - 0.5f, box[3]);
            mat4_copy(box, out);
            return;
        }
        bbox_from_extents(box, p0, r_x, r_y, r_z);
        box_swap_axis(box, 2, 0, 1, box);
        // Cursor is on voxel centers (*.5). Shift up so the shape's lowest
        // Z sits on the bottom face of that voxel (not an extra voxel up).
        if (goxel.brush_origin_at_base)
            box[3][2] += r_z - 0.5f;
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
        box[3][2] += r_z - 0.5f;
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
    bool alt = curs->flags & CURSOR_LEFT_ALT;
    int merge_mode;
    float spacing;
    bool surface_paint_mode;

    float target[3];
    vec3_copy(curs->pos, target);
    if (alt) {
        target[2] = brush->start_pos[2];
    }

    if (gest->state == GESTURE_BEGIN) {
        image_history_push(goxel.image);
        /* Add mode on a group parent: new child instead of parent volume. */
        if (painter.mode == MODE_OVER &&
                !image_ensure_layer_for_adding(goxel.image))
            return 0;
        brush->layer_key_at_stroke_start =
            volume_get_key(goxel.image->active_layer->volume);
        volume_set(brush->volume_orig, goxel.image->active_layer->volume);
        brush->last_op.mode = 0; // Discard last op.
        vec3_copy(target, brush->last_pos);
        vec3_copy(curs->normal, brush->stroke_normal);
        volume_clear(brush->volume);
        if (!brush->delta) brush->delta = volume_new();
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, brush->volume_orig);
        if (goxel.brush_auto_plane) {
            float plane_pos[3];
            vec3_addk(curs->pos, curs->normal, -curs->snap_offset, plane_pos);
            plane_from_normal(goxel.tool_plane, plane_pos, curs->normal);
        }
    }

    painter = *(painter_t*)USER_GET(user, 1);
    if (    (gest->state == GESTURE_UPDATE) &&
            (check_can_skip(brush, curs, painter.mode))) {
        return 0;
    }

    merge_mode = painter.mode;
    surface_paint_mode = (merge_mode == MODE_PAINT && goxel.brush_surface_paint);
    // MODE_PAINT of soft/partial coverage is not idempotent: merging each
    // frame's stamps re-applies falloff onto already-painted voxels and
    // hardens AA edges vs one merge of the accumulated mask (commit path).
    bool rebuild_preview = (merge_mode == MODE_PAINT &&
                            (painter.smoothness > 0 || painter.color[3] < 255));
    painter.mode = MODE_MAX;

    if (!brush->delta) brush->delta = volume_new();
    volume_clear(brush->delta);

    // Step ~ brush radius (min axis): small brushes stay 1-voxel dense;
    // large brushes avoid a volume_op per voxel of travel.
    spacing = surface_paint_mode ? max(0.7f, min(r_x, r_y))
                                 : max(0.7f, min3(r_x, r_y, r_z));

    // Shift+click: connect previous stroke end → target.
    if (gest->state == GESTURE_BEGIN && shift) {
        if (r_x <= 0.5f && r_y <= 0.5f) {
            // 1×1 footprint: stamp cubes so stepped paths stay solid.
            painter.shape = &shape_cube;
            nb = ceil(vec3_dist(brush->start_pos, target) / spacing);
            nb = max(nb, 1);
            for (i = 0; i <= nb; i++) {
                vec3_mix(brush->start_pos, target, (float)i / nb, pos);
                if (surface_paint_mode) {
                    volume_brush_surface_stamp(brush->delta, brush->volume_orig,
                                               &painter, pos, r_x, r_y,
                                               MODE_MAX);
                } else {
                    get_box3(pos, NULL, brush->stroke_normal,
                             r_x, r_y, r_z, NULL, box);
                    volume_op(brush->delta, &painter, box);
                }
            }
        } else if (!surface_paint_mode && painter.shape == &shape_sphere) {
            // Larger spheres: one tube along the segment (not stamped spheres).
            painter.shape = &shape_cylinder;
            get_box3(brush->start_pos, target, brush->stroke_normal,
                     r_x, r_y, r_z, NULL, box);
            volume_op(brush->delta, &painter, box);
            painter.shape = &shape_sphere;
        } else {
            // Cube / cylinder: stamp the brush along the path (full height
            // on diagonals; origin-at-base walls stay uniform).
            nb = ceil(vec3_dist(brush->start_pos, target) / spacing);
            nb = max(nb, 1);
            for (i = 0; i <= nb; i++) {
                vec3_mix(brush->start_pos, target, (float)i / nb, pos);
                if (surface_paint_mode) {
                    volume_brush_surface_stamp(brush->delta, brush->volume_orig,
                                               &painter, pos, r_x, r_y,
                                               MODE_MAX);
                } else {
                    get_box3(pos, NULL, brush->stroke_normal,
                             r_x, r_y, r_z, NULL, box);
                    volume_op(brush->delta, &painter, box);
                }
            }
        }
    }

    // Stamp along the segment so fast motion does not leave gaps.
    nb = ceil(vec3_dist(target, brush->last_pos) / spacing);
    nb = max(nb, 1);
    for (i = 0; i < nb; i++) {
        vec3_mix(brush->last_pos, target, (i + 1.0) / nb, pos);
        if (surface_paint_mode) {
            volume_brush_surface_stamp(brush->delta, brush->volume_orig,
                                       &painter, pos, r_x, r_y, MODE_MAX);
        } else {
            get_box3(pos, NULL, brush->stroke_normal,
                     r_x, r_y, r_z, NULL, box);
            volume_op(brush->delta, &painter, box);
        }
    }

    // Keep full stroke mask via cheap tile merge.
    if (surface_paint_mode) {
        // Surface stamps are sparse: a tile-wide MODE_MAX would take the
        // source RGB of untouched (empty) voxels and blacken the mask.
        volume_merge_sparse_from(brush->volume, brush->delta, MODE_MAX);
    } else {
        volume_merge_from(brush->volume, brush->delta, MODE_MAX, NULL);
    }

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
        mat4_copy(plane_null, goxel.tool_plane);
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
    bool surface_paint_mode = (painter->mode == MODE_PAINT &&
                               goxel.brush_surface_paint);

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

    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, volume);
    if (surface_paint_mode) {
        volume_brush_surface_stamp(goxel.tool_volume, volume, painter, curs->pos,
                                   goxel.radius_x, goxel.radius_y, painter->mode);
    } else {
        get_box3(curs->pos, NULL, curs->normal,
                 goxel.radius_x, goxel.radius_y, goxel.radius_z, NULL, box);
        volume_op(goxel.tool_volume, painter, box);
    }

    brush->last_op.volume_key = volume_get_key(goxel.tool_volume);

    return 0;
}


static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    goxel_set_help_text("Click to brush - there are hotkeys for changing modes etc! TIP: Holding shift will toggle line mode, Ctrl+click will color pick from under the cursor.");
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

    if (goxel.brush_auto_plane) {
        if (!plane_is_null(goxel.tool_plane)) {
            render_grid(&goxel.rend, goxel.tool_plane, goxel.grid_color,
                        goxel.image->box);
        } else if ((curs->flags & CURSOR_SHIFT) && curs->snaped) {
            float plane[4][4], plane_pos[3];
            vec3_addk(curs->pos, curs->normal, -curs->snap_offset, plane_pos);
            plane_from_normal(plane, plane_pos, curs->normal);
            render_grid(&goxel.rend, plane, goxel.grid_color, goxel.image->box);
        }
    }

    return tool->state;
}


static int gui(tool_t *tool)
{
    int i, tex_count;
    float cell = 64.f;
    char textures_dir[1024];
    bool has_textures_dir;
    bool is_paint_mode = (goxel.painter.mode == MODE_PAINT);
    /* Defer reload: swatches may already be in this frame's ImGui draw list. */
    static bool textures_reload_pending = false;

    (void)tool;
    if (textures_reload_pending) {
        goxel_brush_textures_reload();
        textures_reload_pending = false;
    }
    has_textures_dir = goxel_brush_textures_dir(textures_dir, sizeof(textures_dir));
    if (!has_textures_dir)
        textures_dir[0] = '\0';

    if (is_paint_mode && goxel.brush_surface_paint)
        tool_gui_radius_xy();
    else
        tool_gui_radius();

    if (is_paint_mode) {
        gui_checkbox("Surface paint", &goxel.brush_surface_paint,
                     "Ignore Diameter Z; paint air-exposed surface down each column under the X/Y shape");
    }
    gui_enabled_begin(!(is_paint_mode && goxel.brush_surface_paint));
    gui_checkbox("Origin at base", &goxel.brush_origin_at_base,
                 "Lowest Z of the shape is at the cursor (Z-up), not the center");
    gui_checkbox("Block face align", &goxel.brush_block_face_alignment,
                 "Diameter Z follows the block face normal (paint walls side-on)");
    gui_checkbox("Auto-plane", &goxel.brush_auto_plane,
                 "Lock to the hovered block face plane on brush start");
    gui_enabled_end();
    tool_gui_smoothness();

    gui_dummy(0, 8);
    {
        static const char *source_tabs[] = {"Color", "Texture"};
        if (gui_tabsheet_begin("##brush_source", source_tabs, 2,
                               &goxel.brush_source_mode)) {
            if (goxel.brush_source_mode == BRUSH_SOURCE_COLOR) {
                tool_gui_color(false);
                gui_section_end();
            }

            if (goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE) {
                tex_count = goxel_brush_textures_count();
                if (gui_section_begin("Textures", true)) {
                    if (tex_count == 0) {
                        gui_text("No textures found in your goxel/textures folder.");
                    } else {
                        int cols = max(1, (int)((gui_content_avail_x() + 6.f) / (cell + 6.f)));
                        for (i = 0; i < tex_count; i++) {
                            const brush_texture_t *tex = goxel_brush_texture_get(i);
                            texture_t *preview = goxel_brush_texture_preview_get(i);
                            char id[64];
                            snprintf(id, sizeof(id), "brush_tex_%d", i);
                            if (i && (i % cols))
                                gui_same_line_spaced(6.f);
                            if (gui_texture_swatch_entry(
                                        id,
                                        preview ? preview->tex : 0,
                                        preview ? preview->tex_w : 0,
                                        preview ? preview->tex_h : 0,
                                        preview ? preview->w : 0,
                                        preview ? preview->h : 0,
                                        tex ? tex->name : NULL,
                                        goxel.brush_texture_index == i,
                                        cell)) {
                                goxel_brush_texture_set_current(i);
                            }
                        }
                    }
                    if (gui_button("Refresh", 0, 0))
                        textures_reload_pending = true;
                    gui_same_line_spaced(6.f);
                    gui_enabled_begin(has_textures_dir);
                    if (gui_button("Open folder", 0, 0)) {
                        if (!gui_open_in_shell(textures_dir))
                            gui_alert("Open folder", "Could not open the textures folder.");
                    }
                    gui_enabled_end();
                }
                gui_dummy(0, 8);
                gui_input_float("Hue", &goxel.brush_texture_hue, 1.f, -180.f, 180.f,
                                "%.1f");
                gui_input_float("Saturation", &goxel.brush_texture_saturation, 1.f,
                                0.f, 200.f, "%.1f");
                gui_input_float("Lightness", &goxel.brush_texture_lightness, 1.f,
                                -100.f, 100.f, "%.1f");
                if (goxel.painter.mode == MODE_PAINT) {
                    gui_color_opacity(goxel.painter.color);
                }
                if (gui_button("Reset", 0, 0)) {
                    goxel.brush_texture_hue = 0.f;
                    goxel.brush_texture_saturation = 100.f;
                    goxel.brush_texture_lightness = 0.f;
                    goxel.painter.color[3] = 255;
                    /* Persist reset onto the active texture's remembered values. */
                    if (goxel.brush_texture_index >= 0 &&
                        goxel.brush_texture_index < goxel.brush_textures_count) {
                        brush_texture_t *cur =
                            &goxel.brush_textures[goxel.brush_texture_index];
                        cur->hue = 0.f;
                        cur->saturation = 100.f;
                        cur->lightness = 0.f;
                        cur->opacity = 255;
                    }
                }
                gui_section_end();
            }
            gui_tabsheet_end();
        }
    }

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
