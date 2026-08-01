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
 * Split a layer's voxels into connected components as child layers under the
 * source (which becomes an empty parent group). Skips the layer entirely if
 * the component count would exceed Max sublayers.
 */

typedef struct {
    filter_t filter;
    int max_sublayers;
    bool allow_diagonals;
} filter_layer_splitting_t;

typedef struct {
    int *pos;   /* packed x,y,z triples */
    int head;
    int count;
    int cap;
} pos_queue_t;

static bool queue_push(pos_queue_t *q, const int pos[3])
{
    int *nbuf;
    int ncap;
    int idx;

    if (q->head + q->count >= q->cap) {
        /* Compact if there is slack at the front, else grow. */
        if (q->head > 0 && q->count > 0) {
            memmove(q->pos, q->pos + q->head * 3,
                    (size_t)q->count * 3 * sizeof(int));
            q->head = 0;
        }
        if (q->head + q->count >= q->cap) {
            ncap = q->cap ? q->cap * 2 : 4096;
            while (ncap < q->head + q->count + 1)
                ncap *= 2;
            nbuf = realloc(q->pos, (size_t)ncap * 3 * sizeof(int));
            if (!nbuf)
                return false;
            q->pos = nbuf;
            q->cap = ncap;
        }
    }
    idx = q->head + q->count;
    q->pos[idx * 3 + 0] = pos[0];
    q->pos[idx * 3 + 1] = pos[1];
    q->pos[idx * 3 + 2] = pos[2];
    q->count++;
    return true;
}

static void queue_pop_front(pos_queue_t *q, int pos[3])
{
    int idx = q->head;
    pos[0] = q->pos[idx * 3 + 0];
    pos[1] = q->pos[idx * 3 + 1];
    pos[2] = q->pos[idx * 3 + 2];
    q->head++;
    q->count--;
    if (q->count == 0)
        q->head = 0;
}

static void queue_free(pos_queue_t *q)
{
    free(q->pos);
    q->pos = NULL;
    q->head = 0;
    q->count = 0;
    q->cap = 0;
}

/* Flood-fill solids from seed into out; mark them in visited.
 * allow_diagonals: 26-connected; otherwise face-adjacent (6-connected). */
static bool flood_component(const volume_t *src, volume_t *visited,
                            const int seed[3], volume_t *out,
                            bool allow_diagonals)
{
    pos_queue_t queue = {0};
    volume_accessor_t src_acc, vis_acc, out_acc;
    uint8_t color[4];
    int pos[3], npos[3];
    int dx, dy, dz;
    bool ok = true;

    src_acc = volume_get_accessor(src);
    vis_acc = volume_get_accessor(visited);
    out_acc = volume_get_accessor(out);

    volume_get_at(src, &src_acc, seed, color);
    if (!color[3])
        return true;
    if (volume_get_alpha_at(visited, &vis_acc, seed))
        return true;

    if (!queue_push(&queue, seed))
        return false;

    volume_set_at(visited, &vis_acc, seed, (uint8_t[]){255, 255, 255, 255});
    volume_set_at(out, &out_acc, seed, color);

    while (queue.count > 0) {
        queue_pop_front(&queue, pos);
        for (dz = -1; dz <= 1; dz++) {
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int manhattan;

                    if (dx == 0 && dy == 0 && dz == 0)
                        continue;
                    manhattan = (dx < 0 ? -dx : dx) +
                                (dy < 0 ? -dy : dy) +
                                (dz < 0 ? -dz : dz);
                    if (!allow_diagonals && manhattan != 1)
                        continue;
                    npos[0] = pos[0] + dx;
                    npos[1] = pos[1] + dy;
                    npos[2] = pos[2] + dz;
                    if (volume_get_alpha_at(visited, &vis_acc, npos))
                        continue;
                    volume_get_at(src, &src_acc, npos, color);
                    if (!color[3])
                        continue;
                    volume_set_at(visited, &vis_acc, npos,
                                  (uint8_t[]){255, 255, 255, 255});
                    volume_set_at(out, &out_acc, npos, color);
                    if (!queue_push(&queue, npos)) {
                        ok = false;
                        goto done;
                    }
                }
            }
        }
    }

done:
    queue_free(&queue);
    return ok;
}

