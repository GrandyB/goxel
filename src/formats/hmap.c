/* Goxel 3D voxels editor
 *
 * copyright (c) 2017 Guillaume Chereau <guillaume@noctua-software.com>
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
#include "file_format.h"

// Heightmaps are a scale from lowest z (0) upwards, from black to white.
// Bildramer's heightmap tool starts at RGB(8,8,8) for z1 and increments by 4 for each subsequent z value upwards.
static int get_hmap_color(int z) {
    return z == 0 ? 0 : 8 + ((z-1) * 4);
}

static int export_as_heightmap(const image_t *image, const char *path)
{
    float box[4][4];
    const mesh_t *mesh;
    int x, y, z, w, h, d, pos[3], start_pos[3];
    uint8_t c[4];
    uint8_t *img;
    mesh_iterator_t iter = {0};

    mesh = goxel_get_layers_mesh(image);
    mat4_copy(image->box, box);
    if (box_is_null(box)) mesh_get_box(mesh, true, box);
    w = box[0][0] * 2;
    h = box[1][1] * 2;
    d = box[2][2] * 2;
    start_pos[0] = box[0][0];
    start_pos[1] = box[3][1] - box[1][1];
    start_pos[2] = box[3][2] - box[2][2];
    printf("w: %u, h: %u, d: %u\n", w,h,d);
    img = calloc(w * h, 4);
    for (x = 0; x < w; x++)
    for (y = 0; y < h; y++)
    for (z = 0; z < d; z++) {
        pos[0] = start_pos[0] - x; // x seemed to be flipped, this fixes it despite looking like an outlier
        pos[1] = y + start_pos[1];
        pos[2] = z + start_pos[2];
        mesh_get_at(mesh, &iter, pos, c);
        bool hasBlock = c[3] != 0; // Completely transparent colour of block

        // Always insert for z=0 (bottom indestructible layer), or if there's a block on this z
        if (hasBlock || z == 0) {
            int val = get_hmap_color(z);
            c[0] = val;
            c[1] = val;
            c[2] = val;
            c[3] = 255; // No transparency

            // Images are one big long array of values, e.g. an 4x2x4 area would mean...
            //      (0,0,0) = index 0
            //      (3,0,0) = index 3
            //      (0,1,0) = index 5, as that's not in the first row
            //      (3,1,3) = index 8, at the end of the second row of 4, etc
            // In a heightmap's case we're overlaying stuff on top of each other so z 
            // doesn't matter other than making sure we are increasing z as we loop through
            int img_index = ((y * w) + x);
            img[img_index * 4 + 0] = c[0];
            img[img_index * 4 + 1] = c[1];
            img[img_index * 4 + 2] = c[2];
            img[img_index * 4 + 3] = c[3];
        }
    }
    img_write(img, w, h, 4, path);
    free(img);
    return 0;
}

FILE_FORMAT_REGISTER(heightmap,
    .name = "heightmap",
    .ext = "png\0*.png\0",
    .export_func = export_as_heightmap,
)
