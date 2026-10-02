/* Goxel 3D voxels editor
 *
 * copyright (c) 2016 Guillaume Chereau <guillaume@noctua-software.com>
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

typedef struct {
    uint8_t c[4];
    uint32_t n;
} value_t;

static UT_icd value_icd = {sizeof(value_t), NULL, NULL, NULL};

typedef struct {
    UT_array *values;
} bucket_t;

static void bucket_add(bucket_t *b, const uint8_t c[4], int n, bool check)
{
    assert(b->values);
    int size = utarray_len(b->values);
    value_t v, *values = (value_t*)utarray_front(b->values);
    int i;

    assert(n);
    if (check) {
        for (i = 0; i < size; i++) {
            if (memcmp(values[i].c, c, 4) == 0) {
                values[i].n += n;
                return;
            }
        }
    }
    memcpy(v.c, c, 4);
    v.n = n;
    utarray_push_back(b->values, &v);
}

static int g_k; // Used in the sorting algo.
                // qsort_r is not portable!
static int value_cmp(const void *a_, const void *b_)
{
    int k = g_k;
    const value_t *a = a_;
    const value_t *b = b_;
    return cmp(a->c[k], b->c[k]);
}

// Split a bucket into two new buckets.
static void bucket_split(const bucket_t *bucket, bucket_t *a, bucket_t *b)
{
    int size = utarray_len(bucket->values);
    value_t *values = (value_t*)utarray_front(bucket->values);
    int i, j, k, nb = 0;
    uint8_t min_c[4] = {255, 255, 255, 255};
    uint8_t max_c[4] = {0, 0, 0, 0};
    // Find the channel with the max range
    for (i = 0; i < size; i++) {
        nb += values[i].n;
        for (k = 0; k < 4; k++) {
            min_c[k] = min(min_c[k], values[i].c[k]);
            max_c[k] = max(max_c[k], values[i].c[k]);
        }
    }
    k = 0;
    for (i = 0; i < 4; i++)
        if (max_c[i] - min_c[i] > max_c[k] - min_c[k])
            k = i;
    // Sort the values by color.
    g_k = k;
    qsort(values, size, sizeof(*values), value_cmp);
    // Now take the bottom half into the first buket, and the top half into
    // the second one.
    utarray_new(a->values, &value_icd);
    utarray_new(b->values, &value_icd);
    for (i = 0, j = 0; i < size; i++) {
        if (j < nb / 2)
            bucket_add(a, values[i].c, min(values[i].n, nb / 2 - j), false);
        j += values[i].n;
        if (j > nb / 2)
            bucket_add(b, values[i].c, min(values[i].n, j - nb / 2), false);
    }
}

static void bucket_average_color(const bucket_t *b, uint8_t out[4])
{
    int size = utarray_len(b->values);
    value_t *values = (value_t*)utarray_front(b->values);
    int s[4] = {}, n = 0, i, k;
    if (size == 0) {
        memset(out, 0, 4);
        return;
    }
    for (i = 0; i < size; i++) {
        assert(values[i].n);
        n += values[i].n;
        for (k = 0; k < 4; k++)
            s[k] += values[i].n * values[i].c[k];
    }
    out[0] = s[0] / n;
    out[1] = s[1] / n;
    out[2] = s[2] / n;
    out[3] = s[3] / n;
}

static int bucket_cmp(const void *a_, const void *b_)
{
    const bucket_t *a = (void*)a_;
    const bucket_t *b = (void*)b_;
    int na = a->values ? utarray_len(a->values) : -1;
    int nb = b->values ? utarray_len(b->values) : -1;
    return cmp(nb, na);
}

static bool color_in_exclude(const uint8_t c[4],
                             const uint8_t (*exclude)[4], int n_exclude)
{
    int i;
    if (!exclude || n_exclude <= 0) return false;
    for (i = 0; i < n_exclude; i++) {
        if (exclude[i][3] != 255) continue;
        if (exclude[i][0] == c[0] && exclude[i][1] == c[1] &&
            exclude[i][2] == c[2])
            return true;
    }
    return false;
}

// Generate a palette of up to `nb` colors from a volume.
// This is based on https://en.wikipedia.org/wiki/Median_cut.
//
// If the volume has at most `nb` distinct opaque colours (after skipping
// `exclude`), they are copied exactly into the first slots and the rest are
// cleared.  Median-cut only runs when there are more unique colours than
// slots (otherwise it would duplicate colours to fill the palette).
//
// `exclude` / `n_exclude`: optional opaque RGBs already reserved elsewhere
// in the caller's palette; matching volume colours are omitted from the
// fill.  Pass NULL / 0 when unused.
void quantization_gen_palette(const volume_t *volume, int nb,
                              uint8_t (*palette)[4],
                              const uint8_t (*exclude)[4], int n_exclude)
{
    uint8_t v[4];
    int i, n, pos[3];
    bucket_t *buckets, b;
    volume_iterator_t iter;
    value_t *values;

    if (nb <= 0) return;

    buckets = calloc(nb, sizeof(*buckets));

    // Fill the initial bucket.
    utarray_new(buckets[0].values, &value_icd);
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!voxel_is_solid(v)) continue;
        v[3] = 255;
        if (color_in_exclude(v, exclude, n_exclude)) continue;
        bucket_add(&buckets[0], v, 1, true);
    }

    n = utarray_len(buckets[0].values);
    if (n <= nb) {
        values = (value_t *)utarray_front(buckets[0].values);
        for (i = 0; i < n; i++)
            memcpy(palette[i], values[i].c, 4);
        /* Clear unused slots so callers that pre-filled a default palette
         * (e.g. MagicaVoxel) do not leave leftover colours behind. */
        for (i = n; i < nb; i++)
            memset(palette[i], 0, 4);
        utarray_free(buckets[0].values);
        free(buckets);
        return;
    }

    // Split until we get nb buckets.  I do it a bit stupidly, by sorting
    // the buckets at every iterations!  I should use a stack!
    while (!buckets[nb - 1].values) {
        assert(!buckets[nb - 1].values);
        b = buckets[0];
        memset(&buckets[0], 0, sizeof(buckets[0]));
        bucket_split(&b, &buckets[0], &buckets[nb - 1]);
        utarray_free(b.values);
        qsort(buckets, nb, sizeof(*buckets), bucket_cmp);
    }

    // Fill the palette colors and cleanup.
    for (i = 0; i < nb; i++) {
        assert(buckets[i].values);
        bucket_average_color(&buckets[i], palette[i]);
        utarray_free(buckets[i].values);
    }
    free(buckets);
}

