/* Goxel 3D voxels editor — Clone Stamp tool (Paint.NET-style). */

#include "goxel.h"
#include "utils/clone_stamp_op.h"

/* Purple: Ctrl-hold pick preview (exact sample blocks only). */
static const uint8_t k_pick_marker[4] = {160, 180, 255, 180};
/* Yellow: confirmed clone source (exact sample blocks). */
static const uint8_t k_source_marker[4] = {255, 200, 80, 180};

typedef struct {
    tool_t tool;

    volume_t *volume_orig; /* Layer snapshot for sampling during a stroke. */
    volume_t *stroke;      /* Accumulated clone paints (no source marker). */
    /* Exact sample-block highlights; drawn on top so other layers' blocks
     * remain visible when sampling the merged map. */
    volume_t *source_markers;

    bool has_source;
    float source_pos[3];
    /* Face normal locked at Ctrl+Click (inherit axis / source UV). */
    float source_normal[3];
    /* Paint-face normal locked at drag begin (brush Z / target UV). */
    float stroke_normal[3];
    bool offset_locked;
    float offset[3];

    /* Sampling options (GUI). */
    bool sample_inited;
    bool wall_mode;          /* face-normal brush + inherit */
    bool surface_mode;       /* top-down exposed shell; ignores Diameter Z */
    bool static_source;      /* when true, source stays fixed while dragging */
    bool take_uppermost;     /* default true once sample_inited; ignored in wall */
    int  depth;              /* inherit distance; always used in wall mode */
    bool restrict_to_layer;  /* default false: sample all visible layers */
    float opacity;           /* MODE_PAINT strength 0..1; default 1 */

    float last_pos[3];

    /* Stroke path (target centres) — rebuilt when antialiasing / partial
     * opacity is on so soft MODE_PAINT does not harden on overlapping stamps. */
    float (*path)[3];
    int path_count;
    int path_cap;

    struct {
        float    pos[3];
        bool     pressed;
        bool     has_source;
        float    source_pos[3];
        float    source_normal[3];
        float    stroke_normal[3];
        uint64_t volume_key;
        float    radius_x, radius_y, radius_z;
        bool     wall_mode;
        bool     surface_mode;
        bool     static_source;
        bool     take_uppermost;
        int      depth;
        bool     restrict_to_layer;
        float    opacity;
        float    smoothness;
        float    dithering;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
        gesture3d_t ctrl_hover; /* Ctrl held: pick-source footprint at cursor */
        gesture3d_t set_source;
    } gestures;
} tool_clone_stamp_t;

static void ensure_sample_defaults(tool_clone_stamp_t *cs)
{
    if (cs->sample_inited) return;
    cs->wall_mode = false;
    cs->surface_mode = false;
    cs->static_source = false;
    cs->take_uppermost = true;
    cs->depth = 0;
    cs->restrict_to_layer = false;
    cs->opacity = 1.f;
    vec3_set(cs->source_normal, 0, 0, 1);
    vec3_set(cs->stroke_normal, 0, 0, 1);
    cs->sample_inited = true;
}

/* Soft MODE_PAINT needs a full path rebuild — overlapping stamps harden. */
static bool needs_stroke_rebuild(const tool_clone_stamp_t *cs)
{
    return goxel.painter.smoothness > 0.f || cs->opacity < 1.f;
}

/* Match extrude/brush: nearest cube face for an axis-aligned normal. */
static int clone_get_face(const float n[3])
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

/*
 * Wall mode: source_face from Ctrl+Click normal, target_face from paint
 * face (drag-locked stroke_normal, or live hover normal).  Remaps UV so a
 * forward-facing source previews correctly on a left-facing wall.
 */
static clone_stamp_sample_t sample_opts(const tool_clone_stamp_t *cs,
                                        const float *target_n)
{
    int src_face = -1, tgt_face = -1;
    if (cs->wall_mode) {
        src_face = clone_get_face(cs->source_normal);
        if (target_n)
            tgt_face = clone_get_face(target_n);
        else
            tgt_face = clone_get_face(cs->stroke_normal);
    }
    return (clone_stamp_sample_t){
        /* Wall mode: depth only — no infinite inherit along the normal. */
        .take_uppermost = cs->wall_mode ? false : cs->take_uppermost,
        .surface_mode = cs->surface_mode,
        .depth = cs->depth,
        .source_face = src_face,
        .target_face = tgt_face,
    };
}

