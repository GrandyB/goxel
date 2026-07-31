/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
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

#include <math.h>

typedef struct {
    tool_t tool;
} tool_cursor_t;

typedef struct {
    int state; /* 0 idle, 1 hover, 2 drag */
    layer_t *layer;
    int face;
    bool history_pushed;
    float box[4][4];
    float start_box[4][4];
    float drag_plane[4][4];
} cursor_edit_t;

static cursor_edit_t g_edit = {};
/* Set during the GUI frame when the mouse is over a layers-panel row. */
static layer_t *g_panel_hover = NULL;

static bool layer_gets_gizmo(const image_t *img, const layer_t *layer)
{
    layer_t *active;

    if (!img || !layer || !layer_effectively_visible(img, layer))
        return false;

    active = img->active_layer;
    if (!active) {
        /* Nothing selected: all leaf layers. */
        return !layer_has_children(img, layer);
    }
    /* Selected: that layer and every descendant. */
    return layer_is_ancestor(img, active, layer);
}

static void normalize_box(const float box[4][4], float out[4][4])
{
    float vertices[8][3];
    mat4_copy(box, out);
    box_get_vertices(box, vertices);
    bbox_from_npoints(out, 8, vertices);
}

static bool layer_gizmo_box(const layer_t *layer, float box[4][4])
{
    const volume_t *vol;

    if (!layer) return false;
    vol = goxel_get_layer_move_volume(layer);
    if (!vol) return false;
    volume_get_box(vol, true, box);
    if (layer->shape)
        normalize_box(layer->mat, box);
    if (box_is_null(box)) return false;
    /* Groups: pad so the parent wireframe sits outside child boxes. */
    if (layer_has_children(goxel.image, layer))
        bbox_grow(box, 0.5f, 0.5f, 0.5f, box);
    return true;
}

static float box_volume_approx(const float box[4][4])
{
    return fabsf(box[0][0] * box[1][1] * box[2][2]) * 8.f;
}

static void apply_move(layer_t *layer, const float transf[4][4])
{
    image_t *img = goxel.image;
    if (layer_has_children(img, layer))
        image_move_layer_content_subtree(img, layer, transf, false);
    else
        do_move_layer(layer, transf, NULL, false);
}

static void render_face_gizmo(const float box[4][4], int face)
{
    uint8_t color[4] = {255, 0, 0, 16};
    float face_plane[4][4], a[3], b[3], dir[3];

    mat4_mul(box, FACES_MATS[face], face_plane);
    mat4_iscale(face_plane, 2, 2, 1);
    mat4_itranslate(face_plane, 0, 0, 0.001);
    render_rect_fill(&goxel.rend, face_plane, color);
    vec3_normalize(face_plane[2], dir);
    vec3_copy(face_plane[3], a);
    vec3_addk(a, dir, 3, b);
    color[3] = 255;
    render_line(&goxel.rend, a, b, color, EFFECT_ARROW);
}

static void box_center(const float box[4][4], float out[3])
{
    vec3_copy(box[3], out);
}

static void draw_gizmo_boxes(const image_t *img)
{
    layer_t *layer;
    float box[4][4];

    /* Panel hover solos a single layer/group box. */
    if (g_panel_hover) {
        if (layer_effectively_visible(img, g_panel_hover) &&
            layer_gizmo_box(g_panel_hover, box))
            render_box(&goxel.rend, box, NULL, EFFECT_STRIP | EFFECT_WIREFRAME);
        return;
    }

    DL_FOREACH(img->layers, layer) {
        if (!layer_gets_gizmo(img, layer)) continue;
        if (!layer_gizmo_box(layer, box)) continue;
        render_box(&goxel.rend, box, NULL, EFFECT_STRIP | EFFECT_WIREFRAME);
    }
}

static void apply_drag(cursor_edit_t *edit, cursor_t *curs,
                       const float viewport[4])
{
    camera_t *cam;
    float opos[3], onorm[3], wpos[3], pos[3], local[3];
    float face_plane[4][4], nrm[3], d[3], ofs[3];
    float transf[4][4] = MAT4_IDENTITY;
    float box[4][4];

    cam = goxel.image->active_camera;
    if (!cam || !edit->layer) return;

    wpos[0] = curs->xy[0];
    wpos[1] = curs->xy[1];
    wpos[2] = 0;
    camera_get_ray(cam, wpos, viewport, opos, onorm);
    if (!plane_line_intersection(edit->drag_plane, opos, onorm, pos))
        return;
    local[0] = pos[0];
    local[1] = pos[1];
    local[2] = 0;
    mat4_mul_vec3(edit->drag_plane, local, pos);
    pos[0] = roundf(pos[0]);
    pos[1] = roundf(pos[1]);
    pos[2] = roundf(pos[2]);

    if (!layer_gizmo_box(edit->layer, box)) return;
    mat4_mul(box, FACES_MATS[edit->face], face_plane);
    vec3_normalize(face_plane[2], nrm);
    vec3_add(box[3], face_plane[2], d);
    vec3_sub(pos, d, ofs);
    vec3_project(ofs, nrm, ofs);
    if (ofs[0] == 0 && ofs[1] == 0 && ofs[2] == 0) return;

    if (!edit->history_pushed) {
        image_history_push(goxel.image);
        edit->history_pushed = true;
    }
    mat4_itranslate(transf, ofs[0], ofs[1], ofs[2]);
    apply_move(edit->layer, transf);
    if (layer_gizmo_box(edit->layer, box))
        render_face_gizmo(box, edit->face);
}