int quantization_nearest(const uint8_t c[4],
                         const uint8_t (*palette)[4], int n)
{
    const uint8_t *p;
    int i, dist, best = -1, best_dist = 1024;

    for (i = 0; i < n; i++) {
        p = palette[i];
        if (p[3] != 255) continue;
        dist = abs((int)p[0] - (int)c[0]) +
               abs((int)p[1] - (int)c[1]) +
               abs((int)p[2] - (int)c[2]);
        if (dist == 0) return i;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

void quantization_remap_volume(volume_t *volume,
                               const uint8_t (*palette)[4], int n)
{
    volume_iterator_t iter;
    int pos[3], idx;
    uint8_t v[4];

    if (!volume || !palette || n <= 0) return;

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!voxel_is_solid(v)) continue;
        idx = quantization_nearest(v, palette, n);
        if (idx < 0) continue;
        memcpy(v, palette[idx], 4);
        volume_set_at(volume, &iter, pos, v);
    }
}

static uint8_t quant_snap_channel(uint8_t c, int step)
{
    int v;

    if (step <= 1)
        return c;
    v = ((int)c + step / 2) / step * step;
    return (uint8_t)clamp(v, 0, 255);
}

void quantization_uniform_snap(const uint8_t in[4], int step, uint8_t out[4])
{
    step = clamp(step, 1, 255);
    out[0] = quant_snap_channel(in[0], step);
    out[1] = quant_snap_channel(in[1], step);
    out[2] = quant_snap_channel(in[2], step);
    out[3] = in[3];
}

void quantization_remap_volume_uniform(volume_t *volume, int step)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];

    if (!volume)
        return;
    step = clamp(step, 1, 255);
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!voxel_is_solid(v))
            continue;
        quantization_uniform_snap(v, step, v);
        volume_set_at(volume, &iter, pos, v);
    }
}