static void free_components(volume_t **comps, int n)
{
    int i;
    if (!comps)
        return;
    for (i = 0; i < n; i++)
        volume_delete(comps[i]);
    free(comps);
}

/*
 * Collect connected components. Returns false on OOM.
 * If more than max_sublayers components exist, *exceeded is set, comps are
 * freed, and *out_n is 0 (caller must not modify the layer).
 */
static bool collect_components(const volume_t *src, int max_sublayers,
                               bool allow_diagonals, volume_t ***out_comps,
                               int *out_n, bool *exceeded)
{
    volume_t *visited = NULL;
    volume_t **comps = NULL;
    int n = 0;
    int cap = 0;
    volume_iterator_t iter;
    int pos[3];
    uint8_t color[4];

    *out_comps = NULL;
    *out_n = 0;
    *exceeded = false;

    visited = volume_new();
    if (!visited)
        return false;

    iter = volume_get_iterator(src, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_t *comp;

        volume_get_at(src, &iter, pos, color);
        if (!color[3])
            continue;
        if (volume_get_alpha_at(visited, NULL, pos))
            continue;

        if (n >= max_sublayers) {
            *exceeded = true;
            free_components(comps, n);
            volume_delete(visited);
            return true;
        }

        comp = volume_new();
        if (!comp ||
            !flood_component(src, visited, pos, comp, allow_diagonals)) {
            volume_delete(comp);
            free_components(comps, n);
            volume_delete(visited);
            return false;
        }

        if (n >= cap) {
            int ncap = cap ? cap * 2 : 8;
            volume_t **nbuf = realloc(comps, (size_t)ncap * sizeof(*comps));
            if (!nbuf) {
                volume_delete(comp);
                free_components(comps, n);
                volume_delete(visited);
                return false;
            }
            comps = nbuf;
            cap = ncap;
        }
        comps[n++] = comp;
    }

    volume_delete(visited);
    *out_comps = comps;
    *out_n = n;
    return true;
}

static bool apply_components(layer_t *parent, volume_t **comps, int n)
{
    layer_t *child;
    layer_t *active_saved;
    char base[256];
    char name[256];
    char suffix[16];
    int i, max_base;

    active_saved = goxel.image->active_layer;

    /* Clear parent first so image_add_child_layer does not peel content. */
    volume_clear(parent->volume);
    parent->collapsed = false;

    for (i = 0; i < n; i++) {
        child = image_add_child_layer(goxel.image, parent);
        if (!child)
            return false;

        volume_set(child->volume, comps[i]);
        child->visible = true;
        child->material = parent->material;
        child->opacity = parent->opacity;
        child->volume_snap = parent->volume_snap;
        memcpy(child->marker_color, parent->marker_color,
               sizeof(child->marker_color));
        mat4_copy(parent->mat, child->mat);

        snprintf(suffix, sizeof(suffix), " %d", i + 1);
        max_base = (int)sizeof(base) - 1 - (int)strlen(suffix);
        if (max_base < 0)
            max_base = 0;
        snprintf(base, sizeof(base), "%.*s%s", max_base, parent->name, suffix);
        make_uniq_name(name, sizeof(name), base, goxel.image,
                       layer_name_exists);
        snprintf(child->name, sizeof(child->name), "%s", name);

        /* image_add_child_layer inserts as topmost; keep earlier components
         * above later ones in the UI. */
        if (i > 0)
            image_reparent_layer_as_last_child(goxel.image, child, parent);
    }

    goxel.image->active_layer = active_saved;
    return true;
}

static int count_layers(const image_t *img)
{
    layer_t *layer;
    int n = 0;
    if (!img)
        return 0;
    DL_COUNT(img->layers, layer, n);
    return n;
}

static int effective_max_sublayers(const filter_layer_splitting_t *filter)
{
    int max_sub = filter->max_sublayers;
    int headroom;

    if (max_sub == -1) {
        headroom = LAYER_SUBTREE_MAX - count_layers(goxel.image);
        return headroom > 0 ? headroom : 0;
    }
    if (max_sub < 1)
        max_sub = 1;
    if (max_sub > LAYER_SUBTREE_MAX - 1)
        max_sub = LAYER_SUBTREE_MAX - 1;
    return max_sub;
}

