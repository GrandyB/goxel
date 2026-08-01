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

/* Height reserved under the scrollable layer list (toolbar, crop, bbox,
 * marker/opacity/snap, shape tools, material). Keep in sync when adding widgets. */
#define LAYERS_PANEL_BOTTOM_RESERVE_PX 290
#define LAYER_DND_TYPE "GOXEL_LAYER_PTR"

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

/* Select a layer; clear brush/shape previews that still belong to the old
 * active layer (hover END is skipped while the mouse is over this panel). */
static void select_layer(image_t *img, layer_t *layer)
{
    if (img->active_layer == layer) return;
    img->active_layer = layer;
    tool_cursor_clear_edit();
    tool_clear_preview();
}

static void clear_layer_selection(image_t *img)
{
    if (!img) return;
    img->active_layer = NULL;
    tool_cursor_clear_edit();
    tool_clear_preview();
}

/* Same as shift+click empty list space: unfocus and frame the image box. */
static void unfocus_and_frame_image(void)
{
    image_clear_layer_focus();
    tool_clear_preview();
    goxel_frame_image_box_in_orbit();
}

/* Indent for in-group drop lines: aligns with the name column at that depth. */
static float layer_dnd_indent(int depth)
{
    if (depth <= 0) return 0.f;
    return (float)depth * 12.f + gui_icon_height(true);
}

/* Ancestor of layer whose depth equals `depth` (layer itself if at depth). */
static layer_t *layer_at_depth(const image_t *img, layer_t *layer, int depth)
{
    while (layer && layer_depth(img, layer) > depth)
        layer = layer_find(img, layer->parent_id);
    return layer;
}

static void try_pending_drop(layer_t **pending_drag, layer_t **pending_target,
                             int *pending_kind, int drop_kind,
                             layer_t *drop_payload, layer_t *target)
{
    if (!drop_kind || !drop_payload || !target) return;
    *pending_drag = drop_payload;
    *pending_target = target;
    *pending_kind = drop_kind;
}

/* Gaps in the spacing above `layer` (or after the list if layer is NULL).
 * When leaving deeper nests, emit last-child gaps per exited level, then
 * (if layer) an insert-above gap for the next row. */
static void render_layer_dnd_gaps(image_t *img, layer_t *prev, int prev_depth,
                                  layer_t *layer, int depth,
                                  layer_t **pending_drag, layer_t **pending_target,
                                  int *pending_kind)
{
    layer_t *drop_payload = NULL;
    layer_t *target;
    int drop_kind;
    int n_exit, n_slots, slot, d;
    float indent;

    /* Top of list: drop line above the first visible row. */
    if (!prev) {
        if (!layer) return;
        gui_dummy(1, 2);
        indent = layer_dnd_indent(depth);
        drop_kind = gui_dnd_gap_target(LAYER_DND_TYPE, &drop_payload,
                                       (int)sizeof(drop_payload), 3.f,
                                       indent, 2, 0, 1);
        try_pending_drop(pending_drag, pending_target, pending_kind,
                         drop_kind, drop_payload, layer);
        return;
    }

    n_exit = (prev_depth > depth) ? (prev_depth - depth) : 0;
    n_slots = n_exit + (layer ? 1 : 0);
    if (n_slots <= 0) return;

    /* Extra space under a child group so the sibling/beneath-parent line is
     * not covered by the next row. */
    if (n_exit > 0)
        gui_dummy(1, 1);

    slot = 0;
    for (d = prev_depth; d > depth; d--) {
        /* Parent that owns the child list at depth d. */
        target = layer_at_depth(img, prev, d - 1);
        indent = layer_dnd_indent(d);
        drop_kind = gui_dnd_gap_target(LAYER_DND_TYPE, &drop_payload,
                                       (int)sizeof(drop_payload), 3.f,
                                       indent, 4, slot, n_slots);
        try_pending_drop(pending_drag, pending_target, pending_kind,
                         drop_kind, drop_payload, target);
        slot++;
    }

    if (layer) {
        indent = layer_dnd_indent(depth);
        drop_kind = gui_dnd_gap_target(LAYER_DND_TYPE, &drop_payload,
                                       (int)sizeof(drop_payload), 3.f,
                                       indent, 2, slot, n_slots);
        try_pending_drop(pending_drag, pending_target, pending_kind,
                         drop_kind, drop_payload, layer);
    }
}