void quantization_pin_colors(uint8_t (*palette)[4], int n,
                             const uint8_t (*pinned)[4], int n_pinned)
{
    bool used[256];
    int i, j, best, dist, best_dist;
    const uint8_t *p;
    const uint8_t *c;

    if (!palette || !pinned || n <= 0 || n_pinned <= 0)
        return;
    n = min(n, 256);
    memset(used, 0, sizeof(used));

    for (i = 0; i < n_pinned; i++) {
        c = pinned[i];
        if (c[3] != 255)
            continue;

        best = -1;
        best_dist = 1 << 30;

        /* Prefer an unused empty slot so we do not displace a generated colour
         * when the palette was not full. */
        for (j = 0; j < n; j++) {
            if (used[j])
                continue;
            if (palette[j][3] != 255) {
                best = j;
                best_dist = -1;
                break;
            }
        }

        if (best < 0) {
            for (j = 0; j < n; j++) {
                if (used[j])
                    continue;
                p = palette[j];
                dist = abs((int)p[0] - (int)c[0]) +
                       abs((int)p[1] - (int)c[1]) +
                       abs((int)p[2] - (int)c[2]);
                if (dist < best_dist) {
                    best_dist = dist;
                    best = j;
                    if (dist == 0)
                        break;
                }
            }
        }

        if (best < 0)
            break;
        memcpy(palette[best], c, 4);
        used[best] = true;
    }
}

/* ---- Shared unique-colour collection for octree / Wu / k-means++ ---- */

static void collect_unique_opaque(const volume_t *volume,
                                  const uint8_t (*exclude)[4], int n_exclude,
                                  UT_array **out)
{
    volume_iterator_t iter;
    int pos[3];
    uint8_t v[4];
    bucket_t b;

    utarray_new(*out, &value_icd);
    if (!volume)
        return;
    memset(&b, 0, sizeof(b));
    b.values = *out;
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);
    while (volume_iter(&iter, pos)) {
        volume_get_at(volume, &iter, pos, v);
        if (!voxel_is_solid(v))
            continue;
        v[3] = 255;
        if (color_in_exclude(v, exclude, n_exclude))
            continue;
        bucket_add(&b, v, 1, true);
    }
}

static void palette_from_exact(const value_t *values, int n, int nb,
                               uint8_t (*palette)[4])
{
    int i;

    for (i = 0; i < n && i < nb; i++)
        memcpy(palette[i], values[i].c, 4);
    for (i = n; i < nb; i++)
        memset(palette[i], 0, 4);
}

/* ---- Octree quantization ---- */

typedef struct oct_node {
    int64_t sum[3];
    uint32_t count;
    int n_children;
    int depth;
    struct oct_node *child[8];
    struct oct_node *parent;
} oct_node_t;

static oct_node_t *oct_node_new(oct_node_t *parent, int depth)
{
    oct_node_t *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->parent = parent;
    n->depth = depth;
    return n;
}

static void oct_node_free(oct_node_t *n)
{
    int i;

    if (!n)
        return;
    for (i = 0; i < 8; i++)
        oct_node_free(n->child[i]);
    free(n);
}

static int oct_child_index(const uint8_t c[3], int depth)
{
    int shift = 7 - depth;
    return ((c[0] >> shift) & 1) << 2 |
           ((c[1] >> shift) & 1) << 1 |
           ((c[2] >> shift) & 1);
}

static void oct_insert(oct_node_t *root, const uint8_t c[3], uint32_t n)
{
    oct_node_t *node = root;
    int d, idx;

    for (d = 0; d < 8; d++) {
        idx = oct_child_index(c, d);
        if (!node->child[idx]) {
            node->child[idx] = oct_node_new(node, d + 1);
            if (!node->child[idx])
                return;
            node->n_children++;
        }
        node = node->child[idx];
        node->sum[0] += (int64_t)c[0] * n;
        node->sum[1] += (int64_t)c[1] * n;
        node->sum[2] += (int64_t)c[2] * n;
        node->count += n;
    }
}

static void oct_collect_leaves(oct_node_t *node, oct_node_t **leaves, int *n,
                               int cap)
{
    int i;
    bool any = false;

    if (!node)
        return;
    for (i = 0; i < 8; i++) {
        if (node->child[i]) {
            any = true;
            oct_collect_leaves(node->child[i], leaves, n, cap);
        }
    }
    if (!any && node->count > 0 && *n < cap)
        leaves[(*n)++] = node;
}

