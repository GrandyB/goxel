/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
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

#include <float.h>
#include <limits.h>
#include <math.h>

/*
 * Plan - Buildings
 *
 * Paint 1-block-high filled footprints on the active layer.  Each of four
 * colours maps to a floor count (1–4).  Generate replaces those footprints
 * with inward-walled buildings: base slab, clear-height storeys with
 * perimeter walls, intermediate slabs, and a shaped roof.
 */

#define BUILDING_FLOOR_COUNTS 4
#define BUILDING_MAX_ROOF_PAIRS 32
#define BUILDING_NOISE_INTENSITY 5
#define BUILDING_NOISE_SATURATION 5
/* PCA OBB interior must be at least this occupied (by cell-centre samples). */
#define BUILDING_RECT_FILL_MIN 0.90f
/* Empty OBB samples may be at most this fraction of ncells (plus 1). */
#define BUILDING_RECT_HOLE_FRAC 0.05f

typedef struct {
    filter_t filter;
    int floor_height;      /* clear height between slabs */
    int floor_thickness;   /* slab thickness */
    int wall_thickness;    /* inward wall bands */
    int max_roof_height;   /* roof stops and stays flat past this */
    uint8_t floor_colors[BUILDING_FLOOR_COUNTS][4];
    uint8_t roof_colors[BUILDING_MAX_ROOF_PAIRS][2][4];
    int roof_pair_count;
} filter_buildings_t;

typedef struct {
    int x, y;
} building_xy_t;

typedef struct {
    int floors;
    int base_z;
    building_xy_t *cells;
    int ncells;
    int cells_cap;
    int xmin, ymin, xmax, ymax;
    uint8_t *occ; /* (xmax-xmin+1) * (ymax-ymin+1) */
} building_group_t;

static const uint8_t k_default_colors[BUILDING_FLOOR_COUNTS][4] = {
    {220, 70, 70, 255},
    {70, 180, 90, 255},
    {70, 110, 220, 255},
    {220, 190, 60, 255},
};

static const uint8_t k_default_roof_colors[][2][4] = {
    {{0x28, 0x33, 0x3f, 255}, {0x31, 0x3b, 0x47, 255}},
    {{0x20, 0x24, 0x29, 255}, {0x2f, 0x33, 0x37, 255}},
    {{0x3f, 0x3f, 0x3f, 255}, {0x33, 0x32, 0x32, 255}},
    {{0x39, 0x2f, 0x2a, 255}, {0x2d, 0x24, 0x1e, 255}},
};

static const uint8_t k_base_grey[4] = {128, 128, 128, 255};
static const uint8_t k_empty[4] = {0, 0, 0, 0};

static void reset_defaults(filter_buildings_t *filter)
{
    int i;

    filter->floor_height = 4;
    filter->floor_thickness = 1;
    filter->wall_thickness = 1;
    filter->max_roof_height = 8;
    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++)
        memcpy(filter->floor_colors[i], k_default_colors[i], 4);
    filter->roof_pair_count = (int)ARRAY_SIZE(k_default_roof_colors);
    memcpy(filter->roof_colors, k_default_roof_colors,
           sizeof(k_default_roof_colors));
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int floor_count_for_color(const filter_buildings_t *filter,
                                 const uint8_t color[4])
{
    int i;

    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++) {
        if (memcmp(filter->floor_colors[i], color, 4) == 0)
            return i + 1;
    }
    return 0;
}

static void make_building_color(int x, int y, int z, uint8_t out[4])
{
    int rgb[3];

    memcpy(out, k_base_grey, 4);
    rgb[0] = out[0];
    rgb[1] = out[1];
    rgb[2] = out[2];
    blend_with_noise_alpha(rgb,
                           uniform_noise((float)x, (float)y, (float)z),
                           (float)BUILDING_NOISE_INTENSITY,
                           (float)BUILDING_NOISE_SATURATION,
                           rgb);
    out[0] = (uint8_t)clampi(rgb[0], 0, 255);
    out[1] = (uint8_t)clampi(rgb[1], 0, 255);
    out[2] = (uint8_t)clampi(rgb[2], 0, 255);
    out[3] = 255;
}

