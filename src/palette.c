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

#include <errno.h>

/*
 * Function: palette_search
 * Search a given color in a palette
 *
 * Parameters:
 *   palette    - A palette.
 *   col        - The color we are looking for.
 *   exact      - If set to true, return -1 if no color is found, else
 *                return the closest color.
 *
 * Return:
 *   The index of the color in the palette.
 */
int palette_search(const palette_t *palette, const uint8_t col[4],
                   bool exact)
{
    int i;
    assert(exact); // For the moment.
    for (i = 0; i < palette->size; i++) {
        if (memcmp(col, palette->entries[i].color, 4) == 0)
            return i;
    }
    return -1;
}

void palette_insert(palette_t *p, const uint8_t col[4], const char *name)
{
    palette_entry_t *e;
    if (palette_search(p, col, true) != -1) return;
    if (p->allocated <= p->size) {
        p->allocated += 64;
        p->entries = realloc(p->entries, p->allocated * sizeof(*p->entries));
    }
    e = &p->entries[p->size];
    memset(e, 0, sizeof(*e));
    memcpy(e->color, col, 4);
    if (name)
        snprintf(e->name, sizeof(e->name), "%s", name);
    p->size++;
}

void palette_remove_at(palette_t *p, int idx)
{
    if (!p || idx < 0 || idx >= p->size)
        return;
    memmove(p->entries + idx, p->entries + idx + 1,
            (size_t)(p->size - idx - 1) * sizeof(*p->entries));
    p->size--;
}

void palette_free(palette_t *p)
{
    if (!p)
        return;
    free(p->entries);
    free(p);
}

palette_t *palette_clone(const palette_t *src, const char *new_name)
{
    palette_t *d;

    d = calloc(1, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", new_name);
    d->columns = src->columns;
    d->size = src->size;
    d->allocated = src->size;
    if (src->size > 0) {
        d->entries = malloc(d->allocated * sizeof(*d->entries));
        memcpy(d->entries, src->entries, src->size * sizeof(*d->entries));
    }
    return d;
}

palette_t *palette_new_empty(const char *name)
{
    palette_t *p;

    p = calloc(1, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->size = 0;
    p->allocated = 0;
    p->entries = NULL;
    return p;
}

void palette_list_remove(palette_t **list_head, palette_t *p)
{
    DL_DELETE(*list_head, p);
    palette_free(p);
}

bool palette_name_in_use(const palette_t *list, const char *name,
                          const palette_t *except)
{
    const palette_t *q;

    for (q = list; q; q = q->next) {
        if (q == except)
            continue;
        if (strcmp(q->name, name) == 0)
            return true;
    }
    return false;
}

static void palette_sanitize_basename(const char *name, char *out, size_t out_sz)
{
    size_t i = 0;
    const char *s;

    if (!name || !name[0]) {
        snprintf(out, out_sz, "palette");
        return;
    }
    for (s = name; *s && i + 1 < out_sz; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
            c == '.')
            out[i++] = (char)c;
        else
            out[i++] = '_';
    }
    out[i] = '\0';
    while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '.'))
        out[--i] = '\0';
    if (!out[0])
        snprintf(out, out_sz, "palette");
}

static void fprint_gpl_one_line(FILE *f, const char *str)
{
    const char *s;

    for (s = str ? str : ""; *s; s++) {
        if (*s == '\n' || *s == '\r')
            fputc(' ', f);
        else
            fputc(*s, f);
    }
}

int palette_save_user_gpl(const palette_t *p)
{
    const char *root;
    char *path = NULL;
    char base_fn[240];
    FILE *f;
    int i, cols, err;

    if (!p)
        return -2;
    root = sys_get_user_dir();
    if (!root || !root[0])
        return -1;

    palette_sanitize_basename(p->name, base_fn, sizeof(base_fn));
    asprintf(&path, "%s/palettes/%s.gpl", root, base_fn);
    if (!path)
        return -2;

    if (sys_make_dir(path) != 0) {
        LOG_E("palette_save_user_gpl: sys_make_dir failed for %s", path);
        free(path);
        return -2;
    }

    f = fopen(path, "wb");
    if (!f) {
        LOG_E("palette_save_user_gpl: cannot open %s", path);
        free(path);
        return -2;
    }

    cols = p->columns > 0 ? p->columns : 8;
    fprintf(f, "GIMP Palette\nName: ");
    fprint_gpl_one_line(f, p->name);
    fprintf(f, "\nColumns: %d\n#\n", cols);

    for (i = 0; i < p->size; i++) {
        const palette_entry_t *e = &p->entries[i];
        int r = e->color[0];
        int g = e->color[1];
        int b = e->color[2];

        fprintf(f, "%d\t%d\t%d\t", r, g, b);
        fprint_gpl_one_line(f, e->name);
        fprintf(f, "\n");
    }

    err = fclose(f);
    free(path);
    if (err != 0) {
        LOG_E("palette_save_user_gpl: fclose failed");
        return -2;
    }
    return 0;
}