/* Collapse all children of `node` into a single leaf (sums already aggregated). */
static void oct_collapse(oct_node_t *node)
{
    int i;

    if (!node)
        return;
    for (i = 0; i < 8; i++) {
        if (node->child[i]) {
            oct_node_free(node->child[i]);
            node->child[i] = NULL;
        }
    }
    node->n_children = 0;
}

/* Pick the parent of the lowest-count leaf to collapse (reduces leaf count). */
static oct_node_t *oct_pick_collapse(oct_node_t **leaves, int n_leaves)
{
    int i;
    oct_node_t *best = NULL;
    oct_node_t *p;

    for (i = 0; i < n_leaves; i++) {
        p = leaves[i]->parent;
        if (!p || p->n_children == 0)
            continue;
        if (!best || p->count < best->count ||
            (p->count == best->count && p->depth > best->depth))
            best = p;
    }
    return best;
}

void quantization_gen_palette_octree(const volume_t *volume, int nb,
                                     uint8_t (*palette)[4],
                                     const uint8_t (*exclude)[4],
                                     int n_exclude)
{
    UT_array *arr = NULL;
    value_t *values;
    int i, n, n_leaves;
    oct_node_t *root;
    oct_node_t *collapse;
    oct_node_t **leaves;

    if (nb <= 0)
        return;

    collect_unique_opaque(volume, exclude, n_exclude, &arr);
    values = (value_t *)utarray_front(arr);
    n = utarray_len(arr);
    if (n <= nb) {
        palette_from_exact(values, n, nb, palette);
        utarray_free(arr);
        return;
    }

    root = oct_node_new(NULL, 0);
    if (!root) {
        utarray_free(arr);
        return;
    }
    for (i = 0; i < n; i++)
        oct_insert(root, values[i].c, values[i].n);
    utarray_free(arr);

    leaves = calloc((size_t)n, sizeof(*leaves));
    if (!leaves) {
        oct_node_free(root);
        return;
    }

    n_leaves = 0;
    oct_collect_leaves(root, leaves, &n_leaves, n);
    while (n_leaves > nb) {
        collapse = oct_pick_collapse(leaves, n_leaves);
        if (!collapse)
            break;
        oct_collapse(collapse);
        n_leaves = 0;
        oct_collect_leaves(root, leaves, &n_leaves, n);
    }

    for (i = 0; i < n_leaves && i < nb; i++) {
        palette[i][0] = (uint8_t)(leaves[i]->sum[0] / leaves[i]->count);
        palette[i][1] = (uint8_t)(leaves[i]->sum[1] / leaves[i]->count);
        palette[i][2] = (uint8_t)(leaves[i]->sum[2] / leaves[i]->count);
        palette[i][3] = 255;
    }
    for (; i < nb; i++)
        memset(palette[i], 0, 4);

    free(leaves);
    oct_node_free(root);
}

/* ---- Wu quantization (Graphics Gems / Xiaolin Wu) ---- */

#define WU_SIZE 33
#define WU_IDX(r, g, b) ((r) * WU_SIZE * WU_SIZE + (g) * WU_SIZE + (b))

typedef struct {
    int r0, r1, g0, g1, b0, b1;
} wu_box_t;

typedef struct {
    int64_t *wt;
    int64_t *mr;
    int64_t *mg;
    int64_t *mb;
    double *m2;
} wu_mom_t;

static void wu_moments_free(wu_mom_t *m)
{
    free(m->wt);
    free(m->mr);
    free(m->mg);
    free(m->mb);
    free(m->m2);
    memset(m, 0, sizeof(*m));
}

static bool wu_moments_alloc(wu_mom_t *m)
{
    size_t n = (size_t)WU_SIZE * WU_SIZE * WU_SIZE;
    memset(m, 0, sizeof(*m));
    m->wt = calloc(n, sizeof(*m->wt));
    m->mr = calloc(n, sizeof(*m->mr));
    m->mg = calloc(n, sizeof(*m->mg));
    m->mb = calloc(n, sizeof(*m->mb));
    m->m2 = calloc(n, sizeof(*m->m2));
    if (!m->wt || !m->mr || !m->mg || !m->mb || !m->m2) {
        wu_moments_free(m);
        return false;
    }
    return true;
}