static int occ_index(const building_group_t *g, int x, int y)
{
    return (y - g->ymin) * (g->xmax - g->xmin + 1) + (x - g->xmin);
}

static bool occ_get(const building_group_t *g, int x, int y)
{
    if (x < g->xmin || x > g->xmax || y < g->ymin || y > g->ymax)
        return false;
    return g->occ[occ_index(g, x, y)] != 0;
}

/* True if (x,y) is within `wall_t` orthogonal steps of a non-footprint cell. */
static bool is_wall_cell(const building_group_t *g, int x, int y, int wall_t)
{
    int d, dx, dy, ax, ay;

    for (d = 1; d <= wall_t; d++) {
        for (dx = -d; dx <= d; dx++) {
            ax = abs(dx);
            dy = d - ax;
            ay = y + dy;
            if (!occ_get(g, x + dx, ay))
                return true;
            if (dy != 0 && !occ_get(g, x + dx, y - dy))
                return true;
        }
    }
    return false;
}

static void paint_voxel(volume_t *vol, int x, int y, int z)
{
    int pos[3] = {x, y, z};
    uint8_t c[4];

    make_building_color(x, y, z, c);
    volume_set_at(vol, NULL, pos, c);
}

static void paint_color_voxel(volume_t *vol, int x, int y, int z,
                              const uint8_t color[4])
{
    int pos[3] = {x, y, z};
    volume_set_at(vol, NULL, pos, color);
}

static void clear_voxel(volume_t *vol, int x, int y, int z)
{
    int pos[3] = {x, y, z};
    volume_set_at(vol, NULL, pos, k_empty);
}

static void paint_slab(volume_t *vol, const building_group_t *g,
                       int z0, int thickness)
{
    int i, t, z;

    for (i = 0; i < g->ncells; i++) {
        for (t = 0; t < thickness; t++) {
            z = z0 + t;
            paint_voxel(vol, g->cells[i].x, g->cells[i].y, z);
        }
    }
}

static void paint_walls(volume_t *vol, const building_group_t *g,
                        int z0, int height, int wall_t)
{
    int i, t, z, x, y;

    for (i = 0; i < g->ncells; i++) {
        x = g->cells[i].x;
        y = g->cells[i].y;
        if (!is_wall_cell(g, x, y, wall_t))
            continue;
        for (t = 0; t < height; t++) {
            z = z0 + t;
            paint_voxel(vol, x, y, z);
        }
    }
}

static void clear_building_column_range(volume_t *vol, const building_group_t *g,
                                        int z0, int height)
{
    int i, t;

    for (i = 0; i < g->ncells; i++) {
        for (t = 0; t < height; t++)
            clear_voxel(vol, g->cells[i].x, g->cells[i].y, z0 + t);
    }
}

/* Oriented bounding box in principal-axis (u, v) frame. */
typedef struct {
    float ux, uy; /* unit axis u */
    float vx, vy; /* unit axis v (perpendicular) */
    float u_min, u_max;
    float v_min, v_max;
} building_obb_t;

static float obb_proj_u(const building_obb_t *o, float x, float y)
{
    return x * o->ux + y * o->uy;
}

static float obb_proj_v(const building_obb_t *o, float x, float y)
{
    return x * o->vx + y * o->vy;
}

static bool obb_contains(const building_obb_t *o, float x, float y,
                         float pad_u, float pad_v)
{
    float pu = obb_proj_u(o, x, y);
    float pv = obb_proj_v(o, x, y);

    return pu >= o->u_min - pad_u && pu <= o->u_max + pad_u &&
           pv >= o->v_min - pad_v && pv <= o->v_max + pad_v;
}

static void obb_set_axis_aligned(building_obb_t *o, const building_group_t *g)
{
    o->ux = 1.f;
    o->uy = 0.f;
    o->vx = 0.f;
    o->vy = 1.f;
    o->u_min = (float)g->xmin;
    o->u_max = (float)g->xmax;
    o->v_min = (float)g->ymin;
    o->v_max = (float)g->ymax;
}

