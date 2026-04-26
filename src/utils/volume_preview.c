/* Goxel — single-volume preview rasterization */

#include "goxel.h"
#include "utils/volume_preview.h"
#include "utils/img.h"
#include "utils/texture.h"

#include <stdlib.h>
#include <string.h>

/* Off-screen: FBO 2*out_w × 2*out_h, downsample to out_w×out_h (see goxel_render_to_buf). */
static int volume_preview_rasterize(
        const volume_t *vol, int out_w, int out_h, int bpp, uint8_t *out)
{
    camera_t *cam;
    int bbox[2][3];
    float box[4][4];
    texture_t *fbo;
    renderer_t rend;
    float rect[4] = {0, 0, (float)out_w * 2, (float)out_h * 2};
    uint8_t *tmp;
    int iw, ih;

    if (!vol || volume_is_empty(vol))
        return -1;

    volume_get_bbox(vol, bbox, true);
    if (bbox[1][0] < bbox[0][0] || bbox[1][1] < bbox[0][1] || bbox[1][2] < bbox[0][2])
        return -1;

    bbox_from_aabb(box, bbox);
    cam = camera_new(NULL);
    camera_fit_box(cam, box);
    camera_turntable(cam, (float)M_PI / 4, (float)M_PI / 4);
    cam->aspect = 1.f;
    camera_update_for_volume(cam, vol);

    iw = out_w * 2;
    ih = out_h * 2;
    fbo = texture_new_buffer(iw, ih, TF_DEPTH);

    rend = goxel.rend;
    mat4_copy(cam->view_mat, rend.view_mat);
    mat4_copy(cam->proj_mat, rend.proj_mat);
    rend.fbo = fbo->framebuffer;
    rend.scale = 1.0;
    rend.items = NULL;

    render_volume(&rend, vol, NULL, 0);
    render_submit(&rend, rect, (bpp == 3) ? goxel.back_color : NULL);

    tmp = calloc((size_t)iw * ih, (size_t)bpp);
    if (!tmp) {
        texture_delete(fbo);
        camera_delete(cam);
        return -1;
    }
    texture_get_data(fbo, iw, ih, bpp, tmp);
    img_downsample(tmp, iw, ih, bpp, out);
    free(tmp);
    texture_delete(fbo);
    camera_delete(cam);
    return 0;
}

int volume_preview_to_rgba(
        const volume_t *vol, int w, int h, uint8_t *buf, int bpp)
{
    if (!buf)
        return -1;
    if (bpp != 3 && bpp != 4)
        return -1;
    if (w < 1 || h < 1)
        return -1;
    return volume_preview_rasterize(vol, w, h, bpp, buf);
}

texture_t *volume_preview_to_texture(const volume_t *vol, int size)
{
    uint8_t *pix;
    texture_t *tex;

    if (size < 2)
        return NULL;

    pix = malloc((size_t)size * size * 4);
    if (!pix)
        return NULL;
    if (volume_preview_rasterize(vol, size, size, 4, pix) != 0) {
        free(pix);
        return NULL;
    }
    tex = texture_new_from_buf(pix, size, size, 4, TF_NEAREST);
    free(pix);
    return tex;
}