static void wu_hist_to_moments(wu_mom_t *m)
{
    int r, g, b, i;
    int64_t area[WU_SIZE], area_r[WU_SIZE], area_g[WU_SIZE], area_b[WU_SIZE];
    double area2[WU_SIZE];
    int64_t line, line_r, line_g, line_b;
    double line2;

    for (r = 1; r < WU_SIZE; r++) {
        memset(area, 0, sizeof(area));
        memset(area_r, 0, sizeof(area_r));
        memset(area_g, 0, sizeof(area_g));
        memset(area_b, 0, sizeof(area_b));
        memset(area2, 0, sizeof(area2));
        for (g = 1; g < WU_SIZE; g++) {
            line = line_r = line_g = line_b = 0;
            line2 = 0;
            for (b = 1; b < WU_SIZE; b++) {
                i = WU_IDX(r, g, b);
                line += m->wt[i];
                line_r += m->mr[i];
                line_g += m->mg[i];
                line_b += m->mb[i];
                line2 += m->m2[i];

                area[b] += line;
                area_r[b] += line_r;
                area_g[b] += line_g;
                area_b[b] += line_b;
                area2[b] += line2;

                m->wt[i] = m->wt[WU_IDX(r - 1, g, b)] + area[b];
                m->mr[i] = m->mr[WU_IDX(r - 1, g, b)] + area_r[b];
                m->mg[i] = m->mg[WU_IDX(r - 1, g, b)] + area_g[b];
                m->mb[i] = m->mb[WU_IDX(r - 1, g, b)] + area_b[b];
                m->m2[i] = m->m2[WU_IDX(r - 1, g, b)] + area2[b];
            }
        }
    }
}

static int64_t wu_vol(const int64_t *m, const wu_box_t *cube)
{
    return m[WU_IDX(cube->r1, cube->g1, cube->b1)]
         - m[WU_IDX(cube->r1, cube->g1, cube->b0)]
         - m[WU_IDX(cube->r1, cube->g0, cube->b1)]
         + m[WU_IDX(cube->r1, cube->g0, cube->b0)]
         - m[WU_IDX(cube->r0, cube->g1, cube->b1)]
         + m[WU_IDX(cube->r0, cube->g1, cube->b0)]
         + m[WU_IDX(cube->r0, cube->g0, cube->b1)]
         - m[WU_IDX(cube->r0, cube->g0, cube->b0)];
}

static double wu_vol_f(const double *m, const wu_box_t *cube)
{
    return m[WU_IDX(cube->r1, cube->g1, cube->b1)]
         - m[WU_IDX(cube->r1, cube->g1, cube->b0)]
         - m[WU_IDX(cube->r1, cube->g0, cube->b1)]
         + m[WU_IDX(cube->r1, cube->g0, cube->b0)]
         - m[WU_IDX(cube->r0, cube->g1, cube->b1)]
         + m[WU_IDX(cube->r0, cube->g1, cube->b0)]
         + m[WU_IDX(cube->r0, cube->g0, cube->b1)]
         - m[WU_IDX(cube->r0, cube->g0, cube->b0)];
}

static int64_t wu_bottom(const int64_t *m, const wu_box_t *cube, int dir)
{
    switch (dir) {
    case 0: /* R */
        return -m[WU_IDX(cube->r0, cube->g1, cube->b1)]
               + m[WU_IDX(cube->r0, cube->g1, cube->b0)]
               + m[WU_IDX(cube->r0, cube->g0, cube->b1)]
               - m[WU_IDX(cube->r0, cube->g0, cube->b0)];
    case 1: /* G */
        return -m[WU_IDX(cube->r1, cube->g0, cube->b1)]
               + m[WU_IDX(cube->r1, cube->g0, cube->b0)]
               + m[WU_IDX(cube->r0, cube->g0, cube->b1)]
               - m[WU_IDX(cube->r0, cube->g0, cube->b0)];
    default: /* B */
        return -m[WU_IDX(cube->r1, cube->g1, cube->b0)]
               + m[WU_IDX(cube->r1, cube->g0, cube->b0)]
               + m[WU_IDX(cube->r0, cube->g1, cube->b0)]
               - m[WU_IDX(cube->r0, cube->g0, cube->b0)];
    }
}

