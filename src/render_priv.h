/* Internal render helpers shared by render.c and render_bake.c. */
#ifndef RENDER_PRIV_H
#define RENDER_PRIV_H

#include "goxel.h"
#include "shader_cache.h"

#define RENDER_BATCH_QUAD_COUNT (1 << 14)

GLuint render_priv_index_buffer(void);
GLuint render_priv_bump_tex(void);
GLuint render_priv_occlusion_tex(void);
const char **render_priv_volume_attr_names(void);
void render_priv_volume_shader_init(gl_shader_t *shader);
int render_priv_voxel_attr_count(void);
void render_priv_enable_voxel_attribs(void);
void render_priv_disable_voxel_attribs(void);
void render_priv_bind_voxel_attribs(void);
void render_priv_get_light_dir(const renderer_t *rend, float out[3]);

/* Used by render_submit for ITEM_BAKED_VOLUME. */
void render_bake_draw(renderer_t *rend, const render_bake_t *bake,
                      const material_t *material, int effects,
                      const float model[4][4]);

#endif
