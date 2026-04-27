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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>


typedef struct {
    tool_t tool;
    float  box[4][4];

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

bool flood_fill_volume(volume_t *volume, const float start_pos[3], const uint8_t fill_color[4])
{
    queue_t queue;
    visited_voxel_t *visited = NULL;
    volume_iterator_t iter, new_vol_iter;
    uint8_t voxel[4];
    int box_dimensions[3], box_start_pos[3];
    uint64_t layer_key0 = volume_get_key(volume);
    volume_t *new_vol = volume_new();

    const int start[3] = {
        (int)floorf(start_pos[0]),
        (int)floorf(start_pos[1]),
        (int)floorf(start_pos[2])
    };

    static const int directions[4][3] = {
        {  1,  0,  0 },
        { -1,  0,  0 },
        {  0,  1,  0 },
        {  0, -1,  0 }
    };

    box_get_dimensions(goxel.image->box, box_dimensions);
    box_get_start_pos(goxel.image->box, box_start_pos);
    LOG_D("flood_fill: start=(%i,%i,%i)", start[0], start[1], start[2]);
    LOG_D("flood_fill: box_dimensions=(%i,%i,%i)", box_dimensions[0], box_dimensions[1], box_dimensions[2]);
    LOG_D("flood_fill: box_start_pos=(%i,%i,%i)", box_start_pos[0], box_start_pos[1], box_start_pos[2]);

    queue_init(&queue);

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    new_vol_iter = volume_get_iterator(new_vol, VOLUME_ITER_VOXELS);
    volume_get_at(volume, &iter, start, voxel);

    if (!rgba_is_empty(voxel)) {
        //LOG_D("flood_fill: start voxel is not empty -> abort");
        return true;
    }

    if (!queue_push(&queue, start)) {
        //LOG_D("flood_fill: failed to enqueue start");
        return false;
    }

    if (!visited_add(&visited, start)) {
        //LOG_D("flood_fill: failed to add start to visited");
        queue_destroy(&queue);
        return false;
    }

    while (!queue_is_empty(&queue)) {
        int pos[3];

        if (!queue_pop(&queue, pos)) {
            break;
        }

        //LOG_D("visit: (%i,%i,%i)", pos[0], pos[1], pos[2]);

        volume_get_at(volume, &iter, pos, voxel);

        if (!rgba_is_empty(voxel)) {
            LOG_D("skip: (%i,%i,%i) not empty", pos[0], pos[1], pos[2]);
            continue;
        }

        //LOG_D("fill: (%i,%i,%i)", pos[0], pos[1], pos[2]);
        volume_set_at(new_vol, &new_vol_iter, pos, fill_color);

        for (int d = 0; d < 4; d++) {
            int next[3] = {
                pos[0] + directions[d][0],
                pos[1] + directions[d][1],
                pos[2]
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
                return false;
            }

            volume_get_at(volume, &iter, next, voxel);

            if (rgba_is_empty(voxel)) {
                //LOG_D("enqueue: (%i,%i,%i)", next[0], next[1], next[2]);

                if (!queue_push(&queue, next)) {
                    LOG_D("error: failed to enqueue (%i,%i,%i)", next[0], next[1], next[2]);
                    visited_destroy(&visited);
                    queue_destroy(&queue);
                    return false;
                }
            } else {
                //LOG_D("boundary: (%i,%i,%i)", next[0], next[1], next[2]);
            }
        }
    }

    float box[4][4] = MAT4_IDENTITY;
    volume_get_box(new_vol, true, box);
    int existing_mode = goxel.painter.mode;
    goxel.painter.mode = MODE_PAINT;
    volume_op(new_vol, &goxel.painter, box);
    volume_merge(volume, new_vol, MODE_OVER, NULL);
    goxel.painter.mode = existing_mode;
    if (volume_get_key(volume) != layer_key0)
        image_recent_color_push_from_painter(goxel.image, &goxel.painter);
    volume_delete(new_vol);

    LOG_D("flood_fill: complete");

    visited_destroy(&visited);
    queue_destroy(&queue);
    return true;
}

static int on_hover(gesture3d_t *gest, void *user)
{
    float box[4][4] = MAT4_IDENTITY;
    mat4_iscale(box, 0.5, 0.5, 0.5);
    cursor_t *curs = gest->cursor;
    uint8_t box_color[4] = {0, 255, 255, 255};
    vec3_to_mat4(curs->pos, box);

    goxel_set_help_text("Click to floodfill this z level");
    render_box(&goxel.rend, box, box_color, EFFECT_WIREFRAME);
    return 0;
}

static int on_drag(gesture3d_t *gest, void *user)
{
    volume_t *volume = goxel.image->active_layer->volume;

    if (gest->state == GESTURE_BEGIN) {
        image_history_push(goxel.image);

        const painter_t *painter = USER_GET(user, 1);

        cursor_t *curs = gest->cursor;
        flood_fill_volume(volume, curs->pos, painter->color);
    }

    return 0;
}


int tool_fill_iter(tool_t *tool, const painter_t *painter,
                           const float viewport[4])
{
    tool_fill_t *filler = (tool_fill_t*)tool;
    cursor_t *curs = &goxel.cursor;
    
    curs->snap_mask |= SNAP_ROUNDED;
    curs->snap_mask &= ~(SNAP_SELECTION_IN | SNAP_SELECTION_OUT);
    curs->snap_offset = 0.5;
    curs->snap_mask |= SNAP_SELECTION_OUT;

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
    tool_gui_color();
    gui_section_end();
    return 0;
}

TOOL_REGISTER(TOOL_FILL, fill, tool_fill_t,
             .name = "Fill",
             .iter_fn = tool_fill_iter,
             .gui_fn = gui,
             .default_shortcut = "F",
             .flags = TOOL_SHOW_MASK,
             .has_snap = true,
)
