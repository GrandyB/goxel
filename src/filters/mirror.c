/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
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
    filter_t filter;
} filter_mirror_t;

static void volume_mirror(volume_t *volume, int axis, const int aabb[2][3])
{
    volume_iterator_t iter;
    volume_t *out;
    int pos[3], dst[3];
    int sum;
    uint8_t color[4];

    if (!volume || aabb[1][axis] - aabb[0][axis] <= 1)
        return;
    if (volume_is_empty(volume))
        return;

    /* Reflect every voxel around the AABB centre on `axis`.  Unlike the
     * old AABB-scan approach, content outside the image box is included. */
    sum = aabb[0][axis] + aabb[1][axis] - 1;
    out = volume_new();
    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, color);
        if (!color[3])
            continue;
        dst[0] = pos[0];
        dst[1] = pos[1];
        dst[2] = pos[2];
        dst[axis] = sum - pos[axis];
        volume_set_at(out, NULL, dst, color);
    }
    volume_set(volume, out);
    volume_delete(out);
}

/*
 * Mirror one half of the AABB onto the other half along `axis`.
 * side 1 (e.g. X1): low half → high half
 * side 2 (e.g. X2): high half → low half
 * Source half is left unchanged; destination half is overwritten.
 */
static void volume_mirror_half(volume_t *volume, int axis, int side,
                               const int aabb[2][3])
{
    int pos[3];
    int src_pos[3];
    int dst_pos[3];
    int size[3];
    int half;
    int i;
    uint8_t color[4];
    volume_iterator_t iter = {0};

    size[0] = aabb[1][0] - aabb[0][0];
    size[1] = aabb[1][1] - aabb[0][1];
    size[2] = aabb[1][2] - aabb[0][2];
    half = size[axis] / 2;
    if (half == 0)
        return;

    for (pos[0] = 0; pos[0] < size[0]; pos[0]++) {
        for (pos[1] = 0; pos[1] < size[1]; pos[1]++) {
            for (pos[2] = 0; pos[2] < size[2]; pos[2]++) {
                if (side == 1) {
                    if (pos[axis] >= half)
                        continue;
                } else {
                    if (pos[axis] < size[axis] - half)
                        continue;
                }

                for (i = 0; i < 3; i++) {
                    src_pos[i] = aabb[0][i] + pos[i];
                    dst_pos[i] = aabb[0][i] + pos[i];
                }
                dst_pos[axis] = aabb[0][axis] + (size[axis] - 1 - pos[axis]);

                volume_get_at(volume, &iter, src_pos, color);
                volume_set_at(volume, &iter, dst_pos, color);
            }
        }
    }

    volume_remove_empty_tiles(volume, false);
}

static int axis_selection_box(void)
{
    char buf[32];
    static const char *AXIS_NAMES[] = {"X", "Y", "Z"};
    int axis;
    int selected_axis = -1;

    for (axis = 0; axis < 3; axis++) {
        snprintf(buf, sizeof(buf), "Mirror entire %s", AXIS_NAMES[axis]);

        if (gui_button(buf, 1.0, 0)) {
            selected_axis = axis;
        }
    }

    return selected_axis;
}

static bool half_selection_box(int *out_axis, int *out_side)
{
    char buf[8];
    static const char *AXIS_NAMES[] = {"X", "Y", "Z"};
    int axis;
    bool ret = false;

    *out_axis = 0;
    *out_side = 1;

    for (axis = 0; axis < 3; axis++) {
        gui_row_begin(2);

        snprintf(buf, sizeof(buf), "%s1", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0)) {
            *out_axis = axis;
            *out_side = 1;
            ret = true;
        }

        snprintf(buf, sizeof(buf), "%s2", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0)) {
            *out_axis = axis;
            *out_side = 2;
            ret = true;
        }

        gui_row_end();
    }

    return ret;
}

static void mirror_apply(filter_mirror_t *mirror_props, int axis, int side,
                         bool half)
{
    int aabb[2][3];
    layer_t *layer;
    bool current_only = mirror_props->filter.current_only;

    if (current_only && (!goxel.image->active_layer ||
                         !goxel.image->active_layer->visible))
        return;

    if (!goxel_get_filter_aabb(current_only, aabb))
        return;

    image_history_push(goxel.image);

    DL_FOREACH(goxel.image->layers, layer) {
        if (current_only &&
            !layer_in_active_subtree(goxel.image, layer))
            continue;
        if (!layer->volume)
            continue;

        if (half)
            volume_mirror_half(layer->volume, axis, side, aabb);
        else
            volume_mirror(layer->volume, axis, aabb);
    }
}

static int gui(filter_t *filter)
{
    filter_mirror_t *mirror_props = (filter_mirror_t *)filter;
    int axis;
    int half_axis;
    int half_side;
    bool do_half;

    {
        bool has_layer = goxel.image && goxel.image->active_layer;

        if (!has_layer)
            mirror_props->filter.current_only = false;
        gui_enabled_begin(has_layer);
        gui_checkbox(
            "Current layer only",
            &mirror_props->filter.current_only,
            "If checked, only the current layer and its children "
            "(recursively) are mirrored.\n"
            "If unchecked, voxels on all layers will be mirrored."
        );
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }

    gui_text("Mirror the entire content in a specific direction");
    gui_group_begin(NULL);
    axis = axis_selection_box();
    gui_group_end();

    if (axis != -1)
        mirror_apply(mirror_props, axis, 0, false);

    gui_separator();

    gui_text("Mirror one half onto the other");
    gui_group_begin(NULL);
    do_half = half_selection_box(&half_axis, &half_side);
    gui_group_end();

    if (do_half)
        mirror_apply(mirror_props, half_axis, half_side, true);

    return 0;
}

FILTER_REGISTER(mirror, filter_mirror_t,
    .name = "Mirror",
    .menu = "image",
    .panel_width = 325,
    .gui_fn = gui,
)
