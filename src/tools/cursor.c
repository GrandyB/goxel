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
    float start_box[4][4]; /* Ungrown volume AABB at drag start. */
    float applied_ofs[3];  /* Translation already applied this drag. */
    float drag_plane[4][4];
} cursor_edit_t;

static cursor_edit_t g_edit = {};
/* Set during the GUI frame when the mouse is over a layers-panel row. */
static layer_t *g_panel_hover = NULL;
/* Viewport hover while nothing is selected (solid white box). */
static layer_t *g_viewport_hover = NULL;
/* Hold-to-preview for select-layer-under-cursor (any tool). */
static layer_t *g_pick_preview = NULL;
static float g_pick_preview_label_pos[3];
static bool g_pick_preview_has_label = false;

static bool layer_gets_gizmo(const image_t *img, const layer_t *layer)
{
    layer_t *active;

    if (!img || !layer || !layer_effectively_visible(img, layer))
        return false;

    active = img->active_layer;
    if (active)
        return active == layer;

    /* Nothing selected: all non-locked layers. */
    return !layer->locked;
}

static void normalize_box(const float box[4][4], float out[4][4])
{
    float vertices[8][3];
    mat4_copy(box, out);
    box_get_vertices(box, vertices);
    bbox_from_npoints(out, 8, vertices);
}

/* Exact move AABB (no group padding). Used for drag math so translations
 * stay on integer voxels; volume_move resampling is destructive otherwise. */
static bool layer_move_box(const layer_t *layer, float box[4][4])
{
    const volume_t *vol;

    if (!layer) return false;
    vol = goxel_get_layer_move_volume(layer);
    if (!vol) return false;
    volume_get_box(vol, true, box);
    if (layer->shape)
        normalize_box(layer->mat, box);
    return !box_is_null(box);
}

static bool layer_gizmo_box(const layer_t *layer, float box[4][4])
{
    if (!layer_move_box(layer, box)) return false;
    /* Groups: pad so the parent wireframe sits outside child boxes.
     * Visual/hit-test only — never use the grown box for move deltas. */
    if (layer_has_children(goxel.image, layer))
        bbox_grow(box, 0.5f, 0.5f, 0.5f, box);
    return true;
}

static float box_volume_approx(const float box[4][4])
{
    return fabsf(box[0][0] * box[1][1] * box[2][2]) * 8.f;
}

/* Depth epsilon: larger than group gizmo pad (0.5) so nested parent/child
 * faces tie and smaller volume wins; smaller than typical layer separation
 * so a large sparse AABB does not lose to stuff behind it along the ray. */
#define CURSOR_HIT_DEPTH_EPS 1.0f

/*
 * Raycast all six faces; pick the nearest hit in front of the camera.
 * Unlike box_unproject, this works from inside large empty AABBs (roofs)
 * and does not skip the face you are looking at.
 */
