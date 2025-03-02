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

static void toggle_layer_only_visible(layer_t *layer)
{
    layer_t *other;
    bool others_all_invisible = true;
    DL_FOREACH(goxel.image->layers, other) {
        if (other == layer) continue;
        if (other->visible) {
            others_all_invisible = false;
            break;
        }
    }
    DL_FOREACH(goxel.image->layers, other)
        other->visible = others_all_invisible;
    layer->visible = true;
}

static bool render_layer_item(void *item, int idx, bool current)
{
    layer_t *layer = item;
    int icons_count, icons[8];
    bool visible;
    visible = layer->visible;
    icons_count = 0;
    if (layer->base_id) icons[icons_count++] = ICON_LINK;
    if (layer->shape) icons[icons_count++] = ICON_SHAPE;
    char str[12];  // Enough for any 32-bit int
    sprintf(str, "%d", idx);
    gui_color_small_no_label(str, layer->marker_color);
    gui_same_line();
    gui_condensed_layer_item(idx, icons_count, icons, &visible, &current,
                   layer->name, sizeof(layer->name));
    if (visible != layer->visible) {
        layer->visible = visible;
        if (gui_is_key_down(KEY_LEFT_SHIFT))
            toggle_layer_only_visible(layer);
    }
    return current;
}

void gui_layers_panel_impl(bool inner_scroll)
{
    layer_t *layer;
    material_t *material;
    bool bounded;
    int bbox[2][3];

    if (inner_scroll) {
        gui_scrollable_begin(gui_get_available_height() - 210);
    }
    gui_list(&(gui_list_t) {
        .items = (void**)&goxel.image->layers,
        .current = (void**)&goxel.image->active_layer,
        .render = render_layer_item,
    });
    if (inner_scroll) {
        gui_scrollable_end();
    }

    gui_row_begin(0);
    gui_action_button(ACTION_img_new_layer, NULL, 0);
    gui_action_button(ACTION_img_del_layer, NULL, 0);
    gui_action_button(ACTION_img_merge_layer_down, NULL, 0);
    gui_action_button(ACTION_img_move_layer_up, NULL, 0);
    gui_action_button(ACTION_img_move_layer_down, NULL, 0);
    gui_row_end();

    gui_group_begin(NULL);
    gui_action_button(ACTION_img_duplicate_layer, "Duplicate", 1);
    gui_action_button(ACTION_img_clone_layer, "Clone", 1);
    gui_action_button(ACTION_img_merge_visible_layers, "Merge visible", 1);

    layer = goxel.image->active_layer;
    bounded = !box_is_null(layer->box);
    if (bounded && gui_button("Crop to box", 1, 0)) {
        volume_crop(layer->volume, layer->box);
    }
    if (!box_is_null(goxel.image->box) && gui_button("Crop to image", 1, 0)) {
        volume_crop(layer->volume, goxel.image->box);
    }
    if (layer->shape)
        gui_action_button(ACTION_img_unclone_layer, "To Volume", 1);

    if (gui_action_button(ACTION_img_new_shape_layer, "New Shape Layer", 1)) {
        action_exec2(ACTION_tool_set_move);
    }
    gui_action_button(ACTION_delete_hidden_layers, "Delete hidden layers", 1);

    gui_group_end();

    if (layer->base_id) {
        gui_group_begin(NULL);
        gui_action_button(ACTION_img_unclone_layer, "Unclone", 1);
        gui_action_button(ACTION_img_select_parent_layer, "Select parent", 1);
        gui_group_end();
    }
    if (layer->image) {
        gui_action_button(ACTION_img_image_layer_to_volume, "To Volume", 1);
    }
    if (!layer->shape && gui_checkbox("Bounded", &bounded, NULL)) {
        if (bounded) {
            volume_get_bbox(layer->volume, bbox, true);
            if (bbox[0][0] > bbox[1][0]) memset(bbox, 0, sizeof(bbox));
            bbox_from_aabb(layer->box, bbox);
        } else {
            mat4_copy(mat4_zero, layer->box);
        }
    }
    if (bounded)
        gui_bbox(layer->box);
    
    if (layer) {
        gui_color_small("Marker color", layer->marker_color);
    }

    if (layer->shape) {
        tool_gui_drag_mode(&goxel.tool_drag_mode);
        tool_gui_shape(&layer->shape);
        gui_color("##color", layer->color);
    }

    gui_text("Material");
    if (gui_combo_begin("##material",
                        layer->material ? layer->material->name : NULL)) {
        DL_FOREACH(goxel.image->materials, material) {
            if (gui_combo_item(material->name, material == layer->material))
                layer->material = material;
        }
        gui_combo_end();
    }
}

void gui_layers_panel(void) {
    gui_layers_panel_impl(false);
}
void gui_layers_panel_with_scroll() {
    gui_layers_panel_impl(true);
}