/* Eigenvector for the larger eigenvalue of [[a,b],[b,c]]. */
static void pca_principal_axis(float a, float b, float c,
                               float *ux, float *uy)
{
    float diff = a - c;
    float disc = sqrtf(diff * diff + 4.f * b * b);
    float l1 = 0.5f * (a + c + disc);
    float n;

    if (fabsf(b) > 1e-6f || fabsf(l1 - a) > 1e-6f) {
        *ux = b;
        *uy = l1 - a;
    } else {
        *ux = 1.f;
        *uy = 0.f;
    }
    n = sqrtf((*ux) * (*ux) + (*uy) * (*uy));
    if (n < 1e-6f) {
        *ux = 1.f;
        *uy = 0.f;
        return;
    }
    *ux /= n;
    *uy /= n;
}

static bool compute_pca_obb(const building_group_t *g, building_obb_t *o)
{
    int i;
    float cx, cy, a, b, c, dx, dy, pu, pv;

    if (g->ncells < 4)
        return false;

    cx = 0.f;
    cy = 0.f;
    for (i = 0; i < g->ncells; i++) {
        cx += (float)g->cells[i].x;
        cy += (float)g->cells[i].y;
    }
    cx /= (float)g->ncells;
    cy /= (float)g->ncells;

    a = 0.f;
    b = 0.f;
    c = 0.f;
    for (i = 0; i < g->ncells; i++) {
        dx = (float)g->cells[i].x - cx;
        dy = (float)g->cells[i].y - cy;
        a += dx * dx;
        b += dx * dy;
        c += dy * dy;
    }
    a /= (float)g->ncells;
    b /= (float)g->ncells;
    c /= (float)g->ncells;

    /* Near-degenerate (line-like) footprints are not rectangles. */
    if (a + c < 1e-4f)
        return false;

    pca_principal_axis(a, b, c, &o->ux, &o->uy);
    o->vx = -o->uy;
    o->vy = o->ux;

    o->u_min = FLT_MAX;
    o->u_max = -FLT_MAX;
    o->v_min = FLT_MAX;
    o->v_max = -FLT_MAX;
    for (i = 0; i < g->ncells; i++) {
        pu = obb_proj_u(o, (float)g->cells[i].x, (float)g->cells[i].y);
        pv = obb_proj_v(o, (float)g->cells[i].x, (float)g->cells[i].y);
        if (pu < o->u_min) o->u_min = pu;
        if (pu > o->u_max) o->u_max = pu;
        if (pv < o->v_min) o->v_min = pv;
        if (pv > o->v_max) o->v_max = pv;
    }
    return true;
}

static void obb_world_aabb(const building_obb_t *o,
                           int *x0, int *x1, int *y0, int *y1)
{
    int i, j;
    float u, v, wx, wy;
    float us[2] = {o->u_min, o->u_max};
    float vs[2] = {o->v_min, o->v_max};

    *x0 = INT_MAX;
    *x1 = INT_MIN;
    *y0 = INT_MAX;
    *y1 = INT_MIN;
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            u = us[i];
            v = vs[j];
            wx = u * o->ux + v * o->vx;
            wy = u * o->uy + v * o->vy;
            if ((int)floorf(wx) < *x0) *x0 = (int)floorf(wx);
            if ((int)ceilf(wx) > *x1) *x1 = (int)ceilf(wx);
            if ((int)floorf(wy) < *y0) *y0 = (int)floorf(wy);
            if ((int)ceilf(wy) > *y1) *y1 = (int)ceilf(wy);
        }
    }
}

/* Count integer cells whose centres lie in the OBB; holes = empty among them. */
static void count_obb_occupancy(const building_group_t *g,
                                const building_obb_t *o,
                                int *out_inside, int *out_holes)
{
    int x, y, x0, x1, y0, y1, inside = 0, holes = 0;

    obb_world_aabb(o, &x0, &x1, &y0, &y1);
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            if (!obb_contains(o, (float)x, (float)y, 0.f, 0.f))
                continue;
            inside++;
            if (!occ_get(g, x, y))
                holes++;
        }
    }
    *out_inside = inside;
    *out_holes = holes;
}

/*
 * True if the footprint is a filled rectangle in map axes or in its PCA
 * oriented frame.  On success, *obb holds the frame used for a gabled roof.
 */