static int64_t wu_top(const int64_t *m, const wu_box_t *cube, int dir, int pos)
{
    switch (dir) {
    case 0:
        return m[WU_IDX(pos, cube->g1, cube->b1)]
             - m[WU_IDX(pos, cube->g1, cube->b0)]
             - m[WU_IDX(pos, cube->g0, cube->b1)]
             + m[WU_IDX(pos, cube->g0, cube->b0)];
    case 1:
        return m[WU_IDX(cube->r1, pos, cube->b1)]
             - m[WU_IDX(cube->r1, pos, cube->b0)]
             - m[WU_IDX(cube->r0, pos, cube->b1)]
             + m[WU_IDX(cube->r0, pos, cube->b0)];
    default:
        return m[WU_IDX(cube->r1, cube->g1, pos)]
             - m[WU_IDX(cube->r1, cube->g0, pos)]
             - m[WU_IDX(cube->r0, cube->g1, pos)]
             + m[WU_IDX(cube->r0, cube->g0, pos)];
    }
}

static double wu_var(const wu_mom_t *m, const wu_box_t *cube)
{
    double dr = (double)wu_vol(m->mr, cube);
    double dg = (double)wu_vol(m->mg, cube);
    double db = (double)wu_vol(m->mb, cube);
    double xx = wu_vol_f(m->m2, cube);
    int64_t w = wu_vol(m->wt, cube);
    if (w <= 0)
        return 0;
    return xx - (dr * dr + dg * dg + db * db) / (double)w;
}

static double wu_maximize(const wu_mom_t *m, wu_box_t *cube, int dir,
                          int first, int last, int *cut,
                          int64_t whole_r, int64_t whole_g, int64_t whole_b,
                          int64_t whole_w)
{
    int64_t half_r, half_g, half_b, half_w;
    int64_t base_r, base_g, base_b, base_w;
    double temp, maxv = 0.0;
    int i;

    base_r = wu_bottom(m->mr, cube, dir);
    base_g = wu_bottom(m->mg, cube, dir);
    base_b = wu_bottom(m->mb, cube, dir);
    base_w = wu_bottom(m->wt, cube, dir);
    *cut = -1;

    for (i = first; i < last; i++) {
        half_r = base_r + wu_top(m->mr, cube, dir, i);
        half_g = base_g + wu_top(m->mg, cube, dir, i);
        half_b = base_b + wu_top(m->mb, cube, dir, i);
        half_w = base_w + wu_top(m->wt, cube, dir, i);
        if (half_w == 0)
            continue;
        temp = ((double)half_r * half_r + (double)half_g * half_g +
                (double)half_b * half_b) / (double)half_w;
        half_r = whole_r - half_r;
        half_g = whole_g - half_g;
        half_b = whole_b - half_b;
        half_w = whole_w - half_w;
        if (half_w == 0)
            continue;
        temp += ((double)half_r * half_r + (double)half_g * half_g +
                 (double)half_b * half_b) / (double)half_w;
        if (temp > maxv) {
            maxv = temp;
            *cut = i;
        }
    }
    return maxv;
}

static bool wu_cut(const wu_mom_t *m, wu_box_t *set1, wu_box_t *set2)
{
    int dir, cut[3], cutr, cutg, cutb;
    int64_t whole_r, whole_g, whole_b, whole_w;
    double maxr, maxg, maxb;

    whole_r = wu_vol(m->mr, set1);
    whole_g = wu_vol(m->mg, set1);
    whole_b = wu_vol(m->mb, set1);
    whole_w = wu_vol(m->wt, set1);
    if (whole_w == 0)
        return false;

    maxr = wu_maximize(m, set1, 0, set1->r0 + 1, set1->r1, &cutr,
                       whole_r, whole_g, whole_b, whole_w);
    maxg = wu_maximize(m, set1, 1, set1->g0 + 1, set1->g1, &cutg,
                       whole_r, whole_g, whole_b, whole_w);
    maxb = wu_maximize(m, set1, 2, set1->b0 + 1, set1->b1, &cutb,
                       whole_r, whole_g, whole_b, whole_w);

    if (maxr >= maxg && maxr >= maxb) {
        dir = 0;
        if (cutr < 0)
            return false;
    } else if (maxg >= maxr && maxg >= maxb) {
        dir = 1;
    } else {
        dir = 2;
    }

    *set2 = *set1;
    cut[0] = cutr;
    cut[1] = cutg;
    cut[2] = cutb;

    switch (dir) {
    case 0:
        set1->r1 = set2->r0 = cut[0];
        break;
    case 1:
        set1->g1 = set2->g0 = cut[1];
        break;
    default:
        set1->b1 = set2->b0 = cut[2];
        break;
    }
    return true;
}

