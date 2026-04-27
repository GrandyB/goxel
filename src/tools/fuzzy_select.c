/* Goxel 3D voxels editor
 *
 * copyright (c) 2019 Guillaume Chereau <guillaume@noctua-software.com>
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
    int threshold;
    bool global;
    struct {
        gesture3d_t click;
    } gestures;
} tool_fuzzy_select_t;


static bool within_threshold(tool_fuzzy_select_t *tool, uint8_t v0[4], uint8_t v1[4]) {
    if (!v0[3] || !v1[3]) return false;

    int diff = max3(abs(v0[0] - v1[0]), abs(v0[1] - v1[1]), abs(v0[2] - v1[2]));
    return diff <= tool->threshold;
}

static void volume_global_select(volume_t *volume, const int curs_pos[3], tool_fuzzy_select_t *tool, volume_t *selection) {
    
    uint8_t color_at_curs[4];
    uint8_t current_color[4];
    int pos[3];
    volume_iterator_t iter;
    volume_accessor_t volume_accessor, selection_accessor;
    volume_clear(selection);

    volume_accessor = volume_get_accessor(volume);
    selection_accessor = volume_get_accessor(selection);

    // Always include the voxel under cursor
    volume_get_at(volume, &volume_accessor, curs_pos, color_at_curs);
    if (color_at_curs[3] == 0) {
        //LOG_D("No color under cursor");
        return;
    } else {
        //LOG_D("Color under cursor: %i / %i / %i / %i", color_at_curs[0], color_at_curs[1], color_at_curs[2], color_at_curs[3]);
    }
    volume_set_at(selection, &selection_accessor, curs_pos, (uint8_t[]){255, 255, 255, 255});

    // Iterate through the entire volume
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &volume_accessor, pos, current_color);
        if (current_color[3] != 0) {
            //LOG_D("Pos %i / %i / %i", pos[0], pos[1], pos[2]);
        } else {
            continue;
        }
        bool set = within_threshold(tool, color_at_curs, current_color);
        volume_set_at(selection, &selection_accessor, pos, (uint8_t[]){255, 255, 255, set ? 255 : 0});
    }
}

static int select_cond(void *user, const volume_t *volume,
                       const int base_pos[3],
                       const int new_pos[3],
                       volume_accessor_t *volume_accessor)
{
    tool_fuzzy_select_t *tool = (void*)user;
    uint8_t v0[4], v1[4];

    volume_get_at(volume, volume_accessor, base_pos, v0);
    volume_get_at(volume, volume_accessor, new_pos, v1);

    return within_threshold(tool, v0, v1) ? 255 : 0;
}

static int on_click(gesture3d_t *gest, void *user)
{
    volume_t *volume = goxel.image->active_layer->volume;
    volume_t *sel;
    int pi[3];
    cursor_t *curs = gest->cursor;
    tool_fuzzy_select_t *tool = (void*)user;

    pi[0] = floor(curs->pos[0]);
    pi[1] = floor(curs->pos[1]);
    pi[2] = floor(curs->pos[2]);
    sel = volume_new();
    if (!tool->global) {
        volume_select(volume, pi, select_cond, tool, sel);
    } else {
        volume_global_select(volume, pi, tool, sel);
    }
    if (goxel.mask == NULL) goxel.mask = volume_new();
    volume_merge(goxel.mask, sel, goxel.mask_mode ?: MODE_REPLACE, NULL);
    volume_delete(sel);
    return 0;
}


static int iter(tool_t *tool_, const painter_t *painter,
                const float viewport[4])
{
    cursor_t *curs = &goxel.cursor;
    tool_fuzzy_select_t *tool = (void*)tool_;

    curs->snap_offset = -0.5;
    curs->snap_mask &= ~SNAP_ROUNDED;

    if (!tool->gestures.click.type) {
        tool->gestures.click = (gesture3d_t) {
            .type = GESTURE_CLICK,
            .callback = on_click,
        };
    }
    gesture3d(&tool->gestures.click, curs, tool);
    return 0;
}

static layer_t *cut_as_new_layer(image_t *img, layer_t *layer,
                                 const volume_t *mask)
{
    layer_t *new_layer;

    new_layer = image_duplicate_layer(img, layer);
    volume_merge(new_layer->volume, mask, MODE_INTERSECT, NULL);
    volume_merge(layer->volume, mask, MODE_SUB, NULL);
    return new_layer;
}

static int gui(tool_t *tool_)
{
    tool_fuzzy_select_t *tool = (void*)tool_;
    bool use_color = tool->threshold < 255;

    if (gui_checkbox("Use color", &use_color, "Stop at different color")) {
        tool->threshold = use_color ? 0 : 255;
    }
    gui_checkbox("Entire layer", &tool->global, "If unchecked, fuzzy will only select connected voxels; if checked, it will select any across the current layer");
    if (use_color) {
        gui_input_int("Threshold", &tool->threshold, 1, 254);
    }
    
    if (!volume_is_empty(goxel.mask)) {
        gui_group_begin(NULL);
        if (gui_button("Reset", 1, 0)) {
            goxel.mask = volume_new();
        }
        gui_group_end();
    }

    tool_gui_mask_mode();

    if (volume_is_empty(goxel.mask))
        return 0;

    volume_t *volume = goxel.image->active_layer->volume;

    tool_gui_color();
    gui_section_end();
    gui_group_begin(NULL);
    if (gui_button("Delete blocks", 1, 0)) {
        image_history_push(goxel.image);
        volume_merge(volume, goxel.mask, MODE_SUB, NULL);
    }
    if (gui_button("Fill", 1, 0)) {
        uint64_t k0 = volume_get_key(volume);
        image_history_push(goxel.image);
        volume_t *vol = volume_copy(goxel.mask);
        float box[4][4] = MAT4_IDENTITY;
        volume_get_box(goxel.mask, true, box);
        int existing_mode = goxel.painter.mode;
        goxel.painter.mode = MODE_PAINT;
        volume_op(vol, &goxel.painter, box);
        volume_merge(volume, vol, MODE_OVER, NULL);
        goxel.painter.mode = existing_mode;
        if (volume_get_key(volume) != k0)
            image_recent_color_push_from_painter(goxel.image, &goxel.painter);
    }
    if (gui_button("Cut as new layer", 1, 0)) {
        image_history_push(goxel.image);
        cut_as_new_layer(goxel.image, goxel.image->active_layer,
                         goxel.mask);
    }
    gui_group_end();
    return 0;
}

TOOL_REGISTER(TOOL_FUZZY_SELECT, fuzzy_select, tool_fuzzy_select_t,
              .name = "Fuzzy Select",
              .iter_fn = iter,
              .gui_fn = gui,
              .flags = TOOL_REQUIRE_CAN_EDIT | TOOL_SHOW_MASK,
)
