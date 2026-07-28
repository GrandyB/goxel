/* Goxel 3D voxels editor
 *
 * copyright (c) 2015-2022 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"

#include "../ext_src/stb/stb_ds.h"
#include "utils/color.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool str_endswith_case(const char *str, const char *end)
{
    int i, n, m;
    if (!str || !end) return false;
    n = strlen(str);
    m = strlen(end);
    if (m > n) return false;
    for (i = 0; i < m; i++) {
        char a = str[n - m + i];
        char b = end[i];
        if (tolower((unsigned char)a) != tolower((unsigned char)b))
            return false;
    }
    return true;
}

static bool path_join2(char *out, size_t out_sz,
                       const char *a, const char *b)
{
    size_t na, nb;
    if (!out || !out_sz || !a || !b) return false;
    na = strlen(a);
    nb = strlen(b);
    if (na + 1 + nb + 1 > out_sz) return false;
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1, b, nb);
    out[na + 1 + nb] = '\0';
    return true;
}

static void path_normalize_slashes(char *path)
{
    int i;
    if (!path) return;
    for (i = 0; path[i]; i++) {
        if (path[i] == '\\')
            path[i] = '/';
    }
}

static const char *path_basename_ptr(const char *path)
{
    const char *p = path;
    const char *last = path;
    if (!path) return "";
    while (*p) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
        p++;
    }
    return last;
}

static void brush_textures_clear(bool clear_pixels)
{
    int i;
    for (i = 0; i < goxel.brush_textures_count; i++) {
        brush_texture_t *t = &goxel.brush_textures[i];
        free(t->name);
        free(t->path);
        if (clear_pixels) free(t->pixels);
        if (t->preview) texture_delete(t->preview);
    }
    // brush_textures is an stb_ds array (arrput); free() would corrupt the heap.
    arrfree(goxel.brush_textures);
    goxel.brush_textures = NULL;
    goxel.brush_textures_count = 0;
    goxel.brush_texture_index = 0;
}

void goxel_brush_textures_clear(void)
{
    brush_textures_clear(true);
}

void goxel_brush_textures_release_graphics(void)
{
    int i;
    for (i = 0; i < goxel.brush_textures_count; i++) {
        texture_delete(goxel.brush_textures[i].preview);
        goxel.brush_textures[i].preview = NULL;
    }
}

static int brush_texture_cmp(const void *a, const void *b)
{
    const brush_texture_t *ta = a;
    const brush_texture_t *tb = b;
    const char *sa = ta->name ?: "";
    const char *sb = tb->name ?: "";
    int i = 0;
    while (sa[i] && sb[i]) {
        int da = tolower((unsigned char)sa[i]);
        int db = tolower((unsigned char)sb[i]);
        if (da != db) return da - db;
        i++;
    }
    return (unsigned char)sa[i] - (unsigned char)sb[i];
}

static bool brush_texture_add_from_file(const char *path, const char *name)
{
    char *data = NULL, *name_copy = NULL, *path_copy = NULL;
    uint8_t *img = NULL;
    int size = 0, w = 0, h = 0, bpp = 4;

    data = read_file(path, &size);
    if (!data || size <= 0)
        goto fail;
    // Decode all brush textures to RGBA to keep preview/upload and sampling stride consistent.
    img = img_read_from_mem(data, size, &w, &h, &bpp);
    if (!img || w <= 0 || h <= 0)
        goto fail;
    bpp = 4;

    name_copy = strdup(name);
    path_copy = strdup(path);
    if (!name_copy || !path_copy)
        goto fail;

    arrput(goxel.brush_textures, ((brush_texture_t){
        .name = name_copy,
        .path = path_copy,
        .w = w,
        .h = h,
        .bpp = bpp,
        .pixels = img,
        .preview = NULL,
        .hue = 0.f,
        .saturation = 100.f,
        .lightness = 0.f,
        .opacity = 255,
        /* Force first preview bake (identity values differ from unset). */
        .preview_hue = 1.f,
        .preview_saturation = 0.f,
        .preview_lightness = 1.f,
        .preview_opacity = 0,
    }));
    goxel.brush_textures_count = arrlen(goxel.brush_textures);
    free(data);
    return true;
