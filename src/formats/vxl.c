/* Goxel 3D voxels editor
 *
 * copyright (c) 2021 ByteBit/xtreme8000
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

// Support for Ace of Spades map files (vxl)

#include "goxel.h"
#include "file_format.h"
#include <libvxl.h>

#define RED(c) ((c)&0xFF)
#define GREEN(c) (((c) >> 8) & 0xFF)
#define BLUE(c) (((c) >> 16) & 0xFF)

static int import_vxl(const file_format_t *format, image_t* image, const char* path) {
	if(!path)
		return -1;

	int file_size;
	void* file_data = read_file(path, &file_size);

	size_t map_size, map_depth;

	if(!libvxl_size(&map_size, &map_depth, file_data, file_size))
		return -1;

	struct libvxl_map map;

	if(!libvxl_create(&map, map_size, map_size, map_depth, file_data,
					  file_size))
		return -1;

	volume_iterator_t it
		= volume_get_iterator(image->active_layer->volume, VOLUME_ITER_VOXELS);

	for(size_t x = 0; x < map_size; x++) {
		for(size_t y = 0; y < map_size; y++) {
			for(size_t z = 0; z < map_depth; z++) {
				if(libvxl_map_issolid(&map, x, y, z)) {
					uint32_t color = libvxl_map_get(&map, x, y, z);

					volume_set_at(image->active_layer->volume, &it,
								(int[3]) {map_size / 2 - 1 - x,
										  y - map_size / 2,
										  map_depth / 2 - 1 - z},
								(uint8_t[4]) {BLUE(color), GREEN(color),
											  RED(color), 0xFF});
				}
			}
		}
	}

	libvxl_free(&map);

	if(!box_is_null(image->box))
		bbox_from_extents(image->box, vec3_zero, map_size / 2.0F,
						  map_size / 2.0F, map_depth / 2.0F);

	return 0;
}

static int export_as_vxl(const file_format_t *format, const image_t* image, const char* path) {
	if(!path)
		return -1;

	const volume_t* volume = goxel_get_layers_volume(image);

	int bbox[2][3];
	if(!volume_get_bbox(volume, bbox, true))
		return -1;

	struct libvxl_map map;
	if(!libvxl_create(&map, bbox[1][0] - bbox[0][0], bbox[1][1] - bbox[0][1],
					  bbox[1][2] - bbox[0][2], NULL, 0))
		return -1;

	int pos[3];
	volume_iterator_t it = volume_get_iterator(volume, VOLUME_ITER_SKIP_EMPTY);

	while(volume_iter(&it, pos)) {
		uint8_t color[4];
		volume_get_at(volume, &it, pos, color);

		if(color[3] > 0)
			libvxl_map_set(&map, bbox[1][0] - 1 - pos[0], pos[1] - bbox[0][1],
						   bbox[1][2] - 1 - pos[2],
						   RGB(color[2], color[1], color[0]));
	}

	libvxl_writefile(&map, (char*)path);
	libvxl_free(&map);

	return 0;
}

FILE_FORMAT_REGISTER(vxl,
    .name = "vxl",
    .exts = {"*.vxl"},
    .exts_desc = "vxl",
	.import_func = import_vxl,
    .export_func = export_as_vxl
)