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
    int offset[3];
} filter_translate_t;

static void volume_translate(volume_t *volume, const int offset[3])
{
    volume_iterator_t iter;
    volume_t *out;
    int pos[3], dst[3];
    uint8_t color[4];

    if (!volume || volume_is_empty(volume))
        return;
    if (offset[0] == 0 && offset[1] == 0 && offset[2] == 0)
        return;

    out = volume_new();
    iter = volume_get_iterator(volume,
                               VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, color);
        if (!color[3])
            continue;
        dst[0] = pos[0] + offset[0];
        dst[1] = pos[1] + offset[1];
        dst[2] = pos[2] + offset[2];
        volume_set_at(out, NULL, dst, color);
    }
    volume_set(volume, out);
    volume_delete(out);
}

static void reset_defaults(filter_translate_t *filter)
{
    filter->offset[0] = 0;
    filter->offset[1] = 0;
    filter->offset[2] = 0;
}

static void apply_translate(filter_translate_t *filter)
{
    layer_t *layer;
    bool current_only = filter->filter.current_only;

    if (filter->offset[0] == 0 && filter->offset[1] == 0 &&
        filter->offset[2] == 0)
        return;

    if (current_only && (!goxel.image->active_layer ||
                         !goxel.image->active_layer->visible))
        return;

    image_history_push(goxel.image);
    DL_FOREACH(goxel.image->layers, layer) {
        if (current_only &&
            !layer_in_active_subtree(goxel.image, layer))
            continue;
        if (!layer->volume)
            continue;
        volume_translate(layer->volume, filter->offset);
    }
}

static int gui(filter_t *filter_)
{
    filter_translate_t *filter = (void *)filter_;
    bool has_layer;

    has_layer = goxel.image && goxel.image->active_layer;
    if (!has_layer)
        filter->filter.current_only = false;
    gui_enabled_begin(has_layer);
    gui_checkbox(
        "Current layer only",
        &filter->filter.current_only,
        "If checked, only the current layer and its children "
        "(recursively) are translated.\n"
        "If unchecked, voxels on all layers will be translated.");
    gui_enabled_end();
    gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                  "Select a layer first.");

    gui_input_int("x", &filter->offset[0], 0, 0);
    gui_input_int("y", &filter->offset[1], 0, 0);
    gui_input_int("z", &filter->offset[2], 0, 0);

    if (gui_button("Reset", -1, 0))
        reset_defaults(filter);
    if (gui_button_primary("Translate", -1, 0))
        apply_translate(filter);

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_translate_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(translate, filter_translate_t,
    .name = "Translate",
    .menu = "image",
    .on_open = on_open,
    .gui_fn = gui,
)
