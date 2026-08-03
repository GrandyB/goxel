/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#include "goxel.h"
#include "metadata.h"
#include "filters/metadata_gui.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    filter_t filter;
} filter_metadata_t;

#define TEMPLATE_MAX 64

typedef struct {
    char paths[TEMPLATE_MAX][1024];
    char names[TEMPLATE_MAX][256];
    int count;
    int selected;
} template_popup_t;

static template_popup_t g_template_popup;
static bool g_open_template_popup = false;

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
    int need_slash;
    if (!out || !out_sz || !a || !b) return false;
    na = strlen(a);
    nb = strlen(b);
    while (na > 0 && (a[na - 1] == '/' || a[na - 1] == '\\'))
        na--;
    need_slash = na > 0;
    if (na + (need_slash ? 1 : 0) + nb + 1 > out_sz) return false;
    memcpy(out, a, na);
    if (need_slash) {
        out[na] = '/';
        memcpy(out + na + 1, b, nb);
        out[na + 1 + nb] = '\0';
    } else {
        memcpy(out + na, b, nb);
        out[na + nb] = '\0';
    }
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

static void on_open(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(true);
}

static void on_close(filter_t *filter_)
{
    (void)filter_;
    custom_objects_set_editor_active(false);
    custom_objects_set_list_selected(NULL);
}

static int on_template_file(const char *dirpath, const char *name, void *user)
{
    template_popup_t *popup = user;
    char display[256];
    size_t len;

    if (!str_endswith(name, ".json")) return 0;
    if (popup->count >= TEMPLATE_MAX) return 0;
    snprintf(popup->paths[popup->count], sizeof(popup->paths[0]),
             "%s/%s", dirpath, name);
    snprintf(display, sizeof(display), "%s", name);
    len = strlen(display);
    if (len > 5 && strcasecmp(display + len - 5, ".json") == 0)
        display[len - 5] = '\0';
    snprintf(popup->names[popup->count], sizeof(popup->names[0]), "%s", display);
    popup->count++;
    return 0;
}

typedef struct {
    const char *target_dir;
    int copied_count;
    bool force_overwrite;
} template_seed_ctx_t;

typedef struct {
    int json_count;
} template_json_count_ctx_t;

static int metadata_template_count_json(const char *dir, const char *name,
                                        void *user)
{
    template_json_count_ctx_t *ctx = user;
    (void)dir;
    if (str_endswith_case(name, ".json"))
        ctx->json_count++;
    return 0;
}

/* Marker holds GOXEL_VERSION_STR so bundled templates reseed on version bumps. */
static bool metadata_template_marker_matches_version(const char *marker_path)
{
    char buf[128];
    FILE *f;
    size_t n;

    f = fopen(marker_path, "rb");
    if (!f) return false;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
                     buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        buf[--n] = '\0';
    return strcmp(buf, GOXEL_VERSION_STR) == 0;
}

static int metadata_template_seed_copy_asset(int idx, const char *path,
                                             void *user)
{
    template_seed_ctx_t *ctx = user;
    const char *name = path_basename_ptr(path);
    char dst[1024];
    FILE *f = NULL;
    const void *data;
    int sz = 0;

    (void)idx;
    if (!str_endswith_case(path, ".json"))
        return 0;
    LOG_I("[meta-tmpl][asset] candidate: '%s'", path);
    if (!path_join2(dst, sizeof(dst), ctx->target_dir, name)) {
        LOG_I("[meta-tmpl][asset] skip (join failed): '%s'", path);
        return 0;
    }
    path_normalize_slashes(dst);
    LOG_I("[meta-tmpl][asset] destination: '%s'", dst);
    if (!ctx->force_overwrite) {
        f = fopen(dst, "rb");
        if (f) { // Keep user file if already present.
            fclose(f);
            ctx->copied_count++;
            LOG_I("[meta-tmpl][asset] already exists: '%s'", dst);
            return 0;
        }
    }
    data = assets_get(path, &sz);
    if (!data || sz <= 0) {
        LOG_I("[meta-tmpl][asset] assets_get failed: '%s' (size=%d)", path, sz);
        return 0;
    }
    /* Text assets include a trailing NUL in the recorded size. */
    if (sz > 0 && ((const char *)data)[sz - 1] == '\0')
        sz--;
    f = fopen(dst, "wb");
    if (!f) {
        LOG_I("[meta-tmpl][asset] open write failed: '%s' (errno=%d)",
              dst, errno);
        return 0;
    }
    fwrite(data, (size_t)sz, 1, f);
    fclose(f);
    ctx->copied_count++;
    LOG_I("[meta-tmpl][asset] %s: '%s' bytes=%d",
          ctx->force_overwrite ? "overwrote" : "copied", dst, sz);
    return 0;
}