fail:
    free(data);
    free(name_copy);
    free(path_copy);
    free(img);
    return false;
}

typedef struct {
    const char *target_dir;
    int copied_count;
} texture_seed_ctx_t;

typedef struct {
    int png_count;
} texture_png_count_ctx_t;

static int brush_texture_count_png(const char *dir, const char *name, void *user)
{
    texture_png_count_ctx_t *ctx = user;
    (void)dir;
    if (str_endswith_case(name, ".png"))
        ctx->png_count++;
    return 0;
}

static int brush_texture_seed_copy_asset(int idx, const char *path, void *user)
{
    texture_seed_ctx_t *ctx = user;
    const char *name = path_basename_ptr(path);
    char dst[1024];
    FILE *f = NULL;
    const void *data;
    int sz = 0;

    (void)idx;
    if (!str_endswith_case(path, ".png"))
        return 0;
    LOG_I("[brush-tex][asset] candidate: '%s'", path);
    if (!path_join2(dst, sizeof(dst), ctx->target_dir, name)) {
        LOG_I("[brush-tex][asset] skip (join failed): '%s'", path);
        return 0;
    }
    path_normalize_slashes(dst);
    LOG_I("[brush-tex][asset] destination: '%s'", dst);
    f = fopen(dst, "rb");
    if (f) { // Keep user file if already present.
        fclose(f);
        ctx->copied_count++;
        LOG_I("[brush-tex][asset] already exists: '%s'", dst);
        return 0;
    }
    data = assets_get(path, &sz);
    if (!data || sz <= 0) {
        LOG_I("[brush-tex][asset] assets_get failed: '%s' (size=%d)", path, sz);
        return 0;
    }
    f = fopen(dst, "wb");
    if (!f) {
        LOG_I("[brush-tex][asset] open write failed: '%s' (errno=%d)", dst, errno);
        return 0;
    }
    fwrite(data, sz, 1, f);
    fclose(f);
    ctx->copied_count++;
    LOG_I("[brush-tex][asset] copied: '%s' bytes=%d", dst, sz);
    return 0;
}

static int brush_texture_seed_copy_from_disk(const char *dir, const char *name,
                                             void *user)
{
    texture_seed_ctx_t *ctx = user;
    char src[1024], dst[1024];
    const char *base_name = NULL;
    char *data = NULL;
    int size = 0;
    FILE *f = NULL;

    (void)dir;
    if (!str_endswith_case(name, ".png"))
        return 0;
    LOG_I("[brush-tex][disk] candidate: '%s'", name);
    if (!path_join2(src, sizeof(src), "data/textures", name))
        return 0;
    path_normalize_slashes(src);
    base_name = path_basename_ptr(src);
    if (!path_join2(dst, sizeof(dst), ctx->target_dir, base_name)) {
        return 0;
    }
    path_normalize_slashes(dst);
    LOG_I("[brush-tex][disk] src='%s' dst='%s'", src, dst);
    f = fopen(dst, "rb");
    if (f) {
        fclose(f);
        ctx->copied_count++;
        LOG_I("[brush-tex][disk] already exists: '%s'", dst);
        return 0;
    }
    data = read_file(src, &size);
    if (!data || size <= 0) {
        LOG_I("[brush-tex][disk] read failed: '%s' size=%d", src, size);
        free(data);
        return 0;
    }
    f = fopen(dst, "wb");
    if (!f) {
        LOG_I("[brush-tex][disk] open write failed: '%s' (errno=%d)", dst, errno);
        free(data);
        return 0;
    }
    fwrite(data, size, 1, f);
    fclose(f);
    free(data);
    ctx->copied_count++;
    LOG_I("[brush-tex][disk] copied: '%s' bytes=%d", dst, size);
    return 0;
}