static bool analyze_rectangle(const building_group_t *g, building_obb_t *obb)
{
    int w, h, inside, holes, hole_budget;
    float fill;

    if (g->ncells <= 0 || !g->occ)
        return false;

    w = g->xmax - g->xmin + 1;
    h = g->ymax - g->ymin + 1;
    if (g->ncells == w * h) {
        obb_set_axis_aligned(obb, g);
        return true;
    }

    if (!compute_pca_obb(g, obb))
        return false;

    count_obb_occupancy(g, obb, &inside, &holes);
    if (inside < 4)
        return false;
    fill = (float)(inside - holes) / (float)inside;
    if (fill < BUILDING_RECT_FILL_MIN)
        return false;

    hole_budget = 1 + (int)(BUILDING_RECT_HOLE_FRAC * (float)g->ncells);
    if (holes > hole_budget)
        return false;

    return true;
}

static void paint_gabled_roof(volume_t *vol, const building_obb_t *obb, int z0,
                              const uint8_t colors[2][4], int max_h)
{
    float eu = obb->u_max - obb->u_min;
    float ev = obb->v_max - obb->v_min;
    bool shrink_u = eu <= ev;
    float short_span = shrink_u ? eu : ev;
    int short_size = (int)floorf(short_span + 1.f + 1e-3f) + 2;
    /* The natural peak is taller than max_h, so the top layer stays flat. */
    bool truncated = max_h * 2 < short_size;
    int level, x, y, x0, x1, y0, y1;
    float su0, su1, sv0, sv1, pu, pv, pad;
    float corners_u[2], corners_v[2];
    float wx, wy;
    const uint8_t *color;
    bool on_gable, on_eave, flat_top;

    for (level = 0; level < max_h && level * 2 < short_size; level++) {
        color = colors[level & 1];
        /* A flat top is all roof, keeping the gable one layer below it. */
        flat_top = truncated && level == max_h - 1;
        pad = 1.f - (float)level;
        if (shrink_u) {
            su0 = obb->u_min - pad;
            su1 = obb->u_max + pad;
            sv0 = obb->v_min;
            sv1 = obb->v_max;
        } else {
            su0 = obb->u_min;
            su1 = obb->u_max;
            sv0 = obb->v_min - pad;
            sv1 = obb->v_max + pad;
        }

        corners_u[0] = su0;
        corners_u[1] = su1;
        corners_v[0] = sv0;
        corners_v[1] = sv1;
        x0 = INT_MAX;
        x1 = INT_MIN;
        y0 = INT_MAX;
        y1 = INT_MIN;
        for (y = 0; y < 2; y++) {
            for (x = 0; x < 2; x++) {
                wx = corners_u[x] * obb->ux + corners_v[y] * obb->vx;
                wy = corners_u[x] * obb->uy + corners_v[y] * obb->vy;
                if ((int)floorf(wx) < x0) x0 = (int)floorf(wx);
                if ((int)ceilf(wx) > x1) x1 = (int)ceilf(wx);
                if ((int)floorf(wy) < y0) y0 = (int)floorf(wy);
                if ((int)ceilf(wy) > y1) y1 = (int)ceilf(wy);
            }
        }

        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                pu = obb_proj_u(obb, (float)x, (float)y);
                pv = obb_proj_v(obb, (float)x, (float)y);
                /* Paint when the cell centre lies in this level's OBB. */
                if (pu < su0 || pu > su1 || pv < sv0 || pv > sv1)
                    continue;

                /*
                 * Gable ends are the long-axis frontier of this level's slab
                 * (wall colour).  The short-axis frontier is the one-voxel
                 * roof-coloured eave/border.  Depth < 1 matches the old
                 * axis-aligned row test and still catches staircase cells on
                 * rotated footprints (a 0.5 band was too thin there).
                 */
                if (shrink_u) {
                    on_gable = (pv - sv0 < 1.f) || (sv1 - pv < 1.f);
                    on_eave = (pu - su0 < 1.f) || (su1 - pu < 1.f);
                } else {
                    on_gable = (pu - su0 < 1.f) || (su1 - pu < 1.f);
                    on_eave = (pv - sv0 < 1.f) || (sv1 - pv < 1.f);
                }

                if (on_gable && !on_eave && !flat_top)
                    paint_voxel(vol, x, y, z0 + level);
                else
                    paint_color_voxel(vol, x, y, z0 + level, color);
            }
        }
    }
}

