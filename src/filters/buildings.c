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
#include "utils/color.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

/*
 * Plan - Buildings
 *
 * Paint 1-block-high filled footprints on the active layer.  Each marker
 * colour maps to a floor count.  Generate replaces those footprints
 * with inward-walled buildings: base slab, clear-height storeys with
 * perimeter walls, intermediate slabs, and a shaped roof.
 */

#define BUILDING_DEFAULT_FLOOR_COLORS 4
#define BUILDING_MAX_FLOOR_COLORS 32
#define BUILDING_MAX_WALL_COLORS 32
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
    int max_building_size; /* longest-side length per undivided section */
    int max_roof_height;   /* roof stops and stays flat past this */
    bool generate_roofs;   /* when false, stop at the top slab */
    bool generate_windows;
    bool all_one_layer;
    int window_width;
    int window_height;
    int window_min_gap;
    int window_above_floor; /* clear height from slab top to window sill */
    int seed; /* mixes into per-building wall / roof colour picks */
    uint8_t floor_colors[BUILDING_MAX_FLOOR_COLORS][4];
    int floor_color_count;
    uint8_t wall_colors[BUILDING_MAX_WALL_COLORS][4];
    int wall_color_count;
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

static const uint8_t k_default_floor_colors[BUILDING_DEFAULT_FLOOR_COLORS][4] = {
    {220, 70, 70, 255},
    {220, 183, 70, 255},
    {145, 220, 70, 255},
    {70, 220, 108, 255},
};

static const uint8_t k_default_wall_colors[][4] = {
    {128, 128, 128, 255}, /* concrete */
    {154, 145, 132, 255}, /* warm stone */
    {168, 145, 112, 255}, /* sandstone */
    {143, 91, 76, 255},   /* muted brick */
    {181, 174, 151, 255}, /* cream stucco */
    {91, 99, 105, 255},   /* dark slate */
};

static const uint8_t k_default_roof_colors[][2][4] = {
    {{0x28, 0x33, 0x3f, 255}, {0x31, 0x3b, 0x47, 255}},
    {{0x20, 0x24, 0x29, 255}, {0x2f, 0x33, 0x37, 255}},
    {{0x3f, 0x3f, 0x3f, 255}, {0x33, 0x32, 0x32, 255}},
    {{0x39, 0x2f, 0x2a, 255}, {0x2d, 0x24, 0x1e, 255}},
};

static const uint8_t k_empty[4] = {0, 0, 0, 0};

static void reset_defaults(filter_buildings_t *filter)
{
    int i;

    filter->floor_height = 5;
    filter->floor_thickness = 1;
    filter->wall_thickness = 1;
    filter->max_building_size = 16;
    filter->max_roof_height = 8;
    filter->generate_roofs = true;
    filter->generate_windows = true;
    filter->all_one_layer = true;
    filter->window_width = 2;
    filter->window_height = 2;
    filter->window_min_gap = 3;
    filter->window_above_floor = 2;
    filter->seed = 0;
    filter->floor_color_count = BUILDING_DEFAULT_FLOOR_COLORS;
    for (i = 0; i < filter->floor_color_count; i++)
        memcpy(filter->floor_colors[i], k_default_floor_colors[i], 4);
    filter->wall_color_count = (int)ARRAY_SIZE(k_default_wall_colors);
    memcpy(filter->wall_colors, k_default_wall_colors,
           sizeof(k_default_wall_colors));
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

    for (i = 0; i < filter->floor_color_count; i++) {
        if (memcmp(filter->floor_colors[i], color, 4) == 0)
            return i + 1;
    }
    return 0;
}