static bool cursor_box_hit(const camera_t *cam, const float viewport[4],
                           const float pos[2], const float box[4][4],
                           float out[3], float normal[3], int *face,
                           float *t_out)
{
    int f;
    float wpos[3] = {pos[0], pos[1], 0};
    float opos[3], onorm[3];
    float plane[4][4], local[3], world[3], delta[3];
    float best_t = INFINITY;
    float best_world[3], best_n[3];
    int best_face = -1;

    if (!cam || box_is_null(box)) return false;
    camera_get_ray(cam, wpos, viewport, opos, onorm);
    for (f = 0; f < 6; f++) {
        float t;
        mat4_copy(box, plane);
        mat4_imul(plane, FACES_MATS[f]);
        if (!plane_line_intersection(plane, opos, onorm, local))
            continue;
        if (!(local[0] >= -1 && local[0] <= 1 &&
              local[1] >= -1 && local[1] <= 1))
            continue;
        mat4_mul_vec3(plane, local, world);
        vec3_sub(world, opos, delta);
        t = vec3_dot(delta, onorm);
        if (t < 0.f || t >= best_t)
            continue;
        best_t = t;
        best_face = f;
        vec3_copy(world, best_world);
        vec3_normalize(plane[2], best_n);
        /* Facing away from camera (e.g. view from inside): flip. */
        if (vec3_dot(best_n, onorm) > 0.f)
            vec3_imul(best_n, -1.f);
    }
    if (best_face < 0) return false;
    vec3_copy(best_world, out);
    vec3_copy(best_n, normal);
    if (face) *face = best_face;
    if (t_out) *t_out = best_t;
    return true;
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

/* Parent/group wireframes: muted gray, longer strip dashes. Leaf gizmos
 * stay default white with the normal strip period. */
static void render_gizmo_box(const layer_t *layer, const float box[4][4])
{
    const uint8_t parent_gray[4] = {0x99, 0x99, 0x99, 255};
    int effects = EFFECT_STRIP | EFFECT_WIREFRAME;

    if (layer_has_children(goxel.image, layer)) {
        effects |= EFFECT_STRIP_LONG;
        render_box(&goxel.rend, box, parent_gray, effects);
    } else {
        render_box(&goxel.rend, box, NULL, effects);
    }
}

static void render_solo_preview_box(const image_t *img, layer_t *layer)
{
    float box[4][4];
    const uint8_t white[4] = {255, 255, 255, 255};

    if (!layer || !layer_effectively_visible(img, layer)) return;
    if (!layer_gizmo_box(layer, box)) return;
    render_box(&goxel.rend, box, white,
               EFFECT_WIREFRAME | EFFECT_NO_DEPTH_TEST);
}

static void draw_gizmo_boxes(const image_t *img)
{
    layer_t *layer;
    float box[4][4];
    float accent_box[4][4];
    bool have_accent = false;
    const uint8_t yellow[4] = {255, 255, 0, 255};
    const uint8_t white[4] = {255, 255, 255, 255};
    const uint8_t *accent_color = white;

    /* Apostrophe pick preview and panel hover solo a single box. */
    if (g_pick_preview) {
        render_solo_preview_box(img, g_pick_preview);
        return;
    }
    if (g_panel_hover) {
        render_solo_preview_box(img, g_panel_hover);
        return;
    }

    /* Draw strip gizmos first; accent (hover white / selection yellow)
     * last so it wins over other bboxes. NO_DEPTH_TEST keeps it above
     * overlapping wireframes that would otherwise occlude it. */
    DL_FOREACH(img->layers, layer) {
        if (!layer_gets_gizmo(img, layer)) continue;
        if (!layer_gizmo_box(layer, box)) continue;
        if (!img->active_layer && layer == g_viewport_hover) {
            mat4_copy(box, accent_box);
            have_accent = true;
            accent_color = white;
            continue;
        }
        if (layer == img->active_layer) {
            mat4_copy(box, accent_box);
            have_accent = true;
            accent_color = yellow;
            continue;
        }
        render_gizmo_box(layer, box);
    }
    if (have_accent)
        render_box(&goxel.rend, accent_box, accent_color,
                   EFFECT_WIREFRAME | EFFECT_NO_DEPTH_TEST);
}

static void apply_drag(cursor_edit_t *edit, cursor_t *curs,
                       const float viewport[4])
{
    camera_t *cam;
    float opos[3], onorm[3], wpos[3], pos[3], local[3];
    float face_plane[4][4], nrm[3], d[3], ofs[3], delta[3];
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

    /* Offset from drag-start face (ungrown), not the live gizmo. Group
     * gizmos are padded 0.5; using that for deltas caused half-voxel
     * volume_move steps that ate voxels (looked like cropping at the
     * image-box floor when dragging groups downward). */
    mat4_mul(edit->start_box, FACES_MATS[edit->face], face_plane);
    vec3_normalize(face_plane[2], nrm);
    vec3_add(edit->start_box[3], face_plane[2], d);
    vec3_sub(pos, d, ofs);
    vec3_project(ofs, nrm, ofs);
    ofs[0] = roundf(ofs[0]);
    ofs[1] = roundf(ofs[1]);
    ofs[2] = roundf(ofs[2]);
    vec3_sub(ofs, edit->applied_ofs, delta);
    if (delta[0] == 0 && delta[1] == 0 && delta[2] == 0) {
        if (layer_gizmo_box(edit->layer, box))
            render_face_gizmo(box, edit->face);
        return;
    }

    if (!edit->history_pushed) {
        image_history_push(goxel.image);
        edit->history_pushed = true;
    }
    mat4_itranslate(transf, delta[0], delta[1], delta[2]);
    apply_move(edit->layer, transf);
    vec3_copy(ofs, edit->applied_ofs);
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
    float best_t = INFINITY;
    int face = -1, best_face = -1;
    bool pressed = curs->flags & CURSOR_PRESSED;
    bool has_selection;
    static bool prev_pressed = false;
    bool just_pressed = pressed && !prev_pressed;

    (void)tool;
    (void)painter;

    prev_pressed = pressed;
    curs->snap_mask = 0;
    if (!img) return 0;

    cam = img->active_camera;
    if (!cam) return 0;

    has_selection = img->active_layer != NULL;
    g_viewport_hover = NULL;

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

    /* Hit-test only (boxes are drawn from tool_cursor_render).
     * Prefer nearer faces so large sparse AABBs (roofs) stay selectable
     * over smaller layers behind them; at similar depth, smaller volume
     * wins so nested children beat padded group boxes. */
    DL_FOREACH_REVERSE(img->layers, layer) {
        float vol, t;
        if (!layer_gets_gizmo(img, layer)) continue;
        if (!layer_gizmo_box(layer, box)) continue;
        if (!cursor_box_hit(cam, viewport, curs->xy, box, hit, n, &face, &t))
            continue;
        vol = box_volume_approx(box);
        if (t < best_t - CURSOR_HIT_DEPTH_EPS ||
            (t <= best_t + CURSOR_HIT_DEPTH_EPS && vol < best_vol)) {
            best_t = t;
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
        if (just_pressed) {
            img->active_layer = NULL;
            g_edit.state = 0;
            g_edit.layer = NULL;
            g_edit.history_pushed = false;
        }
        return 0;
    }

    /* No selection: whole box selects the layer; no move arrows. */
    if (!has_selection) {
        g_viewport_hover = best;
        goxel_set_help_text("Click to select layer");
        if (just_pressed) {
            img->active_layer = best;
            image_expand_to_show_layer(img, best);
            g_edit.state = 0;
            g_edit.layer = NULL;
        }
        return 0;
    }

    /* Selection active: face arrows + drag to move. */
    g_edit.layer = best;
    g_edit.face = best_face;
    g_edit.state = 1;
    render_face_gizmo(g_edit.box, best_face);
    goxel_set_help_text("Drag to move layer");

    if (just_pressed) {
        float face_plane[4][4], v[3];

        img->active_layer = best;
        image_expand_to_show_layer(img, best);
        g_edit.state = 2;
        g_edit.history_pushed = false;
        vec3_set(g_edit.applied_ofs, 0, 0, 0);
        /* Ungrown AABB for lossless integer volume_move deltas. */
        if (!layer_move_box(best, g_edit.start_box))
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
    gui_text("Click a box to select a layer.");
    gui_text("With a layer selected, drag the arrows to move.");
    gui_text("Hold Alt to show layer names.");
    return 0;
}

void tool_cursor_on_gui_frame(void)
{
    g_panel_hover = NULL;
    /* Release over UI skips tool_iter, so end a drag that is no longer pressed. */
    if (g_edit.state == 2 && !(goxel.cursor.flags & CURSOR_PRESSED)) {
        g_edit.state = 0;
        g_edit.layer = NULL;
        g_edit.history_pushed = false;
    }
}

void tool_cursor_clear_edit(void)
{
    g_edit.state = 0;
    g_edit.layer = NULL;
    g_edit.history_pushed = false;
    g_viewport_hover = NULL;
}

void tool_cursor_set_panel_hover(layer_t *layer)
{
    /* Ignore pointers that are no longer in the live layer list. */
    if (layer && goxel.image && layer_find(goxel.image, layer->id) == layer)
        g_panel_hover = layer;
    else
        g_panel_hover = NULL;
}

void tool_cursor_set_pick_preview(layer_t *layer, const float label_pos[3])
{
    if (layer && goxel.image && layer_find(goxel.image, layer->id) == layer) {
        g_pick_preview = layer;
        if (label_pos) {
            vec3_copy(label_pos, g_pick_preview_label_pos);
            g_pick_preview_has_label = true;
        } else {
            g_pick_preview_has_label = false;
        }
    } else {
        g_pick_preview = NULL;
        g_pick_preview_has_label = false;
    }
}

void tool_cursor_render(void)
{
    image_t *img = goxel.image;

    if (!img) return;

    /* Pick-preview / layers-panel hover bbox for every tool. Full cursor
     * gizmos only run while the Cursor tool is active. */
    if (!goxel.tool || goxel.tool->id != TOOL_CURSOR) {
        if (g_pick_preview)
            render_solo_preview_box(img, g_pick_preview);
        else if (g_panel_hover)
            render_solo_preview_box(img, g_panel_hover);
        return;
    }
    draw_gizmo_boxes(img);
}

void tool_cursor_render_labels(void)
{
    image_t *img = goxel.image;
    layer_t *layer;
    float box[4][4], pos[3];
    uint8_t color[4] = {200, 200, 200, 255};

    if (!img) return;

    /* Apostrophe pick: name sits above the cursor hit, any tool. */
    if (g_pick_preview && g_pick_preview->name[0]) {
        if (g_pick_preview_has_label)
            gui_world_label(g_pick_preview_label_pos, g_pick_preview->name,
                            color);
        else if (layer_gizmo_box(g_pick_preview, box)) {
            box_center(box, pos);
            gui_world_label(pos, g_pick_preview->name, color);
        }
        return;
    }

    if (!goxel.tool || goxel.tool->id != TOOL_CURSOR) return;
    if (!(goxel.cursor.flags & CURSOR_LEFT_ALT)) return;

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
