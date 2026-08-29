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
#include "utils/color.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>


enum {
    FILL_MODE_FILL_SPACE = 0,
    FILL_MODE_RECOLOR = 1,
};

typedef struct {
    tool_t tool;
    float  box[4][4];
    int    mode;
    int color_threshold;
    /* When true (checkbox ticked), boundaries come from the active layer only.
     * When false (default), boundaries use the merged visible layers volume. */
    bool current_layer_only;

    struct {
        gesture3d_t drag;
        gesture3d_t hover;
    } gestures;
} tool_fill_t;

#include "uthash.h"

typedef struct queue_node {
    int pos[3];
    struct queue_node *next;
} queue_node_t;

typedef struct {
    queue_node_t *head;
    queue_node_t *tail;
} queue_t;

typedef struct visited_voxel {
    int pos[3];              // key
    UT_hash_handle hh;
} visited_voxel_t;

static bool rgba_is_empty(const uint8_t rgba[4])
{
    return rgba[3] == 0;
}

static bool color_within_threshold(const uint8_t color[4],
                                   const uint8_t reference[4], int threshold)
{
    int diff;

    if (rgba_is_empty(color) || rgba_is_empty(reference)) return false;
    diff = max3(abs(color[0] - reference[0]),
                abs(color[1] - reference[1]),
                abs(color[2] - reference[2]));
    return diff <= threshold;
}

static void queue_init(queue_t *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
}

static bool queue_is_empty(const queue_t *queue)
{
    return queue->head == NULL;
}

static bool queue_push(queue_t *queue, const int pos[3])
{
    queue_node_t *node = malloc(sizeof(*node));
    if (!node) return false;

    node->pos[0] = pos[0];
    node->pos[1] = pos[1];
    node->pos[2] = pos[2];
    node->next = NULL;

    if (queue->tail) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }

    queue->tail = node;
    return true;
}

static bool queue_pop(queue_t *queue, int out[3])
{
    queue_node_t *node;

    if (!queue->head) return false;

    node = queue->head;
    out[0] = node->pos[0];
    out[1] = node->pos[1];
    out[2] = node->pos[2];

    queue->head = node->next;
    if (!queue->head) {
        queue->tail = NULL;
    }

    free(node);
    return true;
}

static void queue_destroy(queue_t *queue)
{
    int tmp[3];
    while (queue_pop(queue, tmp)) {
        // Drain queue
    }
}

static bool visited_contains(visited_voxel_t *visited, const int pos[3])
{
    visited_voxel_t *entry = NULL;
    HASH_FIND(hh, visited, pos, sizeof(int) * 3, entry);
    return entry != NULL;
}

static bool visited_add(visited_voxel_t **visited, const int pos[3])
{
    visited_voxel_t *entry = NULL;

    HASH_FIND(hh, *visited, pos, sizeof(int) * 3, entry);
    if (entry) return true;

    entry = malloc(sizeof(*entry));
    if (!entry) return false;

    entry->pos[0] = pos[0];
    entry->pos[1] = pos[1];
    entry->pos[2] = pos[2];

    HASH_ADD(hh, *visited, pos, sizeof(int) * 3, entry);
    return true;
}

static void visited_destroy(visited_voxel_t **visited)
{
    visited_voxel_t *cur;
    visited_voxel_t *tmp;

    HASH_ITER(hh, *visited, cur, tmp) {
        HASH_DEL(*visited, cur);
        free(cur);
    }
}

static bool in_box(int dims[3], int start_pos[3], int pos[3]) {
    if (    pos[0] < start_pos[0]
        ||  pos[0] >= dims[0] + start_pos[0]
        ||  pos[1] < start_pos[1]
        ||  pos[1] >= dims[1] + start_pos[1]
        ||  pos[2] < start_pos[2]
        ||  pos[2] >= dims[2] + start_pos[2]) {
            return false;
        }
    return true;
}