static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    image_t *img = goxel.image;
    cursor_t *curs = &goxel.cursor;
    camera_t *cam;
    layer_t *layer, *best = NULL;
    float box[4][4], hit[3], n[3];
    float best_vol = INFINITY;
    int face = -1, best_face = -1;
    bool pressed = curs->flags & CURSOR_PRESSED;

    (void)tool;
    (void)painter;

    curs->snap_mask = 0;
    if (!img) return 0;

    cam = img->active_camera;
    if (!cam) return 0;

    /* Continue an in-progress drag. */
    if (g_edit.state == 2 && g_edit.layer) {
        goxel_set_help_text("Drag to move layer");
        apply_drag(&g_edit, curs, viewport);
        if (!pressed) {
            g_edit.state = 0;
            g_edit.layer = NULL;
        }
        return 0;
    }

    /* Hit-test only (boxes are drawn from tool_cursor_render). */
    DL_FOREACH_REVERSE(img->layers, layer) {
        float vol;
        if (!layer_gets_gizmo(img, layer)) continue;
        if (!layer_gizmo_box(layer, box)) continue;
        if (!box_unproject(cam, viewport, curs->xy, box, false, hit, n, &face))
            continue;
        vol = box_volume_approx(box);
        if (vol < best_vol) {
            best_vol = vol;
            best = layer;
            best_face = face;
            mat4_copy(box, g_edit.box);
            vec3_copy(hit, curs->pos);
            vec3_copy(n, curs->normal);
        }
    }

    if (!best) {
        /* Click empty space: clear selection so all leaf gizmos return. */
        if (pressed) {
            img->active_layer = NULL;
            g_edit.state = 0;
            g_edit.layer = NULL;
        }
        return 0;
    }

    g_edit.layer = best;
    g_edit.face = best_face;
    g_edit.state = 1;
    render_face_gizmo(g_edit.box, best_face);
    goxel_set_help_text("Click to select, drag to move");

    if (pressed) {
        float face_plane[4][4], v[3];

        img->active_layer = best;
        /* Stay on Cursor: selection scopes gizmos to this layer + descendants. */
        g_edit.state = 2;
        g_edit.history_pushed = false;
        mat4_copy(g_edit.box, g_edit.start_box);
        mat4_mul(g_edit.box, FACES_MATS[best_face], face_plane);
        vec3_normalize(face_plane[0], v);
        plane_from_vectors(g_edit.drag_plane, curs->pos, curs->normal, v);
    }

    return 0;
}

static int gui(tool_t *tool)
{
    (void)tool;
    gui_text("Select and move layers.");
    gui_text("Hold Alt to show layer names.");
    return 0;
}

void tool_cursor_on_gui_frame(void)
{
    g_panel_hover = NULL;
}

void tool_cursor_set_panel_hover(layer_t *layer)
{
    g_panel_hover = layer;
}

void tool_cursor_render(void)
{
    image_t *img = goxel.image;

    if (!goxel.tool || goxel.tool->id != TOOL_CURSOR) return;
    if (!img) return;
    draw_gizmo_boxes(img);
}

void tool_cursor_render_labels(void)
{
    image_t *img = goxel.image;
    layer_t *layer;
    float box[4][4], pos[3];
    uint8_t color[4] = {200, 200, 200, 255};

    if (!goxel.tool || goxel.tool->id != TOOL_CURSOR) return;
    if (!(goxel.cursor.flags & CURSOR_LEFT_ALT)) return;
    if (!img) return;

    if (g_panel_hover) {
        if (g_panel_hover->name[0] && layer_gizmo_box(g_panel_hover, box)) {
            box_center(box, pos);
            gui_world_label(pos, g_panel_hover->name, color);
        }
        return;
    }

    DL_FOREACH(img->layers, layer) {
        if (!layer_gets_gizmo(img, layer)) continue;
        if (!layer->name[0]) continue;
        if (!layer_gizmo_box(layer, box)) continue;
        box_center(box, pos);
        gui_world_label(pos, layer->name, color);
    }
}

TOOL_REGISTER(TOOL_CURSOR, cursor, tool_cursor_t,
              .name = "Cursor",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_MOVE,
              .default_shortcut = "J",
)
