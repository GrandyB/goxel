/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
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
#include "metadata.h"
#include "xxhash.h"

/* UI session: id of layer solo-focused in the layers panel (0 = none).
 * g_focused_via_shift: true if that focus was applied with Shift (framed). */
static int g_focused_layer_id = 0;
static bool g_focused_via_shift = false;

/* History
    the images undo history is stored in a linked list.  Every time we call
    image_history_push, we add the current image snapshot in the list.

    For example, if we did three operations, A, B, C, and now the image is
    in the D state, the history list looks like this:

    img->history                                        img
        |                                                |
        v                                                v
    +--------+       +--------+       +--------+      +--------+
    |        |       |        |       |        |      |        |
    |   A    |------>|   B    |------>|   C    |----->|   D    |
    |        |       |        |       |        |      |        |
    +--------+       +--------+       +--------+      +--------+

    After an undo, we get:

    img->history                        img
        |                                |
        v                                v
    +--------+       +--------+       +--------+     +--------+
    |        |       |        |       |        |     |        |
    |   A    |------>|   B    |------>|   C    |---->|   D    |
    |        |       |        |       |        |     |        |
    +--------+       +--------+       +--------+     +--------+


*/


bool material_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const material_t *m;
    DL_FOREACH(img->materials, m) {
        if (strcasecmp(m->name, name) == 0) return true;
    }
    return false;
}

bool layer_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        if (strcasecmp(layer->name, name) == 0) return true;
    }
    return false;
}

bool camera_name_exists(void *user, const char *name)
{
    const image_t *img = user;
    const camera_t *cam;
    DL_FOREACH(img->cameras, cam) {
        if (strcasecmp(cam->name, name) == 0) return true;
    }
    return false;
}

void make_uniq_name(
        char *buf, int size, const char *base, void *user,
        bool (*name_exists)(void *user, const char *name))
{
    int i = 1, n, len;
    const char *ext;

    // If base if of the form 'abc.<num>' then we turn it into 'abc'
    len = strlen(base);
    ext = strrchr(base, '.');
    if (ext) {
        if (sscanf(ext, ".%d%*c", &n) == 1) {
            len -= strlen(ext);
            i = n;
        }
    }

    char temp_buf[size];
    snprintf(temp_buf, size, "%s", buf);
    for (;i < 999; i++) {
        snprintf(temp_buf, size, "%.*s.%d", len, base, i);
        LOG_D("Attempting to create a unique name: %s", temp_buf);
        if (!name_exists(user, temp_buf)) {
            LOG_D("Name is unique: %s", temp_buf);
            snprintf(buf, size, "%s", temp_buf);
            return;
        }
    }
    LOG_E("Error: unable to create a unique name, please report to the developer");
}

static int img_get_new_id(const image_t *img);

layer_t *img_get_layer(const image_t *img, int id)
{
    layer_t *layer;
    if (id == 0) return NULL;
    DL_FOREACH(img->layers, layer)
        if (layer->id == id) return layer;
    assert(false);
    return NULL;
}

layer_t *layer_find(const image_t *img, int id)
{
    layer_t *layer;
    if (!img || id == 0) return NULL;
    DL_FOREACH(img->layers, layer)
        if (layer->id == id) return layer;
    return NULL;
}

int layer_depth(const image_t *img, const layer_t *layer)
{
    int depth = 0;
    const layer_t *p;
    if (!img || !layer) return 0;
    for (p = layer_find(img, layer->parent_id); p;
         p = layer_find(img, p->parent_id))
        depth++;
    return depth;
}

void image_clear_layer_focus(void)
{
    g_focused_layer_id = 0;
    g_focused_via_shift = false;
}

void image_toggle_layer_focus(layer_t *layer)
{
    if (!layer) return;
    if (g_focused_layer_id == layer->id) {
        g_focused_layer_id = 0;
        g_focused_via_shift = false;
    } else {
        g_focused_layer_id = layer->id;
        g_focused_via_shift = false;
    }
}

void image_set_layer_focus(layer_t *layer)
{
    g_focused_layer_id = layer ? layer->id : 0;
    g_focused_via_shift = false;
}

void image_set_layer_focus_shift(layer_t *layer)
{
    g_focused_layer_id = layer ? layer->id : 0;
    g_focused_via_shift = layer != NULL;
}

bool image_layer_focus_was_shift(void)
{
    return g_focused_layer_id != 0 && g_focused_via_shift;
}

layer_t *image_get_focused_layer(const image_t *img)
{
    layer_t *layer;
    if (!g_focused_layer_id || !img) return NULL;
    layer = layer_find(img, g_focused_layer_id);
    if (!layer) {
        g_focused_layer_id = 0;
        g_focused_via_shift = false;
    }
    return layer;
}

bool layer_effectively_visible(const image_t *img, const layer_t *layer)
{
    const layer_t *p;
    const layer_t *focused;
    if (!layer) return false;
    /* Focus overrides stored visibility: show the focused layer and its
     * descendants only (ancestors and siblings stay hidden). */
    if (g_focused_layer_id) {
        focused = layer_find(img, g_focused_layer_id);
        return focused && layer_is_ancestor(img, focused, layer);
    }
    if (!layer->visible) return false;
    for (p = layer_find(img, layer->parent_id); p;
         p = layer_find(img, p->parent_id)) {
        if (!p->visible) return false;
    }
    return true;
}

bool layer_is_ancestor(const image_t *img, const layer_t *ancestor,
                       const layer_t *layer)
{
    const layer_t *p;
    if (!img || !ancestor || !layer) return false;
    for (p = layer; p; p = layer_find(img, p->parent_id)) {
        if (p->id == ancestor->id) return true;
    }
    return false;
}

bool layer_in_active_subtree(const image_t *img, const layer_t *layer)
{
    if (!img || !layer || !img->active_layer) return false;
    return layer_is_ancestor(img, img->active_layer, layer);
}

void image_expand_to_show_layer(image_t *img, const layer_t *layer)
{
    layer_t *p;
    if (!img || !layer) return;
    for (p = layer_find(img, layer->parent_id); p;
         p = layer_find(img, p->parent_id)) {
        p->collapsed = false;
    }
}

bool layer_panel_row_visible(const image_t *img, const layer_t *layer)
{
    const layer_t *p;
    if (!img || !layer) return false;
    for (p = layer_find(img, layer->parent_id); p;
         p = layer_find(img, p->parent_id)) {
        if (p->collapsed) return false;
    }
    return true;
}

bool layer_has_children(const image_t *img, const layer_t *layer)
{
    layer_t *other;
    if (!img || !layer) return false;
    DL_FOREACH(img->layers, other) {
        if (other->parent_id == layer->id) return true;
    }
    return false;
}

static bool layer_is_strict_descendant(const image_t *img,
                                       const layer_t *layer,
                                       const layer_t *root)
{
    return layer && root && layer != root &&
           layer_is_ancestor(img, root, layer);
}

/*
 * Children come before their parent in the forward list so reverse UI shows
 * the parent row above its children. Subtree span is [first .. root].
 */
layer_t *first_in_layer_subtree(layer_t *list, const layer_t *root)
{
    layer_t *first = (layer_t *)root;
    image_t tmp = { .layers = list };
    if (!root || !list) return (layer_t *)root;
    while (first != list) {
        layer_t *prev = first->prev;
        if (!prev) break;
        if (!layer_is_strict_descendant(&tmp, prev, root)) break;
        first = prev;
    }
    return first;
}

layer_t *last_in_layer_subtree(layer_t *list, const layer_t *root)
{
    (void)list;
    return (layer_t *)root;
}

static bool layer_has_own_content(const layer_t *layer);

static void layer_subtree_limit_alert(void)
{
    char msg[128];
    snprintf(msg, sizeof(msg),
             "A nested layer group cannot exceed %d layers.",
             LAYER_SUBTREE_MAX);
    gui_alert("Layers", msg);
}

static int layer_subtree_size(const image_t *img, const layer_t *root)
{
    layer_t *first, *cur;
    int n = 0;
    if (!img || !root) return 0;
    first = first_in_layer_subtree(img->layers, root);
    for (cur = first; cur; cur = cur->next) {
        n++;
        if (cur == root) break;
    }
    return n;
}

/* Returns false (and alerts) if adding `add` nodes under parent would exceed
 * LAYER_SUBTREE_MAX. parent_id 0 (top-level) is unrestricted. */
static bool layer_parent_can_grow(image_t *img, int parent_id, int add)
{
    layer_t *parent;
    if (!parent_id || add <= 0) return true;
    parent = layer_find(img, parent_id);
    if (!parent) return true;
    if (layer_subtree_size(img, parent) + add > LAYER_SUBTREE_MAX) {
        layer_subtree_limit_alert();
        return false;
    }
    return true;
}

