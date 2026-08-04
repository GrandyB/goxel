/* Biomes terrain generator - Triplefox random.txt recipe via mapmaker.
 *
 * Mapmaker originally by James Hofmann (2012), GPLv3.
 * random.txt recipe by Triplefox.
 */

#include "biomes.h"
#include "mapmaker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void stop_rgb(biomes_grad_stop_t *s, int pos, int r, int g, int b)
{
    s->pos = pos;
    s->rgb[0] = (uint8_t)r;
    s->rgb[1] = (uint8_t)g;
    s->rgb[2] = (uint8_t)b;
    s->rgb[3] = 255;
    s->hsb[0] = s->hsb[1] = s->hsb[2] = 0;
    s->is_hsb = false;
}

static void stop_hsb(biomes_grad_stop_t *s, int pos, float h, float sat, float bri)
{
    int r, g, b;
    s->pos = pos;
    s->hsb[0] = h;
    s->hsb[1] = sat;
    s->hsb[2] = bri;
    s->is_hsb = true;
    mm_hsb_to_rgb(h / 360.f, sat / 100.f, bri / 100.f, &r, &g, &b);
    s->rgb[0] = (uint8_t)r;
    s->rgb[1] = (uint8_t)g;
    s->rgb[2] = (uint8_t)b;
    s->rgb[3] = 255;
}

static void add_fixed_seed(biomes_biome_settings_t *b, int x, int y)
{
    int i;
    if (b->n_fixed_seeds >= BIOMES_MAX_FIXED_SEEDS)
        return;
    i = b->n_fixed_seeds++;
    b->fixed_x[i] = (int8_t)x;
    b->fixed_y[i] = (int8_t)y;
}