static void apply_layer_drop(layer_t *drag, layer_t *target, int kind)
{
    image_t *img = goxel.image;
    layer_t *parent;
    layer_t *target_first;

    if (!drag || !target || drag == target) return;
    if (layer_is_ancestor(img, drag, target)) return;

    image_history_push(img);

    if (kind == 1) {
        /* Onto: become child of target (topmost under target in UI). */
        image_reparent_layer(img, drag, target, NULL);
        return;
    }

    if (kind == 4) {
        /* Last child under target (target is the parent). */
        image_reparent_layer_as_last_child(img, drag, target);
        return;
    }

    parent = layer_find(img, target->parent_id);
    target_first = first_in_layer_subtree(img->layers, target);

    if (kind == 2) {
        /* Insert above in UI = after target (target is last of its unit). */
        image_reparent_layer(img, drag, parent, target);
    } else if (kind == 3) {
        /* Insert below in UI = before target's subtree in the forward list.
         * Resolve the insert point after unlink so a drag that currently sits
         * just below the target does not fall back to topmost-child. */
        image_reparent_layer_before(img, drag, parent, target_first);
    }
}

static void render_layers_list(void)
{
    image_t *img = goxel.image;
    layer_t *layer;
    layer_t *prev = NULL;
    int prev_depth = 0;
    int idx = 0;
    int drop_kind;
    layer_t *drop_payload = NULL;
    /* Defer reparent until after the list walk — mutating mid-iteration
     * corrupts DL_FOREACH_REVERSE. */
    layer_t *pending_drag = NULL;
    layer_t *pending_target = NULL;
    int pending_kind = 0;

    gui_group_begin(NULL);
    DL_FOREACH_REVERSE(img->layers, layer) {
        int icons_count, icons[8];
        bool visible, current, has_kids;
        int depth;
        char id[32];

        if (!layer_panel_row_visible(img, layer)) continue;

        depth = layer_depth(img, layer);
        has_kids = layer_has_children(img, layer);
        visible = layer->visible;
        current = (img->active_layer == layer);
        icons_count = 0;
        if (layer->base_id) icons[icons_count++] = ICON_LINK;
        if (layer->shape) icons[icons_count++] = ICON_SHAPE;

        snprintf(id, sizeof(id), "lyr%d", layer->id);
        gui_push_id(id);

        render_layer_dnd_gaps(img, prev, prev_depth, layer, depth,
                              &pending_drag, &pending_target, &pending_kind);

        gui_item_group_begin();
        if (depth > 0) {
            gui_spacing_f((float)depth * 12.f);
            gui_same_line();
        }

        if (has_kids) {
            bool fold = false;
            int fold_icon = layer->collapsed ? ICON_CHEVRON_RIGHT
                                             : ICON_ARROW_DOWNWARD;
            if (gui_condensed_selectable_icon("Expand/collapse", &fold, fold_icon))
                layer->collapsed = !layer->collapsed;
            gui_same_line();
        } else {
            gui_spacing_f(gui_icon_height(true));
            gui_same_line();
        }

        {
            float icon_h = gui_icon_height(true);
            float spacing = gui_style_item_spacing_x();
            float trailing = 3.f * (icon_h + spacing);
            bool add_press = false;
            bool lock_press = false;
            bool focus_press = false;
            bool focus_active = image_get_focused_layer(img) == layer;
            int lock_icon = layer->locked ? ICON_LOCKED : ICON_UNLOCKED;

            gui_condensed_layer_item_trailing(
                    idx, icons_count, icons, &visible, &current,
                    layer->name, sizeof(layer->name), trailing,
                    false, false, NULL, false, false, true, NULL);
            if (visible != layer->visible) {
                layer->visible = visible;
                if (gui_is_key_down(KEY_LEFT_SHIFT))
                    toggle_layer_only_visible(layer);
            }
            if (current)
                select_layer(img, layer);

            /* Bind DnD to the layer name row, before the trailing controls so
             * source/target are not stuck on lock / add-child. */
            if (gui_dnd_source(LAYER_DND_TYPE, &layer, (int)sizeof(layer),
                               layer->name)) {
                select_layer(img, layer);
            }
            drop_kind = gui_dnd_target(LAYER_DND_TYPE, &drop_payload,
                                       (int)sizeof(drop_payload));
            if (drop_kind && drop_payload) {
                pending_drag = drop_payload;
                pending_target = layer;
                pending_kind = drop_kind;
            }

            gui_same_line();
            focus_press = focus_active;
            if (gui_condensed_selectable_icon(
                        focus_active ? "Unfocus layer" : "Focus layer",
                        &focus_press, ICON_FOCUS)) {
                if (gui_is_key_down(KEY_LEFT_SHIFT) ||
                    gui_is_key_down(KEY_RIGHT_SHIFT)) {
                    goxel_shift_focus_layer(layer);
                } else {
                    image_toggle_layer_focus(layer);
                    if (image_get_focused_layer(img) == layer)
                        select_layer(img, layer);
                    tool_clear_preview();
                }
            }
            gui_same_line();
            if (gui_condensed_selectable_icon(
                        layer->locked ? "Unlock layer" : "Lock layer",
                        &lock_press, lock_icon)) {
                layer->locked = !layer->locked;
            }
            gui_same_line();
            if (gui_condensed_selectable_icon("Add child", &add_press, ICON_ADD)) {
                image_history_push(img);
                image_add_child_layer(img, layer);
                layer->collapsed = false;
            }
        }
        gui_item_group_end();
        if (gui_is_item_hovered())
            tool_cursor_set_panel_hover(layer);

        gui_pop_id();
        prev = layer;
        prev_depth = depth;
        idx++;
    }

    /* End of list: last-child exits when nested, then always a line beneath
     * the last visible layer. */
    if (prev) {
        gui_push_id("lyr_end_gaps");
        if (prev_depth > 0) {
            render_layer_dnd_gaps(img, prev, prev_depth, NULL, 0,
                                  &pending_drag, &pending_target,
                                  &pending_kind);
        }
        {
            layer_t *drop_payload = NULL;
            float indent = layer_dnd_indent(prev_depth);
            int drop_kind;

            gui_dummy(1, 2);
            drop_kind = gui_dnd_gap_target(LAYER_DND_TYPE, &drop_payload,
                                           (int)sizeof(drop_payload), 3.f,
                                           indent, 3, 0, 1);
            try_pending_drop(&pending_drag, &pending_target, &pending_kind,
                             drop_kind, drop_payload, prev);
        }
        gui_pop_id();
    }

    /* Empty list space: plain click deselects (any tool). Shift+click also
     * clears focus and frames the image box. */
    if (gui_remaining_space_clicked()) {
        image_t *img = goxel.image;
        bool shift = gui_is_key_down(KEY_LEFT_SHIFT) ||
                     gui_is_key_down(KEY_RIGHT_SHIFT);
        if (shift)
            unfocus_and_frame_image();
        clear_layer_selection(img);
    }

    gui_group_end();

    if (pending_kind && pending_drag)
        apply_layer_drop(pending_drag, pending_target, pending_kind);
}