/* The non-rectangular roof footprint is the building footprint dilated by
 * one voxel in XY, including its diagonal corners. */
static bool pyramid_roof_get(const building_group_t *g, int x, int y)
{
    int dx, dy;

    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            if (occ_get(g, x + dx, y + dy))
                return true;
        }
    }
    return false;
}

static bool is_pyramid_roof_edge(const building_group_t *g, int x, int y,
                                 int distance)
{
    int d, dx, dy, ay;

    for (d = 1; d <= distance; d++) {
        for (dx = -d; dx <= d; dx++) {
            dy = d - abs(dx);
            ay = y + dy;
            if (!pyramid_roof_get(g, x + dx, ay))
                return true;
            if (dy != 0 && !pyramid_roof_get(g, x + dx, y - dy))
                return true;
        }
    }
    return false;
}

static void paint_pyramid_roof(volume_t *vol, const building_group_t *g, int z0,
                               const uint8_t colors[2][4], int max_h)
{
    int level, x, y, painted;

    for (level = 0; level < max_h; level++) {
        painted = 0;
        for (y = g->ymin - 1; y <= g->ymax + 1; y++) {
            for (x = g->xmin - 1; x <= g->xmax + 1; x++) {
                if (!pyramid_roof_get(g, x, y))
                    continue;
                if (level > 0 && is_pyramid_roof_edge(g, x, y, level))
                    continue;
                paint_color_voxel(vol, x, y, z0 + level,
                                  colors[level & 1]);
                painted++;
            }
        }
        if (!painted)
            break;
    }
}

static void generate_group(volume_t *vol, const building_group_t *g,
                           int floor_h, int floor_t, int wall_t,
                           int max_roof_h, const uint8_t roof_colors[2][4])
{
    int i;
    int total_h;
    int z;

    if (g->ncells <= 0)
        return;

    total_h = g->floors * floor_h + (g->floors + 1) * floor_t;
    clear_building_column_range(vol, g, g->base_z, total_h);

    /* Base slab. */
    paint_slab(vol, g, g->base_z, floor_t);
    z = g->base_z + floor_t;

    for (i = 0; i < g->floors; i++) {
        paint_walls(vol, g, z, floor_h, wall_t);
        z += floor_h;
        /* Intermediate floor, or flat roof after the last storey. */
        paint_slab(vol, g, z, floor_t);
        z += floor_t;
    }

    /* Roofs are generated after the complete building shell. */
    {
        building_obb_t obb;

        if (analyze_rectangle(g, &obb))
            paint_gabled_roof(vol, &obb, z, roof_colors, max_roof_h);
        else
            paint_pyramid_roof(vol, g, z, roof_colors, max_roof_h);
    }
}

static bool group_key_equal(const building_group_t *g, int floors, int base_z)
{
    return g->floors == floors && g->base_z == base_z;
}

static building_group_t *find_or_add_group(building_group_t **groups, int *ngroups,
                                           int *cap, int floors, int base_z)
{
    int i;
    building_group_t *g;
    building_group_t *ng;

    for (i = 0; i < *ngroups; i++) {
        if (group_key_equal(&(*groups)[i], floors, base_z))
            return &(*groups)[i];
    }

    if (*ngroups >= *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        ng = realloc(*groups, (size_t)ncap * sizeof(*ng));
        if (!ng)
            return NULL;
        *groups = ng;
        *cap = ncap;
    }

    g = &(*groups)[*ngroups];
    memset(g, 0, sizeof(*g));
    g->floors = floors;
    g->base_z = base_z;
    g->xmin = INT_MAX;
    g->ymin = INT_MAX;
    g->xmax = INT_MIN;
    g->ymax = INT_MIN;
    (*ngroups)++;
    return g;
}

static bool group_add_cell(building_group_t *g, int x, int y)
{
    building_xy_t *nc;

    if (g->ncells >= g->cells_cap) {
        int new_cap = g->cells_cap ? g->cells_cap * 2 : 64;
        nc = realloc(g->cells, (size_t)new_cap * sizeof(*nc));
        if (!nc)
            return false;
        g->cells = nc;
        g->cells_cap = new_cap;
    }

    g->cells[g->ncells].x = x;
    g->cells[g->ncells].y = y;
    g->ncells++;
    if (x < g->xmin) g->xmin = x;
    if (y < g->ymin) g->ymin = y;
    if (x > g->xmax) g->xmax = x;
    if (y > g->ymax) g->ymax = y;
    return true;
}