static int metadata_template_seed_copy_from_disk(const char *dir,
                                                 const char *name, void *user)
{
    template_seed_ctx_t *ctx = user;
    char src[1024], dst[1024];
    const char *base_name = NULL;
    char *data = NULL;
    int size = 0;
    FILE *f = NULL;

    (void)dir;
    if (!str_endswith_case(name, ".json"))
        return 0;
    LOG_I("[meta-tmpl][disk] candidate: '%s'", name);
    if (!path_join2(src, sizeof(src), "data/metadata-templates", name))
        return 0;
    path_normalize_slashes(src);
    base_name = path_basename_ptr(src);
    if (!path_join2(dst, sizeof(dst), ctx->target_dir, base_name))
        return 0;
    path_normalize_slashes(dst);
    LOG_I("[meta-tmpl][disk] src='%s' dst='%s'", src, dst);
    if (!ctx->force_overwrite) {
        f = fopen(dst, "rb");
        if (f) {
            fclose(f);
            ctx->copied_count++;
            LOG_I("[meta-tmpl][disk] already exists: '%s'", dst);
            return 0;
        }
    }
    data = read_file(src, &size);
    if (!data || size <= 0) {
        LOG_I("[meta-tmpl][disk] read failed: '%s' size=%d", src, size);
        free(data);
        return 0;
    }
    f = fopen(dst, "wb");
    if (!f) {
        LOG_I("[meta-tmpl][disk] open write failed: '%s' (errno=%d)",
              dst, errno);
        free(data);
        return 0;
    }
    fwrite(data, size, 1, f);
    fclose(f);
    free(data);
    ctx->copied_count++;
    LOG_I("[meta-tmpl][disk] %s: '%s' bytes=%d",
          ctx->force_overwrite ? "overwrote" : "copied", dst, size);
    return 0;
}

static void metadata_templates_seed_user_dir_once(const char *dir)
{
    char marker[1024];
    FILE *f;
    template_seed_ctx_t ctx = {
        .target_dir = dir, .copied_count = 0, .force_overwrite = false};
    template_json_count_ctx_t count_ctx = {0};
    int n;

    if (!path_join2(marker, sizeof(marker), dir, ".seeded_metadata_templates"))
        return;
    path_normalize_slashes(marker);
    LOG_I("[meta-tmpl] marker path: '%s'", marker);
    f = fopen(marker, "rb");
    if (f) {
        fclose(f);
        if (metadata_template_marker_matches_version(marker)) {
            LOG_I("[meta-tmpl] marker matches version %s; checking dir",
                  GOXEL_VERSION_STR);
            sys_list_dir(dir, metadata_template_count_json, &count_ctx);
            LOG_I("[meta-tmpl] json files currently in dir: %d",
                  count_ctx.json_count);
            if (count_ctx.json_count > 0)
                return;
            LOG_I("[meta-tmpl] marker ok but dir empty; forcing reseed");
            ctx.force_overwrite = true;
        } else {
            LOG_I("[meta-tmpl] marker version mismatch (want %s); "
                  "reseeding with overwrite", GOXEL_VERSION_STR);
            ctx.force_overwrite = true;
        }
    }

    LOG_I("[meta-tmpl] seeding from embedded assets");
    n = assets_list("data/metadata-templates", &ctx,
                    metadata_template_seed_copy_asset);
    LOG_I("[meta-tmpl] assets_list matched=%d copied_or_existing=%d",
          n, ctx.copied_count);
    if (ctx.copied_count == 0) {
        // Fallback for builds where templates are not embedded in assets.
        LOG_I("[meta-tmpl] embedded assets unavailable; trying disk fallback");
        sys_list_dir("data/metadata-templates",
                     metadata_template_seed_copy_from_disk, &ctx);
    }
    LOG_I("Metadata template seeding: target='%s' copied_or_existing=%d "
          "overwrite=%d", dir, ctx.copied_count, (int)ctx.force_overwrite);
    if (ctx.copied_count == 0)
        return;
    f = fopen(marker, "wb");
    if (f) {
        fprintf(f, "%s\n", GOXEL_VERSION_STR);
        fclose(f);
        LOG_I("[meta-tmpl] marker written for version %s", GOXEL_VERSION_STR);
    } else {
        LOG_I("[meta-tmpl] marker write failed (errno=%d)", errno);
    }
}