void gui_layers_panel_impl(bool inner_scroll)
{
    layer_t *layer;
    material_t *material;
    bool bounded;
    int bbox[2][3];

    if (inner_scroll) {
        gui_scrollable_begin(gui_get_available_height() -
                              LAYERS_PANEL_BOTTOM_RESERVE_PX);
    }
    render_layers_list();
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
    if (!layer) {
        if (gui_action_button(ACTION_img_new_shape_layer, "New Shape Layer", 1)) {
            action_exec2(ACTION_tool_set_move);
        }
        gui_action_button(ACTION_delete_hidden_layers, "Delete hidden layers", 1);
        gui_group_end();
        return;
    }
    bounded = !box_is_null(layer->box);
    if (bounded) {
        gui_action_button(ACTION_layer_crop_to_box, "Crop to box", 1);
        gui_tooltip_if_hovered(
                "Delete voxels in the active layer that lie outside this "
                "layer's bounding box.");
    }
    if (!box_is_null(goxel.image->box)) {
        gui_action_button(ACTION_layer_crop_to_image, "Crop to image", 1);
        gui_tooltip_if_hovered(
                "Delete voxels in the active layer that lie outside the "
                "image box.");
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
    
    {
        float opc = layer->opacity * 100.f;
        slider_float("Opacity", &opc, 0.f, 100.f, "%.0f %%");
        layer->opacity = clamp(opc / 100.f, 0.f, 1.f);
    }
    gui_checkbox("Volume snap", &layer->volume_snap,
                 "When off, voxels in this layer are ignored for snap to volume");

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
