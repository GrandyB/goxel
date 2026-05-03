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
#include "utils/volume_preview.h"
#include "utlist.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#   define past_strcasecmp _stricmp
#else
#   define past_strcasecmp strcasecmp
#endif

#ifndef PLACER_PAST_PREVIEW_SIZE
#   define PLACER_PAST_PREVIEW_SIZE 128
#endif
#define BOOL_STR(b) ((b) ? "true" : "false")

/* 0=S, 1=M, 2=L thumbnails; 3=Details (one row per entry: name + x). */
enum { PLACER_HIST_VIEW_S = 0, PLACER_HIST_VIEW_M = 1, PLACER_HIST_VIEW_L = 2,
       PLACER_HIST_VIEW_DETAILS = 3 };
static int placer_history_preview_preset = PLACER_HIST_VIEW_S;

enum { PLACER_HIST_SORT_TIME = 0, PLACER_HIST_SORT_NAME = 1 };
static int placer_history_sort_key = PLACER_HIST_SORT_TIME;
static bool placer_history_sort_asc = false;

/* Post-placement Z rotation randomisation (euler Z in degrees, stepped). */
enum {
    PLACER_RAND_Z_NONE = 0,
    PLACER_RAND_Z_90,
    PLACER_RAND_Z_45,
    PLACER_RAND_Z_225,
};

enum {
    PLACER_RAND_FLIP_NONE = 0,
    PLACER_RAND_FLIP_XY,
};

enum {
    PLACER_CR_NONE = 0,
    PLACER_CR_RANDOM,
    PLACER_CR_SPECIFIC,
};

static file_format_t *ff_import_current = NULL;
static file_format_t *ff_export_current = NULL;

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
        uint32_t   cr_stamp_seq;
    } last_op;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;

    int random_z_mode;
    int random_flip_xy_mode;

    int color_replace_mode;
    uint8_t color_replace_source[4];
    uint8_t color_replace_specific[4];
    float color_replace_saturation;
    float color_replace_brightness;
    float color_replace_rand_h;
    uint32_t color_replace_stamp_seq;
    bool color_replace_defaults_done;

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

static const char *placer_rand_z_label(int mode)
{
    switch (mode) {
    case PLACER_RAND_Z_90:  return "Rotate only 90 degrees";
    case PLACER_RAND_Z_45:  return "Rotate 45 degrees";
    case PLACER_RAND_Z_225: return "Rotate 22.5 degrees";
    case PLACER_RAND_Z_NONE:
    default:                return "None";
    }
}

static const char *placer_rand_flip_xy_label(int mode)
{
    return mode == PLACER_RAND_FLIP_XY ? "Flip X/Y" : "None";
}

static const char *placer_color_replace_mode_label(int mode)
{
    switch (mode) {
    case PLACER_CR_RANDOM:  return "Replace with random";
    case PLACER_CR_SPECIFIC: return "Replace with specific";
    case PLACER_CR_NONE:
    default:                return "None";
    }
}

static void placer_randomize_z_rotation(tool_placer_t *placer)
{
    float degs[3];
    float delta[4][4] = MAT4_IDENTITY;
    float step_deg, rz_new, diff;
    int k_max, k;

    if (placer->random_z_mode == PLACER_RAND_Z_NONE)
        return;
    if (!placer->imported_volume_orig)
        return;

    switch (placer->random_z_mode) {
    case PLACER_RAND_Z_90:  step_deg = 90.f;   break;
    case PLACER_RAND_Z_45:  step_deg = 45.f;   break;
    case PLACER_RAND_Z_225: step_deg = 22.5f;  break;
    default: return;
    }

    k_max = (int)(180.f / step_deg + 0.5f);
    k = random_int(-k_max, k_max);
    rz_new = k * step_deg;

    mat4_to_eul_degxyz(placer->rot, degs);
    diff = rz_new - degs[2];
    while (diff > 180.f) diff -= 360.f;
    while (diff < -180.f) diff += 360.f;

    mat4_irotate(delta, diff * (float)(M_PI / 180.0), 0, 0, 1);
    apply_rotation(placer, delta);
}

static void placer_randomize_flip_xy(tool_placer_t *placer)
{
    float delta[4][4] = MAT4_IDENTITY;
    int r;

    if (placer->random_flip_xy_mode != PLACER_RAND_FLIP_XY)
        return;
    if (!placer->imported_volume_orig)
        return;

    r = random_int(0, 3);
    switch (r) {
    case 0:
        return;
    case 1:
        mat4_iscale(delta, -1, 1, 1);
        break;
    case 2:
        mat4_iscale(delta, 1, -1, 1);
        break;
    case 3:
        mat4_iscale(delta, -1, -1, 1);
        break;
    default:
        return;
    }
    apply_rotation(placer, delta);
}