void biomes_settings_set_defaults(biomes_settings_t *s)
{
    biomes_biome_settings_t *b;
    int i;
    memset(s, 0, sizeof(*s));

    s->displace_jitter = 0.3f;
    s->displace_span_scale = 0.68f;
    s->displace_skip = 4;
    s->biome_jitter = true;
    s->height_smooth_passes = 2;
    s->height_despeckle_passes = 2;

    s->n_biomes = 5;

    /* Grass */
    b = &s->biomes[0];
    snprintf(b->name, sizeof(b->name), "Grass");
    b->height = 0.97f;
    b->variation = -0.1f;
    b->noise = 0.02f;
    b->n_stops = 4;
    stop_rgb(&b->stops[0], 0, 2, 100, 86);
    stop_hsb(&b->stops[1], 1, 102, 73, 58);
    stop_hsb(&b->stops[2], 32, 106, 78, 71);
    stop_hsb(&b->stops[3], 64, 106, 48, 86);
    add_fixed_seed(b, 15, 15);
    b->random_seeds = 2;
    b->region_x = 0;
    b->region_y = 8;
    b->region_w = 32;
    b->region_h = 16;
    b->place_trees = false;
    b->share_gradient_from = -1;
    b->noise_intensity = 15;
    b->noise_saturation = 8;
    b->noise_coverage = 100;

    /* Snow */
    b = &s->biomes[1];
    snprintf(b->name, sizeof(b->name), "Snow");
    b->height = 0.5f;
    b->variation = -0.4f;
    b->noise = 0.11f;
    b->n_stops = 6;
    stop_rgb(&b->stops[0], 0, 101, 193, 214);
    stop_hsb(&b->stops[1], 1, 221, 55, 48);
    stop_hsb(&b->stops[2], 16, 184, 30, 84);
    stop_hsb(&b->stops[3], 48, 160, 20, 87);
    stop_hsb(&b->stops[4], 56, 140, 18, 98);
    stop_hsb(&b->stops[5], 64, 142, 13, 100);
    add_fixed_seed(b, 15, 7);
    b->random_seeds = 0;
    b->region_x = 0;
    b->region_y = 0;
    b->region_w = 32;
    b->region_h = 32;
    b->place_trees = false;
    b->share_gradient_from = -1;
    b->noise_intensity = 8;
    b->noise_saturation = 4;
    b->noise_coverage = 100;

    /* Hill */
    b = &s->biomes[2];
    snprintf(b->name, sizeof(b->name), "Hill");
    b->height = 0.9f;
    b->variation = -0.3f;
    b->noise = 0.07f;
    b->n_stops = 3;
    stop_rgb(&b->stops[0], 0, 2, 100, 86);
    stop_hsb(&b->stops[1], 1, 102, 73, 58);
    stop_hsb(&b->stops[2], 64, 17, 36, 87);
    add_fixed_seed(b, 15, 22);
    b->random_seeds = 1;
    b->region_x = 0;
    b->region_y = 8;
    b->region_w = 32;
    b->region_h = 16;
    b->place_trees = true;
    b->share_gradient_from = -1;
    b->noise_intensity = 12;
    b->noise_saturation = 6;
    b->noise_coverage = 100;

    /* Water */
    b = &s->biomes[3];
    snprintf(b->name, sizeof(b->name), "Water");
    b->height = 1.2f;
    b->variation = -0.16f;
    b->noise = 0.04f;
    b->n_stops = 4;
    stop_rgb(&b->stops[0], 0, 2, 100, 86);
    stop_hsb(&b->stops[1], 1, 64, 26, 70);
    stop_hsb(&b->stops[2], 16, 119, 65, 40);
    stop_hsb(&b->stops[3], 64, 125, 153, 61);
    add_fixed_seed(b, 0, 24);
    add_fixed_seed(b, 31, 24);
    add_fixed_seed(b, 0, 8);
    add_fixed_seed(b, 31, 8);
    add_fixed_seed(b, 0, 31);
    add_fixed_seed(b, 8, 31);
    add_fixed_seed(b, 16, 31);
    add_fixed_seed(b, 24, 31);
    b->random_seeds = 1;
    b->region_x = 8;
    b->region_y = 8;
    b->region_w = 16;
    b->region_h = 16;
    b->place_trees = false;
    b->share_gradient_from = -1;
    b->noise_intensity = 4;
    b->noise_saturation = 3;
    b->noise_coverage = 80;

    /* Tundra - shares snow gradient by default */
    b = &s->biomes[4];
    snprintf(b->name, sizeof(b->name), "Tundra");
    b->height = 1.14f;
    b->variation = -0.19f;
    b->noise = 0.1f;
    b->n_stops = 0;
    add_fixed_seed(b, 0, 0);
    add_fixed_seed(b, 8, 0);
    add_fixed_seed(b, 16, 0);
    add_fixed_seed(b, 24, 0);
    b->random_seeds = 0;
    b->region_x = 0;
    b->region_y = 0;
    b->region_w = 32;
    b->region_h = 32;
    b->place_trees = false;
    b->share_gradient_from = 1; /* Snow */
    b->noise_intensity = 8;
    b->noise_saturation = 4;
    b->noise_coverage = 100;

    s->river_enabled = true;
    s->river_x_half_width = 64;
    s->river_y_increment = 8;
    s->river_x_increment = 12;
    s->river_add_radius = 8;
    s->river_add_depth = 0.005f;
    s->river_set_radius = 2;
    s->river_set_height = 2.0f;

    s->dithering = 10.f;
    s->smooth_colors = true;

    s->trees_min_per_tile = 1;
    s->trees_max_per_tile = 17;
    s->trunk_h_min = 4;
    s->trunk_h_max = 5;
    s->foliage_count = 6;
    {
        const uint8_t greens[6][3] = {
            {98, 193, 69}, {96, 229, 55}, {94, 242, 48},
            {93, 209, 57}, {92, 219, 57}, {88, 210, 66},
        };
        for (i = 0; i < 6; i++) {
            s->foliage_colors[i][0] = greens[i][0];
            s->foliage_colors[i][1] = greens[i][1];
            s->foliage_colors[i][2] = greens[i][2];
            s->foliage_colors[i][3] = 255;
        }
    }
    s->trunk_color[0] = 189;
    s->trunk_color[1] = 124;
    s->trunk_color[2] = 67;
    s->trunk_color[3] = 255;

    s->seed = 0;
    s->resize_image = true;
    s->replace_current_layer = false;
}

/* Rebuild gradient like random.txt: step0 RGB + HSB (or RGB) segments
 * between consecutive stops starting at index 1. */
static void build_gradient_from_stops(mm_gradient_t *g,
                                      const biomes_grad_stop_t *stops,
                                      int n_stops)
{
    int i;
    mm_gradient_init(g);
    if (n_stops <= 0)
        return;
    mm_gradient_set_step_rgb(g, stops[0].pos,
                             stops[0].rgb[0], stops[0].rgb[1], stops[0].rgb[2]);
    for (i = 1; i + 1 < n_stops; i++) {
        const biomes_grad_stop_t *a = &stops[i];
        const biomes_grad_stop_t *b = &stops[i + 1];
        if (a->is_hsb && b->is_hsb) {
            mm_gradient_hsb(g, a->pos, a->hsb[0], a->hsb[1], a->hsb[2],
                            b->pos, b->hsb[0], b->hsb[1], b->hsb[2]);
        } else {
            uint8_t sc[3] = {a->rgb[0], a->rgb[1], a->rgb[2]};
            uint8_t ec[3] = {b->rgb[0], b->rgb[1], b->rgb[2]};
            mm_gradient_rgb(g, a->pos, sc, b->pos, ec);
        }
    }
}