static bool group_build_occupancy(building_group_t *g)
{
    int w, h, i, n;

    if (g->ncells <= 0)
        return true;

    w = g->xmax - g->xmin + 1;
    h = g->ymax - g->ymin + 1;
    n = w * h;
    g->occ = calloc((size_t)n, 1);
    if (!g->occ)
        return false;

    for (i = 0; i < g->ncells; i++)
        g->occ[occ_index(g, g->cells[i].x, g->cells[i].y)] = 1;
    return true;
}

static void free_groups(building_group_t *groups, int ngroups);

static bool split_connected_groups(building_group_t *source, int nsource,
                                   building_group_t **out_groups,
                                   int *out_ngroups)
{
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    building_group_t *groups = NULL;
    int ngroups = 0, cap = 0;
    int si, ci, d;

    for (si = 0; si < nsource; si++) {
        building_group_t *src = &source[si];
        int area = (src->xmax - src->xmin + 1) *
                   (src->ymax - src->ymin + 1);
        uint8_t *visited = calloc((size_t)area, 1);
        building_xy_t *queue = malloc((size_t)src->ncells * sizeof(*queue));

        if (!visited || !queue) {
            free(visited);
            free(queue);
            free_groups(groups, ngroups);
            return false;
        }

        for (ci = 0; ci < src->ncells; ci++) {
            int qhead = 0, qtail = 0;
            int seed_x = src->cells[ci].x;
            int seed_y = src->cells[ci].y;
            building_group_t *dst;

            if (visited[occ_index(src, seed_x, seed_y)])
                continue;

            if (ngroups >= cap) {
                int ncap = cap ? cap * 2 : 8;
                building_group_t *next =
                    realloc(groups, (size_t)ncap * sizeof(*next));
                if (!next) {
                    free(visited);
                    free(queue);
                    free_groups(groups, ngroups);
                    return false;
                }
                groups = next;
                cap = ncap;
            }

            dst = &groups[ngroups++];
            memset(dst, 0, sizeof(*dst));
            dst->floors = src->floors;
            dst->base_z = src->base_z;
            dst->xmin = dst->ymin = INT_MAX;
            dst->xmax = dst->ymax = INT_MIN;

            queue[qtail++] = (building_xy_t){seed_x, seed_y};
            visited[occ_index(src, seed_x, seed_y)] = 1;
            while (qhead < qtail) {
                building_xy_t p = queue[qhead++];

                if (!group_add_cell(dst, p.x, p.y)) {
                    free(visited);
                    free(queue);
                    free_groups(groups, ngroups);
                    return false;
                }
                for (d = 0; d < 4; d++) {
                    int nx = p.x + dirs[d][0];
                    int ny = p.y + dirs[d][1];
                    int ni;
                    if (!occ_get(src, nx, ny))
                        continue;
                    ni = occ_index(src, nx, ny);
                    if (visited[ni])
                        continue;
                    visited[ni] = 1;
                    queue[qtail++] = (building_xy_t){nx, ny};
                }
            }
            if (!group_build_occupancy(dst)) {
                free(visited);
                free(queue);
                free_groups(groups, ngroups);
                return false;
            }
        }
        free(visited);
        free(queue);
    }

    *out_groups = groups;
    *out_ngroups = ngroups;
    return true;
}

static void free_groups(building_group_t *groups, int ngroups)
{
    int i;

    if (!groups)
        return;
    for (i = 0; i < ngroups; i++) {
        free(groups[i].cells);
        free(groups[i].occ);
    }
    free(groups);
}