/* Volume used for clone colour inheritance / source markers. */
static const volume_t *clone_sample_volume(const tool_clone_stamp_t *cs,
                                          const volume_t *layer_volume)
{
    if (cs->restrict_to_layer)
        return layer_volume;
    return goxel_get_layers_volume(goxel.image);
}

static bool check_can_skip(tool_clone_stamp_t *cs, const cursor_t *curs)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (    pressed == cs->last_op.pressed &&
            cs->has_source == cs->last_op.has_source &&
            cs->last_op.volume_key == volume_get_key(volume) &&
            cs->last_op.radius_x == goxel.radius_x &&
            cs->last_op.radius_y == goxel.radius_y &&
            cs->last_op.radius_z == goxel.radius_z &&
            cs->last_op.wall_mode == cs->wall_mode &&
            cs->last_op.surface_mode == cs->surface_mode &&
            cs->last_op.static_source == cs->static_source &&
            cs->last_op.take_uppermost == cs->take_uppermost &&
            cs->last_op.depth == cs->depth &&
            cs->last_op.restrict_to_layer == cs->restrict_to_layer &&
            cs->last_op.opacity == cs->opacity &&
            cs->last_op.smoothness == goxel.painter.smoothness &&
            cs->last_op.dithering == goxel.painter.dithering &&
            vec3_equal(curs->pos, cs->last_op.pos) &&
            (!cs->has_source || vec3_equal(cs->source_pos, cs->last_op.source_pos)) &&
            (!cs->wall_mode ||
             (vec3_equal(cs->source_normal, cs->last_op.source_normal) &&
              /* Drag locks paint face at begin; hover tracks live normal. */
              (pressed ||
               vec3_equal(curs->normal, cs->last_op.stroke_normal))))) {
        return true;
    }
    cs->last_op.pressed = pressed;
    cs->last_op.has_source = cs->has_source;
    cs->last_op.volume_key = volume_get_key(volume);
    cs->last_op.radius_x = goxel.radius_x;
    cs->last_op.radius_y = goxel.radius_y;
    cs->last_op.radius_z = goxel.radius_z;
    cs->last_op.wall_mode = cs->wall_mode;
    cs->last_op.surface_mode = cs->surface_mode;
    cs->last_op.static_source = cs->static_source;
    cs->last_op.take_uppermost = cs->take_uppermost;
    cs->last_op.depth = cs->depth;
    cs->last_op.restrict_to_layer = cs->restrict_to_layer;
    cs->last_op.opacity = cs->opacity;
    cs->last_op.smoothness = goxel.painter.smoothness;
    cs->last_op.dithering = goxel.painter.dithering;
    vec3_copy(curs->pos, cs->last_op.pos);
    if (cs->has_source)
        vec3_copy(cs->source_pos, cs->last_op.source_pos);
    vec3_copy(cs->source_normal, cs->last_op.source_normal);
    if (pressed)
        vec3_copy(cs->stroke_normal, cs->last_op.stroke_normal);
    else
        vec3_copy(curs->normal, cs->last_op.stroke_normal);
    return false;
}

/*
 * Brush box at p0.  Wall mode: Diameter Z along face normal; Diameter X/Y
 * use a stable UV frame (V = world-up on vertical walls) so remapping
 * matches left and right faces — raw FACES_MATS swaps U/V on -X.
 */
static void get_box3(const float p0[3], const float n[3],
                     float r_x, float r_y, float r_z,
                     bool wall_mode, float out[4][4])
{
    float box[4][4];
    int face;
    float u[3], v[3], nn[3];

    if (wall_mode && n && (face = clone_get_face(n)) >= 0) {
        const int *fn = FACES_NORMALS[face];
        int n_axis = fn[0] ? 0 : (fn[1] ? 1 : 2);

        vec3_set(nn, (float)fn[0], (float)fn[1], (float)fn[2]);
        if (n_axis != 2) {
            vec3_set(v, 0, 0, 1);
            vec3_set(u, (float)(-fn[1]), (float)fn[0], 0);
        } else {
            vec3_set(u, 0, 1, 0);
            vec3_set(v, (float)(-fn[2]), 0, 0);
        }

        mat4_set_identity(box);
        vec3_mul(nn, r_z, box[0]);
        vec3_mul(u, r_x, box[1]);
        vec3_mul(v, r_y, box[2]);
        vec3_copy(p0, box[3]);
        if (goxel.brush_origin_at_base)
            vec3_addk(box[3], nn, r_z - 0.5f, box[3]);
        mat4_copy(box, out);
        return;
    }

    bbox_from_extents(box, p0, r_x, r_y, r_z);
    box_swap_axis(box, 2, 0, 1, box);
    if (goxel.brush_origin_at_base)
        box[3][2] += r_z - 0.5f;
    mat4_copy(box, out);
}

