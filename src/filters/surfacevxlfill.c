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

/*
 * Paint hidden (non-surface) voxels using neighbouring surface colours,
 * splitting solid runs 50/50 and favouring the upper surface on odd counts.
 */
typedef struct {
    filter_t filter;
} filter_surfacevxlfill_t;

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

static void paint_hidden_run(uint8_t (*colors)[4], int z_hi, int z_lo,
                             const uint8_t upper_color[4],
                             const uint8_t lower_color[4], bool has_upper,
                             bool has_lower)
{
    int z, n, upper_n, i;

    n = z_hi - z_lo + 1;
    if (n <= 0)
        return;

    if (has_upper && has_lower) {
        upper_n = (n + 1) / 2;
        for (i = 0, z = z_hi; z >= z_lo; z--, i++) {
            if (i < upper_n)
                memcpy(colors[z], upper_color, 4);
            else
                memcpy(colors[z], lower_color, 4);
        }
    } else if (has_upper) {
        for (z = z_hi; z >= z_lo; z--)
            memcpy(colors[z], upper_color, 4);
    } else if (has_lower) {
        for (z = z_hi; z >= z_lo; z--)
            memcpy(colors[z], lower_color, 4);
    }
}

static void process_solid_run(uint8_t (*colors)[4], const bool *is_surface,
                              int z_top, int z_bot)
{
    int z;
    int seg_hi;
    uint8_t upper_color[4] = {0};
    bool have_upper = false;

    /* Walk sky → ground within the solid run. */
    z = z_top;
    while (z >= z_bot) {
        if (is_surface[z]) {
            memcpy(upper_color, colors[z], 4);
            have_upper = true;
            z--;
            continue;
        }

        /* Contiguous hidden segment. */
        seg_hi = z;
        while (z >= z_bot && !is_surface[z])
            z--;
        /* z is now one below the segment (surface or past z_bot). */

        if (z >= z_bot && is_surface[z]) {
            paint_hidden_run(colors, seg_hi, z + 1, upper_color, colors[z],
                             have_upper, true);
        } else {
            paint_hidden_run(colors, seg_hi, z_bot, upper_color, upper_color,
                             have_upper, false);
        }
    }
}

static void apply_surfacevxlfill(volume_t *volume)
{
    float box[4][4];
    int dimensions[3], start_pos[3];
    int x, y, z, pos[3], idx, size;
    volume_iterator_t iter;
    bool *surface_map = NULL;
    uint8_t (*col_colors)[4] = NULL;
    bool *col_surface = NULL;
    bool *col_solid = NULL;
    uint8_t color[4];
    int z_top, z_bot;

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] <= 0)
        return;

    size = dimensions[0] * dimensions[1] * dimensions[2];
    surface_map = calloc(size, sizeof(*surface_map));
    col_colors = malloc(dimensions[2] * sizeof(*col_colors));
    col_surface = malloc(dimensions[2] * sizeof(*col_surface));
    col_solid = malloc(dimensions[2] * sizeof(*col_solid));
    if (!surface_map || !col_colors || !col_surface || !col_solid)
        goto cleanup;

    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS | VOLUME_ITER_SKIP_EMPTY);

    /* Pass 1: classify surface voxels over the AABB. */
    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            for (z = 0; z < dimensions[2]; z++) {
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                idx = (x * dimensions[1] + y) * dimensions[2] + z;
                surface_map[idx] = voxel_is_surface(
                    volume, &iter, pos, start_pos, dimensions);
            }
        }
    }

    /* Pass 2: paint hidden voxels per column. */
    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            for (z = 0; z < dimensions[2]; z++) {
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                idx = (x * dimensions[1] + y) * dimensions[2] + z;
                volume_get_at(volume, &iter, pos, color);
                memcpy(col_colors[z], color, 4);
                col_solid[z] = voxel_is_solid(color);
                col_surface[z] = surface_map[idx];
            }

            z = dimensions[2] - 1;
            while (z >= 0) {
                if (!col_solid[z]) {
                    z--;
                    continue;
                }
                z_top = z;
                while (z >= 0 && col_solid[z])
                    z--;
                z_bot = z + 1;
                process_solid_run(col_colors, col_surface, z_top, z_bot);
            }

            for (z = 0; z < dimensions[2]; z++) {
                if (!col_solid[z] || col_surface[z])
                    continue;
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                volume_set_at(volume, &iter, pos, col_colors[z]);
            }
        }
    }

cleanup:
    free(surface_map);
    free(col_colors);
    free(col_surface);
    free(col_solid);
}

static int gui(filter_t *filter_)
{
    const char *help_text =
        "Use surface voxel colours to paint inner/hidden voxels";

    (void)filter_;
    goxel_set_help_text(help_text);
    gui_text_wrapped(help_text);

    if (gui_button("Apply", -1, 0)) {
        image_history_push(goxel.image);
        apply_surfacevxlfill(goxel.image->active_layer->volume);
    }
    return 0;
}

FILTER_REGISTER(surfacevxlfill, filter_surfacevxlfill_t,
                .name = "Utility - surface colour fill",
                .gui_fn = gui, )