static bool collect_groups(const volume_t *src, const filter_buildings_t *filter,
                           building_group_t **out_groups, int *out_ngroups)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t color[4];
    int floors;
    building_group_t *groups = NULL;
    int ngroups = 0;
    int cap = 0;
    int i;
    building_group_t *buildings = NULL;
    int nbuildings = 0;

    *out_groups = NULL;
    *out_ngroups = 0;

    iter = volume_get_iterator(src, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        building_group_t *g;

        volume_get_at(src, &iter, pos, color);
        if (!color[3])
            continue;
        floors = floor_count_for_color(filter, color);
        if (!floors)
            continue;

        g = find_or_add_group(&groups, &ngroups, &cap, floors, pos[2]);
        if (!g) {
            free_groups(groups, ngroups);
            return false;
        }
        if (!group_add_cell(g, pos[0], pos[1])) {
            free_groups(groups, ngroups);
            return false;
        }
    }

    for (i = 0; i < ngroups; i++) {
        if (!group_build_occupancy(&groups[i])) {
            free_groups(groups, ngroups);
            return false;
        }
    }

    if (!split_connected_groups(groups, ngroups, &buildings, &nbuildings)) {
        free_groups(groups, ngroups);
        return false;
    }
    free_groups(groups, ngroups);
    *out_groups = buildings;
    *out_ngroups = nbuildings;
    return true;
}

static unsigned building_hash(const building_group_t *g)
{
    unsigned h = 2166136261u;
    const int values[] = {
        g->xmin, g->ymin, g->xmax, g->ymax, g->base_z, g->floors, g->ncells,
    };
    int i;

    for (i = 0; i < (int)ARRAY_SIZE(values); i++) {
        h ^= (unsigned)values[i];
        h *= 16777619u;
    }
    return h;
}

static layer_t *prepare_target_layer(layer_t *plan_layer)
{
    layer_t *target;
    const char *suffix = " Buildings";
    int max_base;

    target = image_add_layer(goxel.image, NULL);
    if (!target)
        return NULL;

    target->visible = true;
    max_base = (int)sizeof(target->name) - 1 - (int)strlen(suffix);
    if (max_base < 0)
        max_base = 0;
    snprintf(target->name, sizeof(target->name), "%.*s%s",
             max_base, plan_layer->name, suffix);
    plan_layer->visible = false;
    return target;
}

static void apply_buildings(filter_buildings_t *filter, layer_t *layer)
{
    volume_t *src = NULL;
    layer_t *target_layer;
    building_group_t *groups = NULL;
    int ngroups = 0;
    int i;
    int floor_h, floor_t, wall_t, max_roof_h;

    if (!layer || !layer->volume || volume_is_empty(layer->volume)) {
        gui_alert("Plan - Buildings", "Active layer has no voxels.");
        return;
    }

    floor_h = clampi(filter->floor_height, 1, 256);
    floor_t = clampi(filter->floor_thickness, 1, 64);
    wall_t = clampi(filter->wall_thickness, 1, 64);
    max_roof_h = clampi(filter->max_roof_height, 1, 256);
    filter->floor_height = floor_h;
    filter->floor_thickness = floor_t;
    filter->wall_thickness = wall_t;
    filter->max_roof_height = max_roof_h;

    src = volume_copy(layer->volume);
    if (!src) {
        gui_alert("Plan - Buildings", "Could not copy the active layer.");
        return;
    }

    if (!collect_groups(src, filter, &groups, &ngroups)) {
        gui_alert("Plan - Buildings", "Out of memory while scanning footprints.");
        volume_delete(src);
        return;
    }

    if (ngroups == 0) {
        gui_alert("Plan - Buildings",
                  "No footprint blocks match the configured floor colours.");
        free_groups(groups, ngroups);
        volume_delete(src);
        return;
    }

    image_history_push(goxel.image);
    target_layer = prepare_target_layer(layer);
    if (!target_layer) {
        gui_alert("Plan - Buildings", "Could not create the buildings layer.");
        free_groups(groups, ngroups);
        volume_delete(src);
        return;
    }

    for (i = 0; i < ngroups; i++) {
        int roof_pair = (int)(building_hash(&groups[i]) %
                              (unsigned)filter->roof_pair_count);
        generate_group(target_layer->volume, &groups[i],
                       floor_h, floor_t, wall_t, max_roof_h,
                       filter->roof_colors[roof_pair]);
    }

    free_groups(groups, ngroups);
    volume_delete(src);
}

