/* Goxel 3D voxels editor
 *
 * Baked per-tile volume meshes (wrap preview and similar). Create once,
 * draw with the volume shader without shadow-map sampling or tile walks.
 */

#include "goxel.h"
#include "render_priv.h"
#include "shader_cache.h"

typedef struct {
    int pos[3];
    GLuint vertex_buffer;
    int nb_elements;
    int size;
    int subdivide;
} render_bake_tile_t;

struct render_bake {
    render_bake_tile_t *tiles;
    int count;
    int capacity;
};

render_bake_t *render_bake_volume(const volume_t *volume, int effects)
{
    render_bake_t *bake;
    volume_iterator_t iter;
    int tile_pos[3], nb, size, subdivide;
    render_bake_tile_t *tile;
    voxel_vertex_t *scratch;

    if (!volume) return NULL;

    scratch = calloc((size_t)TILE_SIZE * TILE_SIZE * TILE_SIZE * 6 * 4,
                     sizeof(*scratch));
    if (!scratch) return NULL;

    bake = calloc(1, sizeof(*bake));
    effects &= ~(EFFECT_GRID | EFFECT_EDGES | EFFECT_GRID_ONLY |
                 EFFECT_SHADOW_MAP | EFFECT_RENDER_POS);
    if (effects & EFFECT_MARCHING_CUBES)
        effects &= ~EFFECT_BORDERS;

    iter = volume_get_iterator(volume, VOLUME_ITER_TILES);
    while (volume_iter(&iter, tile_pos)) {
        nb = volume_generate_vertices(volume, tile_pos, effects, scratch,
                                      &size, &subdivide);
        if (nb <= 0) continue;
        if (nb > RENDER_BATCH_QUAD_COUNT) {
            LOG_W("Too many quads!");
            nb = RENDER_BATCH_QUAD_COUNT;
        }
        if (bake->count >= bake->capacity) {
            int ncap = bake->capacity ? bake->capacity * 2 : 256;
            render_bake_tile_t *tiles = realloc(
                    bake->tiles, (size_t)ncap * sizeof(*tiles));
            if (!tiles) break;
            bake->tiles = tiles;
            bake->capacity = ncap;
        }
        tile = &bake->tiles[bake->count++];
        memset(tile, 0, sizeof(*tile));
        memcpy(tile->pos, tile_pos, sizeof(tile->pos));
        tile->nb_elements = nb;
        tile->size = size;
        tile->subdivide = subdivide;
        GL(glGenBuffers(1, &tile->vertex_buffer));
        GL(glBindBuffer(GL_ARRAY_BUFFER, tile->vertex_buffer));
        GL(glBufferData(GL_ARRAY_BUFFER,
                        nb * size * sizeof(*scratch),
                        scratch, GL_STATIC_DRAW));
    }
    free(scratch);

    if (bake->count == 0) {
        render_bake_delete(bake);
        return NULL;
    }
    return bake;
}

void render_bake_delete(render_bake_t *bake)
{
    int i;
    if (!bake) return;
    for (i = 0; i < bake->count; i++) {
        if (bake->tiles[i].vertex_buffer)
            GL(glDeleteBuffers(1, &bake->tiles[i].vertex_buffer));
    }
    free(bake->tiles);
    free(bake);
}

static void draw_baked_tile(const render_bake_tile_t *tile, int tile_id,
                            gl_shader_t *shader, const float model[4][4])
{
    float tile_model[4][4];
    float tile_id_f[2];

    if (tile->nb_elements == 0) return;
    GL(glBindBuffer(GL_ARRAY_BUFFER, tile->vertex_buffer));
    if (gl_has_uniform(shader, "u_tile_id")) {
        tile_id_f[1] = ((tile_id >> 8) & 0xff) / 255.0;
        tile_id_f[0] = ((tile_id >> 0) & 0xff) / 255.0;
        gl_update_uniform(shader, "u_tile_id", tile_id_f);
    }
    gl_update_uniform(shader, "u_pos_scale", 1.f / tile->subdivide);
    render_priv_bind_voxel_attribs();

    mat4_copy(model, tile_model);
    mat4_itranslate(tile_model, tile->pos[0], tile->pos[1], tile->pos[2]);
    gl_update_uniform(shader, "u_model", tile_model);
    if (tile->size == 4) {
        GL(glDrawElements(GL_TRIANGLES, tile->nb_elements * 6,
                          GL_UNSIGNED_SHORT, 0));
    } else {
        GL(glDrawArrays(GL_TRIANGLES, 0, tile->nb_elements * tile->size));
    }
}