/* True if nesting `moved` under `new_parent` would exceed LAYER_SUBTREE_MAX.
 * No-op when already in that subtree (reorder only). */
static bool layer_reparent_would_exceed(image_t *img, layer_t *new_parent,
                                        layer_t *moved)
{
    int dest, add, extra = 0;
    if (!new_parent || !moved) return false;
    if (layer_is_ancestor(img, new_parent, moved))
        return false;
    dest = layer_subtree_size(img, new_parent);
    add = layer_subtree_size(img, moved);
    if (!layer_has_children(img, new_parent) &&
        layer_has_own_content(new_parent))
        extra = 1;
    return dest + extra + add > LAYER_SUBTREE_MAX;
}

/* Collect subtree into out[0..n). Returns n, or -1 if larger than max. */
static int collect_layer_subtree(image_t *img, layer_t *root,
                                 layer_t **out, int max)
{
    layer_t *first, *cur;
    int n = 0;
    if (!img || !root || max <= 0) return 0;
    first = first_in_layer_subtree(img->layers, root);
    for (cur = first; cur; cur = cur->next) {
        if (n >= max)
            return -1;
        out[n++] = cur;
        if (cur == root) break;
    }
    return n;
}

/* Like collect_layer_subtree(…, LAYER_SUBTREE_MAX); alerts and returns -1
 * if the group is too large. */
static int collect_layer_subtree_checked(image_t *img, layer_t *root,
                                         layer_t **out)
{
    int n = collect_layer_subtree(img, root, out, LAYER_SUBTREE_MAX);
    if (n < 0)
        layer_subtree_limit_alert();
    return n;
}

/* Insert nodes[0..n) as a contiguous block immediately before `before`.
 * nodes must be in forward-list order (children … root). Always prepend
 * before the same anchor - iterating 0..n-1 yields [nodes…, before].
 * (Iterating n-1..0 would put the root first and break nesting order.) */
static void layer_insert_block_before(image_t *img, layer_t **nodes, int n,
                                      layer_t *before)
{
    int i;
    for (i = 0; i < n; i++) {
        nodes[i]->prev = nodes[i]->next = NULL;
        DL_PREPEND_ELEM(img->layers, before, nodes[i]);
    }
}

static bool layer_has_own_content(const layer_t *layer)
{
    if (!layer) return false;
    if (layer->shape || layer->image || layer->base_id) return true;
    if (layer->volume && !volume_is_empty(layer->volume)) return true;
    return false;
}

layer_t *image_extract_layer_content_to_child(image_t *img, layer_t *parent)
{
    layer_t *child;
    char name[256];

    if (!img || !parent) return NULL;
    if (layer_has_children(img, parent)) return NULL;
    if (!layer_has_own_content(parent)) return NULL;

    snprintf(name, sizeof(name), "%s", parent->name);
    child = layer_new(name);
    make_uniq_name(child->name, sizeof(child->name), name, img,
                   layer_name_exists);
    child->id = img_get_new_id(img);
    child->visible = true;
    child->opacity = parent->opacity;
    child->volume_snap = parent->volume_snap;
    child->material = parent->material;
    child->locked = parent->locked;
    child->parent_id = parent->id;
    memcpy(child->marker_color, parent->marker_color, sizeof(child->marker_color));

    /* Move content from parent into child. */
    volume_delete(child->volume);
    child->volume = parent->volume;
    parent->volume = volume_new();
    mat4_copy(parent->mat, child->mat);
    mat4_set_identity(parent->mat);
    mat4_copy(parent->box, child->box);
    mat4_copy(mat4_zero, parent->box);
    child->shape = parent->shape;
    parent->shape = NULL;
    child->shape_key = parent->shape_key;
    parent->shape_key = 0;
    memcpy(child->color, parent->color, sizeof(child->color));
    child->image = parent->image;
    parent->image = NULL;
    child->base_id = parent->base_id;
    parent->base_id = 0;
    child->base_volume_key = parent->base_volume_key;
    parent->base_volume_key = 0;

    /* Immediately before parent => just below parent in reverse UI. */
    DL_PREPEND_ELEM(img->layers, parent, child);
    return child;
}

layer_t *image_add_child_layer(image_t *img, layer_t *parent)
{
    layer_t *child;
    int need = 1;

    if (!img || !parent) return NULL;
    /* Content peel inserts an extra child before the new empty one. */
    if (!layer_has_children(img, parent) && layer_has_own_content(parent))
        need = 2;
    if (!layer_parent_can_grow(img, parent->id, need))
        return NULL;

    image_extract_layer_content_to_child(img, parent);

    child = layer_new(NULL);
    make_uniq_name(child->name, sizeof(child->name), "Layer", img,
                   layer_name_exists);
    child->visible = true;
    child->id = img_get_new_id(img);
    child->material = img->active_material;
    child->parent_id = parent->id;
    /* Immediately before parent => just below parent in reverse UI. */
    DL_PREPEND_ELEM(img->layers, parent, child);
    img->active_layer = child;
    return child;
}

layer_t *image_ensure_layer_for_adding(image_t *img)
{
    layer_t *layer;
    layer_t *parent;

    if (!img) return NULL;
    layer = img->active_layer;
    if (!layer) return NULL;
    if (!layer_has_children(img, layer))
        return layer;
    parent = layer;
    layer = image_add_child_layer(img, parent);
    if (layer) {
        parent->collapsed = false;
        tool_clear_preview();
    }
    return layer;
}

static void layer_set_uniq_name(image_t *img, layer_t *layer, const char *name)
{
    if (!layer || !name || !name[0]) return;
    if (!layer_name_exists(img, name)) {
        snprintf(layer->name, sizeof(layer->name), "%s", name);
        return;
    }
    make_uniq_name(layer->name, sizeof(layer->name), name, img,
                   layer_name_exists);
}

layer_t *image_ensure_layer_for_generation(
    image_t *img, const char *name, bool replace_current)
{
    layer_t *layer;
    layer_t *parent;
    const char *base = (name && name[0]) ? name : "Layer";

    if (!img) return NULL;

    if (!img->active_layer) {
        layer = layer_new(NULL);
        layer_set_uniq_name(img, layer, base);
        return image_add_layer(img, layer);
    }

    if (replace_current)
        return img->active_layer;

    parent = img->active_layer;
    layer = image_add_child_layer(img, parent);
    if (!layer) return NULL;
    layer_set_uniq_name(img, layer, base);
    parent->collapsed = false;
    tool_clear_preview();
    return layer;
}

void image_sanitize_layer_parents(image_t *img)
{
    layer_t *layer;
    layer_t *parent;
    if (!img) return;
    DL_FOREACH(img->layers, layer) {
        if (!layer->parent_id) continue;
        parent = layer_find(img, layer->parent_id);
        if (!parent || layer_is_ancestor(img, layer, parent))
            layer->parent_id = 0;
    }
}

void image_reparent_layer(image_t *img, layer_t *layer, layer_t *new_parent,
                          layer_t *after_sibling)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *anchor;
    int n, i, new_parent_id;

    if (!img || !layer) return;
    if (new_parent && layer_is_ancestor(img, layer, new_parent))
        return;
    if (after_sibling && layer_is_ancestor(img, layer, after_sibling))
        return;
    if (layer_reparent_would_exceed(img, new_parent, layer)) {
        layer_subtree_limit_alert();
        return;
    }

    /* First child into a content-bearing layer: peel content into a child. */
    if (new_parent && !layer_has_children(img, new_parent))
        image_extract_layer_content_to_child(img, new_parent);

    new_parent_id = new_parent ? new_parent->id : 0;
    n = collect_layer_subtree_checked(img, layer, nodes);
    if (n <= 0) return;

    for (i = 0; i < n; i++)
        DL_DELETE(img->layers, nodes[i]);

    layer->parent_id = new_parent_id;

    if (after_sibling && layer_find(img, after_sibling->id) == after_sibling) {
        anchor = after_sibling;
        for (i = 0; i < n; i++) {
            nodes[i]->prev = nodes[i]->next = NULL;
            DL_APPEND_ELEM(img->layers, anchor, nodes[i]);
            anchor = nodes[i];
        }
    } else if (new_parent) {
        /* Topmost child under parent: insert block immediately before parent. */
        layer_insert_block_before(img, nodes, n, new_parent);
    } else {
        /* Top-level: append at end of list (top of UI). */
        for (i = 0; i < n; i++) {
            nodes[i]->prev = nodes[i]->next = NULL;
            DL_APPEND(img->layers, nodes[i]);
        }
    }
}