static void refresh_template_list(template_popup_t *popup)
{
    char dir[1024];
    char dir_create[1024];
    const char *user_dir = sys_get_user_dir();
    size_t n;

    popup->count = 0;
    popup->selected = 0;
    if (!user_dir) {
        LOG_I("[meta-tmpl] no user dir; skip template refresh");
        return;
    }
    if (!path_join2(dir, sizeof(dir), user_dir, "metadata-templates"))
        return;
    path_normalize_slashes(dir);
    n = strlen(dir);
    if (n + 2 > sizeof(dir_create))
        return;
    memcpy(dir_create, dir, n);
    dir_create[n] = '/';
    dir_create[n + 1] = '\0';
    path_normalize_slashes(dir_create);
    LOG_I("[meta-tmpl] user_dir='%s'", user_dir);
    LOG_I("Metadata templates dir: '%s'", dir);
    if (sys_make_dir(dir_create) != 0)
        LOG_I("[meta-tmpl] sys_make_dir failed: '%s' (errno=%d)",
              dir_create, errno);
    metadata_templates_seed_user_dir_once(dir);
    sys_list_dir(dir, on_template_file, popup);
    LOG_I("[meta-tmpl] templates listed: %d", popup->count);
}

static int template_popup_gui(void *data)
{
    template_popup_t *popup = &g_template_popup;
    image_t *img = goxel.image;
    const char *names[TEMPLATE_MAX];
    int i, has_items, ret = 0;

    (void)data;
    has_items = popup->count > 0;
    if (has_items) {
        if (popup->selected < 0) popup->selected = 0;
        if (popup->selected >= popup->count)
            popup->selected = popup->count - 1;
        for (i = 0; i < popup->count; i++)
            names[i] = popup->names[i];
        gui_combo("Template", &popup->selected, names, popup->count);
    } else {
        gui_text("No templates found.");
    }

    if (img && img->custom_objects)
        gui_text("Replaces all current items.");

    gui_row_begin(0);
    if (has_items && gui_button("Load template", 0, 0)) {
        image_history_push(img);
        custom_objects_load_template_json(popup->paths[popup->selected], img);
        custom_objects_set_list_selected(NULL);
        ret = 1;
    } else if (gui_button("Cancel", 0, 0)) {
        ret = 2;
    }
    gui_row_end();
    return ret;
}

static void on_mouse(filter_t *filter_, const float viewport[4])
{
    (void)filter_;
    custom_objects_edit_iter(viewport);
}

static int gui(filter_t *filter_)
{
    image_t *img = goxel.image;
    (void)filter_;

    if (!img) return 0;

    gui_text_wrapped(
        "Create and manage metadata about the map.\n"
        "Use the 'Group' type to collect multiple items under one parent.\n"
        "Use the 'S' button to temporarily solo the visibility to the chosen item.");

    if (gui_button("Load template", 1.0, 0))
        g_open_template_popup = true;

    if (g_open_template_popup) {
        g_open_template_popup = false;
        refresh_template_list(&g_template_popup);
        gui_open_popup_sized("Load template", 300, 150, 0,
                             NULL, template_popup_gui);
    }

    gui_checkbox("Show when window is closed",
                 &img->custom_objects_show_when_closed,
                 "Keep spatial metadata items and their labels visible after "
                 "closing this window");

    metadata_gui_panel(img);
    return 0;
}

FILTER_REGISTER(customobjects, filter_metadata_t,
                .name = "Metadata",
                .menu = "view",
                .on_open = on_open,
                .on_close = on_close,
                .override_mouse = true,
                .mouse_fn = on_mouse,
                .panel_width = 380,
                .gui_fn = gui, )