int palette_delete_user_gpl(const palette_t *p)
{
    const char *root;
    char *path = NULL;
    char base_fn[240];

    if (!p)
        return -2;
    root = sys_get_user_dir();
    if (!root || !root[0])
        return 0;

    palette_sanitize_basename(p->name, base_fn, sizeof(base_fn));
    /* (asprintf) bypasses goxel.h's CHECK(asprintf...) macro. */
    if ((asprintf)(&path, "%s/palettes/%s.gpl", root, base_fn) < 0 || !path)
        return -2;

    if (sys_delete_file(path) != 0 && errno != ENOENT) {
        LOG_E("palette_delete_user_gpl: cannot remove %s", path);
        free(path);
        return -2;
    }
    free(path);
    return 0;
}


// Parse a gimp palette.
// XXX: we don't check for buffer overflow!
static int parse_gpl(const char *data, char *name, int *columns,
                     palette_entry_t *entries)
{
    const char *start, *end;
    int linen, r, g, b, nb = 0;
    char entry_name[128];

    for (linen = 1, start = data; *start; start = end + 1, linen++) {
        end = strchr(start, '\n');
        if (!end) end = start + strlen(start);

        if (name && sscanf(start, "Name: %[^\n]", name) == 1) {
            name = NULL;
            continue;
        }
        if (columns && sscanf(start, "Columns: %d", columns) == 1) {
            columns = NULL;
            continue;
        }

        if (sscanf(start, "%d %d %d %[^\n]", &r, &g, &b, entry_name) >= 3) {
            if (entries) {
                strcpy(entries[nb].name, entry_name);
                entries[nb].color[0] = r;
                entries[nb].color[1] = g;
                entries[nb].color[2] = b;
                entries[nb].color[3] = 255;
            }
            nb++;
        }
        if (!*end) break;
    }
    return nb;
}

/*
 * Function: parse_dat
 * Parse a Build engine (duke2d, blood...) palette
 */
static int parse_dat(const uint8_t *data, int len, palette_entry_t *entries)
{
    int i;
    if (len < 768) return -1;
    for (i = 0; i < 256; i++) {
        entries[i].color[0] = data[i * 3 + 0] * 4;
        entries[i].color[1] = data[i * 3 + 1] * 4;
        entries[i].color[2] = data[i * 3 + 2] * 4;
        entries[i].color[3] = 255;
    }
    return 256;
}

/*
 * Function: parse_png
 * Parse a png image into a palette.
 */
static int parse_png(const void *data, int len, palette_t *palette)
{
    int i, w, h, bpp = 3;
    uint8_t *img, color[4];
    img = img_read_from_mem((void*)data, len, &w, &h, &bpp);
    if (!img) return -1;

    for (i = 0; i < w * h; i++) {
        memcpy(color, (uint8_t[]){0, 0, 0, 255}, 4);
        memcpy(color, img + i * bpp, bpp);
        palette_insert(palette, color, NULL);
    }
    free(img);
    return palette->size;
}


static int on_system_palette(int i, const char *path, void *user)
{
    palette_t **list = user;
    const char *data;
    palette_t *pal;

    (void)i;
    data = assets_get(path, NULL);
    if (!data)
        return 0;

    pal = calloc(1, sizeof(*pal));
    pal->size = parse_gpl(data, pal->name, &pal->columns, NULL);
    if (pal->name[0] && palette_name_in_use(*list, pal->name, NULL)) {
        free(pal);
        return 0;
    }
    pal->entries = calloc(pal->size, sizeof(*pal->entries));
    parse_gpl(data, NULL, NULL, pal->entries);
    DL_APPEND(*list, pal);
    return 0;
}