/* Returns number of child layers created, or 0 if unchanged / failed. */
static int split_layer(filter_layer_splitting_t *filter, layer_t *layer)
{
    volume_t **comps = NULL;
    int n = 0;
    bool exceeded = false;
    int max_sub;

    if (!layer || !layer->volume || volume_is_empty(layer->volume))
        return 0;
    if (layer->shape || layer->image)
        return 0;
    if (layer_has_children(goxel.image, layer))
        return 0;

    max_sub = effective_max_sublayers(filter);
    if (max_sub < 1)
        return 0;

    if (!collect_components(layer->volume, max_sub, filter->allow_diagonals,
                            &comps, &n, &exceeded)) {
        gui_alert("Layer splitting", "Out of memory while splitting layers.");
        return 0;
    }
    if (exceeded) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "\"%.*s\" has more than %d connected regions; "
                 "nothing was split out.",
                 200, layer->name, max_sub);
        gui_alert("Layer splitting", msg);
        return 0;
    }
    /* One (or zero) component: already a single piece. */
    if (n < 2) {
        free_components(comps, n);
        return 0;
    }

    if (!apply_components(layer, comps, n)) {
        gui_alert("Layer splitting",
                  "Could not create child layers (group size limit?).");
        free_components(comps, n);
        return 0;
    }
    free_components(comps, n);
    return n;
}

static int gui(filter_t *filter_)
{
    filter_layer_splitting_t *filter = (void *)filter_;
    layer_t *layer;
    layer_t **targets = NULL;
    int ntargets = 0;
    int cap = 0;
    int i;

    const char *help_text =
        "Split each layer into child layers, one per connected group of "
        "voxels. The original layer becomes an empty parent. If a layer has "
        "more connected regions than Max sublayers, it is left unchanged. "
        "Set Max sublayers to 0 to allow up to the remaining layer budget.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false)) {
        gui_text_wrapped(help_text);
    }

    gui_label_size_push(110);
    {
        bool has_layer = goxel.image && goxel.image->active_layer;

        if (!has_layer)
            filter->filter.current_only = false;
        gui_enabled_begin(has_layer);
        gui_checkbox(
            "Current layer only",
            &filter->filter.current_only,
            "If checked, only the current layer and its children "
            "(recursively) are considered.\n"
            "If unchecked, all layers are considered.");
        gui_enabled_end();
        gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                      "Select a layer first.");
    }

    gui_checkbox(
        "Allow diagonals",
        &filter->allow_diagonals,
        "If checked, voxels that only touch at edges or corners are "
        "treated as connected.\n"
        "If unchecked, only face-adjacent voxels are connected.");

    gui_group_begin(NULL);
    gui_input_int("Max sublayers", &filter->max_sublayers, -1, 512);
    gui_group_end();

    if (gui_button("Split", -1, 0)) {
        /* Snapshot targets first: adding children mutates the layer list. */
        DL_FOREACH(goxel.image->layers, layer) {
            if (filter->filter.current_only &&
                !layer_in_active_subtree(goxel.image, layer))
                continue;
            if (!layer->volume || volume_is_empty(layer->volume))
                continue;
            if (layer->shape || layer->image)
                continue;
            if (layer_has_children(goxel.image, layer))
                continue;
            if (ntargets >= cap) {
                int ncap = cap ? cap * 2 : 16;
                layer_t **nbuf = realloc(targets,
                                        (size_t)ncap * sizeof(*targets));
                if (!nbuf) {
                    free(targets);
                    gui_alert("Layer splitting", "Out of memory.");
                    return 0;
                }
                targets = nbuf;
                cap = ncap;
            }
            targets[ntargets++] = layer;
        }

        if (ntargets > 0) {
            image_history_push(goxel.image);
            for (i = 0; i < ntargets; i++)
                split_layer(filter, targets[i]);
        }
        free(targets);
    }
    gui_label_size_pop();
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_layer_splitting_t *filter = (void *)filter_;
    filter->max_sublayers = -1;
    filter->allow_diagonals = true;
}

FILTER_REGISTER(layer_splitting, filter_layer_splitting_t,
                .name = "Layer splitting",
                .menu = "effects",
                .submenu = "utilities",
                .panel_width = 300,
                .on_open = on_open,
                .gui_fn = gui, )