void image_reparent_layer_before(image_t *img, layer_t *layer,
                                 layer_t *new_parent, layer_t *before)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    int n, i, new_parent_id;

    if (!img || !layer || !before) return;
    if (new_parent && layer_is_ancestor(img, layer, new_parent))
        return;
    if (before != layer && layer_is_ancestor(img, layer, before))
        return;
    if (layer_reparent_would_exceed(img, new_parent, layer)) {
        layer_subtree_limit_alert();
        return;
    }

    if (new_parent && !layer_has_children(img, new_parent))
        image_extract_layer_content_to_child(img, new_parent);

    new_parent_id = new_parent ? new_parent->id : 0;
    n = collect_layer_subtree_checked(img, layer, nodes);
    if (n <= 0) return;

    for (i = 0; i < n; i++)
        DL_DELETE(img->layers, nodes[i]);

    layer->parent_id = new_parent_id;

    if (!layer_find(img, before->id)) {
        if (new_parent) {
            layer_insert_block_before(img, nodes, n, new_parent);
        } else {
            for (i = 0; i < n; i++) {
                nodes[i]->prev = nodes[i]->next = NULL;
                DL_APPEND(img->layers, nodes[i]);
            }
        }
        return;
    }

    layer_insert_block_before(img, nodes, n, before);
}

void image_reparent_layer_as_last_child(image_t *img, layer_t *layer,
                                        layer_t *parent)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *first;
    int n, i;

    if (!img || !layer || !parent) return;
    if (layer_is_ancestor(img, layer, parent))
        return;
    if (layer_reparent_would_exceed(img, parent, layer)) {
        layer_subtree_limit_alert();
        return;
    }

    if (!layer_has_children(img, parent))
        image_extract_layer_content_to_child(img, parent);

    n = collect_layer_subtree_checked(img, layer, nodes);
    if (n <= 0) return;

    for (i = 0; i < n; i++)
        DL_DELETE(img->layers, nodes[i]);
    layer->parent_id = parent->id;

    /* Remaining first of parent's block = current bottom-most child start;
     * if no children left, first == parent. Prepend block before that. */
    first = first_in_layer_subtree(img->layers, parent);
    layer_insert_block_before(img, nodes, n, first);
}

void image_move_layer_content_subtree(image_t *img, layer_t *layer,
                                      const float mat[4][4], bool only_origin)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    int n, i;
    if (!img || !layer) return;
    n = collect_layer_subtree_checked(img, layer, nodes);
    if (n <= 0) return;
    for (i = 0; i < n; i++)
        do_move_layer(nodes[i], mat, NULL, only_origin);
}

/* Next free layer id, skipping any already reserved in reserved[0..n_reserved).
 * Needed when allocating several layers before they are linked into img->layers. */
static int img_get_new_id_excluding(const image_t *img,
                                    const int *reserved, int n_reserved)
{
    int id, i;
    layer_t *layer;
    for (id = 1;; id++) {
        DL_FOREACH(img->layers, layer)
            if (layer->id == id) break;
        if (layer != NULL) continue;
        for (i = 0; i < n_reserved; i++)
            if (reserved[i] == id) break;
        if (i == n_reserved) return id;
    }
}

static int img_get_new_id(const image_t *img)
{
    return img_get_new_id_excluding(img, NULL, 0);
}

/* Name check for a batch of not-yet-linked layers (plus existing img layers). */
typedef struct {
    image_t *img;
    layer_t **pending;
    int n_pending;
} layer_batch_name_ctx_t;

static bool layer_batch_name_exists(void *user, const char *name)
{
    layer_batch_name_ctx_t *ctx = user;
    int i;
    if (layer_name_exists(ctx->img, name)) return true;
    for (i = 0; i < ctx->n_pending; i++) {
        if (strcasecmp(ctx->pending[i]->name, name) == 0) return true;
    }
    return false;
}

static layer_t *layer_clone(layer_t *other)
{
    int len;
    layer_t *layer;

    assert(other);
    layer = calloc(1, sizeof(*layer));
    len = sizeof(layer->name) - 1 - strlen(" clone");
    snprintf(layer->name, sizeof(layer->name), "%.*s clone", len, other->name);
    layer->visible = other->visible;
    layer->material = other->material;
    layer->volume = volume_copy(other->volume);
    mat4_set_identity(layer->mat);
    layer->base_id = other->id;
    layer->base_volume_key = volume_get_key(other->volume);
    layer->opacity = other->opacity;
    layer->volume_snap = other->volume_snap;
    layer->locked = other->locked;
    return layer;
}

// Make sure the layer volume is up to date.
void image_update(image_t *img)
{
    painter_t painter = {};
    uint32_t key;
    layer_t *layer, *base;

    DL_FOREACH(img->layers, layer) {
        base = layer->base_id ? layer_find(img, layer->base_id) : NULL;
        if (base && layer->base_volume_key != volume_get_key(base->volume) &&
            layer->visible) {
            volume_set(layer->volume, base->volume);
            volume_move(layer->volume, layer->mat);
            layer->base_volume_key = volume_get_key(base->volume);
        }
        if (layer->shape) {
            key = XXH32(layer->mat, sizeof(layer->mat), 0);
            key = XXH32(layer->shape, sizeof(layer->shape), key);
            key = XXH32(layer->color, sizeof(layer->color), key);
            if (key != layer->shape_key) {
                painter.mode = MODE_OVER;
                painter.shape = layer->shape;
                painter.box = &goxel.image->box;
                vec4_copy(layer->color, painter.color);
                volume_clear(layer->volume);
                volume_op(layer->volume, &painter, layer->mat);
                layer->shape_key = key;
            }
        }
    }
}

image_t *image_new(void)
{
    image_t *img = calloc(1, sizeof(*img));
    img->ref = 1;
    img->recent_color_count = 0;
    img->custom_objects = NULL;
    img->custom_objects_show_when_closed = false;
    const int aabb[2][3] = {{-16, -16, 0}, {16, 16, 32}};
    bbox_from_aabb(img->box, aabb);
    img->export_width = 1024;
    img->export_height = 1024;
    image_add_material(img, NULL);
    image_add_camera(img, NULL);
    image_add_layer(img, NULL);
    DL_APPEND2(img->history, img, history_prev, history_next);
    // Prevent saving an empty image.
    img->saved_key = image_get_key(img);
    return img;
}

/* Same 6-char #RRGGBB identity; alpha and noise are ignored for deduplication. */
static bool image_recent_color_same_hex_rgb(
        const image_recent_color_t *a, const uint8_t rgb[3])
{
    return a->color[0] == rgb[0] && a->color[1] == rgb[1] && a->color[2] == rgb[2];
}

void image_recent_color_push_from_painter(
        image_t *img, const struct painter *p0)
{
    const painter_t *p = (const painter_t *)p0;
    image_recent_color_t e, tmp[GOXEL_RECENT_COLOR_HISTORY_MAX];
    int n, w, i;

    if (!img || !p) return;
    memcpy(e.color, p->color, 4);
    e.noise_enabled = p->noise_enabled;
    e.noise_intensity = p->noise_intensity;
    e.noise_saturation = p->noise_saturation;
    e.noise_coverage = p->noise_coverage;

    /* New entry first, then rest; drop any older entry with the same #RRGGBB
     * so changing noise/alpha reuses one slot. */
    w = 0;
    tmp[w++] = e;
    n = img->recent_color_count;
    for (i = 0; i < n && w < GOXEL_RECENT_COLOR_HISTORY_MAX; i++) {
        if (image_recent_color_same_hex_rgb(&img->recent_colors[i], e.color))
            continue;
        tmp[w++] = img->recent_colors[i];
    }
    for (i = 0; i < w; i++)
        img->recent_colors[i] = tmp[i];
    img->recent_color_count = w;
}

void painter_color_apply_rgb_keep_alpha(uint8_t dst[4], const uint8_t src[4])
{
    const uint8_t alpha = dst[3];
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = alpha;
}

void image_recent_color_apply_to_goxel_painter(
        const image_t *img, int idx, bool rgb_only)
{
    const image_recent_color_t *e;
    if (!img || idx < 0 || idx >= img->recent_color_count) return;
    e = &img->recent_colors[idx];
    if (rgb_only) {
        painter_color_apply_rgb_keep_alpha(goxel.painter.color, e->color);
        return;
    }
    memcpy(goxel.painter.color, e->color, 4);
    goxel.painter.noise_enabled = e->noise_enabled;
    goxel.painter.noise_intensity = e->noise_intensity;
    goxel.painter.noise_saturation = e->noise_saturation;
    goxel.painter.noise_coverage = e->noise_coverage;
}

void image_recent_color_remove_at(image_t *img, int idx)
{
    int n, tail;
    if (!img || idx < 0 || idx >= img->recent_color_count) return;
    n = img->recent_color_count;
    tail = n - idx - 1;
    if (tail > 0) {
        memmove(&img->recent_colors[idx], &img->recent_colors[idx + 1],
                (size_t)tail * sizeof(image_recent_color_t));
    }
    img->recent_color_count = n - 1;
}

