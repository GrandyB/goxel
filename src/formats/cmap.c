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

// Colormap = top-down view of map in full colour, to export as a bmp
static int export_as_colormap(const file_format_t *format, const image_t *image, const char *path)
{
    float box[4][4];
    const volume_t *volume;
    int x, y, z, pos[3], dimensions[3], start_pos[3];;
    uint8_t c[4];
    uint8_t *img;
    volume_iterator_t iter = {0};

    volume = goxel_get_layers_volume(image);
    mat4_copy(image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    img = calloc(dimensions[0] * dimensions[1], 4);
    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            for (z = 0; z < dimensions[2]; z++)
            {
                pos[0] = x + start_pos[0];
                pos[1] = y + start_pos[1];
                pos[2] = z + start_pos[2];
                volume_get_at(volume, &iter, pos, c);
                bool hasBlock = c[3] != 0; // Completely transparent colour of block

                // Always insert for z=0 (bottom indestructible layer), or if there's a block on this z
                if (hasBlock || z == 0)
                {
                    c[3] = 255; // No transparency

                    // Images are one big long array of values, e.g. an 4x2x4 area would mean...
                    //      (0,0,0) = index 0
                    //      (3,0,0) = index 3
                    //      (0,1,0) = index 5, as that's not in the first row
                    //      (3,1,3) = index 8, at the end of the second row of 4, etc
                    // In a heightmap's case we're overlaying stuff on top of each other so z
                    // doesn't matter other than making sure we are increasing z as we loop through
                    int img_index = ((y * dimensions[0]) + x);
                    img[img_index * 4 + 0] = c[0];
                    img[img_index * 4 + 1] = c[1];
                    img[img_index * 4 + 2] = c[2];
                    img[img_index * 4 + 3] = c[3];
                }
            }
        }
    }
    img_write(img, dimensions[0], dimensions[1], 4, bmp, path);
    free(img);
    return 0;
}

static int import_cmap(const file_format_t *format, image_t *image, const char *path)
{
    volume_t *volume;
    volume_iterator_t iter = {0};
    uint8_t *img;
    float box[4][4];
    int x, y, z;
    int file_w, file_h, pos[3], dimensions[3], start_pos[3];;
    uint8_t c[4], c2[4];
    int bpp = 0;

    // Read the image file; file_w and file_h are the original image dimensions
    img = img_read(path, &file_w, &file_h, &bpp);

    volume = image->active_layer->volume;
    mat4_copy(image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);

    // Determine how many pixels we can map: stop when we exceed either the image or volume bounds
    int max_x = (dimensions[0] < file_w) ? dimensions[0] : file_w;
    int max_y = (dimensions[1] < file_h) ? dimensions[1] : file_h;

    // Iterate over the image pixels (which map to x/y coordinates in the volume)
    for (x = 0; x < max_x; x++) {
        for (y = 0; y < max_y; y++) {
            // Calculate the index into the image array (row-major order)
            int img_index = y * file_w + x;
            c[0] = img[img_index * bpp + 0];
            c[1] = img[img_index * bpp + 1];
            c[2] = img[img_index * bpp + 2];
            c[3] = 255;
            //printf("%i/%i: %u/%u/%u/%u\n", x, y, c[0], c[1], c[2], c[3]);

            // Map the image x,y coordinate to a volume coordinate.
            pos[0] = x + start_pos[0];
            pos[1] = y + start_pos[1];

            // Apply this 2D pixel color to any existing block in this position
            for (z = 0; z < dimensions[2]; z++) {
                pos[2] = z + start_pos[2];

                volume_get_at(volume, &iter, pos, c2);
                if (c2[3] != 0) {
                    // Set the block color; force alpha to 255 for full opacity
                    volume_set_at(volume, &iter, pos, c);
                }
            }
        }
    }

    free(img);
    return 0;
}

FILE_FORMAT_REGISTER(colormap,
                     .name = "colormap",
                     .exts = {"*.bmp"},
                     .exts_desc = "bmp",
                     .import_func = import_cmap,
                     .export_func = export_as_colormap,
                     .affect_current_layer = true )