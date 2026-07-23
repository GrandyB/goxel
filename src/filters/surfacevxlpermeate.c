/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
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

#include <limits.h>

/*
 * Permeate surface colours into nearby non-exposed solids through solid
 * voxels only (Manhattan / 6-connected). Only nearest surface(s) contribute
 * (averaged on distance ties); blur band lerps toward the original colour.
 */
typedef struct {
    filter_t filter;
    int depth;
    int blur;
} filter_surfacevxlpermeate_t;

typedef struct {
    int idx;
    int dist;
} bfs_node_t;

static bool voxel_is_solid(const uint8_t color[4])
{
    return color[3] != 0;
}

static bool pos_in_aabb(const int pos[3], const int start_pos[3],
                        const int dimensions[3])
{
    return pos[0] >= start_pos[0] && pos[0] < start_pos[0] + dimensions[0] &&
           pos[1] >= start_pos[1] && pos[1] < start_pos[1] + dimensions[1] &&
           pos[2] >= start_pos[2] && pos[2] < start_pos[2] + dimensions[2];
}

static int aabb_index(const int pos[3], const int start_pos[3],
                      const int dimensions[3])
{
    int x = pos[0] - start_pos[0];
    int y = pos[1] - start_pos[1];
    int z = pos[2] - start_pos[2];
    return (x * dimensions[1] + y) * dimensions[2] + z;
}

/* Exposed only to air inside the image box — box faces do not count. */
static bool voxel_is_surface(const volume_t *volume, volume_iterator_t *iter,
                             const int pos[3], const int start_pos[3],
                             const int dimensions[3])
{
    static const int offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1},
    };
    uint8_t color[4];
    int npos[3];
    int i;

    volume_get_at(volume, iter, pos, color);
    if (!voxel_is_solid(color))
        return false;

    for (i = 0; i < 6; i++) {
        npos[0] = pos[0] + offsets[i][0];
        npos[1] = pos[1] + offsets[i][1];
        npos[2] = pos[2] + offsets[i][2];
        if (!pos_in_aabb(npos, start_pos, dimensions))
            continue;
        volume_get_at(volume, iter, npos, color);
        if (!voxel_is_solid(color))
            return true;
    }
    return false;
}

/* BFS through solids; caller seeds the queue. Skips already-visited. */
static void bfs_solid(bfs_node_t *queue, int *q_head, int *q_tail,
                      int queue_cap, int radius, const bool *solid,
                      const int dimensions[3], const int start_pos[3],
                      uint8_t *visited, uint8_t visit_token,
                      void *user,
                      void (*on_visit)(int idx, int dist, void *user))
{
    static const int offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1},
    };
    int idx, dist, dx, dy, dz, x, y, z, i, npos[3], nidx, ndist;

    while (*q_head < *q_tail) {
        idx = queue[*q_head].idx;
        dist = queue[*q_head].dist;
        (*q_head)++;

        if (on_visit)
            on_visit(idx, dist, user);

        if (dist >= radius)
            continue;

        dz = idx % dimensions[2];
        dy = (idx / dimensions[2]) % dimensions[1];
        dx = idx / (dimensions[2] * dimensions[1]);
        x = dx + start_pos[0];
        y = dy + start_pos[1];
        z = dz + start_pos[2];

        for (i = 0; i < 6; i++) {
            npos[0] = x + offsets[i][0];
            npos[1] = y + offsets[i][1];
            npos[2] = z + offsets[i][2];
            if (!pos_in_aabb(npos, start_pos, dimensions))
                continue;
            nidx = aabb_index(npos, start_pos, dimensions);
            if (!solid[nidx] || visited[nidx] == visit_token)
                continue;
            ndist = dist + 1;
            if (ndist > radius)
                continue;
            if (*q_tail >= queue_cap)
                continue;
            visited[nidx] = visit_token;
            queue[*q_tail].idx = nidx;
            queue[*q_tail].dist = ndist;
            (*q_tail)++;
        }
    }
}