static const shape_t *clone_shape(void)
{
    return goxel.painter.shape ? goxel.painter.shape : &shape_sphere;
}

/*
 * Paint stamp at target.  `paint_n` — wall-mode paint face (NULL → stroke_normal).
 * Brush Diameter Z follows the paint face; inherit/UV source uses source_normal.
 */
static void apply_at(tool_clone_stamp_t *cs, volume_t *dest,
                     const volume_t *sample,
                     const float target[3], const float source[3],
                     const float *paint_n)
{
    float box[4][4];
    const float *n = NULL;
    clone_stamp_sample_t opts;

    if (cs->wall_mode)
        n = paint_n ? paint_n : cs->stroke_normal;
    opts = sample_opts(cs, n);

    get_box3(target, n, goxel.radius_x, goxel.radius_y,
             cs->surface_mode ? 0.5f : goxel.radius_z,
             cs->wall_mode, box);
    clone_stamp_apply(dest, sample, target, source, box, clone_shape(),
                      goxel.painter.smoothness, goxel.painter.dithering,
                      cs->opacity, &opts);
}

/* Build / clear the on-top source-block highlight volume.
 * Markers always use the source face (pick normal or locked source_normal). */
static void set_source_markers(tool_clone_stamp_t *cs, const volume_t *sample,
                               const float source[3], const float *orient_n,
                               const uint8_t marker[4])
{
    float box[4][4];
    const float *n = orient_n ? orient_n
                              : (cs->wall_mode ? cs->source_normal : NULL);
    /* Preview markers: source and target face are the same (source space). */
    clone_stamp_sample_t opts = {
        .take_uppermost = cs->wall_mode ? false : cs->take_uppermost,
        .surface_mode = cs->surface_mode,
        .depth = cs->depth,
        .source_face = cs->wall_mode && n ? clone_get_face(n) : -1,
        .target_face = cs->wall_mode && n ? clone_get_face(n) : -1,
    };

    if (!cs->source_markers) cs->source_markers = volume_new();
    volume_clear(cs->source_markers);
    if (!sample) return;
    get_box3(source, n, goxel.radius_x, goxel.radius_y,
             cs->surface_mode ? 0.5f : goxel.radius_z,
             cs->wall_mode, box);
    clone_stamp_preview_source(cs->source_markers, sample, source, box,
                               clone_shape(), 0.f, &opts, marker);
}

static void clear_source_markers(tool_clone_stamp_t *cs)
{
    if (cs->source_markers)
        volume_clear(cs->source_markers);
}

static void render_source_markers(tool_clone_stamp_t *cs)
{
    /* NO_DEPTH_TEST: markers replace the active-layer tool_volume slot and
     * would otherwise be covered by other layers at the same voxel. */
    if (cs->source_markers && !volume_is_empty(cs->source_markers))
        render_volume(&goxel.rend, cs->source_markers, NULL,
                      EFFECT_NO_DEPTH_TEST);
}

static void path_clear(tool_clone_stamp_t *cs)
{
    cs->path_count = 0;
}

static void path_push(tool_clone_stamp_t *cs, const float pos[3])
{
    if (cs->path_count >= cs->path_cap) {
        int ncap = cs->path_cap ? cs->path_cap * 2 : 64;
        cs->path = realloc(cs->path, (size_t)ncap * sizeof(*cs->path));
        cs->path_cap = ncap;
    }
    vec3_copy(pos, cs->path[cs->path_count]);
    cs->path_count++;
}

static void rebuild_stroke_from_path(tool_clone_stamp_t *cs)
{
    int i;
    float src[3];
    const volume_t *sample = clone_sample_volume(cs, cs->volume_orig);

    volume_set(cs->stroke, cs->volume_orig);
    for (i = 0; i < cs->path_count; i++) {
        if (cs->static_source)
            vec3_copy(cs->source_pos, src);
        else
            vec3_add(cs->path[i], cs->offset, src);
        apply_at(cs, cs->stroke, sample, cs->path[i], src, NULL);
    }
}