/*
 * Generate a copy of the image that can be put into the history.
 */
static image_t *image_snap(image_t *other)
{
    image_t *img;
    layer_t *layer, *other_layer;
    camera_t *camera, *other_camera;
    material_t *material, *other_material;

    img = calloc(1, sizeof(*img));
    *img = *other;

    /* Break shared pointer immediately - do not free the live list. */
    img->custom_objects = NULL;

    img->layers = NULL;
    img->active_layer = NULL;
    DL_FOREACH(other->layers, other_layer) {
        layer = layer_copy(other_layer);
        DL_APPEND(img->layers, layer);
        if (other_layer == other->active_layer)
            img->active_layer = layer;
    }
    /* active_layer may be NULL (e.g. Cursor tool with nothing selected). */

    img->cameras = NULL;
    img->active_camera = NULL;
    DL_FOREACH(other->cameras, other_camera) {
        camera = camera_copy(other_camera);
        DL_APPEND(img->cameras, camera);
        if (other_camera == other->active_camera)
            img->active_camera = camera;
    }

    img->materials = NULL;
    img->active_material = NULL;
    DL_FOREACH(other->materials, other_material) {
        material = material_copy(other_material);
        DL_APPEND(img->materials, material);
        if (other_material == other->active_material)
            img->active_material = material;
        DL_FOREACH(img->layers, layer) {
            if (layer->material == other_material)
                layer->material = material;
        }
    }

    custom_objects_copy_list(&img->custom_objects, other->custom_objects);

    img->history = img->history_next = img->history_prev = NULL;
    return img;
}


void image_delete(image_t *img)
{
    image_t *hist, *snap, *snap_tmp;
    camera_t *cam;
    layer_t *layer;
    material_t *mat;

    if (!img) return;
    if (--img->ref > 0) return;

    /* Session focus is global; only clear when destroying the live document.
     * History snaps also go through here (redo discard on push) and must not
     * wipe focus. */
    if (img == goxel.image)
        image_clear_layer_focus();

    while ((layer = img->layers)) {
        DL_DELETE(img->layers, layer);
        layer_delete(layer);
    }
    while ((cam = img->cameras)) {
        DL_DELETE(img->cameras, cam);
        camera_delete(cam);
    }
    while ((mat = img->materials)) {
        DL_DELETE(img->materials, mat);
        material_delete(mat);
    }
    custom_objects_free_list(&img->custom_objects);

    // Path is shared between images and snaps!
    // XXX: find a better way.
    if (img->history) {
        free(img->path);
        img->path = NULL;
    }

    hist = img->history;
    DL_FOREACH_SAFE2(hist, snap, snap_tmp, history_next) {
        if (snap == img) continue;
        DL_DELETE2(hist, snap, history_prev, history_next);
        image_delete(snap);
    }

    free(img);
}

/* Link *layer* onto *img->layers*. Loaders and image_add_layer append in file
 * order. The UI "new layer" command passes below_active so the row appears
 * just under the selection (see gui_list's DL_FOREACH_REVERSE order). */
static void image_link_layer_on_stack(image_t *img, layer_t *layer,
                                      bool below_active)
{
    if (below_active && img->active_layer)
        DL_PREPEND_ELEM(img->layers, img->active_layer, layer);
    else
        DL_APPEND(img->layers, layer);
}

static layer_t *image_add_layer_impl(image_t *img, layer_t *layer,
                                     bool below_active)
{
    layer_t *active;

    assert(img);
    active = img->active_layer;
    if (active && !layer_parent_can_grow(img, active->parent_id, 1))
        return NULL;
    if (!layer) {
        layer = layer_new(NULL);
        make_uniq_name(layer->name, sizeof(layer->name), "Layer", img,
                       layer_name_exists);
    }
    layer->visible = true;
    layer->id = img_get_new_id(img);
    layer->material = img->active_material;
    if (active)
        layer->parent_id = active->parent_id;
    if (below_active && active) {
        /* Visually below active's block = before the subtree's first node. */
        layer_t *first = first_in_layer_subtree(img->layers, active);
        DL_PREPEND_ELEM(img->layers, first, layer);
    } else {
        DL_APPEND(img->layers, layer);
    }
    img->active_layer = layer;
    return layer;
}

layer_t *image_add_layer(image_t *img, layer_t *layer)
{
    return image_add_layer_impl(img, layer, false);
}

layer_t *image_add_layer_below_active(image_t *img, layer_t *layer)
{
    return image_add_layer_impl(img, layer, true);
}

static layer_t *shape_layer_new_from_ui_state(image_t *img)
{
    layer_t *layer;

    layer = layer_new("shape");
    layer->visible = true;
    layer->shape = &shape_sphere;
    vec4_copy(goxel.painter.color, layer->color);
    // If the selection is on use it, otherwise center it in the image.
    if (!box_is_null(goxel.selection)) {
        mat4_copy(goxel.selection, layer->mat);
    } else {
        vec3_copy(img->box[3], layer->mat[3]);
        mat4_iscale(layer->mat, 4, 4, 4);
    }
    layer->id = img_get_new_id(img);
    return layer;
}

layer_t *image_add_shape_layer(image_t *img)
{
    layer_t *layer;

    assert(img);
    if (img->active_layer &&
        !layer_parent_can_grow(img, img->active_layer->parent_id, 1))
        return NULL;
    layer = shape_layer_new_from_ui_state(img);
    if (img->active_layer)
        layer->parent_id = img->active_layer->parent_id;
    image_link_layer_on_stack(img, layer, false);
    img->active_layer = layer;
    return layer;
}

layer_t *image_add_shape_layer_below_active(image_t *img)
{
    layer_t *layer;

    assert(img);
    if (img->active_layer &&
        !layer_parent_can_grow(img, img->active_layer->parent_id, 1))
        return NULL;
    layer = shape_layer_new_from_ui_state(img);
    if (img->active_layer)
        layer->parent_id = img->active_layer->parent_id;
    image_link_layer_on_stack(img, layer, true);
    img->active_layer = layer;
    return layer;
}

void image_delete_layer(image_t *img, layer_t *layer)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *other;
    int n, i;
    bool active_in_subtree = false;

    assert(img);
    assert(layer);

    /* Delete the layer and every descendant. Subtree is contiguous in the
     * forward list: [first .. layer]. Undo restores the full snapshot. */
    n = collect_layer_subtree(img, layer, nodes, LAYER_SUBTREE_MAX);
    if (n < 0) {
        /* Oversized group (should not happen if LAYER_SUBTREE_MAX is
         * enforced on nest). Peel leaves until the root fits. */
        for (;;) {
            layer_t *leaf = NULL;
            DL_FOREACH(img->layers, other) {
                if (other == layer) continue;
                if (!layer_is_ancestor(img, layer, other)) continue;
                if (layer_has_children(img, other)) continue;
                leaf = other;
                break;
            }
            if (!leaf) break;
            image_delete_layer(img, leaf);
        }
        n = collect_layer_subtree(img, layer, nodes, LAYER_SUBTREE_MAX);
    }
    if (n <= 0) {
        nodes[0] = layer;
        n = 1;
    }

    for (i = 0; i < n; i++) {
        if (nodes[i] == img->active_layer)
            active_in_subtree = true;
        if (g_focused_layer_id == nodes[i]->id) {
            g_focused_layer_id = 0;
            g_focused_via_shift = false;
        }
    }

    for (i = 0; i < n; i++)
        DL_DELETE(img->layers, nodes[i]);

    if (active_in_subtree)
        img->active_layer = NULL;

    DL_FOREACH(img->layers, other) {
        for (i = 0; i < n; i++) {
            if (other->base_id == nodes[i]->id) {
                other->base_id = 0;
                break;
            }
        }
    }

    for (i = 0; i < n; i++)
        layer_delete(nodes[i]);

    if (img->layers == NULL) {
        layer = layer_new("unnamed");
        layer->visible = true;
        layer->id = img_get_new_id(img);
        DL_APPEND(img->layers, layer);
    }
}

/* Walk node up until it is a direct child of parent_id (a sibling unit of
 * the mover). NULL if we hit parent_id itself or leave that family. */
static layer_t *layer_sibling_root(image_t *img, layer_t *node, int parent_id)
{
    while (node) {
        if (node->parent_id == parent_id)
            return node;
        if (parent_id != 0 && node->id == parent_id)
            return NULL;
        if (node->parent_id == 0)
            return NULL;
        node = layer_find(img, node->parent_id);
    }
    return NULL;
}

static void layer_relink_after(image_t *img, layer_t **nodes, int n,
                               layer_t *anchor)
{
    int i;
    for (i = 0; i < n; i++) {
        nodes[i]->prev = nodes[i]->next = NULL;
        DL_APPEND_ELEM(img->layers, anchor, nodes[i]);
        anchor = nodes[i];
    }
}