static void brush_textures_seed_user_dir_once(const char *dir)
{
    char marker[1024];
    FILE *f;
    texture_seed_ctx_t ctx = {.target_dir = dir, .copied_count = 0};
    texture_png_count_ctx_t count_ctx = {0};

    if (!path_join2(marker, sizeof(marker), dir, ".seeded_textures"))
        return;
    path_normalize_slashes(marker);
    LOG_I("[brush-tex] marker path: '%s'", marker);
    f = fopen(marker, "rb");
    if (f) {
        fclose(f);
        LOG_I("[brush-tex] marker exists; checking directory contents");
        sys_list_dir(dir, brush_texture_count_png, &count_ctx);
        LOG_I("[brush-tex] png files currently in dir: %d", count_ctx.png_count);
        if (count_ctx.png_count > 0)
            return;
        LOG_I("[brush-tex] marker exists but dir empty; forcing reseed");
    }

    LOG_I("[brush-tex] seeding from embedded assets");
    assets_list("data/textures", &ctx, brush_texture_seed_copy_asset);
    if (ctx.copied_count == 0) {
        // Fallback for builds where textures are not embedded in assets.
        LOG_I("[brush-tex] embedded assets unavailable; trying disk fallback");
        sys_list_dir("data/textures", brush_texture_seed_copy_from_disk, &ctx);
    }
    LOG_I("Brush texture seeding: target='%s' copied_or_existing=%d",
          dir, ctx.copied_count);
    if (ctx.copied_count == 0)
        return;
    f = fopen(marker, "wb");
    if (f) {
        static const char marker_data[] = "seeded-from-assets\n";
        fwrite(marker_data, sizeof(marker_data) - 1, 1, f);
        fclose(f);
        LOG_I("[brush-tex] marker written");
    } else {
        LOG_I("[brush-tex] marker write failed (errno=%d)", errno);
    }
}

typedef struct {
    const char *dir;
} texture_load_ctx_t;

static int brush_texture_load_from_user_dir(const char *dir, const char *name,
                                            void *user)
{
    texture_load_ctx_t *ctx = user;
    char path[1024];
    char label[256];
    size_t len;

    (void)dir;
    if (!str_endswith_case(name, ".png"))
        return 0;
    LOG_I("[brush-tex][load] found png: '%s'", name);
    if (!path_join2(path, sizeof(path), ctx->dir, name))
        return 0;
    len = strlen(name);
    if (len >= sizeof(label))
        len = sizeof(label) - 1;
    memcpy(label, name, len);
    label[len] = '\0';
    len = strlen(label);
    if (len > 4 && str_endswith_case(label, ".png"))
        label[len - 4] = '\0';
    if (!brush_texture_add_from_file(path, label)) {
        LOG_I("[brush-tex][load] decode failed: '%s'", path);
    } else {
        LOG_I("[brush-tex][load] loaded: '%s' as '%s'", path, label);
    }
    return 0;
}

bool goxel_brush_textures_dir(char *out, size_t out_size)
{
    const char *user_dir = sys_get_user_dir();
    if (!out || !out_size || !user_dir) return false;
    if (!path_join2(out, out_size, user_dir, "textures"))
        return false;
    path_normalize_slashes(out);
    return true;
}

typedef struct {
    char *name;
    float hue;
    float saturation;
    float lightness;
    uint8_t opacity;
} brush_texture_adj_t;

static void brush_texture_store_adjustments(brush_texture_t *t)
{
    if (!t) return;
    t->hue = goxel.brush_texture_hue;
    t->saturation = goxel.brush_texture_saturation;
    t->lightness = goxel.brush_texture_lightness;
    t->opacity = goxel.painter.color[3];
}

