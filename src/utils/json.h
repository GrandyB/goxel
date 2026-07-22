/* Goxel 3D voxels editor
 *
 * copyright (c) 2019 Guillaume Chereau <guillaume@noctua-software.com>
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

#ifndef JSON_H
#define JSON_H

#include "../../ext_src/json/json.h"
#include "../../ext_src/json/json-builder.h"

#include <stdbool.h>
#include <stdint.h>

// Some extra helper functions.

json_value *json_object_push_int(json_value *obj, const json_char *name,
                                 json_int_t v);
json_value *json_object_push_string(json_value *obj, const json_char *name,
                                    const json_char *v);
json_value *json_object_push_bool(json_value *obj, const json_char *name,
                                  bool v);
json_value *json_object_push_float(json_value *obj, const json_char *name,
                                   double v);

json_value *json_data_new(const void *data, uint32_t len, const char *mime);

json_value *json_int_array_new(const int *v, int nb);
json_value *json_float_array_new(const float *v, int nb);

int json_index(json_value *v);

/* Lookup a named field in a JSON object. Returns NULL if missing. */
json_value *json_obj_get(json_value *obj, const char *name);

/* Read [R,G,B] or [R,G,B,A] (0–255) into color; missing A defaults to 255. */
bool json_read_u8_rgba(json_value *v, uint8_t color[4]);

#endif // JSON_H