static bool check_can_skip(tool_placer_t *placer, const cursor_t *curs)
{
    volume_t *volume = goxel.tool_volume;
    const bool pressed = curs->flags & CURSOR_PRESSED;
    if (    pressed == placer->last_op.pressed &&
            placer->last_op.volume_key == volume_get_key(volume) &&
            placer->last_op.cr_stamp_seq == placer->color_replace_stamp_seq &&
            vec3_equal(curs->pos, placer->last_op.pos)) {
        return true;
    }
    placer->last_op.pressed = pressed;
    placer->last_op.volume_key = volume_get_key(volume);
    vec3_copy(curs->pos, placer->last_op.pos);
    return false;
}

static void placer_color_replace_apply_defaults(tool_placer_t *placer)
{
    if (placer->color_replace_defaults_done)
        return;
    placer->color_replace_defaults_done = true;
    placer->color_replace_source[0] = 0;
    placer->color_replace_source[1] = 0;
    placer->color_replace_source[2] = 0;
    placer->color_replace_source[3] = 255;
    placer->color_replace_specific[0] = 255;
    placer->color_replace_specific[1] = 255;
    placer->color_replace_specific[2] = 255;
    placer->color_replace_specific[3] = 255;
    placer->color_replace_saturation = 0.5f;
    placer->color_replace_brightness = 0.5f;
    placer->color_replace_rand_h = 0.f;
}

static void placer_color_replace_reseed(tool_placer_t *placer)
{
    if (placer->color_replace_mode != PLACER_CR_RANDOM)
        return;
    placer->color_replace_rand_h =
            (random_int(0, 16777215) + 0.5f) / 16777216.f;
    placer->color_replace_stamp_seq++;
}

static void hsv_to_rgb_u8(float h, float s, float v, uint8_t rgb[3])
{
    float r, g, b;
    int i;
    float f, p, q, t;

    h = fmodf(h, 1.f);
    if (h < 0.f)
        h += 1.f;
    s = clamp(s, 0.f, 1.f);
    v = clamp(v, 0.f, 1.f);
    i = (int)(h * 6.f);
    f = h * 6.f - (float)i;
    p = v * (1.f - s);
    q = v * (1.f - f * s);
    t = v * (1.f - (1.f - f) * s);
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (uint8_t)clamp(r * 255.f, 0.f, 255.f);
    rgb[1] = (uint8_t)clamp(g * 255.f, 0.f, 255.f);
    rgb[2] = (uint8_t)clamp(b * 255.f, 0.f, 255.f);
}

static void placer_stamp_apply_color_replace(tool_placer_t *placer,
                                             volume_t *stamp)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t vox[4];
    uint8_t repl_rgb[3];

    if (placer->color_replace_mode == PLACER_CR_NONE)
        return;

    if (placer->color_replace_mode == PLACER_CR_SPECIFIC) {
        repl_rgb[0] = placer->color_replace_specific[0];
        repl_rgb[1] = placer->color_replace_specific[1];
        repl_rgb[2] = placer->color_replace_specific[2];
    } else {
        hsv_to_rgb_u8(placer->color_replace_rand_h,
                        placer->color_replace_saturation,
                        placer->color_replace_brightness, repl_rgb);
    }

    iter = volume_get_iterator(stamp, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(stamp, &iter, pos, vox);
        if (memcmp(vox, placer->color_replace_source, 4) != 0)
            continue;
        vox[0] = repl_rgb[0];
        vox[1] = repl_rgb[1];
        vox[2] = repl_rgb[2];
        volume_set_at(stamp, &iter, pos, vox);
    }
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

    //LOG_D("center_origin:");
    //LOG_D("    Cursor: %f / %f / %f", goxel.cursor.pos[0], goxel.cursor.pos[1], goxel.cursor.pos[2]);
    //LOG_D("    BBox: (%i,%i,%i) (%i,%i,%i)", bbox[0][0], bbox[0][1], bbox[0][2], bbox[1][0], bbox[1][1], bbox[1][2]);
    //debug_log_vec3_float("    found center:", pos);
    vec3_copy(pos, placer->center);
    vec3_set(placer->origin, 0, 0, 0);
}