/* Follow share_gradient_from; break cycles by returning the start index. */
static int resolve_gradient_source(const biomes_settings_t *settings, int idx)
{
    int cur = idx;
    int guard = 0;
    int n = settings->n_biomes;
    while (guard++ < n) {
        int src = settings->biomes[cur].share_gradient_from;
        if (src < 0 || src >= n)
            return cur;
        if (src == idx)
            return idx;
        cur = src;
    }
    return idx;
}

static int height_cap(void)
{
    int dimensions[3];
    if (!goxel.image || box_is_null(goxel.image->box))
        return BIOMES_MIN_HEIGHT;
    box_get_dimensions(goxel.image->box, dimensions);
    return max(BIOMES_MIN_HEIGHT, dimensions[2]);
}

/* AoS z (0 top .. 63 bottom) → Goxel relative z from box bottom. */
static int aos_to_goxel_z(int aos_z)
{
    return 63 - aos_z;
}

static void voxel_rgba_from_packed(int packed, uint8_t out[4])
{
    out[0] = (uint8_t)mm_get_r(packed);
    out[1] = (uint8_t)mm_get_g(packed);
    out[2] = (uint8_t)mm_get_b(packed);
    out[3] = 255;
}

static void set_column_aos(volume_t *volume, const int start_pos[3],
                           int x, int y, int z_start, int z_end, int z_color_end,
                           uint8_t color[4], uint8_t fill[4], int cap)
{
    int aos_z;
    int pos[3];
    if (z_end < z_start)
        return;
    for (aos_z = z_start; aos_z <= z_end; aos_z++) {
        int gz = aos_to_goxel_z(aos_z);
        if (gz < 0 || gz >= cap)
            continue;
        pos[0] = x + start_pos[0];
        pos[1] = y + start_pos[1];
        pos[2] = gz + start_pos[2];
        volume_set_at(volume, NULL, pos,
                      (aos_z <= z_color_end) ? color : fill);
    }
}

static void write_heightmap_to_volume(volume_t *volume, mm_heightmap_t *hm,
                                      biomes_settings_t *settings, int cap)
{
    int start_pos[3];
    int idx;
    uint8_t underground[4] = {90, 80, 70, 255};

    if (settings->replace_current_layer)
        volume_clear(volume);

    box_get_start_pos(goxel.image->box, start_pos);

    for (idx = 0; idx < hm->width * hm->height; idx++) {
        int x = idx % hm->width;
        int y = idx / hm->width;
        int h = (int)(hm->hmap[idx] * 63);
        int z_color_end = h + 3;
        uint8_t color[4];
        if (z_color_end > 63)
            z_color_end = 63;
        voxel_rgba_from_packed(hm->cmap[idx], color);
        /* Solid from aos h .. 63 (surface band colored). */
        set_column_aos(volume, start_pos, x, y, h, 63, z_color_end,
                       color, underground, cap);
    }
}