static bool flood_fill_volume(volume_t *paint_volume,
                              const volume_t *sample_volume,
                              const float start_pos[3],
                              const uint8_t fill_color[4],
                              int painter_mode, int color_threshold)
{
    queue_t queue;
    visited_voxel_t *visited = NULL;
    volume_iterator_t iter_sample, new_vol_iter;
    uint8_t voxel[4], reference_color[4];
    int box_dimensions[3], box_start_pos[3];
    uint64_t layer_key0 = volume_get_key(paint_volume);
    volume_t *new_vol = volume_new();
    bool paint_mode = painter_mode == MODE_PAINT;

    const int start[3] = {
        (int)floorf(start_pos[0]),
        (int)floorf(start_pos[1]),
        (int)floorf(start_pos[2])
    };

    static const int directions[6][3] = {
        {  1,  0,  0 },
        { -1,  0,  0 },
        {  0,  1,  0 },
        {  0, -1,  0 },
        {  0,  0,  1 },
        {  0,  0, -1 }
    };
    const int direction_count = paint_mode ? 6 : 4;

    box_get_dimensions(goxel.image->box, box_dimensions);
    box_get_start_pos(goxel.image->box, box_start_pos);
    LOG_D("flood_fill: start=(%i,%i,%i)", start[0], start[1], start[2]);
    LOG_D("flood_fill: box_dimensions=(%i,%i,%i)", box_dimensions[0], box_dimensions[1], box_dimensions[2]);
    LOG_D("flood_fill: box_start_pos=(%i,%i,%i)", box_start_pos[0], box_start_pos[1], box_start_pos[2]);

    queue_init(&queue);

    iter_sample = volume_get_iterator(sample_volume, VOLUME_ITER_VOXELS);
    new_vol_iter = volume_get_iterator(new_vol, VOLUME_ITER_VOXELS);
    volume_get_at(sample_volume, &iter_sample, start, reference_color);

    if (paint_mode ? rgba_is_empty(reference_color)
                   : !rgba_is_empty(reference_color)) {
        volume_delete(new_vol);
        return true;
    }

    if (!queue_push(&queue, start)) {
        volume_delete(new_vol);
        return false;
    }

    if (!visited_add(&visited, start)) {
        queue_destroy(&queue);
        volume_delete(new_vol);
        return false;
    }

    while (!queue_is_empty(&queue)) {
        int pos[3];

        if (!queue_pop(&queue, pos)) {
            break;
        }

        //LOG_D("visit: (%i,%i,%i)", pos[0], pos[1], pos[2]);

        volume_get_at(sample_volume, &iter_sample, pos, voxel);

        if (paint_mode
                ? !color_within_threshold(voxel, reference_color,
                                          color_threshold)
                : !rgba_is_empty(voxel)) {
            LOG_D("skip: (%i,%i,%i) outside fill region",
                  pos[0], pos[1], pos[2]);
            continue;
        }

        uint8_t voxel_fill_color[4];
        memcpy(voxel_fill_color, fill_color, 4);

        if (goxel.brush_source_mode == BRUSH_SOURCE_TEXTURE) {
            const brush_texture_t *tex = goxel_brush_texture_current();
            if (tex && tex->pixels && tex->w > 0 && tex->h > 0) {
                int tx = (pos[0] % tex->w + tex->w) % tex->w;
                int ty = (pos[1] % tex->h + tex->h) % tex->h;
                int bpp = tex->bpp > 0 ? tex->bpp : 4;
                int idx = (ty * tex->w + tx) * bpp;
                voxel_fill_color[0] = tex->pixels[idx + 0];
                voxel_fill_color[1] = tex->pixels[idx + 1];
                voxel_fill_color[2] = tex->pixels[idx + 2];
                voxel_fill_color[3] = (bpp >= 4) ? tex->pixels[idx + 3] : 255;
                srgb8_adjust_hsl(voxel_fill_color,
                                 goxel.brush_texture_hue,
                                 goxel.brush_texture_saturation,
                                 goxel.brush_texture_lightness);
                voxel_fill_color[3] = ((int)voxel_fill_color[3] * (int)fill_color[3]) / 255;
            }
        } else if (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE &&
                   goxel.brush_palette_count > 0) {
            goxel_brush_palette_sample_at(pos, voxel_fill_color);
            voxel_fill_color[3] = ((int)voxel_fill_color[3] * (int)fill_color[3]) / 255;
        } else {
            if (goxel.painter.noise_enabled != 0 &&
                goxel.painter.noise_intensity != 0 &&
                goxel.painter.noise_coverage != 0) {
                float global_p[3] = {
                    (float)noise_tex_coord(pos[0]),
                    (float)noise_tex_coord(pos[1]),
                    (float)noise_tex_coord(pos[2])
                };
                apply_noise_if_applicable(&goxel.painter, global_p, voxel_fill_color);
            }
        }

        volume_set_at(new_vol, &new_vol_iter, pos, voxel_fill_color);

        for (int d = 0; d < direction_count; d++) {
            int next[3] = {
                pos[0] + directions[d][0],
                pos[1] + directions[d][1],
                pos[2] + directions[d][2]
            };

            if (!in_box(box_dimensions, box_start_pos, next)){
                //LOG_D("skip: out of box: (%i,%i,%i)", next[0], next[1], next[2])
                continue;
            }

            if (visited_contains(visited, next)) {
                //LOG_D("skip: (%i,%i,%i) already visited", next[0], next[1], next[2]);
                continue;
            }

            if (!visited_add(&visited, next)) {
                //LOG_D("error: failed to add visited (%i,%i,%i)", next[0], next[1], next[2]);
                visited_destroy(&visited);
                queue_destroy(&queue);
                volume_delete(new_vol);
                return false;
            }

            volume_get_at(sample_volume, &iter_sample, next, voxel);

            if (paint_mode
                    ? color_within_threshold(voxel, reference_color,
                                             color_threshold)
                    : rgba_is_empty(voxel)) {
                //LOG_D("enqueue: (%i,%i,%i)", next[0], next[1], next[2]);

                if (!queue_push(&queue, next)) {
                    LOG_D("error: failed to enqueue (%i,%i,%i)", next[0], next[1], next[2]);
                    visited_destroy(&visited);
                    queue_destroy(&queue);
                    volume_delete(new_vol);
                    return false;
                }
            } else {
                //LOG_D("boundary: (%i,%i,%i)", next[0], next[1], next[2]);
            }
        }
    }

    volume_merge(paint_volume, new_vol,
                 paint_mode ? MODE_PAINT : MODE_OVER, NULL);
    if (volume_get_key(paint_volume) != layer_key0) {
        if (goxel.brush_source_mode == BRUSH_SOURCE_COLOR)
            image_recent_color_push_from_painter(goxel.image, &goxel.painter);
        if (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE)
            goxel_brush_palette_reroll_seed();
    }
    volume_delete(new_vol);

    LOG_D("flood_fill: complete");

    visited_destroy(&visited);
    queue_destroy(&queue);
    return true;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    tool_fill_t *filler = USER_GET(user, 0);
    const painter_t *painter = USER_GET(user, 1);
    const bool recolor = (filler->mode == FILL_MODE_RECOLOR) || (painter->mode == MODE_PAINT);
    float box[4][4] = MAT4_IDENTITY;
    mat4_iscale(box, 0.5, 0.5, 0.5);
    cursor_t *curs = gest->cursor;
    uint8_t box_color[4] = {0, 255, 255, 255};
    vec3_to_mat4(curs->pos, box);

    if (recolor)
        goxel_set_help_text("Click on a block to recolor connected blocks");
    else
        goxel_set_help_text("Click to floodfill this z level");
    render_box(&goxel.rend, box, box_color, EFFECT_WIREFRAME);
    return 0;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    tool_fill_t *filler = USER_GET(user, 0);
    volume_t *paint_volume = goxel.image->active_layer->volume;

    if (gest->state == GESTURE_BEGIN) {
        const painter_t *painter = USER_GET(user, 1);
        cursor_t *curs = gest->cursor;
        const volume_t *sample_volume;
        const bool recolor = (filler->mode == FILL_MODE_RECOLOR) || (painter->mode == MODE_PAINT);
        const int effective_mode = recolor ? MODE_PAINT : painter->mode;

        image_history_push(goxel.image);
        if (effective_mode == MODE_OVER &&
                !image_ensure_layer_for_adding(goxel.image))
            return 0;
        paint_volume = goxel.image->active_layer->volume;
        sample_volume = filler->current_layer_only
            ? paint_volume
            : goxel_get_layers_volume(goxel.image);
        flood_fill_volume(paint_volume, sample_volume, curs->pos,
                          painter->color, effective_mode,
                          filler->color_threshold);
    }

    return 0;
}