static void make_noisy_color(const uint8_t base[4], int x, int y, int z,
                             uint8_t out[4])
{
    int rgb[3];

    memcpy(out, base, 4);
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

static void make_building_color(const uint8_t base[4], int x, int y, int z,
                                uint8_t out[4])
{
    make_noisy_color(base, x, y, z, out);
}

static void make_dark_building_color(const uint8_t base[4],
                                     int x, int y, int z, uint8_t out[4])
{
    int i;

    make_building_color(base, x, y, z, out);
    for (i = 0; i < 3; i++)
        out[i] = (uint8_t)(out[i] * 9 / 10);
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

static void paint_voxel(volume_t *vol, int x, int y, int z,
                        const uint8_t base[4])
{
    int pos[3] = {x, y, z};
    uint8_t c[4];

    make_building_color(base, x, y, z, c);
    volume_set_at(vol, NULL, pos, c);
}

static void paint_color_voxel(volume_t *vol, int x, int y, int z,
                              const uint8_t color[4])
{
    int pos[3] = {x, y, z};
    volume_set_at(vol, NULL, pos, color);
}

static void paint_noisy_color_voxel(volume_t *vol, int x, int y, int z,
                                    const uint8_t color[4])
{
    uint8_t noisy[4];

    make_noisy_color(color, x, y, z, noisy);
    paint_color_voxel(vol, x, y, z, noisy);
}

static void paint_dark_voxel(volume_t *vol, int x, int y, int z,
                             const uint8_t base[4])
{
    uint8_t color[4];

    make_dark_building_color(base, x, y, z, color);
    paint_color_voxel(vol, x, y, z, color);
}

static void clear_voxel(volume_t *vol, int x, int y, int z)
{
    int pos[3] = {x, y, z};
    volume_set_at(vol, NULL, pos, k_empty);
}

static void paint_slab(volume_t *vol, const building_group_t *g,
                       int z0, int thickness, const uint8_t wall_color[4])
{
    int i, t, z;

    for (i = 0; i < g->ncells; i++) {
        for (t = 0; t < thickness; t++) {
            z = z0 + t;
            paint_voxel(vol, g->cells[i].x, g->cells[i].y, z, wall_color);
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

/* Longest-axis slice used by chunk partitioning. */
typedef struct {
    float dx, dy; /* unit direction along the axis */
    float min_p;  /* smallest cell-centre projection on it */
    float len;
} building_axis_t;

static building_axis_t building_long_axis(const building_group_t *g)
{
    building_axis_t axis;
    building_obb_t obb;

    if (!compute_pca_obb(g, &obb))
        obb_set_axis_aligned(&obb, g);

    if (obb.u_max - obb.u_min >= obb.v_max - obb.v_min) {
        axis.dx = obb.ux;
        axis.dy = obb.uy;
        axis.min_p = obb.u_min;
        axis.len = obb.u_max - obb.u_min + 1.f;
    } else {
        axis.dx = obb.vx;
        axis.dy = obb.vy;
        axis.min_p = obb.v_min;
        axis.len = obb.v_max - obb.v_min + 1.f;
    }
    return axis;
}

/*
 * Slice the footprint into nchunks even bands across `axis`, so partitions
 * follow the building's own direction instead of the map grid.
 */
static int *build_chunk_map(const building_group_t *g, int nchunks,
                            const building_axis_t *axis)
{
    int *chunks;
    int area, i;

    if (nchunks <= 1 || axis->len < 1.f)
        return NULL;

    area = (g->xmax - g->xmin + 1) * (g->ymax - g->ymin + 1);
    chunks = malloc((size_t)area * sizeof(*chunks));
    if (!chunks)
        return NULL;

    for (i = 0; i < area; i++)
        chunks[i] = -1;

    for (i = 0; i < g->ncells; i++) {
        int x = g->cells[i].x;
        int y = g->cells[i].y;
        /* Half-cell offset keeps the projection inside its own band. */
        float pos = (float)x * axis->dx + (float)y * axis->dy -
                    axis->min_p + 0.5f;
        int chunk = (int)(pos * (float)nchunks / axis->len);

        chunks[occ_index(g, x, y)] = clampi(chunk, 0, nchunks - 1);
    }
    return chunks;
}

/*
 * Partition walls occupy the higher-numbered side of each chunk boundary.
 * Looking up to wall_t cells into that side gives them the configured wall
 * thickness without doubling it across both adjoining chunks.
 */
static bool is_partition_wall_cell(const building_group_t *g,
                                   const int *chunks, int x, int y,
                                   int wall_t)
{
    int own, d, dx, dy;

    if (!chunks)
        return false;
    own = chunks[occ_index(g, x, y)];
    for (d = 1; d <= wall_t; d++) {
        for (dx = -d; dx <= d; dx++) {
            int other;
            dy = d - abs(dx);
            if (occ_get(g, x + dx, y + dy)) {
                other = chunks[occ_index(g, x + dx, y + dy)];
                if (other >= 0 && other < own)
                    return true;
            }
            if (dy != 0 && occ_get(g, x + dx, y - dy)) {
                other = chunks[occ_index(g, x + dx, y - dy)];
                if (other >= 0 && other < own)
                    return true;
            }
        }
    }
    return false;
}

static void paint_walls(volume_t *vol, const building_group_t *g,
                        int z0, int height, int wall_t, const int *chunks,
                        const uint8_t wall_color[4])
{
    int i, t, z, x, y;

    for (i = 0; i < g->ncells; i++) {
        x = g->cells[i].x;
        y = g->cells[i].y;
        if (!is_wall_cell(g, x, y, wall_t) &&
            !is_partition_wall_cell(g, chunks, x, y, wall_t))
            continue;
        for (t = 0; t < height; t++) {
            z = z0 + t;
            paint_voxel(vol, x, y, z, wall_color);
        }
    }
}

/*
 * A façade is the run of exterior wall cells sharing one outward direction,
 * ordered along the wall.  Grouping by direction keeps a staircased (rotated
 * or diagonal) wall in a single run, so window widths and gaps can be counted
 * in voxels along it instead of measured in oriented-frame units.
 */
typedef struct {
    building_xy_t *cells;
    int *ranks; /* position along the run; cells sharing one step share it */
    int ncells;
    int nranks;
    int dir;    /* index into k_facade_dirs */
    int dx, dy; /* outward direction */
    bool is_end; /* faces along the building's length: a short end wall */
} building_facade_t;

/*
 * Four straight faces followed by four diagonal ones.  A cell on a 45° wall is
 * exposed in two perpendicular directions; giving those their own bucket keeps
 * each wall cell in exactly one run.  Sharing cells between two runs used to
 * cut two different sets of voxels and produce openings wider than requested.
 */
static const int k_facade_dirs[8][2] = {
    {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
};

/* Position along the run: rows for ±X faces, columns for ±Y, the
 * perpendicular diagonal for the diagonal faces. */
static int facade_tangent(int dir, int x, int y)
{
    if (dir < 2)
        return y;
    if (dir < 4)
        return x;
    return k_facade_dirs[dir][0] * k_facade_dirs[dir][1] > 0 ? x - y : x + y;
}

typedef struct {
    int t;
    building_xy_t cell;
} facade_sort_t;

static int compare_facade_sort(const void *a_, const void *b_)
{
    const facade_sort_t *a = a_;
    const facade_sort_t *b = b_;

    if (a->t != b->t) return a->t < b->t ? -1 : 1;
    if (a->cell.y != b->cell.y) return a->cell.y < b->cell.y ? -1 : 1;
    if (a->cell.x != b->cell.x) return a->cell.x < b->cell.x ? -1 : 1;
    return 0;
}

/* True if the wall keeps going sideways with the same face exposed. */
static bool wall_continues(const building_group_t *g, int x, int y, int dir)
{
    int s;

    for (s = -1; s <= 1; s += 2) {
        int nx = x + (dir < 2 ? 0 : s);
        int ny = y + (dir < 2 ? s : 0);

        if (!occ_get(g, nx, ny))
            continue;
        if (!occ_get(g, nx + k_facade_dirs[dir][0],
                     ny + k_facade_dirs[dir][1]))
            return true;
    }
    return false;
}

/*
 * Which façade runs a wall cell belongs to, as a bitmask over k_facade_dirs.
 * Straight faces win where the wall continues sideways; otherwise the cell
 * sits on a diagonal wall and joins the matching diagonal run.  A convex
 * corner continues both ways and joins both runs as their end cell.
 */
static int facade_dirs_for_cell(const building_group_t *g, int x, int y)
{
    int exposed = 0, out = 0, d;

    for (d = 0; d < 4; d++) {
        if (!occ_get(g, x + k_facade_dirs[d][0], y + k_facade_dirs[d][1]))
            exposed |= 1 << d;
    }
    if (!exposed)
        return 0;

    for (d = 0; d < 4; d++) {
        if ((exposed & (1 << d)) && wall_continues(g, x, y, d))
            out |= 1 << d;
    }
    if (out)
        return out;

    for (d = 4; d < 8; d++) {
        int bit_x = k_facade_dirs[d][0] > 0 ? 0 : 1;
        int bit_y = k_facade_dirs[d][1] > 0 ? 2 : 3;

        if ((exposed & (1 << bit_x)) && (exposed & (1 << bit_y)))
            out |= 1 << d;
    }
    return out;
}

static void free_facades(building_facade_t *facades, int nfacades)
{
    int i;

    if (!facades)
        return;
    for (i = 0; i < nfacades; i++) {
        free(facades[i].cells);
        free(facades[i].ranks);
    }
    free(facades);
}

/* Order a run along its wall and number the steps it spans. */
static bool facade_finish(building_facade_t *f)
{
    facade_sort_t *sorted;
    int i, rank = 0;

    if (f->ncells <= 0)
        return true;

    sorted = malloc((size_t)f->ncells * sizeof(*sorted));
    f->ranks = malloc((size_t)f->ncells * sizeof(*f->ranks));
    if (!sorted || !f->ranks) {
        free(sorted);
        return false;
    }
    for (i = 0; i < f->ncells; i++) {
        sorted[i].cell = f->cells[i];
        sorted[i].t = facade_tangent(f->dir, f->cells[i].x, f->cells[i].y);
    }
    qsort(sorted, (size_t)f->ncells, sizeof(*sorted), compare_facade_sort);
    for (i = 0; i < f->ncells; i++) {
        if (i > 0 && sorted[i].t != sorted[i - 1].t)
            rank++;
        f->cells[i] = sorted[i].cell;
        f->ranks[i] = rank;
    }
    f->nranks = rank + 1;
    free(sorted);
    return true;
}

/*
 * A wall facing along the building's length caps one of its ends, so its own
 * width is the building's short side.  Windows skip those.
 */
static bool facade_is_end_wall(const building_facade_t *f,
                               const building_axis_t *axis)
{
    float n = sqrtf((float)(f->dx * f->dx + f->dy * f->dy));
    float ox = (float)f->dx / n;
    float oy = (float)f->dy / n;
    float along = fabsf(ox * axis->dx + oy * axis->dy);
    float across = fabsf(ox * -axis->dy + oy * axis->dx);

    return along > across;
}

/*
 * Collect exterior wall runs.  Cells sharing an outward face are split into
 * diagonally connected components, so separate stretches of the same wall (an
 * L-shape, a courtyard) stay apart.
 */
static building_facade_t *build_facades(const building_group_t *g,
                                        const int *chunks, int wall_t,
                                        const building_axis_t *axis,
                                        int *out_nfacades)
{
    building_facade_t *facades = NULL;
    int nfacades = 0, cap = 0;
    int *dirs = NULL;
    uint8_t *visited = NULL;
    building_xy_t *queue = NULL;
    int area, d, i;

    *out_nfacades = 0;
    if (g->ncells <= 0)
        return NULL;

    area = (g->xmax - g->xmin + 1) * (g->ymax - g->ymin + 1);
    dirs = calloc((size_t)area, sizeof(*dirs));
    visited = malloc((size_t)area);
    queue = malloc((size_t)g->ncells * sizeof(*queue));
    if (!dirs || !visited || !queue)
        goto fail;

    for (i = 0; i < g->ncells; i++) {
        int x = g->cells[i].x;
        int y = g->cells[i].y;

        /* Interior partitions stay solid where they meet the façade. */
        if (is_partition_wall_cell(g, chunks, x, y, wall_t))
            continue;
        dirs[occ_index(g, x, y)] = facade_dirs_for_cell(g, x, y);
    }

    for (d = 0; d < 8; d++) {
        int mask = 1 << d;

        memset(visited, 0, (size_t)area);
        for (i = 0; i < g->ncells; i++) {
            int seed_x = g->cells[i].x;
            int seed_y = g->cells[i].y;
            int seed_i = occ_index(g, seed_x, seed_y);
            int qhead = 0, qtail = 0;
            building_facade_t *f;

            if (!(dirs[seed_i] & mask) || visited[seed_i])
                continue;

            if (nfacades >= cap) {
                int ncap = cap ? cap * 2 : 8;
                building_facade_t *next =
                    realloc(facades, (size_t)ncap * sizeof(*next));
                if (!next)
                    goto fail;
                facades = next;
                cap = ncap;
            }
            f = &facades[nfacades++];
            memset(f, 0, sizeof(*f));
            f->dir = d;
            f->dx = k_facade_dirs[d][0];
            f->dy = k_facade_dirs[d][1];
            f->is_end = facade_is_end_wall(f, axis);
            f->cells = malloc((size_t)g->ncells * sizeof(*f->cells));
            if (!f->cells)
                goto fail;

            visited[seed_i] = 1;
            queue[qtail++] = (building_xy_t){seed_x, seed_y};
            while (qhead < qtail) {
                building_xy_t p = queue[qhead++];
                int ndx, ndy;

                f->cells[f->ncells++] = p;
                for (ndy = -1; ndy <= 1; ndy++) {
                    for (ndx = -1; ndx <= 1; ndx++) {
                        int nx = p.x + ndx;
                        int ny = p.y + ndy;
                        int ni;

                        if (!ndx && !ndy)
                            continue;
                        if (!occ_get(g, nx, ny))
                            continue;
                        ni = occ_index(g, nx, ny);
                        if (!(dirs[ni] & mask) || visited[ni])
                            continue;
                        visited[ni] = 1;
                        queue[qtail++] = (building_xy_t){nx, ny};
                    }
                }
            }
            if (!facade_finish(f))
                goto fail;
        }
    }

    free(dirs);
    free(visited);
    free(queue);
    *out_nfacades = nfacades;
    return facades;

fail:
    free(dirs);
    free(visited);
    free(queue);
    free_facades(facades, nfacades);
    return NULL;
}

/*
 * Windows of width `win_w` spaced at least `min_gap` apart and centred in a
 * run of `len` cells.  The same gap is held back at both ends, so a window
 * never lands against a corner and a wall too short for one stays solid.
 */
static int window_layout(int len, int win_w, int min_gap, int *out_start)
{
    int margin = min_gap > 0 ? min_gap : 1;
    int avail = len - 2 * margin;
    int n, span;

    *out_start = 0;
    if (win_w <= 0 || avail < win_w)
        return 0;
    n = (avail + min_gap) / (win_w + min_gap);
    if (n < 1)
        return 0;
    span = n * win_w + (n - 1) * min_gap;
    *out_start = margin + (avail - span) / 2;
    return n;
}

/* Mark which positions along a run fall inside a window opening. */
static void mark_window_ranks(uint8_t *marks, int nranks, int win_w,
                              int min_gap, int max_building_size)
{
    int nsections, s;

    nsections = max_building_size > 0 ? nranks / max_building_size : 0;
    if (nsections < 1)
        nsections = 1;

    for (s = 0; s < nsections; s++) {
        int lo = s * nranks / nsections;
        int hi = (s + 1) * nranks / nsections;
        int start, n, k, j;

        n = window_layout(hi - lo, win_w, min_gap, &start);
        for (k = 0; k < n; k++) {
            int base = lo + start + k * (win_w + min_gap);

            for (j = 0; j < win_w; j++) {
                if (base + j < hi)
                    marks[base + j] = 1;
            }
        }
    }
}

/* Clear one opening through the wall band, inward from the exterior cell. */
static void clear_window_column(volume_t *vol, const building_group_t *g,
                                const int *chunks, int x, int y,
                                int dx, int dy, int z0, int win_h, int wall_t)
{
    int k, t;

    for (k = 0; k < wall_t; k++) {
        int cx = x - k * dx;
        int cy = y - k * dy;

        if (!occ_get(g, cx, cy))
            break;
        if (k > 0 && is_partition_wall_cell(g, chunks, cx, cy, wall_t))
            break;
        for (t = 0; t < win_h; t++)
            clear_voxel(vol, cx, cy, z0 + t);
    }
}

static void darken_window_edge(volume_t *vol, const building_group_t *g,
                               const int *chunks, int x, int y,
                               int dx, int dy, int z, int wall_t,
                               const uint8_t wall_color[4])
{
    int k;

    if (!vol)
        return;
    for (k = 0; k < wall_t; k++) {
        int cx = x - k * dx;
        int cy = y - k * dy;

        if (!occ_get(g, cx, cy))
            break;
        if (k > 0 && is_partition_wall_cell(g, chunks, cx, cy, wall_t))
            break;
        paint_dark_voxel(vol, cx, cy, z, wall_color);
    }
}

static void cut_windows_floor(volume_t *vol, const building_group_t *g,
                              const building_facade_t *facades, int nfacades,
                              const int *chunks, int z0, int floor_h,
                              int wall_t, int win_w, int win_h, int min_gap,
                              int above, int max_building_size,
                              volume_t *next_floor_vol,
                              const uint8_t wall_color[4])
{
    int f, i;
    int win_z0, win_h_use;

    if (win_w <= 0 || win_h <= 0 || above >= floor_h)
        return;
    win_h_use = win_h;
    if (above + win_h_use > floor_h)
        win_h_use = floor_h - above;
    if (win_h_use <= 0)
        return;
    win_z0 = z0 + above;

    for (f = 0; f < nfacades; f++) {
        const building_facade_t *fac = &facades[f];
        uint8_t *marks;

        if (fac->nranks <= 0 || fac->is_end)
            continue;
        marks = calloc((size_t)fac->nranks, 1);
        if (!marks)
            return;
        mark_window_ranks(marks, fac->nranks, win_w, min_gap,
                          max_building_size);
        for (i = 0; i < fac->ncells; i++) {
            if (!marks[fac->ranks[i]])
                continue;
            clear_window_column(vol, g, chunks, fac->cells[i].x,
                                fac->cells[i].y, fac->dx, fac->dy,
                                win_z0, win_h_use, wall_t);
            darken_window_edge(vol, g, chunks, fac->cells[i].x,
                               fac->cells[i].y, fac->dx, fac->dy,
                               win_z0 - 1, wall_t, wall_color);
            if (win_z0 + win_h_use == z0 + floor_h) {
                darken_window_edge(next_floor_vol, g, chunks,
                                   fac->cells[i].x, fac->cells[i].y,
                                   fac->dx, fac->dy, z0 + floor_h, wall_t,
                                   wall_color);
            }
        }
        free(marks);
    }
}

static void paint_gabled_roof(volume_t *vol, const building_obb_t *obb, int z0,
                              const uint8_t colors[2][4],
                              const uint8_t wall_color[4], int max_h)
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
                    paint_voxel(vol, x, y, z0 + level, wall_color);
                else
                    paint_noisy_color_voxel(vol, x, y, z0 + level, color);
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
                paint_noisy_color_voxel(vol, x, y, z0 + level,
                                        colors[level & 1]);
                painted++;
            }
        }
        if (!painted)
            break;
    }
}

static void generate_group(volume_t *floor_volumes[], volume_t *roof_volume,
                           const building_group_t *g,
                           int floor_h, int floor_t, int wall_t,
                           int max_building_size, bool generate_roofs,
                           int max_roof_h, const uint8_t roof_colors[2][4],
                           const uint8_t wall_color[4],
                           bool generate_windows, int win_w, int win_h,
                           int win_gap, int win_above)
{
    int i;
    int z;
    int nchunks;
    int *chunks;
    building_facade_t *facades = NULL;
    int nfacades = 0;
    building_axis_t axis;

    if (g->ncells <= 0)
        return;

    axis = building_long_axis(g);
    /* A count of 0 or 1 section leaves the interior open. */
    nchunks = max_building_size > 0 ?
              (int)(axis.len / (float)max_building_size) : 0;
    chunks = nchunks > 1 ? build_chunk_map(g, nchunks, &axis) : NULL;

    /* Façades depend only on the footprint, so every storey shares them. */
    if (generate_windows)
        facades = build_facades(g, chunks, wall_t, &axis, &nfacades);

    /*
     * Each floor layer owns the slab beneath that storey and its walls.  The
     * final top slab belongs with the roof, so hiding the roofs layer opens
     * the top storey as well.
     */
    z = g->base_z;
    for (i = 0; i < g->floors; i++) {
        clear_building_column_range(floor_volumes[i], g, z,
                                    floor_t + floor_h);
        z += floor_t + floor_h;
    }
    clear_building_column_range(roof_volume, g, z, floor_t);

    for (i = 0; i < g->floors; i++) {
        z = g->base_z + i * (floor_t + floor_h);
        paint_slab(floor_volumes[i], g, z, floor_t, wall_color);
        z += floor_t;
        paint_walls(floor_volumes[i], g, z, floor_h, wall_t, chunks,
                    wall_color);
    }

    z = g->base_z + g->floors * (floor_t + floor_h);
    paint_slab(roof_volume, g, z, floor_t, wall_color);
    z += floor_t;

    /* Cut windows after all slabs exist, so floor-level lintels stay dark. */
    if (facades) {
        for (i = 0; i < g->floors; i++) {
            volume_t *next_floor_vol =
                i + 1 < g->floors ? floor_volumes[i + 1] : NULL;
            int wall_z = g->base_z + i * (floor_t + floor_h) + floor_t;

            cut_windows_floor(floor_volumes[i], g, facades, nfacades, chunks,
                              wall_z, floor_h, wall_t, win_w, win_h, win_gap,
                              win_above, max_building_size, next_floor_vol,
                              wall_color);
        }
    }

    /* Roofs are generated above the final slab on the roofs layer. */
    if (generate_roofs) {
        building_obb_t obb;

        if (analyze_rectangle(g, &obb))
            paint_gabled_roof(roof_volume, &obb, z, roof_colors, wall_color,
                              max_roof_h);
        else
            paint_pyramid_roof(roof_volume, g, z, roof_colors, max_roof_h);
    }
    free_facades(facades, nfacades);
    free(chunks);
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

static unsigned building_hash(const building_group_t *g, int seed)
{
    unsigned h = 2166136261u;
    const int values[] = {
        g->xmin, g->ymin, g->xmax, g->ymax, g->base_z, g->floors, g->ncells,
        seed,
    };
    int i;

    for (i = 0; i < (int)ARRAY_SIZE(values); i++) {
        h ^= (unsigned)values[i];
        h *= 16777619u;
    }
    return h;
}

static void name_target_layer(layer_t *target, const layer_t *plan_layer,
                              const char *suffix)
{
    int max_base;

    max_base = (int)sizeof(target->name) - 1 - (int)strlen(suffix);
    if (max_base < 0)
        max_base = 0;
    snprintf(target->name, sizeof(target->name), "%.*s%s",
             max_base, plan_layer->name, suffix);
}

static bool prepare_target_layers(layer_t *plan_layer, bool all_one_layer,
                                  int max_floors, layer_t *targets[])
{
    int i, count = all_one_layer ? 1 : max_floors + 1;

    for (i = 0; i < count; i++) {
        char suffix[40];

        targets[i] = image_add_layer(goxel.image, NULL);
        if (!targets[i])
            break;
        targets[i]->visible = true;
        if (all_one_layer) {
            snprintf(suffix, sizeof(suffix), " Buildings");
        } else if (i < max_floors) {
            snprintf(suffix, sizeof(suffix), " Buildings - floor %d", i + 1);
        } else {
            snprintf(suffix, sizeof(suffix), " Buildings - roofs");
        }
        name_target_layer(targets[i], plan_layer, suffix);
    }
    if (i != count) {
        while (i-- > 0)
            image_delete_layer(goxel.image, targets[i]);
        goxel.image->active_layer = plan_layer;
        return false;
    }
    plan_layer->visible = false;
    return true;
}

static void apply_buildings(filter_buildings_t *filter, layer_t *layer)
{
    volume_t *src = NULL;
    layer_t *target_layers[BUILDING_MAX_FLOOR_COLORS + 1] = {0};
    volume_t *floor_volumes[BUILDING_MAX_FLOOR_COLORS];
    volume_t *roof_volume;
    building_group_t *groups = NULL;
    int ngroups = 0;
    int i, max_floors = 0;
    int floor_h, floor_t, wall_t, max_building_size, max_roof_h;
    int win_w, win_h, win_gap, win_above;

    if (!layer || !layer->volume || volume_is_empty(layer->volume)) {
        gui_alert("Plan - Buildings", "Active layer has no voxels.");
        return;
    }

    floor_h = clampi(filter->floor_height, 1, 256);
    floor_t = clampi(filter->floor_thickness, 1, 64);
    wall_t = clampi(filter->wall_thickness, 1, 64);
    max_building_size = clampi(filter->max_building_size, 0, 1024);
    max_roof_h = clampi(filter->max_roof_height, 1, 256);
    win_w = clampi(filter->window_width, 1, 64);
    win_h = clampi(filter->window_height, 1, 64);
    win_gap = clampi(filter->window_min_gap, 0, 64);
    win_above = clampi(filter->window_above_floor, 0, 256);
    filter->floor_color_count =
        clampi(filter->floor_color_count, 1, BUILDING_MAX_FLOOR_COLORS);
    filter->wall_color_count =
        clampi(filter->wall_color_count, 1, BUILDING_MAX_WALL_COLORS);
    /* Both counts are modulo divisors below, so zero would trap. */
    filter->roof_pair_count =
        clampi(filter->roof_pair_count, 1, BUILDING_MAX_ROOF_PAIRS);
    filter->seed = clampi(filter->seed, 0, RAND_MAX);
    filter->floor_height = floor_h;
    filter->floor_thickness = floor_t;
    filter->wall_thickness = wall_t;
    filter->max_building_size = max_building_size;
    filter->max_roof_height = max_roof_h;
    filter->window_width = win_w;
    filter->window_height = win_h;
    filter->window_min_gap = win_gap;
    filter->window_above_floor = win_above;

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

    for (i = 0; i < ngroups; i++) {
        if (groups[i].floors > max_floors)
            max_floors = groups[i].floors;
    }

    image_history_push(goxel.image);
    if (!prepare_target_layers(layer, filter->all_one_layer, max_floors,
                               target_layers)) {
        gui_alert("Plan - Buildings", "Could not create the buildings layers.");
        free_groups(groups, ngroups);
        volume_delete(src);
        return;
    }

    if (filter->all_one_layer) {
        for (i = 0; i < max_floors; i++)
            floor_volumes[i] = target_layers[0]->volume;
        roof_volume = target_layers[0]->volume;
    } else {
        for (i = 0; i < max_floors; i++)
            floor_volumes[i] = target_layers[i]->volume;
        roof_volume = target_layers[max_floors]->volume;
    }

    for (i = 0; i < ngroups; i++) {
        unsigned hash = building_hash(&groups[i], filter->seed);
        int roof_pair = (int)(hash %
                              (unsigned)filter->roof_pair_count);
        int wall_color = (int)((hash ^ (hash >> 16)) %
                               (unsigned)filter->wall_color_count);
        generate_group(floor_volumes, roof_volume, &groups[i],
                       floor_h, floor_t, wall_t, max_building_size,
                       filter->generate_roofs, max_roof_h,
                       filter->roof_colors[roof_pair],
                       filter->wall_colors[wall_color],
                       filter->generate_windows, win_w, win_h, win_gap,
                       win_above);
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
    char wall_color_id[32];
    char roof_color_id[32];
    char remove_id[32];
    char label[16];

    const char *help_text =
        "Paint 1-block-high filled shapes on the active layer using the floor "
        "colours below.  Generate builds walls along the inward edges, slabs "
        "at the base and between storeys, and shaped roofs on new Buildings "
        "layers.  The plan layer is preserved and hidden.";
    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    if (gui_section_begin("Floor marker colours", GUI_SECTION_COLLAPSABLE)) {
        for (i = 0; i < filter->floor_color_count; i++) {
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
            gui_tooltip_if_hovered(
                "Switch the current brush colour to this colour.");
            gui_same_line();
            if (gui_button(set_id, 0, 0))
                memcpy(filter->floor_colors[i], goxel.painter.color, 4);
            gui_tooltip_if_hovered(
                "Set this floor marker to the current brush colour.");
        }
        if (gui_button("Add##floor_marker", -1, 0) &&
            filter->floor_color_count < BUILDING_MAX_FLOOR_COLORS) {
            int n = filter->floor_color_count;

            hsv_to_rgb_u8((float)((n * 45) % 360) / 360.f,
                          150.f / 220.f, 220.f / 255.f,
                          filter->floor_colors[n]);
            filter->floor_colors[n][3] = 255;
            filter->floor_color_count++;
        }
        gui_tooltip_if_hovered(
            "Adds the next floor marker at index × 45 degrees hue.");
    }
    gui_section_end();

    gui_label_size_push(170.0f);
    if (gui_section_begin("Generation", GUI_SECTION_COLLAPSABLE)) {
        gui_checkbox("Generate on one layer", &filter->all_one_layer,
                     "When off, each used floor gets a layer and final slabs "
                     "and roofs share a roofs layer.");
        gui_checkbox("Generate windows", &filter->generate_windows,
                     "Punch window openings into exterior walls on every "
                     "storey.");
        gui_checkbox("Generate roofs", &filter->generate_roofs,
                     "When off, buildings stop at the top floor slab.");
        if (filter->generate_windows) {
            gui_input_int("Window width", &filter->window_width, 1, 64);
            gui_input_int("Window height", &filter->window_height, 1, 64);
            gui_input_int("Min gap between windows", &filter->window_min_gap,
                          0, 64);
            gui_input_int("Distance above floor", &filter->window_above_floor,
                          0, 256);
        }
    }
    gui_section_end();

    if (gui_section_begin("Building settings",
                          GUI_SECTION_COLLAPSABLE_CLOSED)) {
        gui_input_int("Height of each floor", &filter->floor_height, 1, 256);
        gui_tooltip_if_hovered(
            "Clear height between floor slabs. Default 5.");
        gui_input_int("Floor thickness", &filter->floor_thickness, 1, 64);
        gui_tooltip_if_hovered("Thickness of each floor slab. Default 1.");
        gui_input_int("Wall thickness", &filter->wall_thickness, 1, 64);
        gui_tooltip_if_hovered(
            "How many blocks inward from the edge become walls. Default 1.");
        gui_input_int("Maximum building size", &filter->max_building_size,
                      0, 1024);
        gui_tooltip_if_hovered(
            "Longest-side length per interior section. 0 disables splitting. "
            "Default 16.");
    }
    gui_section_end();
    gui_label_size_pop();

    if (gui_section_begin("Walls", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        for (i = 0; i < filter->wall_color_count; i++) {
            snprintf(wall_color_id, sizeof(wall_color_id), "##wall_%d", i);
            gui_color_small_no_label(wall_color_id, filter->wall_colors[i]);
            gui_same_line();
            snprintf(remove_id, sizeof(remove_id), "X##wall_%d", i);
            if (gui_button(remove_id, 0, 0) &&
                filter->wall_color_count > 1) {
                memmove(&filter->wall_colors[i], &filter->wall_colors[i + 1],
                        (size_t)(filter->wall_color_count - i - 1) *
                            sizeof(filter->wall_colors[0]));
                filter->wall_color_count--;
                break;
            }
            if (filter->wall_color_count == 1)
                gui_tooltip_if_hovered(
                    "At least one wall colour is required.");
        }
        if (gui_button("Add##wall_colour", -1, 0) &&
            filter->wall_color_count < BUILDING_MAX_WALL_COLORS) {
            memcpy(filter->wall_colors[filter->wall_color_count],
                   goxel.painter.color, 4);
            filter->wall_colors[filter->wall_color_count][3] = 255;
            filter->wall_color_count++;
        }
        gui_tooltip_if_hovered("Adds the current brush colour.");
    }
    gui_section_end();

    if (filter->generate_roofs) {
        if (gui_section_begin("Roofs", GUI_SECTION_COLLAPSABLE_CLOSED)) {
            gui_input_int("Max roof height", &filter->max_roof_height, 1, 256);
            gui_tooltip_if_hovered(
                "Roof slope stops at this height and finishes flat. Default 8.");
            gui_text_wrapped(
                "Each building picks one pair. Roof layers alternate between "
                "its two colours.");
            for (i = 0; i < filter->roof_pair_count; i++) {
                snprintf(roof_color_id, sizeof(roof_color_id),
                         "##roof_%d_a", i);
                gui_color_small_no_label(roof_color_id,
                                         filter->roof_colors[i][0]);
                gui_same_line();
                snprintf(roof_color_id, sizeof(roof_color_id),
                         "##roof_%d_b", i);
                gui_color_small_no_label(roof_color_id,
                                         filter->roof_colors[i][1]);
                gui_same_line();
                snprintf(remove_id, sizeof(remove_id), "X##roof_%d", i);
                if (gui_button(remove_id, 0, 0) &&
                    filter->roof_pair_count > 1) {
                    memmove(&filter->roof_colors[i],
                            &filter->roof_colors[i + 1],
                            (size_t)(filter->roof_pair_count - i - 1) *
                                sizeof(filter->roof_colors[0]));
                    filter->roof_pair_count--;
                    break;
                }
                if (filter->roof_pair_count == 1)
                    gui_tooltip_if_hovered(
                        "At least one roof colour pair is required.");
            }
            if (gui_button("Add##roof_colour_pair", -1, 0) &&
                filter->roof_pair_count < BUILDING_MAX_ROOF_PAIRS) {
                int n = filter->roof_pair_count;
                memcpy(filter->roof_colors[n], filter->roof_colors[n - 1],
                       sizeof(filter->roof_colors[n]));
                filter->roof_pair_count++;
            }
            gui_tooltip_if_hovered(
                "Adds a copy of the last pair for editing.");
        }
        gui_section_end();
    }

    gui_separator();
    if (gui_button("Reset to defaults", -1, 0))
        reset_defaults(filter);

    gui_input_int("Seed", &filter->seed, 0, RAND_MAX);
    gui_tooltip_if_hovered(
        "Changes which wall and roof colours each building gets. "
        "Same seed + plan reproduces the same picks.");
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        filter->seed = rand();
    }

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
                .name = "Buildings",
                .menu = "effects",
                .submenu = "plan",
                .on_open = on_open,
                .panel_width = 320,
                .gui_fn = gui, )