static void layer_relink_before(image_t *img, layer_t **nodes, int n,
                                layer_t *before)
{
    layer_insert_block_before(img, nodes, n, before);
}

void image_move_layer_subtree(image_t *img, layer_t *layer, int d)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *first, *parent, *neighbor, *sib;
    int n, i;

    assert(img);
    assert(layer);
    assert(d == -1 || d == +1);

    n = collect_layer_subtree_checked(img, layer, nodes);
    if (n <= 0) return;
    first = nodes[0];
    parent = layer_find(img, layer->parent_id);

    if (d == -1) {
        /* Up in UI = toward list tail. */
        neighbor = layer->next;
        if (!neighbor) return;

        if (parent && neighbor == parent) {
            /* Topmost child: exit above parent (sibling just above parent). */
            for (i = 0; i < n; i++)
                DL_DELETE(img->layers, nodes[i]);
            layer->parent_id = parent->parent_id;
            layer_relink_after(img, nodes, n, parent);
            return;
        }

        sib = layer_sibling_root(img, neighbor, layer->parent_id);
        if (!sib) return;
        for (i = 0; i < n; i++)
            DL_DELETE(img->layers, nodes[i]);
        layer->parent_id = sib->parent_id;
        layer_relink_after(img, nodes, n, sib);
        return;
    }

    /* Down in UI = toward list head. */
    neighbor = (first == img->layers) ? NULL : first->prev;
    sib = neighbor ? layer_sibling_root(img, neighbor, layer->parent_id)
                   : NULL;
    if (sib) {
        layer_t *sib_first = first_in_layer_subtree(img->layers, sib);
        for (i = 0; i < n; i++)
            DL_DELETE(img->layers, nodes[i]);
        layer->parent_id = sib->parent_id;
        layer_relink_before(img, nodes, n, sib_first);
        return;
    }

    if (!parent) return;

    /* Bottommost child: exit below parent (sibling just under parent block). */
    for (i = 0; i < n; i++)
        DL_DELETE(img->layers, nodes[i]);
    layer->parent_id = parent->parent_id;
    layer_relink_before(img, nodes, n,
                        first_in_layer_subtree(img->layers, parent));
}

static void image_move_layer(image_t *img, layer_t *layer, int d)
{
    image_move_layer_subtree(img, layer, d);
}

layer_t *image_duplicate_layer(image_t *img, layer_t *other)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *copies[LAYER_SUBTREE_MAX];
    int n, i, j;
    int id_map_from[LAYER_SUBTREE_MAX], id_map_to[LAYER_SUBTREE_MAX];
    layer_batch_name_ctx_t name_ctx;

    assert(img);
    assert(other);

    if (!layer_parent_can_grow(img, other->parent_id,
                               layer_subtree_size(img, other)))
        return NULL;

    n = collect_layer_subtree_checked(img, other, nodes);
    if (n <= 0) return NULL;

    name_ctx.img = img;
    name_ctx.pending = copies;
    for (i = 0; i < n; i++) {
        copies[i] = layer_copy(nodes[i]);
        name_ctx.n_pending = i;
        make_uniq_name(copies[i]->name, sizeof(copies[i]->name),
                       nodes[i]->name, &name_ctx, layer_batch_name_exists);
        copies[i]->visible = true;
        copies[i]->id = img_get_new_id_excluding(img, id_map_to, i);
        id_map_from[i] = nodes[i]->id;
        id_map_to[i] = copies[i]->id;
        copies[i]->prev = copies[i]->next = NULL;
    }

    /* Place copy block immediately before `other`'s first (visually below
     * the whole other subtree in reverse UI). */
    layer_insert_block_before(img, copies, n, nodes[0]);

    for (i = 0; i < n; i++) {
        if (copies[i]->parent_id) {
            for (j = 0; j < n; j++) {
                if (id_map_from[j] == copies[i]->parent_id) {
                    copies[i]->parent_id = id_map_to[j];
                    break;
                }
            }
        }
        if (copies[i]->base_id) {
            for (j = 0; j < n; j++) {
                if (id_map_from[j] == copies[i]->base_id) {
                    copies[i]->base_id = id_map_to[j];
                    break;
                }
            }
        }
    }

    /* Active = copy of the root (last in nodes / copies). */
    img->active_layer = copies[n - 1];
    return copies[n - 1];
}

layer_t *image_clone_layer(image_t *img, layer_t *other)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *clones[LAYER_SUBTREE_MAX];
    int n, i, j;
    int id_map_from[LAYER_SUBTREE_MAX], id_map_to[LAYER_SUBTREE_MAX];
    layer_batch_name_ctx_t name_ctx;

    img = img ?: goxel.image;
    other = other ?: img->active_layer;
    assert(img && other);

    if (!layer_parent_can_grow(img, other->parent_id,
                               layer_subtree_size(img, other)))
        return NULL;

    n = collect_layer_subtree_checked(img, other, nodes);
    if (n <= 0) return NULL;

    name_ctx.img = img;
    name_ctx.pending = clones;
    for (i = 0; i < n; i++) {
        clones[i] = layer_clone(nodes[i]);
        name_ctx.n_pending = i;
        if (layer_batch_name_exists(&name_ctx, clones[i]->name)) {
            make_uniq_name(clones[i]->name, sizeof(clones[i]->name),
                           clones[i]->name, &name_ctx, layer_batch_name_exists);
        }
        clones[i]->visible = true;
        clones[i]->collapsed = nodes[i]->collapsed;
        clones[i]->parent_id = nodes[i]->parent_id;
        clones[i]->id = img_get_new_id_excluding(img, id_map_to, i);
        id_map_from[i] = nodes[i]->id;
        id_map_to[i] = clones[i]->id;
        clones[i]->prev = clones[i]->next = NULL;
    }

    /* Place clone block immediately before `other`'s first (visually below
     * the whole other subtree in reverse UI). */
    layer_insert_block_before(img, clones, n, nodes[0]);

    /* Remap parent_id within the new block only. Keep base_id pointing at
     * the originals so clones stay live-linked. */
    for (i = 0; i < n; i++) {
        if (!clones[i]->parent_id) continue;
        for (j = 0; j < n; j++) {
            if (id_map_from[j] == clones[i]->parent_id) {
                clones[i]->parent_id = id_map_to[j];
                break;
            }
        }
    }

    img->active_layer = clones[n - 1];
    return clones[n - 1];
}

void image_delete_hidden_layers(image_t *img) {
    layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        if (!layer->visible) {
            DL_DELETE(img->layers, layer);
        }
    }
}

void image_unclone_layer(image_t *img, layer_t *layer)
{
    assert(img);
    assert(layer);
    layer->base_id = 0;
    layer->shape = NULL;
}

void image_merge_visible_layers(image_t *img)
{
    layer_t *layer, *other, *last = NULL;
    assert(img);
    DL_FOREACH(img->layers, layer) {
        if (!layer_effectively_visible(img, layer)) continue;
        image_unclone_layer(img, layer);

        if (last) {
            DL_FOREACH(img->layers, other) {
                if (other->base_id == last->id)
                    other->base_id = 0;
            }
            SWAP(layer->volume, last->volume);
            volume_merge(layer->volume, last->volume, MODE_OVER, NULL);
            DL_DELETE(img->layers, last);
            layer_delete(last);
        }
        last = layer;
    }
    if (last) {
        last->parent_id = 0;
        last->collapsed = false;
        img->active_layer = last;
        /* Clear remaining layers' parent links into deleted ids via sanitize. */
        image_sanitize_layer_parents(img);
    }
}

void image_merge_layer_down(image_t *img) {
    assert(img);
    layer_t *active_layer = img->active_layer;
    layer_t *previous = NULL, *layer = NULL;
    bool next = false;
    DL_FOREACH(img->layers, layer) {
        next = layer == active_layer;
        if (next && previous != NULL) {
            if (previous->parent_id != active_layer->parent_id)
                break;
            if (layer_is_ancestor(img, previous, active_layer) ||
                layer_is_ancestor(img, active_layer, previous))
                break;
            previous->visible = true;
            image_unclone_layer(img, previous);
            image_unclone_layer(img, layer);
            SWAP(layer->volume, previous->volume);
            volume_merge(layer->volume, previous->volume, MODE_OVER, NULL);
            memcpy(&layer->name, previous->name, sizeof(layer->name));
            DL_DELETE(img->layers, previous);
            layer_delete(previous);
            image_sanitize_layer_parents(img);
            break;
        } else if (next) {
            break;
        }
        previous = layer;
    }
   img->active_layer = active_layer;
}

/* Delete all descendants of parent; parent remains. Clears base_id on any
 * remaining layers that pointed at the deleted ids. */