static void refresh_tool_preview(tool_clone_stamp_t *cs)
{
    const volume_t *sample;

    if (!cs->stroke) return;
    sample = clone_sample_volume(cs, cs->volume_orig);
    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, cs->stroke);
    if (cs->has_source)
        set_source_markers(cs, sample, cs->source_pos, NULL, k_source_marker);
    else
        clear_source_markers(cs);
    cs->last_op.volume_key = volume_get_key(goxel.tool_volume);
}

/* Layer view + exact sample blocks at `at` with the given marker colour.
 * `orient_n` — wall-mode face while picking (live cursor). */
static void show_exact_source_preview(tool_clone_stamp_t *cs,
                                      const float at[3],
                                      const float *orient_n,
                                      const uint8_t marker[4])
{
    volume_t *layer = goxel.image->active_layer->volume;
    const volume_t *sample = clone_sample_volume(cs, layer);

    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, layer);
    set_source_markers(cs, sample, at, orient_n, marker);
    cs->last_op.volume_key = volume_get_key(goxel.tool_volume);
    cs->last_op.has_source = cs->has_source;
    cs->last_op.restrict_to_layer = cs->restrict_to_layer;
    cs->last_op.wall_mode = cs->wall_mode;
    cs->last_op.surface_mode = cs->surface_mode;
    vec3_copy(at, cs->last_op.pos);
    if (cs->has_source)
        vec3_copy(cs->source_pos, cs->last_op.source_pos);
    vec3_copy(cs->source_normal, cs->last_op.source_normal);
    if (orient_n)
        vec3_copy(orient_n, cs->last_op.stroke_normal);
}

static void update_source_from_target(tool_clone_stamp_t *cs,
                                      const float target[3])
{
    if (cs->static_source || !cs->offset_locked) return;
    vec3_add(target, cs->offset, cs->source_pos);
}

static int on_set_source(gesture3d_t *gest, void *user)
{
    tool_clone_stamp_t *cs = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;

    if (!curs->snaped) return 0;
    if (gest->state == GESTURE_BEGIN || gest->state == GESTURE_UPDATE ||
        gest->state == GESTURE_END) {
        if (gest->state != GESTURE_END) {
            vec3_copy(curs->pos, cs->source_pos);
            vec3_copy(curs->normal, cs->source_normal);
            cs->has_source = true;
            cs->offset_locked = false;
            goxel_set_help_text(
                "Clone source set — click and drag to paint from this location");
        }
        /* Confirmed source: yellow exact-block highlight. */
        if (cs->has_source)
            show_exact_source_preview(cs, cs->source_pos, cs->source_normal,
                                      k_source_marker);
    }
    return 0;
}

