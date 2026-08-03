/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

/* MagicaVoxel .vox export tailored for Trenchblocks:
 * - Palette indices 1-8: spawn/pickup metadata colours
 * - Indices 9-16: reserved empty
 * - Indices 17+: opaque recent map colours, then quantized fill
 * - Named scene objects T1-T4 (2x2 of 256xy tiles)
 */

#include "goxel.h"
#include "file_format.h"
#include "metadata.h"

#include <limits.h>
#include <string.h>

#define WRITE(type, v, file) \
    ({ type v_ = v; fwrite(&v_, sizeof(v_), 1, file); })

#define VOX_TILE 256
#define TB_PAL_META_FIRST 1
#define TB_PAL_MAP_FIRST 17

typedef struct {
    int ox, oy, oz;
    int sx, sy, sz;
    int nb_vox;
    int filled;
    uint8_t *voxels;
    const char *name;
} tb_tile_t;

typedef struct {
    int x, y, z;
    uint8_t index;
} tb_stamp_t;

static const uint8_t TB_META_RGB[8][3] = {
    {255, 66, 66},   /* 1 FlagRed */
    {123, 0, 0},     /* 2 SpawnRed */
    {25, 148, 255},  /* 3 FlagBlue */
    {0, 91, 172},    /* 4 SpawnBlue */
    {237, 79, 255},  /* 5 PickupHealth */
    {255, 182, 0},   /* 6 PickupAmmo */
    {0, 0, 0},       /* 7 PickupSatchel */
    {255, 255, 255}, /* 8 SpectatorSpawnPoint */
};

static int tb_meta_index_for_name(const char *name)
{
    if (!name || !name[0]) return -1;
    if (strcmp(name, "FlagRed") == 0) return 1;
    if (strcmp(name, "SpawnRed") == 0) return 2;
    if (strcmp(name, "FlagBlue") == 0) return 3;
    if (strcmp(name, "SpawnBlue") == 0) return 4;
    if (strcmp(name, "PickupHealth") == 0) return 5;
    if (strcmp(name, "PickupAmmo") == 0) return 6;
    if (strcmp(name, "PickupSatchel") == 0) return 7;
    if (strcmp(name, "SpectatorSpawnPoint") == 0) return 8;
    return -1;
}

static const char *tb_effective_meta_name(const custom_object_t *obj)
{
    if (obj->group && obj->group->name[0])
        return obj->group->name;
    return obj->name;
}

static int tb_get_map_color_index(uint8_t v[4], uint8_t (*palette)[4])
{
    const uint8_t *c;
    int i, dist, best = TB_PAL_MAP_FIRST, best_dist = INT_MAX;
    bool have_best = false;

    for (i = TB_PAL_MAP_FIRST; i < 256; i++) {
        c = palette[i];
        if (c[3] != 255) continue; /* skip empty / unused slots */
        dist = abs((int)c[0] - (int)v[0]) +
               abs((int)c[1] - (int)v[1]) +
               abs((int)c[2] - (int)v[2]);
        if (dist == 0) return i;
        if (!have_best || dist < best_dist) {
            best_dist = dist;
            best = i;
            have_best = true;
        }
    }
    return have_best ? best : TB_PAL_MAP_FIRST;
}

static bool tb_palette_has_opaque_rgb(uint8_t (*palette)[4], int last_excl,
                                      const uint8_t rgb[3])
{
    int i;
    for (i = 1; i < last_excl; i++) {
        if (palette[i][3] != 255) continue;
        if (palette[i][0] == rgb[0] && palette[i][1] == rgb[1] &&
            palette[i][2] == rgb[2])
            return true;
    }
    return false;
}

