/* Goxel 3D voxels editor
 *
 * Wrap preview: bake image-box tiles once, draw up to 8 translated copies
 * with the volume shader and live goxel.rend settings (no shadow map).
 */

#include "goxel.h"
#include "wrap_view.h"

void wrap_view_neighbor_offsets(const float box[4][4],
                                float out[WRAP_VIEW_NEIGHBOR_COUNT][3])
{
    float half[3];
    float full[3];

    box_get_size(box, half);
    full[0] = half[0] * 2.0f;
    full[1] = half[1] * 2.0f;
    full[2] = half[2] * 2.0f;

    out[0][0] = -full[0]; out[0][1] =  0;       out[0][2] = 0;
    out[1][0] =  full[0]; out[1][1] =  0;       out[1][2] = 0;
    out[2][0] =  0;       out[2][1] = -full[1]; out[2][2] = 0;
    out[3][0] =  0;       out[3][1] =  full[1]; out[3][2] = 0;
    out[4][0] = -full[0]; out[4][1] = -full[1]; out[4][2] = 0;
    out[5][0] = -full[0]; out[5][1] =  full[1]; out[5][2] = 0;
    out[6][0] =  full[0]; out[6][1] = -full[1]; out[6][2] = 0;
    out[7][0] =  full[0]; out[7][1] =  full[1]; out[7][2] = 0;
}

/* True if the image box translated by offset intersects the camera frustum.
 * Conservative: boxes that straddle the near plane are kept. */
static bool wrap_box_visible(const float box[4][4], const float offset[3],
                             const float view_proj[4][4])
{
    float verts[8][3];
    float p[4];
    bool all_left = true, all_right = true;
    bool all_bottom = true, all_top = true;
    bool all_near = true, all_far = true;
    bool any_front = false, any_behind = false;
    int i;

    box_get_vertices(box, verts);
    for (i = 0; i < 8; i++) {
        vec3_add(verts[i], offset, verts[i]);
        vec4_set(p, verts[i][0], verts[i][1], verts[i][2], 1.f);
        mat4_mul_vec4(view_proj, p, p);
        if (p[3] <= 0.f) {
            any_behind = true;
            continue;
        }
        any_front = true;
        if (p[0] >= -p[3]) all_left = false;
        if (p[0] <=  p[3]) all_right = false;
        if (p[1] >= -p[3]) all_bottom = false;
        if (p[1] <=  p[3]) all_top = false;
        if (p[2] >= -p[3]) all_near = false;
        if (p[2] <=  p[3]) all_far = false;
    }
    if (any_behind && any_front) return true;
    if (!any_front) return false;
    return !(all_left || all_right || all_bottom || all_top ||
             all_near || all_far);
}

static void wrap_view_clear(void)
{
    if (goxel.wrap_view_bake) {
        render_bake_delete(goxel.wrap_view_bake);
        goxel.wrap_view_bake = NULL;
    }
}

static void wrap_view_build(void)
{
    volume_t *volume;
    painter_t painter;
    int effects;

    wrap_view_clear();
    if (box_is_null(goxel.image->box))
        return;

    volume = volume_copy(goxel_get_layers_volume(goxel.image));
    painter = (painter_t){
        .shape = &shape_cube,
        .mode = MODE_INTERSECT,
        .color = {255, 255, 255, 255},
    };
    volume_op(volume, &painter, goxel.image->box);

    effects = goxel.rend.settings.effects;
    effects &= ~(EFFECT_GRID | EFFECT_EDGES | EFFECT_GRID_ONLY |
                 EFFECT_SHADOW_MAP | EFFECT_RENDER_POS);
    goxel.wrap_view_bake = render_bake_volume(volume, effects);
    volume_delete(volume);
}

void goxel_wrap_view_set(bool enabled)
{
    if (!enabled) {
        wrap_view_clear();
        goxel.wrap_view = false;
        return;
    }
    goxel.wrap_view = true;
    wrap_view_build();
}

void wrap_view_render(renderer_t *rend, int effects)
{
    float off[WRAP_VIEW_NEIGHBOR_COUNT][3];
    float m[4][4];
    float view_proj[4][4];
    const material_t *mat = NULL;
    const layer_t *rl;
    const camera_t *camera;
    int i;

    if (!goxel.wrap_view || !goxel.wrap_view_bake)
        return;
    if (box_is_null(goxel.image->box))
        return;

    camera = goxel.image->active_camera;
    if (!camera) return;

    wrap_view_neighbor_offsets(goxel.image->box, off);

    rl = goxel_get_render_layers(false);
    if (rl) mat = rl->material;
    if (!mat && goxel.image->active_layer)
        mat = goxel.image->active_layer->material;

    mat4_mul(camera->proj_mat, camera->view_mat, view_proj);
    for (i = 0; i < WRAP_VIEW_NEIGHBOR_COUNT; i++) {
        if (!wrap_box_visible(goxel.image->box, off[i], view_proj))
            continue;
        mat4_set_identity(m);
        mat4_itranslate(m, off[i][0], off[i][1], off[i][2]);
        render_bake_ref(rend, goxel.wrap_view_bake, mat, effects, m);
    }
}