/* Ctrl held: purple exact-block pick preview at the cursor only. */
static int on_ctrl_hover(gesture3d_t *gest, void *user)
{
    tool_clone_stamp_t *cs = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;

    if (gest->state == GESTURE_END || !curs->snaped)
        return 0;

    if (gest->state != GESTURE_BEGIN && goxel.tool_volume &&
        check_can_skip(cs, curs))
        return 0;

    show_exact_source_preview(cs, curs->pos, curs->normal, k_pick_marker);
    return 0;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_clone_stamp_t *cs = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;
    float r_x = goxel.radius_x;
    float r_y = goxel.radius_y;
    float r_z = goxel.radius_z;
    float spacing, pos[3], target[3];
    int nb, i;

    if (!cs->has_source) {
        if (gest->state == GESTURE_BEGIN) {
            gui_alert("Clone Stamp",
                      "Ctrl+Click to set the clone source, then click and "
                      "drag to paint from that location.");
        }
        return GESTURE_FAILED;
    }

    vec3_copy(curs->pos, target);

    if (gest->state == GESTURE_BEGIN) {
        if (!cs->static_source)
            vec3_sub(cs->source_pos, target, cs->offset);
        cs->offset_locked = true;
        vec3_copy(target, cs->last_pos);
        /* Lock paint-face orientation for the whole stroke. */
        if (cs->wall_mode)
            vec3_copy(curs->normal, cs->stroke_normal);

        if (!cs->volume_orig)
            cs->volume_orig = volume_new();
        volume_set(cs->volume_orig, goxel.image->active_layer->volume);
        image_history_push(goxel.image);

        if (!cs->stroke) cs->stroke = volume_new();
        volume_set(cs->stroke, cs->volume_orig);
        path_clear(cs);
        path_push(cs, target);
        apply_at(cs, cs->stroke, clone_sample_volume(cs, cs->volume_orig),
                 target, cs->source_pos, NULL);
        refresh_tool_preview(cs);
        return 0;
    }

    if (gest->state == GESTURE_UPDATE && check_can_skip(cs, curs))
        return 0;

    if (!cs->stroke) {
        cs->stroke = volume_new();
        volume_set(cs->stroke, cs->volume_orig);
    }

    update_source_from_target(cs, target);

    spacing = cs->surface_mode ? max(0.7f, min(r_x, r_y))
                               : max(0.7f, min3(r_x, r_y, r_z));
    nb = ceil(vec3_dist(curs->pos, cs->last_pos) / spacing);
    nb = max(nb, 1);
    {
        const volume_t *sample = clone_sample_volume(cs, cs->volume_orig);
        for (i = 0; i < nb; i++) {
            float src[3];
            vec3_mix(cs->last_pos, curs->pos, (i + 1.0f) / nb, pos);
            path_push(cs, pos);
            if (!needs_stroke_rebuild(cs)) {
                if (cs->static_source)
                    vec3_copy(cs->source_pos, src);
                else {
                    vec3_add(pos, cs->offset, src);
                    vec3_copy(src, cs->source_pos);
                }
                apply_at(cs, cs->stroke, sample, pos, src, NULL);
            }
        }
    }

    if (needs_stroke_rebuild(cs))
        rebuild_stroke_from_path(cs);

    vec3_copy(target, cs->last_pos);
    refresh_tool_preview(cs);

    if (gest->state == GESTURE_END) {
        volume_set(goxel.image->active_layer->volume, cs->stroke);
        volume_set(cs->volume_orig, cs->stroke);
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        cs->offset_locked = false;
        path_clear(cs);
        /* Keep the yellow marker at the current source location. */
        if (cs->has_source)
            set_source_markers(cs, clone_sample_volume(cs, cs->volume_orig),
                               cs->source_pos, NULL, k_source_marker);
    }
    return 0;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    tool_clone_stamp_t *cs = USER_GET(user, 0);
    cursor_t *curs = gest->cursor;
    volume_t *layer = goxel.image->active_layer->volume;

    if (gest->state == GESTURE_END || !curs->snaped) {
        /* Ctrl-hover owns the preview while Ctrl is held — do not clear it. */
        if (!(curs->flags & CURSOR_PRESSED) && !(curs->flags & CURSOR_CTRL)) {
            volume_delete(goxel.tool_volume);
            goxel.tool_volume = NULL;
            if (cs->has_source)
                set_source_markers(cs, clone_sample_volume(cs, layer),
                                   cs->source_pos, NULL, k_source_marker);
            else
                clear_source_markers(cs);
        }
        return 0;
    }

    /* No source yet: previews are Ctrl-only (purple pick). */
    if (!cs->has_source) {
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        clear_source_markers(cs);
        return 0;
    }

    /* Force rebuild on BEGIN (e.g. after releasing Ctrl). */
    if (gest->state != GESTURE_BEGIN && goxel.tool_volume &&
        check_can_skip(cs, curs))
        return 0;

    if (!goxel.tool_volume) goxel.tool_volume = volume_new();
    volume_set(goxel.tool_volume, layer);
    {
        const volume_t *sample = clone_sample_volume(cs, layer);
        /* Hover: remap source face → live paint face under the cursor. */
        apply_at(cs, goxel.tool_volume, sample, curs->pos, cs->source_pos,
                 curs->normal);
        set_source_markers(cs, sample, cs->source_pos, NULL, k_source_marker);
    }

    cs->last_op.volume_key = volume_get_key(goxel.tool_volume);
    return 0;
}

