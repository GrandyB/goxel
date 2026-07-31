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

/*
 * A filter for moving voxels along an axis and wrapping at the boundary
 * of an AABB.
 */

typedef struct
{
    filter_t filter;
    int distance;
} filter_wrap_t;

static void volume_wrap(volume_t *volume, int axis, int sign,
                        const int aabb[2][3], int filter_distance)
{
    volume_iterator_t iter;
    volume_t *out;
    int pos[3], dst[3], local, size;
    int distance;
    uint8_t color[4];

    size = aabb[1][axis] - aabb[0][axis];
    if (!volume || size <= 1)
        return;
    if (volume_is_empty(volume))
        return;

    distance = (filter_distance % size) * sign;
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
        local = pos[axis] - aabb[0][axis] + distance;
        local %= size;
        if (local < 0)
            local += size;
        dst[axis] = aabb[0][axis] + local;
        volume_set_at(out, NULL, dst, color);
    }
    volume_set(volume, out);
    volume_delete(out);
}

static bool wrap_box(int *out_axis, int *sign)
{
    char buf[8];
    bool ret = false;
    int axis;
    const char *AXIS_NAMES[] = {"X", "Y", "Z"};

    *out_axis = 0;
    *sign = 1;

    for (axis = 0; axis < 3; axis++)
    {
        gui_row_begin(2);

        snprintf(buf, sizeof(buf), "-%s", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0))
        {
            *out_axis = axis;
            *sign = -1;
            ret = true;
        }

        snprintf(buf, sizeof(buf), "+%s", AXIS_NAMES[axis]);
        if (gui_button(buf, 1.0, 0))
        {
            *out_axis = axis;
            *sign = 1;
            ret = true;
        }

        gui_row_end();
    }

    return ret;
}

static int gui(filter_t *filter)
{
    filter_wrap_t *wrap = (void *)filter;
    int axis, sign;
    int aabb[2][3];
    bool should_wrap;
    layer_t *layer;
    bool current_only = wrap->filter.current_only;

    gui_checkbox(
        "Current layer only",
        &wrap->filter.current_only,
        "If checked, only the current layer and its children "
        "(recursively) are wrapped.\n"
        "If unchecked, voxels on all layers will be wrapped.");

    gui_input_int("Distance", &wrap->distance, 0, 9999);

    gui_group_begin(NULL);
    should_wrap = wrap_box(&axis, &sign);
    gui_group_end();

    if (should_wrap)
    {
        if (current_only && !goxel.image->active_layer->visible)
            return 0;

        if (!goxel_get_filter_aabb(current_only, aabb))
            return 0;

        image_history_push(goxel.image);
        DL_FOREACH(goxel.image->layers, layer)
        {
            if (current_only &&
                !layer_in_active_subtree(goxel.image, layer))
                continue;
            if (!layer->volume)
                continue;

            volume_wrap(layer->volume, axis, sign, aabb, wrap->distance);
        }
    }

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_wrap_t *filter = (void *)filter_;
    filter->distance = 1;
}

FILTER_REGISTER(wrap, filter_wrap_t,
                .name = "Wrap",
                .menu = "image",
                .on_open = on_open,
                .gui_fn = gui, )