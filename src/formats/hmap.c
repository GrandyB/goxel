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
static int get_hmap_color(int z)
{
    return clamp(z == 0 ? 0 : 8 + ((z - 1) * 4), 0, 255);
}

static int get_hmap_z(int color)
{
    if (color < 0) return 0;
    if (color < 8) return 1;
    // Re-arranging get_hmap_color function (more or less, as we want 0-8 to be level 1)
    return clamp(2 + (color - 8) / 4, 0, 63);
}

static int export_as_heightmap(const file_format_t *format, const image_t *image, const char *path)
{
    float box[4][4];
    const volume_t *mesh;
    int x, y, z, w, h, d, pos[3], start_pos[3];
    uint8_t c[4];
    uint8_t *img;
    volume_iterator_t iter = {0};

    mesh = goxel_get_layers_volume(image);
    mat4_copy(image->box, box);
    if (box_is_null(box))
        volume_get_box(mesh, true, box);
    w = box[0][0] * 2;
    h = box[1][1] * 2;
    d = box[2][2] * 2;
    start_pos[0] = box[0][0];
    start_pos[1] = box[3][1] - box[1][1];
    start_pos[2] = box[3][2] - box[2][2];
    printf("w: %u, h: %u, d: %u\n", w, h, d);
    img = calloc(w * h, 4);
    for (x = 0; x < w; x++)
        for (y = 0; y < h; y++)
            for (z = 0; z < d; z++)
            {
                pos[0] = start_pos[0] - x - 1; // x seemed to be flipped, this fixes it despite looking like an outlier
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                volume_get_at(mesh, &iter, pos, c);
                bool hasBlock = c[3] != 0; // Completely transparent colour of block

                // Always insert for z=0 (bottom indestructible layer), or if there's a block on this z
                if (hasBlock || z == 0)
                {
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
    img_write(img, w, h, 4, bmp, path);
    free(img);
    return 0;
}

static int import_hmap(const file_format_t *format, image_t *image, const char *path)
{
    volume_t *volume;
    volume_iterator_t iter = {0};
    uint8_t *img;
    float box[4][4];
    int x, y, z;
    int file_w, file_h, vol_w, vol_h, vol_d;
    int pos[3], start_pos[3];
    uint8_t c[4];
    int bpp = 0;

    // Read the image file; file_w and file_h are the original image dimensions
    img = img_read(path, &file_w, &file_h, &bpp);

    volume = image->active_layer->volume;
    mat4_copy(image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    // Compute volume dimensions from the box (assumes box[0][0], etc., are half-dimensions)
    vol_w = box[0][0] * 2; // volume width
    vol_h = box[1][1] * 2; // volume height
    vol_d = box[2][2] * 2; // volume depth

    // Determine starting positions in the volume; these offsets map the image's (0,0) to the volume
    start_pos[0] = box[0][0];                // x starting position
    start_pos[1] = box[3][1] - box[1][1];      // y starting position
    start_pos[2] = box[3][2] - box[2][2];      // z starting position

    LOG_D("Volume dimensions: %d x %d x %d\n", vol_w, vol_h, vol_d);

    // Determine how many pixels we can map: stop when we exceed either the image or volume bounds
    int max_x = (vol_w < file_w) ? vol_w : file_w;
    int max_y = (vol_h < file_h) ? vol_h : file_h;

    // Iterate over the image pixels (which map to x/y coordinates in the volume)
    for (y = 0; y < max_y; y++) {
        for (x = 0; x < max_x; x++) {
            // Calculate the index into the image array (row-major order)
            int img_index = y * file_w + x;
            c[0] = img[img_index * bpp + 0];
            c[1] = img[img_index * bpp + 1];
            c[2] = img[img_index * bpp + 2];
            c[3] = 255;

            int z_height = get_hmap_z(c[0]);
            //LOG_D("%i/%i: %u/%u/%u/%u\n", x, y, c[0], c[1], c[2], c[3]);

            // Map the image x,y coordinate to a volume coordinate.
            // Note: The x axis is flipped compared to the image, so subtract x (and an extra 1) from start_pos[0].
            int vol_x = start_pos[0] - x - 1;
            int vol_y = y + start_pos[1];

            // Apply this 2D pixel color to any existing block in this position
            for (z = 0; z < z_height; z++) {
                int vol_z = z + start_pos[2];
                pos[0] = vol_x;
                pos[1] = vol_y;
                pos[2] = vol_z;

                // Set the block color; force alpha to 255 for full opacity
                volume_set_at(volume, &iter, pos, c);
            }
        }
    }

    free(img);
    return 0;
}

FILE_FORMAT_REGISTER(colormap,
                     .name = "heightmap",
                     .exts = {"*.bmp"},
                     .exts_desc = "bmp",
                     .import_func = import_hmap,
                     .export_func = export_as_heightmap, )