static void wu_mark_average(const wu_mom_t *m, const wu_box_t *cube,
                            uint8_t out[4])
{
    int64_t w = wu_vol(m->wt, cube);
    if (w <= 0) {
        memset(out, 0, 4);
        return;
    }
    out[0] = (uint8_t)(wu_vol(m->mr, cube) / w);
    out[1] = (uint8_t)(wu_vol(m->mg, cube) / w);
    out[2] = (uint8_t)(wu_vol(m->mb, cube) / w);
    out[3] = 255;
}

void quantization_gen_palette_wu(const volume_t *volume, int nb,
                                 uint8_t (*palette)[4],
                                 const uint8_t (*exclude)[4], int n_exclude)
{
    UT_array *arr = NULL;
    value_t *values;
    int i, n, next, k, n_out;
    wu_mom_t mom;
    wu_box_t *cubes;
    double *vv;
    double temp;
    int r, g, b, idx;

    if (nb <= 0)
        return;

    collect_unique_opaque(volume, exclude, n_exclude, &arr);
    values = (value_t *)utarray_front(arr);
    n = utarray_len(arr);
    if (n <= nb) {
        palette_from_exact(values, n, nb, palette);
        utarray_free(arr);
        return;
    }

    if (!wu_moments_alloc(&mom)) {
        utarray_free(arr);
        return;
    }

    for (i = 0; i < n; i++) {
        r = (values[i].c[0] >> 3) + 1;
        g = (values[i].c[1] >> 3) + 1;
        b = (values[i].c[2] >> 3) + 1;
        idx = WU_IDX(r, g, b);
        mom.wt[idx] += values[i].n;
        mom.mr[idx] += (int64_t)values[i].c[0] * values[i].n;
        mom.mg[idx] += (int64_t)values[i].c[1] * values[i].n;
        mom.mb[idx] += (int64_t)values[i].c[2] * values[i].n;
        mom.m2[idx] += (double)values[i].n *
            ((double)values[i].c[0] * values[i].c[0] +
             (double)values[i].c[1] * values[i].c[1] +
             (double)values[i].c[2] * values[i].c[2]);
    }
    utarray_free(arr);
    wu_hist_to_moments(&mom);

    cubes = calloc((size_t)nb, sizeof(*cubes));
    vv = calloc((size_t)nb, sizeof(*vv));
    if (!cubes || !vv) {
        free(cubes);
        free(vv);
        wu_moments_free(&mom);
        return;
    }

    cubes[0].r0 = cubes[0].g0 = cubes[0].b0 = 0;
    cubes[0].r1 = cubes[0].g1 = cubes[0].b1 = WU_SIZE - 1;
    next = 0;
    n_out = nb;

    for (i = 1; i < nb; i++) {
        if (wu_cut(&mom, &cubes[next], &cubes[i])) {
            vv[next] = (cubes[next].r1 > cubes[next].r0 ||
                        cubes[next].g1 > cubes[next].g0 ||
                        cubes[next].b1 > cubes[next].b0)
                           ? wu_var(&mom, &cubes[next]) : 0.0;
            vv[i] = (cubes[i].r1 > cubes[i].r0 ||
                     cubes[i].g1 > cubes[i].g0 ||
                     cubes[i].b1 > cubes[i].b0)
                        ? wu_var(&mom, &cubes[i]) : 0.0;
        } else {
            vv[next] = 0.0;
            i--;
        }
        next = 0;
        temp = vv[0];
        for (k = 1; k <= i; k++) {
            if (vv[k] > temp) {
                temp = vv[k];
                next = k;
            }
        }
        if (temp <= 0.0) {
            n_out = i + 1;
            break;
        }
    }

    for (i = 0; i < n_out; i++)
        wu_mark_average(&mom, &cubes[i], palette[i]);
    for (; i < nb; i++)
        memset(palette[i], 0, 4);

    free(cubes);
    free(vv);
    wu_moments_free(&mom);
}

/* ---- k-means++ ---- */

#define KMEANS_MAX_ITERS 40