/* Insert opaque recent colours at palette[17..], return how many were added. */
static int tb_add_recent_colors(uint8_t (*palette)[4], const image_t *image)
{
    int i, n = 0, slot;
    const image_recent_color_t *e;

    if (!image) return 0;
    for (i = 0; i < image->recent_color_count; i++) {
        e = &image->recent_colors[i];
        /* Skip any transparency (only fully opaque colours are reserved). */
        if (e->color[3] != 255) continue;
        slot = TB_PAL_MAP_FIRST + n;
        if (slot >= 256) break;
        if (tb_palette_has_opaque_rgb(palette, slot, e->color))
            continue;
        palette[slot][0] = e->color[0];
        palette[slot][1] = e->color[1];
        palette[slot][2] = e->color[2];
        palette[slot][3] = 255;
        n++;
    }
    return n;
}

static int tb_voxel_cmp(const void *a_, const void *b_)
{
    const uint8_t *a = a_;
    const uint8_t *b = b_;
    if (a[2] != b[2]) return cmp(a[2], b[2]);
    if (a[1] != b[1]) return cmp(a[1], b[1]);
    if (a[0] != b[0]) return cmp(a[0], b[0]);
    return 0;
}

static void tb_write_string(FILE *file, const char *s)
{
    int32_t len = (int32_t)strlen(s);
    WRITE(int32_t, len, file);
    fwrite(s, 1, len, file);
}

static void tb_write_dict_entry(FILE *file, const char *key, const char *value)
{
    tb_write_string(file, key);
    tb_write_string(file, value);
}

static void tb_write_size_xyzi(FILE *file, int sx, int sy, int sz,
                               const uint8_t *voxels, int nb_vox)
{
    int i;

    fprintf(file, "SIZE");
    WRITE(uint32_t, 4 * 3, file);
    WRITE(uint32_t, 0, file);
    WRITE(uint32_t, sx, file);
    WRITE(uint32_t, sy, file);
    WRITE(uint32_t, sz, file);

    fprintf(file, "XYZI");
    WRITE(uint32_t, 4 * nb_vox + 4, file);
    WRITE(uint32_t, 0, file);
    WRITE(uint32_t, nb_vox, file);
    for (i = 0; i < nb_vox; i++)
        fwrite(voxels + i * 4, 4, 1, file);
}

static void tb_write_rgba(FILE *file, uint8_t (*palette)[4])
{
    int i;

    fprintf(file, "RGBA");
    WRITE(uint32_t, 4 * 256, file);
    WRITE(uint32_t, 0, file);
    for (i = 1; i < 256; i++) {
        WRITE(uint8_t, palette[i][0], file);
        WRITE(uint8_t, palette[i][1], file);
        WRITE(uint8_t, palette[i][2], file);
        WRITE(uint8_t, palette[i][3], file);
    }
    WRITE(uint32_t, 0, file);
}

static int tb_dict_entry_bytes(const char *key, const char *value)
{
    return 4 + (int)strlen(key) + 4 + (int)strlen(value);
}

static int tb_ntrn_content_size(const char *name, const char *trans)
{
    int size = 4; /* node id */

    size += 4; /* node dict entry count */
    if (name)
        size += tb_dict_entry_bytes("_name", name);
    size += 4 + 4 + 4 + 4; /* child, reserved, layer, num frames */
    if (trans) {
        size += 4; /* frame dict entry count */
        size += tb_dict_entry_bytes("_t", trans);
    } else {
        size += 4; /* empty frame dict */
    }
    return size;
}

static void tb_write_ntrn(FILE *file, int node_id, int child_id,
                          const char *name, const char *trans)
{
    WRITE(uint32_t, tb_ntrn_content_size(name, trans), file);
    WRITE(uint32_t, 0, file);
    WRITE(int32_t, node_id, file);
    if (name) {
        WRITE(int32_t, 1, file);
        tb_write_dict_entry(file, "_name", name);
    } else {
        WRITE(int32_t, 0, file);
    }
    WRITE(int32_t, child_id, file);
    WRITE(int32_t, -1, file);
    WRITE(int32_t, 0, file);
    WRITE(int32_t, 1, file);
    if (trans) {
        WRITE(int32_t, 1, file);
        tb_write_dict_entry(file, "_t", trans);
    } else {
        WRITE(int32_t, 0, file);
    }
}