static int iter(tool_t *tool, const painter_t *painter,
                const float viewport[4])
{
    tool_clone_stamp_t *cs = (tool_clone_stamp_t *)tool;
    cursor_t *curs = &goxel.cursor;

    (void)painter;
    (void)viewport;

    ensure_sample_defaults(cs);

    if (!cs->gestures.drag.type) {
        cs->gestures.drag = (gesture3d_t){
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
        cs->gestures.hover = (gesture3d_t){
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
        cs->gestures.ctrl_hover = (gesture3d_t){
            .type = GESTURE_HOVER,
            .buttons = CURSOR_CTRL,
            .callback = on_ctrl_hover,
        };
        cs->gestures.set_source = (gesture3d_t){
            .type = GESTURE_DRAG,
            .buttons = CURSOR_CTRL,
            .callback = on_set_source,
        };
    }

    if (cs->has_source) {
        goxel_set_help_text(
            cs->wall_mode
                ? "Wall Clone — click and drag to stamp; Ctrl+Click to move "
                  "the source"
                : "Clone Stamp — click and drag to paint; Ctrl+Click to move "
                  "the source");
    } else {
        goxel_set_help_text(
            cs->wall_mode
                ? "Wall Clone — Ctrl+Click a wall face to set the source, "
                  "then click and drag to stamp"
                : "Clone Stamp — Ctrl+Click to set the clone source, then "
                  "click and drag to paint");
    }

    curs->snap_mask |= SNAP_ROUNDED;
    curs->snap_offset = goxel.snap_offset * goxel.radius_x - 0.5f;

    /* Ctrl pick-source before normal hover so Ctrl does not wipe the preview. */
    gesture3d(&cs->gestures.set_source, curs, USER_PASS(cs, painter));
    gesture3d(&cs->gestures.ctrl_hover, curs, USER_PASS(cs, painter));
    gesture3d(&cs->gestures.drag, curs, USER_PASS(cs, painter));
    gesture3d(&cs->gestures.hover, curs, USER_PASS(cs, painter));

    render_source_markers(cs);

    return tool->state;
}

static int gui(tool_t *tool)
{
    tool_clone_stamp_t *cs = (tool_clone_stamp_t *)tool;

    ensure_sample_defaults(cs);

    if (cs->surface_mode)
        tool_gui_radius_xy();
    else
        tool_gui_radius();
    if (gui_checkbox("Surface mode", &cs->surface_mode,
                     "Ignore Diameter Z; clone onto the air-exposed surface "
                     "down each column under the X/Y shape") &&
        cs->surface_mode) {
        cs->wall_mode = false;
    }
    gui_enabled_begin(!cs->surface_mode);
    gui_checkbox("Origin at base", &goxel.brush_origin_at_base,
                 "Lowest Z of the shape is at the cursor (Z-up), not the center");
    gui_checkbox("Wall mode", &cs->wall_mode,
                 "Inherit along the source face normal; orient the brush to "
                 "the paint face.");
    gui_enabled_end();
    tool_gui_smoothness();

    if (gui_section_begin("Clone source", true)) {

        if (cs->has_source) {
            gui_text("Source set at %.0f, %.0f, %.0f",
                     floor(cs->source_pos[0]),
                     floor(cs->source_pos[1]),
                     floor(cs->source_pos[2]));
            if (gui_button("Clear source", -1, 0)) {
                cs->has_source = false;
                cs->offset_locked = false;
                clear_source_markers(cs);
            }
        } else {
            gui_text("Ctrl+Click on the map to set\nthe clone source.");
        }
        gui_separator();
        gui_text("Choose how colors are chosen\nrelative to the clone source.");
        gui_checkbox("Restrict to layer", &cs->restrict_to_layer,
                     "Only sample clone colours from the current layer. "
                     "When disabled, use all visible layers.");
        gui_checkbox("Static source", &cs->static_source,
                     "Keep the clone source fixed while dragging. "
                     "When disabled (default), the source moves with the brush.");
        if (!cs->wall_mode) {
            gui_checkbox("Inherit infinitely", &cs->take_uppermost,
                         "Always take the uppermost block in each column "
                         "(full map height). As the source moves onto taller "
                         "terrain, those higher blocks are used.");
        }
        if (cs->wall_mode || !cs->take_uppermost) {
            if (gui_input_int("Depth", &cs->depth, 0, 128))
                cs->depth = clamp(cs->depth, 0, 128);
            gui_tooltip_if_hovered(
                cs->wall_mode
                    ? "Distance along the source face normal to search for "
                      "a colour (into / out of the wall)"
                    : "Distance from clone source location vertically we will "
                      "copy/inherit color from; useful if under a roof/other "
                      "structure");
        }
        {
            int opacity_pct = (int)(cs->opacity * 100.f + 0.5f);
            if (gui_input_int("Opacity %", &opacity_pct, 0, 100)) {
                opacity_pct = clamp(opacity_pct, 0, 100);
                cs->opacity = opacity_pct / 100.f;
            }
            gui_tooltip_if_hovered(
                "How strongly cloned colours paint onto the destination "
                "(100% = full replace, lower blends with existing colour)");
        }
    }
    gui_section_end();

    return 0;
}

TOOL_REGISTER(TOOL_CLONE_STAMP, clone_stamp, tool_clone_stamp_t,
              .name = "Clone Stamp",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT,
              .has_snap = true,
              .has_shape = true,
)