static void image_delete_layer_descendants(image_t *img, layer_t *parent)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *other;
    int n, i;

    assert(img && parent);
    n = collect_layer_subtree_checked(img, parent, nodes);
    if (n <= 1) return;

    for (i = 0; i < n - 1; i++) {
        if (g_focused_layer_id == nodes[i]->id) {
            g_focused_layer_id = 0;
            g_focused_via_shift = false;
        }
        if (img->active_layer == nodes[i])
            img->active_layer = parent;
        DL_DELETE(img->layers, nodes[i]);
    }

    DL_FOREACH(img->layers, other) {
        for (i = 0; i < n - 1; i++) {
            if (other->base_id == nodes[i]->id) {
                other->base_id = 0;
                break;
            }
        }
    }

    for (i = 0; i < n - 1; i++)
        layer_delete(nodes[i]);

    parent->collapsed = false;
}

void image_merge_layer_children(image_t *img, layer_t *parent)
{
    layer_t *nodes[LAYER_SUBTREE_MAX];
    layer_t *peers[LAYER_SUBTREE_MAX];
    layer_t *other, *merge_target, *base;
    volume_t *merged;
    int n, i, n_peers;

    assert(img);
    if (!parent || !layer_has_children(img, parent)) return;

    /* Prefer flattening the clone base when the selection is a clone of a
     * still-nested parent. That keeps live links and lets peers just drop
     * matching children instead of baking a second merged volume. */
    merge_target = parent;
    if (parent->base_id) {
        base = layer_find(img, parent->base_id);
        if (base && layer_has_children(img, base))
            merge_target = base;
    }

    /* Bake shape/clone volumes before we flatten. */
    image_update(img);

    n = collect_layer_subtree_checked(img, merge_target, nodes);
    if (n <= 1) {
        if (merge_target == parent) return;
        merge_target = parent;
        n = collect_layer_subtree_checked(img, merge_target, nodes);
        if (n <= 1) return;
    }

    for (i = 0; i < n; i++) {
        if (nodes[i]->image)
            image_image_layer_to_volume(img, nodes[i]);
        image_unclone_layer(img, nodes[i]);
    }

    /* Forward order: children first, parent last (same as move-gizmo merge). */
    merged = volume_new();
    for (i = 0; i < n; i++) {
        if (nodes[i]->volume)
            volume_merge(merged, nodes[i]->volume, MODE_OVER, NULL);
    }
    volume_delete(merge_target->volume);
    merge_target->volume = merged;
    merge_target->collapsed = false;

    /* Delete every descendant; merge_target is nodes[n - 1]. */
    for (i = 0; i < n - 1; i++) {
        if (g_focused_layer_id == nodes[i]->id) {
            g_focused_layer_id = 0;
            g_focused_via_shift = false;
        }
        if (img->active_layer == nodes[i])
            img->active_layer = merge_target;
        DL_DELETE(img->layers, nodes[i]);
    }

    DL_FOREACH(img->layers, other) {
        for (i = 0; i < n - 1; i++) {
            if (other->base_id == nodes[i]->id) {
                other->base_id = 0;
                break;
            }
        }
    }

    for (i = 0; i < n - 1; i++)
        layer_delete(nodes[i]);

    /* Clone peers (and the invoked layer if we flattened its base) keep the
     * same hierarchy: strip their children so they match the flat parent. */
    n_peers = 0;
    DL_FOREACH(img->layers, other) {
        if (other == merge_target) continue;
        if (other->base_id != merge_target->id && other != parent) continue;
        if (!layer_has_children(img, other)) continue;
        if (n_peers < LAYER_SUBTREE_MAX)
            peers[n_peers++] = other;
    }
    for (i = 0; i < n_peers; i++) {
        image_delete_layer_descendants(img, peers[i]);
        if (peers[i]->base_id == merge_target->id)
            peers[i]->base_volume_key = 0;
    }

    img->active_layer = layer_find(img, parent->id);
    if (!img->active_layer)
        img->active_layer = merge_target;
}

camera_t *image_add_camera(image_t *img, camera_t *cam)
{
    assert(img);
    if (!cam) {
        cam = camera_new(NULL);
        make_uniq_name(cam->name, sizeof(cam->name), "Camera", img,
                       camera_name_exists);
    }
    DL_APPEND(img->cameras, cam);
    img->active_camera = cam;
    return cam;
}

void image_delete_camera(image_t *img, camera_t *cam)
{
    img = img ?: goxel.image;
    cam = cam ?: img->active_camera;
    if (!cam) return;
    DL_DELETE(img->cameras, cam);
    if (cam == img->active_camera)
        img->active_camera = img->cameras;
    camera_delete(cam);
}

static void image_move_camera(image_t *img, camera_t *cam, int d)
{
    // XXX: make a generic algo to move objects in a list.
    assert(d == -1 || d == +1);
    camera_t *other = NULL;
    img = img ?: goxel.image;
    cam = cam ?: img->active_camera;
    if (!cam) return;
    if (d == -1) {
        other = cam->next;
        SWAP(other, cam);
    } else if (cam != img->cameras) {
        other = cam->prev;
    }
    if (!other || !cam) return;
    DL_DELETE(img->cameras, cam);
    DL_PREPEND_ELEM(img->cameras, other, cam);
}

material_t *image_add_material(image_t *img, material_t *mat)
{
    img = img ?: goxel.image;
    if (!mat) {
        mat = material_new(NULL);
        make_uniq_name(mat->name, sizeof(mat->name), "Material", img,
                       material_name_exists);
    }
    assert(!mat->prev);
    DL_APPEND(img->materials, mat);
    img->active_material = mat;
    return mat;
}

void image_delete_material(image_t *img, material_t *mat)
{
    layer_t *layer;

    img = img ?: goxel.image;
    mat = mat ?: img->active_material;
    if (!mat) return;
    DL_DELETE(img->materials, mat);
    if (mat == img->active_material) img->active_material = NULL;
    material_delete(mat);
    DL_FOREACH(img->layers, layer)
        if (layer->material == mat) layer->material = NULL;
}

void image_auto_resize(image_t *img)
{
    float box[4][4] = {}, layer_box[4][4];
    layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        layer_get_bounding_box(layer, layer_box);
        box_union(box, layer_box, box);
    }
    mat4_copy(box, img->box);
}

static void a_image_auto_resize(void)
{
    image_auto_resize(goxel.image);
}

void image_auto_resize_reset(image_t *img)
{
    float box[4][4] = {}, layer_box[4][4];

    // Collect a bounding box from all _visible_ layers
    layer_t *layer;
    DL_FOREACH(goxel.image->layers, layer) {
        if (layer->visible) {
            layer_get_bounding_box(layer, layer_box);
            box_union(box, layer_box, box);
        }
    }
    //image_merge_visible_layers(img);
    //debug_log_44_matrix("box (pre)", box);

    float trans[4][4] = MAT4_IDENTITY;
    // distance from origin + local origin
    trans[3][0] = -box[3][0] + box[0][0]; // -originX + sizeX*0.5
    trans[3][1] = -box[3][1] + box[1][1]; // -originY + sizeY*0.5
    trans[3][2] = -box[3][2] + box[2][2]; // -originZ + sizeZ*0.5
    //debug_log_44_matrix("trans", trans);

    box[3][0] = box[0][0];
    box[3][1] = box[1][1];
    box[3][2] = box[2][2];

    //debug_log_44_matrix("img->box", img->box);
    //debug_log_44_matrix("box (post)", box);

    mat4_copy(box, img->box);

    // Origin etc has moved, move volume with it
    DL_FOREACH(img->layers, layer) {
        if (layer->visible) {
            volume_move(layer->volume, trans);
        }
    }
}

void image_set_image_dimensions_and_center(image_t *img, int w, int h, int d) {
    float box[4][4];
    float trans[4][4] = MAT4_IDENTITY;
    mat4_copy(img->box, box);

    // Find difference between current origin and new origin
    trans[3][0] = (-box[3][0] + box[0][0] - w/2); // x = 12, w = 512, new = -128, diff = -12-w/2
    trans[3][1] = (-box[3][1] + box[1][1] - h/2);
    trans[3][2] = (-box[3][2] + box[2][2] - d/2);

    // Set new dimensions
    box[0][0] = w/2;
    box[1][1] = h/2;
    box[2][2] = d/2;
    box[3][0] = 0;
    box[3][1] = 0;
    box[3][2] = 0;
    //debug_log_44_matrix("box", box);
    mat4_copy(box, img->box);

    // Move all layers by the difference
    layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        volume_move(layer->volume, trans);
    }
}

void image_z_range(const image_t *img, int *z0, int *z1)
{
    int start[3], dims[3];
    float box[4][4];
    if (box_is_null(img->box)) {
        *z0 = 0;
        *z1 = 31;
        return;
    }
    mat4_copy(img->box, box);
    box_get_start_pos(box, start);
    box_get_dimensions(box, dims);
    *z0 = start[2];
    *z1 = start[2] + dims[2] - 1;
    if (*z1 < *z0) *z1 = *z0;
}

