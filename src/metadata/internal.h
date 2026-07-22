/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#ifndef METADATA_INTERNAL_H
#define METADATA_INTERNAL_H

#include "metadata.h"

#include <stdbool.h>

bool custom_object_name_exists(void *user, const char *name);
bool is_descendant_of(const custom_object_t *obj,
                      const custom_object_t *ancestor);
custom_object_t *last_in_group_subtree(custom_object_t *list,
                                       const custom_object_t *group);
void zone_corners(const custom_object_t *obj, int min[3], int max[3]);
void normalize_corners(int a[3], int b[3]);

void init_enum_defaults(custom_object_t *obj);
void init_value_fields(custom_object_t *obj);
void init_object_coords(image_t *img, custom_object_t *obj);
void random_object_color(uint8_t color[4]);

/* Clear editor session refs that point at obj (or any object in list). */
void custom_objects_clear_edit_refs(const custom_object_t *obj);
void custom_objects_on_list_freed(custom_object_t *list);

#endif /* METADATA_INTERNAL_H */