static int gui(filter_t *filter_)
{
    filter_buildings_t *filter = (void *)filter_;
    layer_t *layer = goxel.image->active_layer;
    int i;
    char color_id[32];
    char switch_id[32];
    char set_id[32];
    char roof_color_id[32];
    char remove_id[32];
    char label[8];

    const char *help_text =
        "Paint 1-block-high filled shapes on the active layer using the floor "
        "colours below.  Generate builds walls along the inward edges, slabs "
        "at the base and between storeys, and shaped roofs on a new Buildings "
        "layer.  The plan layer is preserved and hidden.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    gui_label_size_push(150.0f);
    gui_input_int("Height of each floor", &filter->floor_height, 1, 256);
    gui_tooltip_if_hovered(
        "Clear height between floor slabs (wall / interior height). Default 4.");
    gui_input_int("Floor thickness", &filter->floor_thickness, 1, 64);
    gui_tooltip_if_hovered("Thickness of each floor slab. Default 1.");
    gui_input_int("Wall thickness", &filter->wall_thickness, 1, 64);
    gui_tooltip_if_hovered(
        "How many blocks inward from the footprint edge become walls. Default 1.");
    gui_input_int("Max roof height", &filter->max_roof_height, 1, 256);
    gui_tooltip_if_hovered(
        "Roof slope stops at this height and finishes flat. Default 8.");
    gui_label_size_pop();

    gui_separator();
    gui_text("Number of floors:");

    for (i = 0; i < BUILDING_FLOOR_COUNTS; i++) {
        snprintf(label, sizeof(label), "%d", i + 1);
        snprintf(color_id, sizeof(color_id), "##floor_color_%d", i + 1);
        snprintf(switch_id, sizeof(switch_id), "Switch to##floor_%d", i + 1);
        snprintf(set_id, sizeof(set_id), "Set##floor_%d", i + 1);

        gui_text("%s", label);
        gui_same_line();
        gui_color_small_no_label(color_id, filter->floor_colors[i]);
        gui_same_line();
        if (gui_button(switch_id, 0, 0))
            memcpy(goxel.painter.color, filter->floor_colors[i], 4);
        gui_tooltip_if_hovered("Switch the current brush colour to this colour.");
        gui_same_line();
        if (gui_button(set_id, 0, 0))
            memcpy(filter->floor_colors[i], goxel.painter.color, 4);
        gui_tooltip_if_hovered("Set this floor marker to the current brush colour.");
    }

    gui_separator();
    gui_text("Roof colour pairs:");
    gui_text_wrapped(
        "Each building picks one pair. Roof layers alternate between its two "
        "colours.");
    for (i = 0; i < filter->roof_pair_count; i++) {
        snprintf(roof_color_id, sizeof(roof_color_id), "##roof_%d_a", i);
        gui_color_small_no_label(roof_color_id, filter->roof_colors[i][0]);
        gui_same_line();
        snprintf(roof_color_id, sizeof(roof_color_id), "##roof_%d_b", i);
        gui_color_small_no_label(roof_color_id, filter->roof_colors[i][1]);
        gui_same_line();
        snprintf(remove_id, sizeof(remove_id), "Remove##roof_%d", i);
        if (gui_button(remove_id, 0, 0) && filter->roof_pair_count > 1) {
            memmove(&filter->roof_colors[i], &filter->roof_colors[i + 1],
                    (size_t)(filter->roof_pair_count - i - 1) *
                        sizeof(filter->roof_colors[0]));
            filter->roof_pair_count--;
            break;
        }
        if (filter->roof_pair_count == 1)
            gui_tooltip_if_hovered("At least one roof colour pair is required.");
    }
    if (gui_button("Add roof colour pair", -1, 0) &&
        filter->roof_pair_count < BUILDING_MAX_ROOF_PAIRS) {
        int n = filter->roof_pair_count;
        memcpy(filter->roof_colors[n], filter->roof_colors[n - 1],
               sizeof(filter->roof_colors[n]));
        filter->roof_pair_count++;
    }
    gui_tooltip_if_hovered("Adds a copy of the last pair for editing.");

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    if (gui_button("Generate", -1, 0))
        apply_buildings(filter, layer);

    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_buildings_t *filter = (void *)filter_;
    reset_defaults(filter);
}

FILTER_REGISTER(buildings, filter_buildings_t,
                .name = "Plan - Buildings",
                .on_open = on_open,
                .panel_width = 320,
                .gui_fn = gui, )