void image_bottom_left(const image_t *img, int out[3])
{
    float box[4][4];
    if (box_is_null(img->box)) {
        out[0] = out[1] = out[2] = 0;
        return;
    }
    mat4_copy(img->box, box);
    box_get_start_pos(box, out);
}

void image_center(const image_t *img, int out[3])
{
    int start[3], dims[3];
    float box[4][4];
    if (box_is_null(img->box)) {
        out[0] = out[1] = 0;
        out[2] = 16;
        return;
    }
    mat4_copy(img->box, box);
    box_get_start_pos(box, start);
    box_get_dimensions(box, dims);
    out[0] = start[0] + dims[0] / 2;
    out[1] = start[1] + dims[1] / 2;
    out[2] = start[2] + dims[2] / 2;
}

static void a_image_auto_resize_reset(void)
{
    image_t *img = goxel.image;

    image_history_push(img);
    image_auto_resize_reset(img);
}

void image_crop_to_box(image_t *img)
{
    layer_t *layer;

    if (box_is_null(img->box))
        return;
    DL_FOREACH(img->layers, layer)
        volume_crop(layer->volume, img->box);
}

static void a_image_crop_to_box(void)
{
    image_t *img = goxel.image;

    if (box_is_null(img->box))
        return;
    image_history_push(img);
    image_crop_to_box(img);
}

static void a_layer_crop_to_box(void)
{
    image_t *img = goxel.image;
    layer_t *layer = img->active_layer;

    if (!layer || box_is_null(layer->box))
        return;
    image_history_push(img);
    volume_crop(layer->volume, layer->box);
}

static void a_layer_crop_to_image(void)
{
    image_t *img = goxel.image;
    layer_t *layer = img->active_layer;

    if (!layer || box_is_null(img->box))
        return;
    image_history_push(img);
    volume_crop(layer->volume, img->box);
}

void image_set(image_t *img, image_t *other)
{
    layer_t *layer, *tmp, *other_layer;
    DL_FOREACH_SAFE(img->layers, layer, tmp) {
        DL_DELETE(img->layers, layer);
        layer_delete(layer);
    }
    img->active_layer = NULL;
    DL_FOREACH(other->layers, other_layer) {
        layer = layer_copy(other_layer);
        DL_APPEND(img->layers, layer);
        if (other_layer == other->active_layer)
            img->active_layer = layer;
    }
}

#if 0 // For debugging purpose.
static void debug_print_history(image_t *img)
{
    int i = 0;
    image_t *hist;
    DL_FOREACH2(img->history, hist, history_next) {
        printf("%d%s  ", i++, hist == img ? "*" : " ");
    }
    printf("\n");
}
#else
static void debug_print_history(image_t *img) {}
#endif

void image_history_push(image_t *img)
{
    image_t *snap;
    image_t *hist;

    snap = image_snap(img);

    // Discard previous undo.
    while ((hist = img->history_next)) {
        DL_DELETE2(img->history, hist, history_prev, history_next);
        assert(hist != img->history_next);
        image_delete(hist);
    }

    DL_DELETE2(img->history, img,  history_prev, history_next);
    DL_APPEND2(img->history, snap, history_prev, history_next);
    DL_APPEND2(img->history, img,  history_prev, history_next);
    debug_print_history(img);
}

void image_history_resize(image_t *img, int size)
{
    int i, nb = 0;
    image_t *hist;
    layer_t *layer, *layer_tmp;

    // First cound the size of the history to compute how many we are going
    // to remove.
    for (hist = img->history; hist != img; hist = hist->history_next) nb++;
    nb = max(0, nb - size);
    for (i = 0; i < nb; i++) {
        hist = img->history;

        // XXX: do that in a function!
        DL_FOREACH_SAFE(hist->layers, layer, layer_tmp) {
            assert(layer);
            DL_DELETE(hist->layers, layer);
            layer_delete(layer);
        }
        DL_DELETE2(img->history, hist, history_prev, history_next);
        free(hist);
    }
}

// XXX: not clear what this is doing.  We should try to remove it.
// It swap the content of two images without touching their pointer or
// history.
static void swap(image_t *a, image_t *b)
{
    SWAP(*a, *b);
    SWAP(a->history, b->history);
    SWAP(a->history_next, b->history_next);
    SWAP(a->history_prev, b->history_prev);
}

void image_undo(image_t *img)
{
    image_t *prev = img->history_prev;
    if (img->history == img) {
        LOG_D("No more undo");
        return;
    }
    DL_DELETE2(img->history, img, history_prev, history_next);
    DL_PREPEND_ELEM2(img->history, prev, img, history_prev, history_next);
    swap(img, prev);

    // Don't move the camera for an undo.
    if (img->active_camera && prev->active_camera &&
            strcmp(img->active_camera->name, prev->active_camera->name) == 0) {
        camera_set(img->active_camera, prev->active_camera);
    }

    debug_print_history(img);
}

void image_redo(image_t *img)
{
    image_t *next = img->history_next;
    if (!next) {
        LOG_D("No more redo");
        return;
    }
    DL_DELETE2(img->history, next, history_prev, history_next);
    DL_PREPEND_ELEM2(img->history, img, next, history_prev, history_next);
    swap(img, next);
    debug_print_history(img);
}

static void image_clear_layer(void)
{
    painter_t painter;
    image_t *img = goxel.image;
    layer_t *layer = img->active_layer;

    if (!layer || !image_layer_can_edit(img, layer))
        return;

    /* Full-layer clear is a no-op if already empty. */
    if (box_is_null(goxel.selection) && volume_is_empty(goxel.mask) &&
            volume_is_empty(layer->volume))
        return;

    /*
     * Snapshot must be taken before mutating the volume; action_exec's
     * ACTION_TOUCH_IMAGE push runs after cfunc and would record the
     * post-clear state (undo would not restore voxels).
     */
    image_history_push(img);

    if (box_is_null(goxel.selection) && volume_is_empty(goxel.mask)) {
        volume_clear(layer->volume);
        return;
    }

    // Use the mask in priority if it exists.
    if (!volume_is_empty(goxel.mask)) {
        volume_merge(layer->volume, goxel.mask, MODE_SUB, NULL);
        return;
    }

    painter = (painter_t) {
        .shape = &shape_cube,
        .mode = MODE_SUB,
        .color = {255, 255, 255, 255},
    };
    volume_op(layer->volume, &painter, goxel.selection);
}

bool image_layer_can_edit(const image_t *img, const layer_t *layer)
{
    if (!layer) return false;
    return !layer->base_id && !layer->image && !layer->shape;
}

/*
 * Function: image_get_key
 * Return a value that is garantied to change when the image change.
 */
uint32_t image_get_key(const image_t *img)
{
    uint32_t key = 0, k;
    layer_t *layer;
    camera_t *camera;
    material_t *material;

    DL_FOREACH(img->layers, layer) {
        k = layer_get_key(layer);
        key = XXH32(&k, sizeof(k), key);
        key = XXH32(&layer->opacity, sizeof(layer->opacity), key);
    }
    DL_FOREACH(img->cameras, camera) {
        k = camera_get_key(camera);
        key = XXH32(&k, sizeof(k), key);
    }
    DL_FOREACH(img->materials, material) {
        k = material_get_hash(material);
        key = XXH32(&k, sizeof(k), key);
    }
    {
        custom_object_t *obj;
        uint8_t show = img->custom_objects_show_when_closed ? 1 : 0;
        key = XXH32(&show, sizeof(show), key);
        DL_FOREACH(img->custom_objects, obj) {
            key = XXH32(obj->name, sizeof(obj->name), key);
            key = XXH32(&obj->type, sizeof(obj->type), key);
            key = XXH32(obj->color, sizeof(obj->color), key);
            key = XXH32(&obj->visible, sizeof(obj->visible), key);
            key = XXH32(obj->p0, sizeof(obj->p0), key);
            key = XXH32(obj->p1, sizeof(obj->p1), key);
        }
    }
    return key;
}

bool image_is_empty(const image_t *img)
{
    layer_t *layer;
    DL_FOREACH(img->layers, layer) {
        if (!volume_is_empty(layer->volume)) return false;
    }
    return true;
}

/*
 * Turn an image layer into a volume of 1 voxel depth.
 * Does not push history; callers must snapshot first if needed.
 */
