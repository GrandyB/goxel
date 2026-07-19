/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#ifndef CUSTOM_OBJECTS_H
#define CUSTOM_OBJECTS_H

#include "image.h"
#include "render.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void custom_objects_free_list(custom_object_t **list);
void custom_objects_copy_list(custom_object_t **dst, const custom_object_t *src);
custom_object_t *custom_object_add(image_t *img, custom_object_type_t type);
custom_object_t *custom_object_duplicate(image_t *img, custom_object_t *src);
void custom_object_delete(image_t *img, custom_object_t *obj);
void custom_object_set_type(image_t *img, custom_object_t *obj,
                            custom_object_type_t type);

/* Build AABB box for a zone / point (for render & edit). */
void custom_object_get_box(const image_t *img, const custom_object_t *obj,
                           float box[4][4]);

void custom_objects_render(renderer_t *rend, const image_t *img);

/* Hover/drag gizmos; call from filter mouse_fn when filter is open. */
void custom_objects_set_editor_active(bool active);
void custom_objects_edit_iter(const float viewport[4]);

/* Binary blob for .gox CUST chunk. Caller frees *out. */
uint8_t *custom_objects_serialize(const image_t *img, int *out_len);
void custom_objects_deserialize(image_t *img, const uint8_t *data, int len);

void custom_objects_export_log(const image_t *img);

const char *custom_object_type_name(custom_object_type_t type);

#endif /* CUSTOM_OBJECTS_H */