static void brush_texture_load_adjustments(const brush_texture_t *t)
{
    if (!t) return;
    goxel.brush_texture_hue = t->hue;
    goxel.brush_texture_saturation = t->saturation;
    goxel.brush_texture_lightness = t->lightness;
    goxel.painter.color[3] = t->opacity;
}

static int brush_texture_name_eq(const char *a, const char *b)
{
    int i;
    if (!a || !b) return 0;
    for (i = 0; a[i] && b[i]; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return a[i] == b[i];
}

static void brush_textures_free_adj_snapshot(brush_texture_adj_t *saved,
                                            char *current_name)
{
    int i;
    for (i = 0; i < arrlen(saved); i++)
        free(saved[i].name);
    arrfree(saved);
    free(current_name);
}

/* Snapshot HSL/opacity keyed by display name so Refresh can restore them. */
static brush_texture_adj_t *brush_textures_snapshot_adjustments(
        char **out_current_name)
{
    brush_texture_adj_t *saved = NULL;
    int i;

    *out_current_name = NULL;
    if (goxel.brush_texture_index >= 0 &&
        goxel.brush_texture_index < goxel.brush_textures_count) {
        brush_texture_t *cur =
            &goxel.brush_textures[goxel.brush_texture_index];
        brush_texture_store_adjustments(cur);
        if (cur->name)
            *out_current_name = strdup(cur->name);
    }
    for (i = 0; i < goxel.brush_textures_count; i++) {
        brush_texture_t *t = &goxel.brush_textures[i];
        if (!t->name) continue;
        arrput(saved, ((brush_texture_adj_t){
            .name = strdup(t->name),
            .hue = t->hue,
            .saturation = t->saturation,
            .lightness = t->lightness,
            .opacity = t->opacity,
        }));
    }
    return saved;
}

static void brush_textures_restore_adjustments(brush_texture_adj_t *saved,
                                              const char *current_name)
{
    int i, j;
    int saved_count = arrlen(saved);

    for (i = 0; i < goxel.brush_textures_count; i++) {
        brush_texture_t *t = &goxel.brush_textures[i];
        if (!t->name) continue;
        for (j = 0; j < saved_count; j++) {
            if (!brush_texture_name_eq(t->name, saved[j].name))
                continue;
            t->hue = saved[j].hue;
            t->saturation = saved[j].saturation;
            t->lightness = saved[j].lightness;
            t->opacity = saved[j].opacity;
            break;
        }
    }

    goxel.brush_texture_index = 0;
    if (current_name) {
        for (i = 0; i < goxel.brush_textures_count; i++) {
            if (!brush_texture_name_eq(goxel.brush_textures[i].name,
                                       current_name))
                continue;
            goxel.brush_texture_index = i;
            break;
        }
    }
    if (goxel.brush_texture_index >= goxel.brush_textures_count)
        goxel.brush_texture_index = 0;
    if (goxel.brush_textures_count > 0)
        brush_texture_load_adjustments(
                &goxel.brush_textures[goxel.brush_texture_index]);
}

void goxel_brush_textures_reload(void)
{
    char dir[1024];
    char dir_create[1024];
    texture_load_ctx_t ctx;
    const char *user_dir = sys_get_user_dir();
    brush_texture_adj_t *saved_adj = NULL;
    char *saved_current_name = NULL;

    saved_adj = brush_textures_snapshot_adjustments(&saved_current_name);
    brush_textures_clear(true);
    if (!goxel_brush_textures_dir(dir, sizeof(dir))) {
        brush_textures_free_adj_snapshot(saved_adj, saved_current_name);
        return;
    }
    {
        size_t n = strlen(dir);
        if (n + 2 > sizeof(dir_create)) {
            brush_textures_free_adj_snapshot(saved_adj, saved_current_name);
            return;
        }
        memcpy(dir_create, dir, n);
        dir_create[n] = '/';
        dir_create[n + 1] = '\0';
    }
    path_normalize_slashes(dir_create);
    LOG_I("[brush-tex] user_dir='%s'", user_dir);
    LOG_I("Brush textures dir: '%s'", dir);
    sys_make_dir(dir_create);
    brush_textures_seed_user_dir_once(dir);
    ctx = (texture_load_ctx_t){.dir = dir};
    sys_list_dir(dir, brush_texture_load_from_user_dir, &ctx);
    goxel.brush_textures_count = arrlen(goxel.brush_textures);
    LOG_I("[brush-tex] runtime textures loaded: %d", goxel.brush_textures_count);
    if (goxel.brush_textures_count > 1)
        qsort(goxel.brush_textures, goxel.brush_textures_count,
              sizeof(goxel.brush_textures[0]), brush_texture_cmp);
    brush_textures_restore_adjustments(saved_adj, saved_current_name);
    brush_textures_free_adj_snapshot(saved_adj, saved_current_name);
}

int goxel_brush_textures_count(void)
{
    return goxel.brush_textures_count;
}

const brush_texture_t *goxel_brush_texture_get(int idx)
{
    if (idx < 0 || idx >= goxel.brush_textures_count) return NULL;
    return &goxel.brush_textures[idx];
}

const brush_texture_t *goxel_brush_texture_current(void)
{
    return goxel_brush_texture_get(goxel.brush_texture_index);
}

void goxel_brush_texture_set_current(int idx)
{
    if (idx < 0 || idx >= goxel.brush_textures_count) return;
    if (idx == goxel.brush_texture_index) return;
    /* Persist HSL/opacity onto the texture we are leaving. */
    if (goxel.brush_texture_index >= 0 &&
        goxel.brush_texture_index < goxel.brush_textures_count) {
        brush_texture_store_adjustments(
                &goxel.brush_textures[goxel.brush_texture_index]);
    }
    goxel.brush_texture_index = idx;
    brush_texture_load_adjustments(&goxel.brush_textures[idx]);
}

static void brush_texture_bake_preview(brush_texture_t *t)
{
    int i, n, bpp;
    uint8_t *buf;

    if (!t || !t->pixels || t->w <= 0 || t->h <= 0 ||
        !goxel.graphics_initialized)
        return;

    bpp = t->bpp > 0 ? t->bpp : 4;
    n = t->w * t->h;
    buf = malloc((size_t)n * 4);
    if (!buf) return;

    for (i = 0; i < n; i++) {
        const uint8_t *src = t->pixels + i * bpp;
        uint8_t *dst = buf + i * 4;
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = (bpp >= 4) ? src[3] : 255;
        srgb8_adjust_hsl(dst, t->hue, t->saturation, t->lightness);
        dst[3] = (uint8_t)(((int)dst[3] * (int)t->opacity) / 255);
    }

    if (!t->preview) {
        t->preview = texture_new_from_buf(buf, t->w, t->h, 4, TF_NEAREST);
    } else {
        texture_set_data(t->preview, buf, t->w, t->h, 4);
    }
    free(buf);
    t->preview_hue = t->hue;
    t->preview_saturation = t->saturation;
    t->preview_lightness = t->lightness;
    t->preview_opacity = t->opacity;
}

texture_t *goxel_brush_texture_preview_get(int idx)
{
    brush_texture_t *t;
    bool dirty;

    if (idx < 0 || idx >= goxel.brush_textures_count) return NULL;
    t = &goxel.brush_textures[idx];

    /* Keep the active texture's stored values in sync with the live sliders. */
    if (idx == goxel.brush_texture_index)
        brush_texture_store_adjustments(t);

    dirty = !t->preview ||
            t->preview_hue != t->hue ||
            t->preview_saturation != t->saturation ||
            t->preview_lightness != t->lightness ||
            t->preview_opacity != t->opacity;
    if (dirty)
        brush_texture_bake_preview(t);
    return t->preview;
}