static int tb_ngrp_content_size(int nb_children)
{
    return 4 + 4 + 4 + 4 * nb_children;
}

static void tb_write_ngrp(FILE *file, int node_id, const int *children, int nb)
{
    int i;

    WRITE(uint32_t, tb_ngrp_content_size(nb), file);
    WRITE(uint32_t, 0, file);
    WRITE(int32_t, node_id, file);
    WRITE(int32_t, 0, file);
    WRITE(int32_t, nb, file);
    for (i = 0; i < nb; i++)
        WRITE(int32_t, children[i], file);
}

static int tb_nshp_content_size(void)
{
    return 4 + 4 + 4 + 4 + 4;
}

static void tb_write_nshp(FILE *file, int node_id, int model_id)
{
    WRITE(uint32_t, tb_nshp_content_size(), file);
    WRITE(uint32_t, 0, file);
    WRITE(int32_t, node_id, file);
    WRITE(int32_t, 0, file);
    WRITE(int32_t, 1, file);
    WRITE(int32_t, model_id, file);
    WRITE(int32_t, 0, file);
}

static int tb_size_xyzi_chunk_bytes(int nb_vox)
{
    return (12 + 12) + (12 + 4 + 4 * nb_vox);
}

static const char *tb_tile_name(int tx, int ty, int nx, int ny)
{
    bool low_x = (tx == 0);
    bool high_y = (ty == ny - 1);

    if (high_y)
        return low_x ? "T1" : "T2";
    return low_x ? "T3" : "T4";
}

static int tb_find_stamp_index(const tb_stamp_t *stamps, int n,
                               int x, int y, int z)
{
    int i;
    for (i = 0; i < n; i++) {
        if (stamps[i].x == x && stamps[i].y == y && stamps[i].z == z)
            return stamps[i].index;
    }
    return -1;
}

/* Heightmap: INT_MIN = empty column. Indexed as (x - xmin) + (y - ymin) * sx. */
static int *tb_build_heightmap(const volume_t *volume,
                               int xmin, int ymin, int sx, int sy,
                               int zmin, int zmax_excl)
{
    int *hm;
    int pos[3], ix, iy, n;
    uint8_t v[4];
    volume_iterator_t iter;

    n = sx * sy;
    hm = malloc((size_t)n * sizeof(*hm));
    if (!hm) return NULL;
    for (ix = 0; ix < n; ix++)
        hm[ix] = INT_MIN;

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        if (pos[0] < xmin || pos[0] >= xmin + sx) continue;
        if (pos[1] < ymin || pos[1] >= ymin + sy) continue;
        if (pos[2] < zmin || pos[2] >= zmax_excl) continue;
        volume_get_at(volume, &iter, pos, v);
        if (v[3] < 127) continue;
        ix = pos[0] - xmin;
        iy = pos[1] - ymin;
        if (pos[2] > hm[ix + iy * sx])
            hm[ix + iy * sx] = pos[2];
    }
    return hm;
}

static bool tb_add_stamp(tb_stamp_t **stamps, int *n, int *cap,
                         int x, int y, int z, uint8_t index,
                         int zmin, int *zmax_excl_io)
{
    tb_stamp_t *ns;
    int i;

    if (z < zmin || z >= zmin + VOX_TILE) {
        gui_alert("vox (Trenchblocks)",
                  "Metadata marker would exceed the 256 block height limit.");
        return false;
    }
    if (z + 1 > *zmax_excl_io)
        *zmax_excl_io = z + 1;
    if (*zmax_excl_io - zmin > VOX_TILE) {
        gui_alert("vox (Trenchblocks)",
                  "Metadata marker would exceed the 256 block height limit.");
        return false;
    }

    for (i = 0; i < *n; i++) {
        if ((*stamps)[i].x == x && (*stamps)[i].y == y && (*stamps)[i].z == z) {
            (*stamps)[i].index = index;
            return true;
        }
    }

    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        ns = realloc(*stamps, (size_t)(*cap) * sizeof(**stamps));
        if (!ns) {
            gui_alert("vox (Trenchblocks)", "Out of memory.");
            return false;
        }
        *stamps = ns;
    }
    (*stamps)[*n].x = x;
    (*stamps)[*n].y = y;
    (*stamps)[*n].z = z;
    (*stamps)[*n].index = index;
    (*n)++;
    return true;
}