void image_image_layer_to_volume(image_t *img, layer_t *layer)
{
    uint8_t *data;
    int x, y, w, h, bpp = 0, pos[3];
    uint8_t c[4];
    float p[3];
    assert(img);
    assert(layer);
    assert(layer->image);
    volume_accessor_t acc;

    data = img_read(layer->image->path, &w, &h, &bpp);
    acc = volume_get_accessor(layer->volume);
    for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) {
        vec3_set(p, (x / (float)w) - 0.5, - ((y + 1) / (float)h) + 0.5, 0);
        mat4_mul_vec3(layer->mat, p, p);
        pos[0] = round(p[0]);
        pos[1] = round(p[1]);
        pos[2] = round(p[2]);
        memset(c, 0, 4);
        c[3] = 255;
        memcpy(c, data + (y * w + x) * bpp, bpp);
        // Do not create voxels the mesher would skip. Same alpha threshold as
        // volume_generate_vertices (src/volume_to_vertices.c) and import_cmap
        // (src/formats/cmap.c). Writing alpha < 127 leaves invisible "ghost"
        // voxels that later paint ops can promote to solid visible blocks.
        if (c[3] < 127)
            continue;
        volume_set_at(layer->volume, &acc, pos, c);
    }
    texture_delete(layer->image);
    layer->image = NULL;
    free(data);
}

ACTION_REGISTER(ACTION_layer_clear,
    .help = "Clear the current layer",
    .cfunc = image_clear_layer,
    .icon = ICON_DELETE,
    .default_shortcut = "Delete",
)

static void a_image_add_layer(void)
{
    image_add_layer_below_active(goxel.image, NULL);
}

ACTION_REGISTER(ACTION_img_new_layer,
    .help = "Add a new layer to the image",
    .cfunc = a_image_add_layer,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

static void a_image_delete_layer(void)
{
    if (!goxel.image->active_layer) return;
    /*
     * Snapshot before mutating. ACTION_TOUCH_IMAGE pushes after cfunc and
     * would only record the post-delete state - nested children added via
     * the layers panel (push-before, no post snap) would never be
     * restorable. Same pattern as ACTION_layer_clear.
     */
    image_history_push(goxel.image);
    image_delete_layer(goxel.image, goxel.image->active_layer);
}

ACTION_REGISTER(ACTION_img_del_layer,
    .help = "Delete the active layer and its children",
    .cfunc = a_image_delete_layer,
    .icon = ICON_REMOVE,
)

static void a_image_move_layer_up(void)
{
    if (!goxel.image->active_layer) return;
    image_move_layer(goxel.image, goxel.image->active_layer, -1);
}

static void a_image_move_layer_down(void)
{
    if (!goxel.image->active_layer) return;
    image_move_layer(goxel.image, goxel.image->active_layer, +1);
}


ACTION_REGISTER(ACTION_img_move_layer_up,
    .help = "Move the active layer up",
    .cfunc = a_image_move_layer_up,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_UPWARD,
)

ACTION_REGISTER(ACTION_img_move_layer_down,
    .help = "Move the active layer down",
    .cfunc = a_image_move_layer_down,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_DOWNWARD,
)

static void a_image_duplicate_layer(void)
{
    if (!goxel.image->active_layer) return;
    image_duplicate_layer(goxel.image, goxel.image->active_layer);
}

ACTION_REGISTER(ACTION_img_duplicate_layer,
    .help = "Duplicate the active layer",
    .cfunc = a_image_duplicate_layer,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_image_clone_layer(void)
{
    if (!goxel.image->active_layer) return;
    image_clone_layer(goxel.image, goxel.image->active_layer);
}

static void a_delete_hidden_layers(void)
{
    image_delete_hidden_layers(goxel.image);
}

ACTION_REGISTER(ACTION_delete_hidden_layers,
    .help = "Delete hidden layers",
    .cfunc = a_delete_hidden_layers,
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(ACTION_img_clone_layer,
    .help = "Clone the active layer",
    .cfunc = a_image_clone_layer,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_image_unclone_layer(void)
{
    if (!goxel.image->active_layer) return;
    image_unclone_layer(goxel.image, goxel.image->active_layer);
}

ACTION_REGISTER(ACTION_img_unclone_layer,
    .help = "Unclone the active layer",
    .cfunc = a_image_unclone_layer,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_img_select_parent_layer(void)
{
    image_t *image = goxel.image;
    if (!image->active_layer || !image->active_layer->base_id) return;
    image->active_layer = img_get_layer(image, image->active_layer->base_id);
}


ACTION_REGISTER(ACTION_img_select_parent_layer,
    .help = "Select the parent of a layer",
    .cfunc = a_img_select_parent_layer,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_img_merge_layer_down(void)
{
    image_merge_layer_down(goxel.image);
}
ACTION_REGISTER(ACTION_img_merge_layer_down,
    .help = "Merge layer down",
    .cfunc = a_img_merge_layer_down,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_MENU,
)

static void a_img_merge_visible_layers(void)
{
    image_merge_visible_layers(goxel.image);
}

ACTION_REGISTER(ACTION_img_merge_visible_layers,
    .help = "Merge all the visible layers",
    .cfunc = a_img_merge_visible_layers,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_img_merge_layer_children(void)
{
    layer_t *layer = goxel.image->active_layer;
    if (!layer || !layer_has_children(goxel.image, layer)) return;
    /* Snapshot before delete; same pattern as ACTION_img_del_layer. */
    image_history_push(goxel.image);
    image_merge_layer_children(goxel.image, layer);
}

ACTION_REGISTER(ACTION_img_merge_layer_children,
    .help = "Merge all children into the active layer",
    .cfunc = a_img_merge_layer_children,
)

static void a_img_new_camera(void)
{
    image_add_camera(goxel.image, NULL);
}

ACTION_REGISTER(ACTION_img_new_camera,
    .help = "Add a new camera to the image",
    .cfunc = a_img_new_camera,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

static void a_img_del_camera(void)
{
    image_delete_camera(goxel.image, goxel.image->active_camera);
}

ACTION_REGISTER(ACTION_img_del_camera,
    .help = "Delete the active camera",
    .cfunc = a_img_del_camera,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_REMOVE,
)

static void a_img_move_camera_up(void)
{
    image_move_camera(goxel.image, goxel.image->active_camera, -1);
}

static void a_img_move_camera_down(void)
{
    image_move_camera(goxel.image, goxel.image->active_camera, +1);
}

ACTION_REGISTER(ACTION_img_move_camera_up,
    .help = "Move the active camera up",
    .cfunc = a_img_move_camera_up,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_UPWARD,
)

ACTION_REGISTER(ACTION_img_move_camera_down,
    .help = "Move the active camera down",
    .cfunc = a_img_move_camera_down,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ARROW_DOWNWARD,
)

static void a_img_image_layer_to_volume(void)
{
    /* Snapshot before mutate; ACTION_TOUCH_IMAGE also pushes after. */
    image_history_push(goxel.image);
    image_image_layer_to_volume(goxel.image, goxel.image->active_layer);
}

ACTION_REGISTER(ACTION_img_image_layer_to_volume,
    .help = "Turn an image layer into a volume",
    .cfunc = a_img_image_layer_to_volume,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_img_new_shape_layer(void)
{
    image_add_shape_layer_below_active(goxel.image);
}

ACTION_REGISTER(ACTION_img_new_shape_layer,
    .help = "Add a new shape layer to the image",
    .cfunc = a_img_new_shape_layer,
    .flags = ACTION_TOUCH_IMAGE,
)

static void a_img_new_material(void)
{
    image_add_material(goxel.image, NULL);
}

ACTION_REGISTER(ACTION_img_new_material,
    .help = "Add a new material to the image",
    .cfunc = a_img_new_material,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_ADD,
)

static void a_img_del_material(void)
{
    image_delete_material(goxel.image, goxel.image->active_material);
}

ACTION_REGISTER(ACTION_img_del_material,
    .help = "Delete a material",
    .cfunc = a_img_del_material,
    .flags = ACTION_TOUCH_IMAGE,
    .icon = ICON_REMOVE,
)

ACTION_REGISTER(ACTION_img_auto_resize,
    .help = "Auto resize the image to fit the layers",
    .cfunc = a_image_auto_resize,
    .flags = ACTION_TOUCH_IMAGE,
)

ACTION_REGISTER(ACTION_img_auto_resize_reset,
    .help = "Fit the image box to visible layers, move its origin to the "
            "content corner, and shift visible layers to match. Does not "
            "delete voxels.",
    .cfunc = a_image_auto_resize_reset,
)

ACTION_REGISTER(ACTION_img_crop_to_box,
    .help = "Delete voxels outside the image box in every layer. The image "
            "box size is unchanged.",
    .cfunc = a_image_crop_to_box,
)

ACTION_REGISTER(ACTION_layer_crop_to_box,
    .help = "Delete voxels in the active layer that lie outside this layer's "
            "bounding box.",
    .cfunc = a_layer_crop_to_box,
)

ACTION_REGISTER(ACTION_layer_crop_to_image,
    .help = "Delete voxels in the active layer that lie outside the image box.",
    .cfunc = a_layer_crop_to_image,
)