static void post_import(tool_placer_t *placer)
{
    placer->imported_volume_orig = volume_copy(placer->imported_volume);
    center_origin(placer);
}

static int on_drag(gesture3d_t *gest, void *user)
{
    if (gest->state == GESTURE_END) {
        tool_placer_t *placer = USER_GET(user, 0);
        const bool did_place = goxel.image->active_layer->volume && goxel.tool_volume;

        image_history_push(goxel.image);
        if (did_place) {
            volume_set(goxel.image->active_layer->volume, goxel.tool_volume);
        }
        volume_delete(goxel.tool_volume);
        goxel.tool_volume = NULL;
        if (did_place && placer->imported_volume) {
            placer_randomize_z_rotation(placer);
            placer_randomize_flip_xy(placer);
            if (placer->color_replace_mode == PLACER_CR_RANDOM)
                placer_color_replace_reseed(placer);
        }
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
        volume_t *stamp;

        move_to(placer, curs->pos);

        if (placer->color_replace_mode != PLACER_CR_NONE) {
            stamp = volume_copy(placer->imported_volume);
            placer_stamp_apply_color_replace(placer, stamp);
            volume_merge(volume, stamp, MODE_OVER, NULL);
            volume_delete(stamp);
        } else {
            volume_merge(volume, placer->imported_volume, MODE_OVER, NULL);
        }
        if (!goxel.tool_volume) goxel.tool_volume = volume_new();
        volume_set(goxel.tool_volume, volume);
        placer->last_op.cr_stamp_seq = placer->color_replace_stamp_seq;
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

    placer_color_replace_apply_defaults(placer);
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

static const char *make_label(const file_format_t *f, char *buf, int len, char *id)
{
    const char *ext = f->exts[0] + 1;
    snprintf(buf, len, "%s (%s)##%s", f->name, ext, id);
    return buf;
}

static void on_import_format(void *user, file_format_t *f)
{
    char label[128];
    make_label(f, label, sizeof(label), "import");
    if (gui_combo_item(label, f == ff_import_current)) {
        ff_import_current = f;
    }
}

static void on_export_format(void *user, file_format_t *f)
{
    char label[128];
    make_label(f, label, sizeof(label), "export");
    if (gui_combo_item(label, f == ff_export_current)) {
        ff_export_current = f;
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
    texture_t *preview;
    /* true: in-memory preview attempted (import) or disk lazy load finished */
    bool preview_ready;
    int64_t imported_at; /* unix seconds when added (not shown in tile UI) */
};
past_import_t *past_files = NULL;

void placer_past_files_clear(void) {
    past_import_t *f, *tmp;
    DL_FOREACH_SAFE(past_files, f, tmp) {
        free((void *)f->path);
        free((void *)f->file_name);
        texture_delete(f->preview);
        DL_DELETE(past_files, f);
        free(f);
    }
    past_files = NULL;
}

char *placer_past_files_serialize_gox(size_t *out_len) {
    const past_import_t *i;
    size_t tot, hlen, rem;
    char *buf, *w;

    static const char hdr[] = "goxel-placer-history 2\n";
    hlen = sizeof(hdr) - 1;
    tot = hlen;
    DL_FOREACH_REVERSE(past_files, i) {
        tot += strlen(i->format->name) + 1 + strlen(i->path) + 1 + 24 + 1;
    }
    buf = malloc(tot + 1);
    if (!buf) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    w = buf;
    memcpy(w, hdr, hlen);
    w += hlen;
    rem = tot + 1 - hlen;
    DL_FOREACH_REVERSE(past_files, i) {
        int n;
        n = snprintf(w, rem, "%s\t%s\t%lld\n", i->format->name, i->path,
                (long long)i->imported_at);
        if (n < 0 || (size_t)n >= rem) {
            free(buf);
            if (out_len) *out_len = 0;
            return NULL;
        }
        w += n;
        rem -= (size_t)n;
    }
    *w = '\0';
    if (out_len) *out_len = (size_t)(w - buf);
    return buf;
}

void placer_past_files_load_gox(const char *data, size_t len) {
    char *copy, *p, *line, *tofree, *tab;
    const file_format_t *ff;
    int plac_ver;

    if (!data || !len) return;
    placer_past_files_clear();
    tofree = copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, data, len);
    copy[len] = '\0';
    p = copy;
    line = strsep(&p, "\n");
    if (!line) {
        free(tofree);
        return;
    }
    if (strncmp(line, "goxel-placer-history 2", 22) == 0)
        plac_ver = 2;
    else if (strncmp(line, "goxel-placer-history 1", 22) == 0)
        plac_ver = 1;
    else {
        free(tofree);
        return;
    }
    while (p && (line = strsep(&p, "\n"))) {
        if (!*line) continue;
        tab = strchr(line, '\t');
        if (!tab) {
            LOG_W("PLAC: line missing tab, skip");
            continue;
        }
        *tab = '\0';
        ff = file_format_by_name(line);
        if (!ff) {
            LOG_W("PLAC: unknown format '%s', skip", line);
            continue;
        }
        {
            char *path_start = tab + 1;
            int64_t imported_at = 0;

            if (plac_ver >= 2) {
                char *tab2 = strchr(path_start, '\t');

                if (tab2) {
                    *tab2 = '\0';
                    imported_at = (int64_t)strtoll(tab2 + 1, NULL, 10);
                }
            }
            {
                const char *path = path_start;
                char *name = get_file_name_from_path(path);
                past_import_t *past, *f, *tmp;

                if (!name) {
                    free(tofree);
                    return;
                }
                past = calloc(1, sizeof(*past));
                *past = (past_import_t) {
                    .path = strdup(path),
                    .file_name = name,
                    .format = ff,
                    .imported_at = imported_at,
                };
                DL_FOREACH_SAFE(past_files, f, tmp) {
                    if (strcmp(f->file_name, name) != 0) continue;
                    free((void *)f->path);
                    free((void *)f->file_name);
                    texture_delete(f->preview);
                    DL_DELETE(past_files, f);
                    free(f);
                }
                DL_APPEND(past_files, past);
            }
        }
    }
    free(tofree);
}

static void placer_past_remove(past_import_t *e) {
    if (!e) return;
    free((void *)e->path);
    free((void *)e->file_name);
    texture_delete(e->preview);
    DL_DELETE(past_files, e);
    free(e);
}

/* Lazily build preview for entries restored from .gox (path only, no in-memory import yet). */
static void placer_ensure_past_preview(past_import_t *i)
{
    volume_t *v;
    int err;

    if (i->preview || i->preview_ready)
        return;
    i->preview_ready = true;
    v = volume_new();
    err = i->format->import_volume_func(i->format, v, i->path);
    if (!err)
        i->preview = volume_preview_to_texture(v, PLACER_PAST_PREVIEW_SIZE);
    volume_delete(v);
}

static void on_file_import(const char *path, const char *file_name, const file_format_t *format) {
    past_import_t *past;
    past = calloc(1, sizeof(*past));
    *past = (past_import_t) {
        .path = path,
        .file_name = strdup(file_name),
        .format = format,
        .imported_at = (int64_t)time(NULL),
    };

    past_import_t *f, *tmp;
    DL_FOREACH_SAFE(past_files, f, tmp) {
        if (strcmp(f->file_name, file_name) != 0) continue;
        LOG_D("Delete %s", f->file_name);
        free((void *)f->path);
        free((void *)f->file_name);
        texture_delete(f->preview);
        DL_DELETE(past_files, f);
        free(f);
    }

    LOG_D("On file import: %s / %s / %s", past->path, past->file_name, past->format->name);
    DL_APPEND(past_files, past);
    past->preview_ready = true;
    if (goxel.tool && goxel.tool->id == TOOL_PLACER) {
        tool_placer_t *pl = (tool_placer_t *)goxel.tool;
        past->preview = volume_preview_to_texture(pl->imported_volume,
                                                  PLACER_PAST_PREVIEW_SIZE);
    }
}

/* paths from sys_open_multi_file_dialog: '|'-separated (tinyfd). */
static void placer_import_selected_paths(tool_placer_t *placer, char *paths_mut)
{
    char *saveptr = NULL;
    char *token;
    bool reset_done = false;

    for (token = strtok_r(paths_mut, "|", &saveptr); token;
         token = strtok_r(NULL, "|", &saveptr)) {
        const file_format_t *f = file_format_for_path(token,
                ff_import_current->name, "r");
        const char *file_name;
        int err;

        if (!f)
            continue;
        if (!reset_done) {
            reset(placer);
            reset_done = true;
        }
        err = f->import_volume_func(f, placer->imported_volume, token);
        if (err)
            continue;
        file_name = get_file_name_from_path(token);
        on_file_import(strdup(token), file_name, f);
        free((void *)file_name);
        post_import(placer);
    }
}

static int past_cmp_for_qsort(const void *a, const void *b)
{
    past_import_t *x = *(past_import_t * const *)a;
    past_import_t *y = *(past_import_t * const *)b;

    if (placer_history_sort_key == PLACER_HIST_SORT_NAME) {
        int c = past_strcasecmp(x->file_name, y->file_name);
        if (c != 0)
            return c;
    }
    if (x->imported_at < y->imported_at)
        return -1;
    if (x->imported_at > y->imported_at)
        return 1;
    if (placer_history_sort_key == PLACER_HIST_SORT_TIME) {
        int c = past_strcasecmp(x->file_name, y->file_name);
        if (c != 0)
            return c;
    }
    return 0;
}

static past_import_t **placer_history_sorted_order(int *out_n)
{
    int n = 0;
    past_import_t *e;
    past_import_t **arr;
    DL_FOREACH(past_files, e) n++;
    *out_n = n;
    if (!n)
        return NULL;
    arr = malloc((size_t)n * sizeof(*arr));
    if (!arr)
        return NULL;
    n = 0;
    DL_FOREACH(past_files, e) arr[n++] = e;
    qsort(arr, (size_t)*out_n, sizeof(*arr), past_cmp_for_qsort);
    return arr;
}

static float placer_history_entry_size(void)
{
    static const float fracs[] = { 0.052f, 0.104f, 0.175f };
    static const float mins[] = { 44.f, 88.f, 128.f };
    static const float maxs[] = { 68.f, 132.f, 240.f };
    int p = placer_history_preview_preset;

    if (p < 0) p = 0;
    if (p > 2) p = 2;
    float s = (float)goxel.screen_size[0] * fracs[p];
    if (s < mins[p]) s = mins[p];
    if (s > maxs[p]) s = maxs[p];
    return s;
}

static float placer_history_label_scale(void)
{
    static const float scales[] = { 0.62f, 1.f, 1.38f };
    int p = placer_history_preview_preset;

    if (p < 0) p = 0;
    if (p > 2) p = 2;
    return scales[p];
}

static void placer_gui_history_preset_bar(void)
{
    float need_sort;
    char tlab[48], nlab[48];

    gui_text("Size:");
    gui_same_line_spaced(8.f);
    if (gui_toolbar_segment("S##pl_hist_s",
                placer_history_preview_preset == PLACER_HIST_VIEW_S))
        placer_history_preview_preset = PLACER_HIST_VIEW_S;
    gui_same_line_spaced(4.f);
    if (gui_toolbar_segment("M##pl_hist_m",
                placer_history_preview_preset == PLACER_HIST_VIEW_M))
        placer_history_preview_preset = PLACER_HIST_VIEW_M;
    gui_same_line_spaced(4.f);
    if (gui_toolbar_segment("L##pl_hist_l",
                placer_history_preview_preset == PLACER_HIST_VIEW_L))
        placer_history_preview_preset = PLACER_HIST_VIEW_L;
    gui_same_line_spaced(4.f);
    if (gui_toolbar_segment("Details##pl_hist_d",
                placer_history_preview_preset == PLACER_HIST_VIEW_DETAILS))
        placer_history_preview_preset = PLACER_HIST_VIEW_DETAILS;

    need_sort = gui_calc_text_width("Sort:") + 8.f
            + gui_toolbar_segment_width("Time v##pl_sort_t") + 4.f
            + gui_toolbar_segment_width("Name v##pl_sort_n") + 4.f
            + 8.f + gui_calc_text_width("Clear history") + 24.f;
    if (gui_content_avail_x() < need_sort)
        gui_new_line();
    else
        gui_same_line_spaced(16.f);

    gui_text("Sort:");
    gui_same_line_spaced(8.f);
    if (placer_history_sort_key == PLACER_HIST_SORT_TIME)
        snprintf(tlab, sizeof(tlab), "Time %s##pl_sort_t",
                placer_history_sort_asc ? "^" : "v");
    else
        snprintf(tlab, sizeof(tlab), "Time##pl_sort_t");
    if (gui_toolbar_segment(tlab, placer_history_sort_key
                == PLACER_HIST_SORT_TIME)) {
        if (placer_history_sort_key == PLACER_HIST_SORT_TIME)
            placer_history_sort_asc = !placer_history_sort_asc;
        else {
            placer_history_sort_key = PLACER_HIST_SORT_TIME;
            placer_history_sort_asc = false;
        }
    }
    gui_same_line_spaced(4.f);
    if (placer_history_sort_key == PLACER_HIST_SORT_NAME)
        snprintf(nlab, sizeof(nlab), "Name %s##pl_sort_n",
                placer_history_sort_asc ? "^" : "v");
    else
        snprintf(nlab, sizeof(nlab), "Name##pl_sort_n");
    if (gui_toolbar_segment(nlab, placer_history_sort_key
                == PLACER_HIST_SORT_NAME)) {
        if (placer_history_sort_key == PLACER_HIST_SORT_NAME)
            placer_history_sort_asc = !placer_history_sort_asc;
        else {
            placer_history_sort_key = PLACER_HIST_SORT_NAME;
            placer_history_sort_asc = true;
        }
    }
    gui_same_line_spaced(8.f);
    if (gui_button("Clear history##pl_hist_clear", 0, 0))
        placer_past_files_clear();
}

static void placer_gui_history_body(tool_placer_t *placer)
{
    past_import_t *i, *remove_me;
    past_import_t **order = NULL;
    int n, k;
    char idbuf[32];
    float min_x, max_x, x, y, row_h, sp_x, sp_y;
    float history_cell;
    float label_scale;

    placer_gui_history_preset_bar();
    gui_separator();

    if (!past_files) {
        gui_text_wrapped("No import history yet. Use Import in the Placer tool.");
        return;
    }

    order = placer_history_sorted_order(&n);
    if (!order) {
        gui_text_wrapped("Could not arrange import history.");
        return;
    }

    remove_me = NULL;

    if (placer_history_preview_preset == PLACER_HIST_VIEW_DETAILS) {
        for (k = 0; k < n; k++) {
            bool do_remove;
            bool do_load;

            i = order[placer_history_sort_asc ? k : (n - 1 - k)];
            snprintf(idbuf, sizeof(idbuf), "%p", (void *)i);
            gui_push_id(idbuf);
            do_remove = false;
            do_load = gui_placer_past_details_row(
                    i->file_name, i->path, &do_remove);
            if (do_load) {
                reset(placer);
                i->format->import_volume_func(i->format, placer->imported_volume,
                                              i->path);
                post_import(placer);
            }
            if (do_remove)
                remove_me = i;
            gui_pop_id();
        }
        free(order);
        if (remove_me) placer_past_remove(remove_me);
        return;
    }

    history_cell = placer_history_entry_size();
    label_scale = placer_history_label_scale();
    min_x = gui_window_content_region_min_x();
    max_x = gui_window_content_region_max_x();
    x = min_x;
    y = gui_get_cursor_pos_y();
    row_h = 0.f;
    sp_x = gui_style_item_spacing_x();
    sp_y = gui_style_item_spacing_y();

    for (k = 0; k < n; k++) {
        bool do_remove, do_load;

        i = order[placer_history_sort_asc ? k : (n - 1 - k)];
        if (x > min_x + 0.5f && x + history_cell > max_x) {
            x = min_x;
            y += row_h + sp_y;
            row_h = 0.f;
        }
        gui_set_cursor_pos(x, y);
        placer_ensure_past_preview(i);
        snprintf(idbuf, sizeof(idbuf), "%p", (void *)i);
        gui_push_id(idbuf);
        do_remove = false;
        if (i->preview) {
            do_load = gui_placer_past_entry(
                    i->preview->tex, i->preview->tex_w, i->preview->tex_h,
                    i->preview->w, i->preview->h, i->file_name, i->path,
                    &do_remove, history_cell, label_scale);
        } else {
            do_load = gui_placer_past_entry(
                    0, 0, 0, 0, 0, i->file_name, i->path, &do_remove,
                    history_cell, label_scale);
        }
        if (do_load) {
            reset(placer);
            i->format->import_volume_func(i->format, placer->imported_volume,
                                          i->path);
            post_import(placer);
        }
        if (do_remove)
            remove_me = i;
        gui_pop_id();
        {
            float ih = gui_get_item_rect_size_y();
            if (ih > row_h) row_h = ih;
        }
        x += history_cell + sp_x;
    }
    if (row_h > 0.f)
        gui_set_cursor_pos(min_x, y + row_h + sp_y);
    free(order);
    if (remove_me) placer_past_remove(remove_me);
}

void placer_gui_history_floating(void)
{
    tool_placer_t *placer;

    if (!goxel.tool || goxel.tool->id != TOOL_PLACER)
        return;
    placer = (tool_placer_t *)goxel.tool;

    gui_floating_panel_begin("Placer history##placer_history_win", 440.f, 440.f);
    placer_gui_history_body(placer);
    gui_floating_panel_end();
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
        LOG_D("Acquired selection from fuzzy mask");
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
    int prev_cr_mode;

    placer_color_replace_apply_defaults(placer);
    float rotation[4][4] = MAT4_IDENTITY;
    float rot_degs[3];
    bool reset_rotation = false;
    int origin_x, origin_y, origin_z, offset_x, offset_y, offset_z;
    bool changed = false;
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

    prev_cr_mode = placer->color_replace_mode;
    if (gui_section_begin("Colour replace", true)) {
        if (gui_combo_begin("##placer_cr_mode",
                    placer_color_replace_mode_label(placer->color_replace_mode))) {
            if (gui_combo_item("None",
                        placer->color_replace_mode == PLACER_CR_NONE))
                placer->color_replace_mode = PLACER_CR_NONE;
            if (gui_combo_item("Replace with random",
                        placer->color_replace_mode == PLACER_CR_RANDOM))
                placer->color_replace_mode = PLACER_CR_RANDOM;
            if (gui_combo_item("Replace with specific",
                        placer->color_replace_mode == PLACER_CR_SPECIFIC))
                placer->color_replace_mode = PLACER_CR_SPECIFIC;
            gui_combo_end();
        }
        if (placer->color_replace_mode != prev_cr_mode) {
            if (placer->color_replace_mode == PLACER_CR_RANDOM)
                placer_color_replace_reseed(placer);
            else
                placer->color_replace_stamp_seq++;
        }

        if (placer->color_replace_mode == PLACER_CR_RANDOM ||
            placer->color_replace_mode == PLACER_CR_SPECIFIC) {
            if (gui_color_small("Source", placer->color_replace_source))
                placer->color_replace_stamp_seq++;
        }

        if (placer->color_replace_mode == PLACER_CR_RANDOM) {
            if (slider_float("Saturation", &placer->color_replace_saturation,
                        0.f, 1.f, "%.2f"))
                placer->color_replace_stamp_seq++;
            if (slider_float("Brightness", &placer->color_replace_brightness,
                        0.f, 1.f, "%.2f"))
                placer->color_replace_stamp_seq++;
        }

        if (placer->color_replace_mode == PLACER_CR_SPECIFIC) {
            if (gui_color_small("Replacement", placer->color_replace_specific))
                placer->color_replace_stamp_seq++;
        }

        if (placer->color_replace_mode == PLACER_CR_RANDOM) {
            if (gui_button("Re-seed", -1, 0))
                placer_color_replace_reseed(placer);
        }
    }
    gui_section_end();

    if(gui_section_begin("Import as", true)) {
        char label[128];
        if (!ff_import_current) ff_import_current = file_formats_import_to_volume; // First one.

        make_label(ff_import_current, label, sizeof(label), "placerimport");
        if (gui_combo_begin("Import as##plcaerimport", label)) {
            file_format_iter("v", NULL, on_import_format);
            gui_combo_end();
        }

        if (ff_import_current->import_gui)
            ff_import_current->import_gui(ff_import_current);

        if (gui_button("Import", 1, 0)) {
            const char *paths_raw = sys_open_multi_file_dialog(
                    "Import", NULL,
                    ff_import_current->exts, ff_import_current->exts_desc);
            if (paths_raw) {
                char *paths_work = strdup(paths_raw);
                if (paths_work) {
                    placer_import_selected_paths(placer, paths_work);
                    free(paths_work);
                }
            }
        }
        
        if (gui_button("Reset", 1, 0)) {
            reset(placer);
        }
    } gui_section_end();

    if (gui_section_begin("Export as", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        char label[128];
        if (!ff_export_current) ff_export_current = file_formats_export_to_volume; // First one.

        make_label(ff_export_current, label, sizeof(label), "export");
        if (gui_combo_begin("Export as##placerexport", label)) {
            file_format_iter("t", NULL, on_export_format);
            gui_combo_end();
        }

        if (ff_export_current->export_gui)
            ff_export_current->export_gui(ff_export_current);
        
        if (gui_button("Export placer content", -1, 0)) {
            const char* path = sys_get_save_path("", ff_export_current->exts, ff_export_current->exts_desc);
            ff_export_current->export_volume_func(ff_export_current, placer->imported_volume, path);
            char *fname = get_file_name_from_path(path);
            on_file_import(path, fname, ff_export_current);
            free(fname);
        }
    } gui_section_end();

    tool_gui_snap();
    
    if (placer->imported_volume) {
        origin_x = (int)round(placer->origin[0]);
        origin_y = (int)round(placer->origin[1]);
        origin_z = (int)round(placer->origin[2]);

        offset_x = (int)round(placer->offset[0]);
        offset_y = (int)round(placer->offset[1]);
        offset_z = (int)round(placer->offset[2]);
            
        gui_row_begin(1);
        char *msg;
        asprintf(&msg, "Rotation: %.1f / %.1f / %.1f", zero(rot_degs[0]), zero(rot_degs[1]), zero(rot_degs[2]));
        gui_text(msg);
        if (gui_button("Reset##rotation", 0, 0)) {
            reset_rotation = true;
        }
        gui_row_end();

        if (gui_section_begin("Rotation", true)) {
            gui_group_begin(NULL);
            gui_row_begin(2);
            if (gui_button("-X", 0, 0)) {
                mat4_irotate(rotation, -M_PI / 8, 1, 0, 0);
                changed = true;
            }
            if (gui_button("+X", 0, 0)) {
                mat4_irotate(rotation, +M_PI / 8, 1, 0, 0);
                changed = true;
            }
            gui_row_end();

            gui_row_begin(2);
            if (gui_button("-Y", 0, 0)) {
                mat4_irotate(rotation, -M_PI / 8, 0, 1, 0);
                changed = true;
            }
            if (gui_button("+Y", 0, 0)) {
                mat4_irotate(rotation, +M_PI / 8, 0, 1, 0);
                changed = true;
            }
            gui_row_end();

            gui_row_begin(2);
            if (gui_button("-Z", 0, 0)) {
                mat4_irotate(rotation, -M_PI / 8, 0, 0, 1);
                changed = true;
            }
            if (gui_button("+Z", 0, 0)) {
                mat4_irotate(rotation, +M_PI / 8, 0, 0, 1);
                changed = true;
            }
            gui_row_end();

            gui_text("Random");
            gui_same_line_spaced(8.f);
            if (gui_combo_begin("##placer_rand_z",
                                placer_rand_z_label(placer->random_z_mode))) {
                if (gui_combo_item(placer_rand_z_label(PLACER_RAND_Z_NONE),
                                   placer->random_z_mode == PLACER_RAND_Z_NONE))
                    placer->random_z_mode = PLACER_RAND_Z_NONE;
                if (gui_combo_item(placer_rand_z_label(PLACER_RAND_Z_90),
                                   placer->random_z_mode == PLACER_RAND_Z_90))
                    placer->random_z_mode = PLACER_RAND_Z_90;
                if (gui_combo_item(placer_rand_z_label(PLACER_RAND_Z_45),
                                   placer->random_z_mode == PLACER_RAND_Z_45))
                    placer->random_z_mode = PLACER_RAND_Z_45;
                if (gui_combo_item(placer_rand_z_label(PLACER_RAND_Z_225),
                                   placer->random_z_mode == PLACER_RAND_Z_225))
                    placer->random_z_mode = PLACER_RAND_Z_225;
                gui_combo_end();
            }
            gui_group_end();
        }
        gui_section_end();

        if (gui_section_begin("Flip", true)) {
            gui_row_begin(3);
            if (gui_button("X", -1, 0)) {
                mat4_iscale(rotation, -1,  1,  1);
                changed = true;
            }
            if (gui_button("Y", -1, 0)) {
                mat4_iscale(rotation,  1, -1,  1);
                changed = true;
            }
            if (gui_button("Z", -1, 0)) {
                mat4_iscale(rotation,  1,  1, -1);
                changed = true;
            }
            gui_row_end();

            gui_text("Random");
            gui_same_line_spaced(8.f);
            if (gui_combo_begin("##placer_rand_flip_xy",
                                placer_rand_flip_xy_label(placer->random_flip_xy_mode))) {
                if (gui_combo_item(placer_rand_flip_xy_label(PLACER_RAND_FLIP_NONE),
                                   placer->random_flip_xy_mode == PLACER_RAND_FLIP_NONE))
                    placer->random_flip_xy_mode = PLACER_RAND_FLIP_NONE;
                if (gui_combo_item(placer_rand_flip_xy_label(PLACER_RAND_FLIP_XY),
                                   placer->random_flip_xy_mode == PLACER_RAND_FLIP_XY))
                    placer->random_flip_xy_mode = PLACER_RAND_FLIP_XY;
                gui_combo_end();
            }
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

    if (reset_rotation || (placer->imported_volume && changed)) {
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