int tool_fill_iter(tool_t *tool, const painter_t *painter,
                           const float viewport[4])
{
    tool_fill_t *filler = (tool_fill_t*)tool;
    cursor_t *curs = &goxel.cursor;
    const bool recolor = (filler->mode == FILL_MODE_RECOLOR) || (painter->mode == MODE_PAINT);
    
    curs->snap_mask &= ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT);
    if (recolor) {
        curs->snap_mask &= ~SNAP_ROUNDED;
        curs->snap_mask |= SNAP_VOLUME;
        curs->snap_offset = -0.5;
    } else {
        curs->snap_mask |= SNAP_ROUNDED;
        curs->snap_mask |= SNAP_SELECTION_OUT;
        curs->snap_offset = 0.5;
    }

    if (!filler->gestures.drag.type) {
        filler->gestures.drag = (gesture3d_t) {
            .type = GESTURE_DRAG,
            .callback = on_drag,
        };
        filler->gestures.hover = (gesture3d_t) {
            .type = GESTURE_HOVER,
            .callback = on_hover,
        };
    }
    gesture3d(&filler->gestures.drag, curs, USER_PASS(filler, painter));
    gesture3d(&filler->gestures.hover, curs, USER_PASS(filler, painter));

    return tool->state;
}

static int gui(tool_t *tool)
{
    tool_fill_t *filler = (void *)tool;
    static const char *fill_tabs[] = {"Fill space", "Recolor blocks"};
    static const char *source_tabs[] = {"Color", "Texture", "Palette"};
    static bool textures_reload_pending = false;
    char textures_dir[1024];
    bool has_textures_dir;
    float cell = 64.f;
    int i, tex_count;

    if (textures_reload_pending) {
        goxel_brush_textures_reload();
        textures_reload_pending = false;
    }
    has_textures_dir = goxel_brush_textures_dir(textures_dir, sizeof(textures_dir));
    if (!has_textures_dir)
        textures_dir[0] = '\0';

    if (gui_tabsheet_begin("##fill_mode", fill_tabs, 2, &filler->mode)) {
        gui_dummy(0, 4);
        gui_label_size_push(0);
        gui_checkbox(
                "Current layer only", &filler->current_layer_only,
                "Restrict floodfill to be within blocks only on the current layer");
        gui_label_size_pop();

        if (filler->mode == FILL_MODE_RECOLOR || goxel.painter.mode == MODE_PAINT) {
            gui_input_int("Threshold", &filler->color_threshold, 0, 255);
            gui_tooltip_if_hovered("Color difference tolerance for connected blocks (0 = exact match)");
        }

        gui_dummy(0, 8);
        int prev_mode = goxel.brush_source_mode;
        if (gui_tabsheet_begin("##fill_source", source_tabs, 3, &goxel.brush_source_mode)) {
            if (prev_mode == BRUSH_SOURCE_PALETTE &&
                goxel.brush_source_mode != BRUSH_SOURCE_PALETTE) {
                goxel_brush_palette_exit_to_mode(goxel.brush_source_mode);
            }

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
                            snprintf(id, sizeof(id), "fill_tex_%d", i);
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
                gui_input_float("Hue", &goxel.brush_texture_hue, 1.f, -180.f, 180.f, "%.1f");
                gui_input_float("Saturation", &goxel.brush_texture_saturation, 1.f, 0.f, 200.f, "%.1f");
                gui_input_float("Lightness", &goxel.brush_texture_lightness, 1.f, -100.f, 100.f, "%.1f");
                gui_color_opacity(goxel.painter.color);
                if (gui_button("Reset", 0, 0)) {
                    goxel.brush_texture_hue = 0.f;
                    goxel.brush_texture_saturation = 100.f;
                    goxel.brush_texture_lightness = 0.f;
                    goxel.painter.color[3] = 255;
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

            if (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE) {
                if (gui_section_begin("Palette", true)) {
                    if (goxel.brush_palette_count <= 0) {
                        gui_text("Shift+click colours in the Palette panel to "
                                 "fill with multiple colours.");
                    } else {
                        gui_text("%d colours selected (Shift+click to toggle).",
                                 goxel.brush_palette_count);
                        {
                            gui_icon_info_t *pgrid;
                            int pi, pidx = -1;
                            pgrid = calloc((size_t)goxel.brush_palette_count,
                                           sizeof(*pgrid));
                            for (pi = 0; pi < goxel.brush_palette_count; pi++) {
                                pgrid[pi] = (gui_icon_info_t){
                                    .label = "",
                                    .icon = 0,
                                    .color = {
                                        VEC4_SPLIT(
                                            goxel.brush_palette_colors[pi])},
                                };
                            }
                            gui_color_swatches_grid(
                                    goxel.brush_palette_count, pgrid, NULL,
                                    &pidx);
                            free(pgrid);
                        }
                    }
                }
                gui_section_end();
            }
            gui_tabsheet_end();
        }
        gui_tabsheet_end();
    }
    return 0;
}

TOOL_REGISTER(TOOL_FILL, fill, tool_fill_t,
             .name = "Fill",
             .iter_fn = tool_fill_iter,
             .gui_fn = gui,
             .default_shortcut = "N",
             .flags = TOOL_SHOW_MASK,
             .has_snap = true,
)