static void place_tree(volume_t *volume, const int start_pos[3],
                       mm_heightmap_t *hm, mm_rng_t *rng,
                       biomes_settings_t *settings,
                       int x, int y, int cap)
{
    int trunk_h, z, fi;
    uint8_t green[4], brown[4];
    /* Space trees into an "x" pattern */
    x = ((x >> 1) << 1) + (y % 2);
    if (x <= 1 || y <= 1 || x >= 510 || y >= 510)
        return;

    trunk_h = mm_rng_int(rng, settings->trunk_h_min, settings->trunk_h_max);
    z = (int)(mm_hm_get(hm, x, y) * 63 - 3 - trunk_h);
    if (z < 0 || z + 3 + trunk_h >= 62)
        return;

    fi = mm_rng_int(rng, 0, settings->foliage_count - 1);
    if (fi < 0)
        fi = 0;
    memcpy(green, settings->foliage_colors[fi], 4);
    memcpy(brown, settings->trunk_color, 4);

    {
        struct {
            int x, y, z0, z1, zc;
            uint8_t *col;
        } parts[] = {
            {x, y, z, z + 3, z + 3, green},
            {x + 1, y, z, z + 3, z + 3, green},
            {x - 1, y, z, z + 3, z + 3, green},
            {x, y + 1, z, z + 3, z + 3, green},
            {x, y - 1, z, z + 3, z + 3, green},
            {x, y, z + 3, z + 3 + trunk_h, z + 3 + trunk_h - 1, brown},
            {x + 1, y + 1, z + 1, z + 2, z + 2, green},
            {x - 1, y + 1, z + 1, z + 2, z + 2, green},
            {x + 1, y - 1, z + 1, z + 2, z + 2, green},
            {x - 1, y - 1, z + 1, z + 2, z + 2, green},
            {x + 2, y, z + 1, z + 2, z + 2, green},
            {x - 2, y, z + 1, z + 2, z + 2, green},
            {x, y - 2, z + 1, z + 2, z + 2, green},
            {x, y + 2, z + 1, z + 2, z + 2, green},
        };
        int i;
        for (i = 0; i < (int)(sizeof(parts) / sizeof(parts[0])); i++) {
            set_column_aos(volume, start_pos, parts[i].x, parts[i].y,
                           parts[i].z0, parts[i].z1, parts[i].zc,
                           parts[i].col, parts[i].col, cap);
        }
    }
}

static void apply_surface_color_noise(mm_heightmap_t *hm, const int *biome_ids,
                                      const biomes_settings_t *settings, int n_biomes)
{
    int idx;
    int n = hm->width * hm->height;

    for (idx = 0; idx < n; idx++) {
        int bi = biome_ids[idx];
        const biomes_biome_settings_t *bs;
        int intensity, saturation, coverage;
        float cov, nv;
        int rgb[3];
        int mid;
        int x, y;

        if (bi < 0 || bi >= n_biomes)
            bi = 0;
        bs = &settings->biomes[bi];
        intensity = clamp(bs->noise_intensity, 0, 100);
        saturation = clamp(bs->noise_saturation, 0, 100);
        coverage = clamp(bs->noise_coverage, 0, 100);
        if (intensity <= 0 || coverage <= 0)
            continue;
        cov = (float)coverage / 100.f;
        x = idx % hm->width;
        y = idx / hm->width;
        nv = uniform_noise((float)x, (float)y, 0.f);
        if (nv >= cov)
            continue;
        mid = hm->cmap[idx];
        rgb[0] = mm_get_r(mid);
        rgb[1] = mm_get_g(mid);
        rgb[2] = mm_get_b(mid);
        blend_with_noise_alpha(rgb, nv, (float)intensity, (float)saturation,
                               rgb);
        hm->cmap[idx] = mm_make_color(rgb[0], rgb[1], rgb[2]);
    }
}