typedef struct {
    int *min_dist;
} min_dist_ctx_t;

static void on_visit_min_dist(int idx, int dist, void *user)
{
    min_dist_ctx_t *ctx = user;
    if (dist < ctx->min_dist[idx])
        ctx->min_dist[idx] = dist;
}

typedef struct {
    const bool *surface;
    const int *min_dist;
    const uint8_t *surf_color;
    float *sum_r;
    float *sum_g;
    float *sum_b;
    int *sum_n;
} accumulate_ctx_t;

static void on_visit_accumulate(int idx, int dist, void *user)
{
    accumulate_ctx_t *ctx = user;

    if (dist <= 0 || ctx->surface[idx])
        return;
    if (dist != ctx->min_dist[idx])
        return;
    ctx->sum_r[idx] += (float)ctx->surf_color[0];
    ctx->sum_g[idx] += (float)ctx->surf_color[1];
    ctx->sum_b[idx] += (float)ctx->surf_color[2];
    ctx->sum_n[idx]++;
}

static void apply_surfacevxlpermeate(volume_t *volume, int depth, int blur)
{
    float box[4][4];
    int dimensions[3], start_pos[3];
    int x, y, z, pos[3], idx, size, radius;
    volume_iterator_t iter;
    bool *solid = NULL;
    bool *surface = NULL;
    uint8_t (*colors)[4] = NULL;
    float *sum_r = NULL;
    float *sum_g = NULL;
    float *sum_b = NULL;
    int *sum_n = NULL;
    int *min_dist = NULL;
    uint8_t *visited = NULL;
    bfs_node_t *queue = NULL;
    uint8_t visit_token = 0;
    uint8_t out[4];
    float surf_r, surf_g, surf_b, t;
    int i, dx, dy, dz, q_head, q_tail;
    min_dist_ctx_t min_ctx;
    accumulate_ctx_t acc_ctx;

    radius = depth + blur;
    if (radius <= 0)
        return;

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] <= 0)
        return;

    size = dimensions[0] * dimensions[1] * dimensions[2];
    solid = calloc(size, sizeof(*solid));
    surface = calloc(size, sizeof(*surface));
    colors = malloc(size * sizeof(*colors));
    sum_r = calloc(size, sizeof(*sum_r));
    sum_g = calloc(size, sizeof(*sum_g));
    sum_b = calloc(size, sizeof(*sum_b));
    sum_n = calloc(size, sizeof(*sum_n));
    min_dist = malloc(size * sizeof(*min_dist));
    visited = calloc(size, sizeof(*visited));
    queue = malloc(size * sizeof(*queue));
    if (!solid || !surface || !colors || !sum_r || !sum_g || !sum_b ||
        !sum_n || !min_dist || !visited || !queue)
        goto cleanup;

    for (i = 0; i < size; i++)
        min_dist[i] = INT_MAX;

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

    /* Pass 1: classify solids / surfaces and cache colours. */
    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            for (z = 0; z < dimensions[2]; z++) {
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                idx = (x * dimensions[1] + y) * dimensions[2] + z;
                volume_get_at(volume, &iter, pos, colors[idx]);
                solid[idx] = voxel_is_solid(colors[idx]);
                surface[idx] = solid[idx] &&
                    voxel_is_surface(volume, &iter, pos, start_pos, dimensions);
            }
        }
    }

    /* Pass 2a: multi-source BFS — Manhattan distance to nearest surface. */
    q_head = 0;
    q_tail = 0;
    visit_token = 1;
    for (idx = 0; idx < size; idx++) {
        if (!surface[idx])
            continue;
        visited[idx] = visit_token;
        min_dist[idx] = 0;
        queue[q_tail].idx = idx;
        queue[q_tail].dist = 0;
        q_tail++;
    }
    min_ctx.min_dist = min_dist;
    bfs_solid(queue, &q_head, &q_tail, size, radius, solid, dimensions,
              start_pos, visited, visit_token, &min_ctx, on_visit_min_dist);

    /* Pass 2b: per-surface BFS — average colours of nearest surfaces only. */
    acc_ctx.surface = surface;
    acc_ctx.min_dist = min_dist;
    acc_ctx.sum_r = sum_r;
    acc_ctx.sum_g = sum_g;
    acc_ctx.sum_b = sum_b;
    acc_ctx.sum_n = sum_n;
    for (idx = 0; idx < size; idx++) {
        if (!surface[idx])
            continue;
        visit_token++;
        if (visit_token == 0) {
            memset(visited, 0, (size_t)size);
            visit_token = 1;
        }
        q_head = 0;
        q_tail = 0;
        visited[idx] = visit_token;
        queue[q_tail].idx = idx;
        queue[q_tail].dist = 0;
        q_tail++;
        acc_ctx.surf_color = colors[idx];
        bfs_solid(queue, &q_head, &q_tail, size, radius, solid, dimensions,
                  start_pos, visited, visit_token, &acc_ctx, on_visit_accumulate);
    }

    /* Pass 3: write RGB for non-surface solids in range. */
    for (idx = 0; idx < size; idx++) {
        if (!solid[idx] || surface[idx])
            continue;
        if (min_dist[idx] > radius || sum_n[idx] <= 0)
            continue;

        surf_r = sum_r[idx] / (float)sum_n[idx];
        surf_g = sum_g[idx] / (float)sum_n[idx];
        surf_b = sum_b[idx] / (float)sum_n[idx];

        if (min_dist[idx] <= depth || blur <= 0) {
            out[0] = (uint8_t)(surf_r + 0.5f);
            out[1] = (uint8_t)(surf_g + 0.5f);
            out[2] = (uint8_t)(surf_b + 0.5f);
        } else {
            t = (float)(min_dist[idx] - depth) / (float)blur;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            out[0] = (uint8_t)(surf_r * (1.0f - t) +
                               (float)colors[idx][0] * t + 0.5f);
            out[1] = (uint8_t)(surf_g * (1.0f - t) +
                               (float)colors[idx][1] * t + 0.5f);
            out[2] = (uint8_t)(surf_b * (1.0f - t) +
                               (float)colors[idx][2] * t + 0.5f);
        }
        out[3] = colors[idx][3];

        dz = idx % dimensions[2];
        dy = (idx / dimensions[2]) % dimensions[1];
        dx = idx / (dimensions[2] * dimensions[1]);
        pos[0] = dx + start_pos[0];
        pos[1] = dy + start_pos[1];
        pos[2] = dz + start_pos[2];
        volume_set_at(volume, &iter, pos, out);
    }

cleanup:
    free(solid);
    free(surface);
    free(colors);
    free(sum_r);
    free(sum_g);
    free(sum_b);
    free(sum_n);
    free(min_dist);
    free(visited);
    free(queue);
}

static int gui(filter_t *filter_)
{
    filter_surfacevxlpermeate_t *filter = (void *)filter_;
    const char *help_text =
        "Permeate surface colours X blocks into non-exposed blocks around them";

    goxel_set_help_text(help_text);
    gui_text_wrapped(help_text);

    gui_input_int("Depth", &filter->depth, 0, 9999);
    gui_input_int("Blur", &filter->blur, 0, 9999);

    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        apply_surfacevxlpermeate(goxel.image->active_layer->volume,
                                 filter->depth, filter->blur);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_surfacevxlpermeate_t *filter = (void *)filter_;
    filter->depth = 2;
    filter->blur = 0;
}

FILTER_REGISTER(surfacevxlpermeate, filter_surfacevxlpermeate_t,
                .name = "Utility - .vxl color permeation",
                .on_open = on_open,
                .gui_fn = gui, )