static bool tb_stamp_column(const int *hm, int xmin, int ymin, int sx, int sy,
                            int x, int y, uint8_t index, int zmin,
                            int *zmax_excl_io, tb_stamp_t **stamps,
                            int *n, int *cap)
{
    int ix, iy, top;

    if (x < xmin || x >= xmin + sx || y < ymin || y >= ymin + sy)
        return true;
    ix = x - xmin;
    iy = y - ymin;
    top = hm[ix + iy * sx];
    if (top == INT_MIN)
        return true; /* empty column: skip */
    return tb_add_stamp(stamps, n, cap, x, y, top + 1, index, zmin, zmax_excl_io);
}

static bool tb_collect_stamps(const image_t *image, const int *hm,
                              int xmin, int ymin, int sx, int sy, int zmin,
                              int *zmax_excl_io, tb_stamp_t **stamps,
                              int *n_stamps)
{
    const custom_object_t *obj;
    const char *meta_name;
    int index, x0, x1, y0, y1, x, y, cap = 0;

    *stamps = NULL;
    *n_stamps = 0;

    DL_FOREACH(image->custom_objects, obj) {
        if (obj->type == CUSTOM_OBJ_GROUP)
            continue;

        meta_name = tb_effective_meta_name(obj);
        index = tb_meta_index_for_name(meta_name);
        if (index < 0)
            continue;

        if (obj->type == CUSTOM_OBJ_ZONE_2D || obj->type == CUSTOM_OBJ_ZONE_3D) {
            x0 = min(obj->p0[0], obj->p1[0]);
            x1 = max(obj->p0[0], obj->p1[0]);
            y0 = min(obj->p0[1], obj->p1[1]);
            y1 = max(obj->p0[1], obj->p1[1]);
            for (y = y0; y <= y1; y++) {
                for (x = x0; x <= x1; x++) {
                    if (!tb_stamp_column(hm, xmin, ymin, sx, sy, x, y,
                                         (uint8_t)index, zmin, zmax_excl_io,
                                         stamps, n_stamps, &cap))
                        return false;
                }
            }
        } else if (obj->type == CUSTOM_OBJ_POINT_2D ||
                   obj->type == CUSTOM_OBJ_POINT_3D) {
            if (!tb_stamp_column(hm, xmin, ymin, sx, sy,
                                 obj->p0[0], obj->p0[1], (uint8_t)index,
                                 zmin, zmax_excl_io, stamps, n_stamps, &cap))
                return false;
        }
    }
    return true;
}

static void tb_init_palette(uint8_t (*palette)[4])
{
    int i;

    memset(palette, 0, 256 * sizeof(*palette));
    for (i = 0; i < 8; i++) {
        palette[TB_PAL_META_FIRST + i][0] = TB_META_RGB[i][0];
        palette[TB_PAL_META_FIRST + i][1] = TB_META_RGB[i][1];
        palette[TB_PAL_META_FIRST + i][2] = TB_META_RGB[i][2];
        palette[TB_PAL_META_FIRST + i][3] = 255;
    }
    /* 9-16 remain empty (reserved). */
}