void render_bake_draw(renderer_t *rend, const render_bake_t *bake,
                      const material_t *material, int effects,
                      const float base_model[4][4])
{
    gl_shader_t *shader;
    float model[4][4], camera[4][4];
    float light_dir[3], alpha;
    int i, tile_id;

    if (!bake || bake->count <= 0 || !material) return;
    if (base_model) mat4_copy(base_model, model);
    else mat4_set_identity(model);
    render_priv_get_light_dir(rend, light_dir);

    if (effects & EFFECT_MARCHING_CUBES)
        effects &= ~EFFECT_BORDERS;

    {
        shader_define_t defines[] = {
            {"SHADOW", false},
            {"MATERIAL_UNLIT", (rend->settings.effects & EFFECT_UNLIT) ||
                               (effects & EFFECT_EDGES)},
            {"HAS_TANGENTS", effects & EFFECT_BORDERS},
            {"ONLY_EDGES", effects & EFFECT_EDGES},
            {"HAS_OCCLUSION_MAP", rend->settings.occlusion_strength > 0},
            {"VERTEX_LIGHTNING", !(effects & (EFFECT_BORDERS | EFFECT_UNLIT))},
            {"SMOOTHNESS", rend->settings.smoothness > 0},
            {}
        };
        shader = shader_get("volume", defines,
                            render_priv_volume_attr_names(),
                            render_priv_volume_shader_init);
    }

    GL(glEnable(GL_DEPTH_TEST));
    GL(glDepthFunc(GL_LEQUAL));
    GL(glEnable(GL_CULL_FACE));
    GL(glCullFace(GL_BACK));

    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, render_priv_bump_tex()));
    GL(glActiveTexture(GL_TEXTURE1));
    GL(glBindTexture(GL_TEXTURE_2D, render_priv_occlusion_tex()));
    GL(glDisable(GL_BLEND));

    alpha = material->base_color[3];
    if (effects & EFFECT_SEMI_TRANSPARENT) alpha *= 0.75;
    if (alpha < 1) {
        GL(glEnable(GL_BLEND));
        GL(glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR));
        GL(glBlendColor(alpha, alpha, alpha, alpha));
    }

    GL(glUseProgram(shader->prog));
    gl_update_uniform(shader, "u_proj", rend->proj_mat);
    gl_update_uniform(shader, "u_view", rend->view_mat);
    gl_update_uniform(shader, "u_normal_sampler", 0);
    gl_update_uniform(shader, "u_occlusion_tex", 1);
    gl_update_uniform(shader, "u_normal_scale",
                      effects & EFFECT_BORDERS ? 0.5 : 0.0);
    gl_update_uniform(shader, "u_l_dir", light_dir);
    gl_update_uniform(shader, "u_l_int", rend->light.intensity);
    gl_update_uniform(shader, "u_l_amb", rend->settings.ambient);
    gl_update_uniform(shader, "u_m_metallic", material->metallic);
    gl_update_uniform(shader, "u_m_roughness", material->roughness);
    gl_update_uniform(shader, "u_m_base_color", material->base_color);
    gl_update_uniform(shader, "u_m_emissive_factor", material->emission);
    gl_update_uniform(shader, "u_m_smoothness", rend->settings.smoothness);
    gl_update_uniform(shader, "u_occlusion_strength",
                      rend->settings.occlusion_strength);
    mat4_invert(rend->view_mat, camera);
    gl_update_uniform(shader, "u_camera", camera[3]);

    render_priv_enable_voxel_attribs();
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, render_priv_index_buffer()));

    tile_id = 1;
    for (i = 0; i < bake->count; i++)
        draw_baked_tile(&bake->tiles[i], tile_id++, shader, model);

    render_priv_disable_voxel_attribs();
    GL(glDisable(GL_BLEND));
}