/* First run: copy embedded palettes into the user folder, then create bundled_marker.
 * Later runs: marker present → skip copy so the user controls which files exist. */
static int copy_bundled_palette_asset(int i, const char *path, void *user)
{
    const char *root = user;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const void *data;
    int sz;
    char *outpath = NULL;
    FILE *f;

    (void)i;
    if (!str_endswith(base, ".gpl"))
        return 0;
    data = assets_get(path, &sz);
    if (!data || sz <= 0)
        return 0;
    asprintf(&outpath, "%s/palettes/%s", root, base);
    if (sys_make_dir(outpath) != 0) {
        LOG_E("copy_bundled_palette_asset: sys_make_dir failed for %s", outpath);
        free(outpath);
        return 0;
    }
    f = fopen(outpath, "wb");
    if (!f) {
        LOG_E("copy_bundled_palette_asset: cannot open %s", outpath);
        free(outpath);
        return 0;
    }
    if (fwrite(data, 1, (size_t)sz, f) != (size_t)sz) {
        LOG_E("copy_bundled_palette_asset: short write %s", outpath);
        fclose(f);
        free(outpath);
        return 0;
    }
    if (fclose(f) != 0) {
        LOG_E("copy_bundled_palette_asset: fclose failed %s", outpath);
        free(outpath);
        return 0;
    }
    free(outpath);
    return 0;
}

static void palette_seed_bundled_if_needed(void)
{
    const char *root;
    char *marker_path = NULL;
    FILE *mf;

    root = sys_get_user_dir();
    if (!root || !root[0])
        return;
    asprintf(&marker_path, "%s/palettes/bundled_marker", root);

    mf = fopen(marker_path, "rb");
    if (mf) {
        fclose(mf);
        free(marker_path);
        return;
    }

    assets_list("data/palettes/", (void *)root, copy_bundled_palette_asset);

    mf = fopen(marker_path, "wb");
    if (mf)
        fclose(mf);
    else
        LOG_E("palette_seed_bundled_if_needed: cannot create %s", marker_path);
    free(marker_path);
}

static int on_palette2(const char *dir, const char *name, void *user)
{
    palette_t **list = user;
    char *data, *path;
    int size, err = 0;
    palette_t *pal;

    if (    !str_endswith(name, ".gpl") &&
            !str_endswith(name, ".dat") &&
            !str_endswith(name, ".png"))
        return 0;

    asprintf(&path, "%s/%s", dir, name);
    pal = calloc(1, sizeof(*pal));
    data = read_file(path, &size);
    if (str_endswith(name, ".gpl")) {
        pal->size = parse_gpl(data, pal->name, &pal->columns, NULL);
        pal->entries = calloc(pal->size, sizeof(*pal->entries));
        err = parse_gpl(data, NULL, NULL, pal->entries);
    }
    else if (str_endswith(name, ".dat")) {
        snprintf(pal->name, sizeof(pal->name), "%s", name);
        pal->size = 256;
        pal->entries = calloc(pal->size, sizeof(*pal->entries));
        err = parse_dat((void*)data, size, pal->entries);
    }
    else if (str_endswith(name, ".png")) {
        snprintf(pal->name, sizeof(pal->name), "%s", name);
        err = parse_png(data, size, pal);
    }

    if (err < 0) {
        LOG_E("Cannot parse palette %s", path);
        free(pal);
        goto end;
    }

    DL_APPEND(*list, pal);
end:
    free(path);
    free(data);
    return 0;
}


void palette_load_all(palette_t **list)
{
    char *dir;

    if (sys_get_user_dir()) {
        palette_seed_bundled_if_needed();
        asprintf(&dir, "%s/palettes", sys_get_user_dir());
        sys_list_dir(dir, on_palette2, list);
        free(dir);
    } else {
        assets_list("data/palettes/", list, on_system_palette);
    }
}

palette_t *palette_reload_all(palette_t **list, const char *prefer_name)
{
    palette_t *p, *tmp;

    DL_FOREACH_SAFE(*list, p, tmp) {
        DL_DELETE(*list, p);
        palette_free(p);
    }
    *list = NULL;

    palette_load_all(list);

    if (!prefer_name || !prefer_name[0])
        return NULL;

    DL_FOREACH(*list, p) {
        if (strcmp(p->name, prefer_name) == 0)
            return p;
    }
    return NULL;
}