static double kmeans_dist2(const uint8_t a[3], const double b[3])
{
    double dr = (double)a[0] - b[0];
    double dg = (double)a[1] - b[1];
    double db = (double)a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

static float kmeans_rand01(void)
{
    return (random_int(0, 16777215) + 0.5f) / 16777216.f;
}

void quantization_gen_palette_kmeans(const volume_t *volume, int nb,
                                     uint8_t (*palette)[4],
                                     const uint8_t (*exclude)[4],
                                     int n_exclude)
{
    UT_array *arr = NULL;
    value_t *values;
    int i, j, n, iter, changed;
    double (*centres)[3] = NULL;
    double (*sums)[3] = NULL;
    uint32_t *counts = NULL;
    int *assign = NULL;
    double *d2 = NULL;
    double sum_d, r, best, dist;
    int best_j;

    if (nb <= 0)
        return;

    collect_unique_opaque(volume, exclude, n_exclude, &arr);
    values = (value_t *)utarray_front(arr);
    n = utarray_len(arr);
    if (n <= nb) {
        palette_from_exact(values, n, nb, palette);
        utarray_free(arr);
        return;
    }

    centres = calloc((size_t)nb, sizeof(*centres));
    sums = calloc((size_t)nb, sizeof(*sums));
    counts = calloc((size_t)nb, sizeof(*counts));
    assign = calloc((size_t)n, sizeof(*assign));
    d2 = calloc((size_t)n, sizeof(*d2));
    if (!centres || !sums || !counts || !assign || !d2)
        goto cleanup;

    /* k-means++ seeding. */
    i = random_int(0, n - 1);
    centres[0][0] = values[i].c[0];
    centres[0][1] = values[i].c[1];
    centres[0][2] = values[i].c[2];

    for (j = 1; j < nb; j++) {
        sum_d = 0;
        for (i = 0; i < n; i++) {
            best = kmeans_dist2(values[i].c, centres[0]);
            for (best_j = 1; best_j < j; best_j++) {
                dist = kmeans_dist2(values[i].c, centres[best_j]);
                if (dist < best)
                    best = dist;
            }
            d2[i] = best * (double)values[i].n;
            sum_d += d2[i];
        }
        if (sum_d <= 0)
            break;
        r = (double)kmeans_rand01() * sum_d;
        for (i = 0; i < n; i++) {
            r -= d2[i];
            if (r <= 0)
                break;
        }
        if (i >= n)
            i = n - 1;
        centres[j][0] = values[i].c[0];
        centres[j][1] = values[i].c[1];
        centres[j][2] = values[i].c[2];
    }

    for (iter = 0; iter < KMEANS_MAX_ITERS; iter++) {
        memset(sums, 0, (size_t)nb * sizeof(*sums));
        memset(counts, 0, (size_t)nb * sizeof(*counts));
        changed = 0;

        for (i = 0; i < n; i++) {
            best_j = 0;
            best = kmeans_dist2(values[i].c, centres[0]);
            for (j = 1; j < nb; j++) {
                dist = kmeans_dist2(values[i].c, centres[j]);
                if (dist < best) {
                    best = dist;
                    best_j = j;
                }
            }
            if (assign[i] != best_j) {
                assign[i] = best_j;
                changed = 1;
            }
            sums[best_j][0] += (double)values[i].c[0] * values[i].n;
            sums[best_j][1] += (double)values[i].c[1] * values[i].n;
            sums[best_j][2] += (double)values[i].c[2] * values[i].n;
            counts[best_j] += values[i].n;
        }

        for (j = 0; j < nb; j++) {
            if (counts[j] == 0)
                continue;
            centres[j][0] = sums[j][0] / counts[j];
            centres[j][1] = sums[j][1] / counts[j];
            centres[j][2] = sums[j][2] / counts[j];
        }
        if (!changed && iter > 0)
            break;
    }

    for (j = 0; j < nb; j++) {
        if (counts[j] == 0) {
            memset(palette[j], 0, 4);
            continue;
        }
        palette[j][0] = (uint8_t)clamp((int)(centres[j][0] + 0.5), 0, 255);
        palette[j][1] = (uint8_t)clamp((int)(centres[j][1] + 0.5), 0, 255);
        palette[j][2] = (uint8_t)clamp((int)(centres[j][2] + 0.5), 0, 255);
        palette[j][3] = 255;
    }

cleanup:
    free(centres);
    free(sums);
    free(counts);
    free(assign);
    free(d2);
    utarray_free(arr);
}