static int vox_trenchblocks_export(const file_format_t *format,
                                   const image_t *image, const char *path)
{
    FILE *file = NULL;
    const volume_t *src_volume;
    volume_t *volume = NULL;
    uint8_t (*palette)[4] = NULL;
    int *heightmap = NULL;
    tb_stamp_t *stamps = NULL;
    tb_tile_t *tiles = NULL;
    int *tile_child_ids = NULL;
    uint8_t *voxels = NULL;
    int n_stamps = 0;
    int start[3], dims[3];
    int xmin, ymin, zmin, xmax, ymax, zmax;
    int sx, sy, sz, nx, ny, tx, ty, ti, i, pos[3];
    int nb_vox = 0, nb_tiles = 0, model_i, children_size;
    int stamp_i, color_index;
    uint8_t v[4], stamp_rgb[4];
    volume_iterator_t iter;
    char trans[64];
    float box[4][4];

    (void)format;

    if (box_is_null(image->box)) {
        gui_alert("vox (Trenchblocks)", "Image has no map box set.");
        return -1;
    }

    mat4_copy(image->box, box);
    box_get_start_pos(box, start);
    box_get_dimensions(box, dims);
    xmin = start[0];
    ymin = start[1];
    zmin = start[2];
    sx = dims[0];
    sy = dims[1];
    sz = dims[2];
    xmax = xmin + sx;
    ymax = ymin + sy;
    zmax = zmin + sz;

    if (sx <= 0 || sy <= 0 || sz <= 0) {
        gui_alert("vox (Trenchblocks)", "Invalid map box dimensions.");
        return -1;
    }
    if (sz > VOX_TILE) {
        gui_alert("vox (Trenchblocks)",
                  "Map is taller than 256 blocks in Z. "
                  "Reduce the map height before exporting.");
        return -1;
    }

    nx = (sx + VOX_TILE - 1) / VOX_TILE;
    ny = (sy + VOX_TILE - 1) / VOX_TILE;
    if (nx > 2 || ny > 2 || nx * ny > 4) {
        gui_alert("vox (Trenchblocks)",
                  "Map XY needs more than four 256x256 tiles (T1-T4). "
                  "Reduce the map size or wait for T5-T8 support.");
        return -1;
    }

    src_volume = goxel_get_layers_volume(image);
    volume = volume_copy(src_volume);
    if (!volume) {
        gui_alert("vox (Trenchblocks)", "Out of memory.");
        return -1;
    }

    heightmap = tb_build_heightmap(volume, xmin, ymin, sx, sy, zmin, zmax);
    if (!heightmap) {
        gui_alert("vox (Trenchblocks)", "Out of memory.");
        goto error;
    }

    if (!tb_collect_stamps(image, heightmap, xmin, ymin, sx, sy, zmin, &zmax,
                           &stamps, &n_stamps))
        goto error;
    sz = zmax - zmin;

    /* Apply stamps into the working volume (reserved RGB). */
    for (i = 0; i < n_stamps; i++) {
        stamp_rgb[0] = TB_META_RGB[stamps[i].index - 1][0];
        stamp_rgb[1] = TB_META_RGB[stamps[i].index - 1][1];
        stamp_rgb[2] = TB_META_RGB[stamps[i].index - 1][2];
        stamp_rgb[3] = 255;
        pos[0] = stamps[i].x;
        pos[1] = stamps[i].y;
        pos[2] = stamps[i].z;
        volume_set_at(volume, NULL, pos, stamp_rgb);
    }

    palette = calloc(256, sizeof(*palette));
    if (!palette) {
        gui_alert("vox (Trenchblocks)", "Out of memory.");
        goto error;
    }
    tb_init_palette(palette);

    /* Reserve opaque recent-map colours, then quantize into whatever is left. */
    {
        int n_recent = tb_add_recent_colors(palette, image);
        int quant_first = TB_PAL_MAP_FIRST + n_recent;
        int quant_count = 256 - quant_first;

        /* Count source voxels (for quantization) and export voxels. */
        iter = volume_get_iterator(src_volume, VOLUME_ITER_VOXELS);
        while (volume_iter(&iter, pos)) {
            if (pos[0] < xmin || pos[0] >= xmax) continue;
            if (pos[1] < ymin || pos[1] >= ymax) continue;
            if (pos[2] < zmin || pos[2] >= zmin + dims[2]) continue;
            volume_get_at(src_volume, &iter, pos, v);
            if (v[3] < 127) continue;
            nb_vox++;
        }
        if (nb_vox > 0 && quant_count > 0) {
            /* Quantize from the pre-stamp source so metadata / recent RGBs do
             * not consume the remaining map palette slots incorrectly. */
            quantization_gen_palette(src_volume, quant_count,
                                     (void *)(palette + quant_first));
        }
    }

    nb_vox = 0;
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        if (pos[0] < xmin || pos[0] >= xmax) continue;
        if (pos[1] < ymin || pos[1] >= ymax) continue;
        if (pos[2] < zmin || pos[2] >= zmax) continue;
        volume_get_at(volume, &iter, pos, v);
        if (v[3] < 127) continue;
        nb_vox++;
    }
    if (nb_vox == 0) {
        gui_alert("vox (Trenchblocks)", "Nothing to export.");
        goto error;
    }

    tiles = calloc((size_t)nx * (size_t)ny, sizeof(*tiles));
    if (!tiles) {
        gui_alert("vox (Trenchblocks)", "Out of memory.");
        goto error;
    }

    for (ty = 0; ty < ny; ty++)
    for (tx = 0; tx < nx; tx++) {
        ti = tx + ty * nx;
        tiles[ti].ox = xmin + tx * VOX_TILE;
        tiles[ti].oy = ymin + ty * VOX_TILE;
        tiles[ti].oz = zmin;
        tiles[ti].sx = min(VOX_TILE, xmax - tiles[ti].ox);
        tiles[ti].sy = min(VOX_TILE, ymax - tiles[ti].oy);
        tiles[ti].sz = sz;
        tiles[ti].name = tb_tile_name(tx, ty, nx, ny);
    }

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        if (pos[0] < xmin || pos[0] >= xmax) continue;
        if (pos[1] < ymin || pos[1] >= ymax) continue;
        if (pos[2] < zmin || pos[2] >= zmax) continue;
        volume_get_at(volume, &iter, pos, v);
        if (v[3] < 127) continue;
        tx = (pos[0] - xmin) / VOX_TILE;
        ty = (pos[1] - ymin) / VOX_TILE;
        tiles[tx + ty * nx].nb_vox++;
    }

    nb_tiles = 0;
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        tiles[i].voxels = calloc(tiles[i].nb_vox, 4);
        if (!tiles[i].voxels) {
            gui_alert("vox (Trenchblocks)", "Out of memory.");
            goto error;
        }
        nb_tiles++;
    }

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        if (pos[0] < xmin || pos[0] >= xmax) continue;
        if (pos[1] < ymin || pos[1] >= ymax) continue;
        if (pos[2] < zmin || pos[2] >= zmax) continue;
        volume_get_at(volume, &iter, pos, v);
        if (v[3] < 127) continue;
        tx = (pos[0] - xmin) / VOX_TILE;
        ty = (pos[1] - ymin) / VOX_TILE;
        ti = tx + ty * nx;
        i = tiles[ti].filled++;
        voxels = tiles[ti].voxels;
        voxels[i * 4 + 0] = (uint8_t)(pos[0] - tiles[ti].ox);
        voxels[i * 4 + 1] = (uint8_t)(pos[1] - tiles[ti].oy);
        voxels[i * 4 + 2] = (uint8_t)(pos[2] - tiles[ti].oz);
        stamp_i = tb_find_stamp_index(stamps, n_stamps, pos[0], pos[1], pos[2]);
        if (stamp_i >= 0)
            color_index = stamp_i;
        else {
            v[3] = 255;
            color_index = tb_get_map_color_index(v, palette);
        }
        voxels[i * 4 + 3] = (uint8_t)color_index;
    }

    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        qsort(tiles[i].voxels, tiles[i].nb_vox, 4, tb_voxel_cmp);
    }

    children_size = 0;
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        children_size += tb_size_xyzi_chunk_bytes(tiles[i].nb_vox);
    }
    children_size += 12 + tb_ntrn_content_size(NULL, NULL);
    children_size += 12 + tb_ngrp_content_size(nb_tiles);
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        snprintf(trans, sizeof(trans), "%d %d %d",
                 tiles[i].ox + tiles[i].sx / 2,
                 tiles[i].oy + tiles[i].sy / 2,
                 tiles[i].oz + tiles[i].sz / 2);
        children_size += 12 + tb_ntrn_content_size(tiles[i].name, trans);
        children_size += 12 + tb_nshp_content_size();
    }
    children_size += 12 + 4 * 256; /* RGBA always written */

    file = fopen(path, "wb");
    if (!file) {
        gui_alert("vox (Trenchblocks)", "Could not open the output file.");
        goto error;
    }

    fprintf(file, "VOX ");
    WRITE(uint32_t, 150, file);
    fprintf(file, "MAIN");
    WRITE(uint32_t, 0, file);
    WRITE(uint32_t, children_size, file);

    model_i = 0;
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        tb_write_size_xyzi(file, tiles[i].sx, tiles[i].sy, tiles[i].sz,
                           tiles[i].voxels, tiles[i].nb_vox);
        model_i++;
    }

    tile_child_ids = calloc(nb_tiles, sizeof(*tile_child_ids));
    if (!tile_child_ids) {
        gui_alert("vox (Trenchblocks)", "Out of memory.");
        goto error;
    }
    model_i = 0;
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        tile_child_ids[model_i] = 2 + 2 * model_i;
        model_i++;
    }

    fprintf(file, "nTRN");
    tb_write_ntrn(file, 0, 1, NULL, NULL);

    fprintf(file, "nGRP");
    tb_write_ngrp(file, 1, tile_child_ids, nb_tiles);

    model_i = 0;
    for (i = 0; i < nx * ny; i++) {
        if (!tiles[i].nb_vox) continue;
        snprintf(trans, sizeof(trans), "%d %d %d",
                 tiles[i].ox + tiles[i].sx / 2,
                 tiles[i].oy + tiles[i].sy / 2,
                 tiles[i].oz + tiles[i].sz / 2);
        fprintf(file, "nTRN");
        tb_write_ntrn(file, 2 + 2 * model_i, 3 + 2 * model_i,
                      tiles[i].name, trans);
        fprintf(file, "nSHP");
        tb_write_nshp(file, 3 + 2 * model_i, model_i);
        model_i++;
    }

    tb_write_rgba(file, palette);

    fclose(file);
    file = NULL;

    free(tile_child_ids);
    for (i = 0; i < nx * ny; i++)
        free(tiles[i].voxels);
    free(tiles);
    free(stamps);
    free(heightmap);
    free(palette);
    volume_delete(volume);
    return 0;

error:
    if (file) fclose(file);
    free(tile_child_ids);
    if (tiles) {
        for (i = 0; i < nx * ny; i++)
            free(tiles[i].voxels);
        free(tiles);
    }
    free(stamps);
    free(heightmap);
    free(palette);
    if (volume) volume_delete(volume);
    return -1;
}

FILE_FORMAT_REGISTER(vox_trenchblocks,
    .name = "vox (Trenchblocks)",
    .exts = {"*.vox"},
    .exts_desc = "vox",
    .export_func = vox_trenchblocks_export,
)

/* Exposed so goxel_init can ensure the constructor-registered entry is present
 * (and force the TU to stay linked). */
void goxel_ensure_vox_trenchblocks_format(void)
{
    if (file_format_by_name("vox (Trenchblocks)"))
        return;
    /* Constructor did not run; register now. Re-use the static instance. */
    file_format_register(&GOX_format_vox_trenchblocks);
}