void generate_biomes_terrain(volume_t *volume, biomes_settings_t *settings)
{
    mm_rng_t rng;
    mm_gradient_t grads[BIOMES_MAX];
    mm_biome_t biomes[BIOMES_MAX];
    mm_biomemap_t bmap;
    mm_heightmap_t hmap;
    mm_biome_point_t points[128];
    int n_points = 0;
    int i, j, cap, n;
    int grad_tables[BIOMES_MAX][MM_GRAD_STEPS * 3];
    const int *grad_ptrs[BIOMES_MAX];
    int start_pos[3];

    if (!volume || !settings || !goxel.image)
        return;

    n = settings->n_biomes;
    if (n < 1)
        n = 1;
    if (n > BIOMES_MAX)
        n = BIOMES_MAX;

    mm_rng_seed(&rng, (uint32_t)settings->seed);
    cap = height_cap();

    for (i = 0; i < n; i++) {
        const biomes_biome_settings_t *bs = &settings->biomes[i];
        int src = resolve_gradient_source(settings, i);
        const biomes_biome_settings_t *gs = &settings->biomes[src];
        const biomes_grad_stop_t *stops = gs->stops;
        int n_stops = gs->n_stops;

        build_gradient_from_stops(&grads[i], stops, n_stops);
        biomes[i].gradient = &grads[i];
        biomes[i].height = bs->height;
        biomes[i].variation = bs->variation;
        biomes[i].noise = bs->noise;
        biomes[i].id = i;
        mm_gradient_array(&grads[i], grad_tables[i]);
        grad_ptrs[i] = grad_tables[i];
    }

    if (!mm_biomemap_init(&bmap, biomes, n, BIOMES_MAP_TILES, BIOMES_MAP_TILES))
        return;

    for (i = 0; i < n; i++) {
        const biomes_biome_settings_t *bs = &settings->biomes[i];
        for (j = 0; j < bs->n_fixed_seeds && n_points < 128; j++) {
            points[n_points].x = (int)bs->fixed_x[j];
            points[n_points].y = (int)bs->fixed_y[j];
            points[n_points].biome = &biomes[i];
            n_points++;
        }
        if (bs->random_seeds > 0 && n_points < 128) {
            n_points += mm_bm_random_points(
                &bmap, &rng, bs->random_seeds, &biomes[i],
                bs->region_x, bs->region_y, bs->region_w, bs->region_h,
                &points[n_points], 128 - n_points);
        }
    }

    if (n_points < 1) {
        /* Ensure flood fill has at least one seed. */
        points[0].x = BIOMES_MAP_TILES / 2;
        points[0].y = BIOMES_MAP_TILES / 2;
        points[0].biome = &biomes[0];
        n_points = 1;
    }

    mm_bm_point_flood(&bmap, points, n_points);
    if (settings->biome_jitter)
        mm_bm_jitter(&bmap, &rng);

    if (!mm_bm_create_heightmap(&bmap, &rng, &hmap)) {
        mm_biomemap_free(&bmap);
        return;
    }

    mm_hm_midpoint_displace(&hmap, &rng, settings->displace_jitter,
                            settings->displace_span_scale, settings->displace_skip);
    mm_hm_jitter_colors(&hmap, &rng, settings->dithering);

    if (settings->river_enabled) {
        int xmin = 256 - settings->river_x_half_width;
        int xmax = 256 + settings->river_x_half_width;
        int x = mm_rng_int(&rng, xmin, xmax);
        int y;
        int yinc = settings->river_y_increment;
        int xinc = settings->river_x_increment;
        if (yinc < 1)
            yinc = 1;
        for (y = yinc; y < 513; y += yinc) {
            int nx = mm_rng_int(&rng, x - xinc, x + xinc);
            if (nx < xmin)
                nx = xmin;
            if (nx > xmax)
                nx = xmax;
            mm_hm_line_add(&hmap, x, y - yinc, nx, y,
                           settings->river_add_radius, settings->river_add_depth);
            mm_hm_line_set(&hmap, x, y - yinc, nx, y,
                           settings->river_set_radius, settings->river_set_height);
            x = nx;
        }
    }

    {
        int p;
        int smooth_n = settings->height_smooth_passes;
        if (smooth_n < 1)
            smooth_n = 1;
        for (p = 0; p < smooth_n; p++)
            mm_hm_smoothing(&hmap);
    }
    mm_hm_truncate(&hmap);
    mm_hm_despeckle_heights(&hmap, settings->height_despeckle_passes);
    mm_hm_truncate(&hmap);
    {
        int map_n = hmap.width * hmap.height;
        int *biome_ids = malloc(sizeof(int) * (size_t)map_n);
        if (biome_ids)
            memcpy(biome_ids, hmap.cmap, sizeof(int) * (size_t)map_n);
        mm_hm_rewrite_gradient_fill(&hmap, &rng, grad_ptrs, n);
        if (biome_ids) {
            apply_surface_color_noise(&hmap, biome_ids, settings, n);
            free(biome_ids);
        }
    }
    if (settings->smooth_colors)
        mm_hm_smooth_colors(&hmap);

    write_heightmap_to_volume(volume, &hmap, settings, cap);

    {
        box_get_start_pos(goxel.image->box, start_pos);
        for (i = 0; i < bmap.width * bmap.height; i++) {
            int tx = i % bmap.width;
            int ty = i / bmap.width;
            int bi = (int)(bmap.tmap[i] - biomes);
            if (bi < 0 || bi >= n || !settings->biomes[bi].place_trees)
                continue;
            {
                int left, top, right, bottom, ct, t;
                int amin = settings->trees_min_per_tile;
                int amax = settings->trees_max_per_tile;
                if (amax < amin)
                    amax = amin;
                mm_bm_rect_of_point(&bmap, tx, ty, &left, &top, &right, &bottom);
                ct = mm_rng_int(&rng, amin, amax);
                for (t = 0; t < ct; t++) {
                    int px = mm_rng_int(&rng, left, right);
                    int py = mm_rng_int(&rng, top, bottom);
                    place_tree(volume, start_pos, &hmap, &rng, settings,
                               px, py, cap);
                }
            }
        }
    }

    mm_heightmap_free(&hmap);
    mm_biomemap_free(&bmap);
}
