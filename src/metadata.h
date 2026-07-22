/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 */

#ifndef METADATA_H
#define METADATA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef IMAGE_T_DEFINED
typedef struct image image_t;
#   define IMAGE_T_DEFINED
#endif
#ifndef RENDERER_T_DEFINED
typedef struct renderer renderer_t;
#   define RENDERER_T_DEFINED
#endif

typedef enum {
    CUSTOM_OBJ_POINT_2D = 0,
    CUSTOM_OBJ_POINT_3D,
    CUSTOM_OBJ_ZONE_2D,
    CUSTOM_OBJ_ZONE_3D,
    CUSTOM_OBJ_FLOAT,
    CUSTOM_OBJ_TEXT,
    CUSTOM_OBJ_COLOR,
    CUSTOM_OBJ_ENUM,
    CUSTOM_OBJ_GROUP,
} custom_object_type_t;

#define CUSTOM_OBJ_ENUM_OPTIONS_MAX 32
#define CUSTOM_OBJ_ENUM_OPTION_LEN  64

typedef struct custom_object custom_object_t;
struct custom_object {
    /* Leading int required: gui_list casts items to {int; next; prev;}. */
    int ref;
    custom_object_t *next, *prev;
    custom_object_t *group; /* Parent group, or NULL if top-level. */
    char name[128];
    custom_object_type_t type;
    uint8_t color[4];
    bool visible;
    /* World coords in goxel space (X/Y floor, Z height).
     * Points: p0 used; p1 unused.
     * Zones: p0 / p1 = inclusive AABB corners. */
    int p0[3], p1[3];
    /* Non-spatial value fields (type selects which is used). */
    float fvalue;
    char text_value[256];
    int enum_index;
    int enum_option_count;
    char enum_options[CUSTOM_OBJ_ENUM_OPTIONS_MAX][CUSTOM_OBJ_ENUM_OPTION_LEN];
    /* Groups: default type for the + add-child button. */
    custom_object_type_t default_child_type;
    /* Groups: when true, children cannot change type in the metadata UI. */
    bool lock_child_types_to_default;
};

void custom_objects_free_list(custom_object_t **list);
void custom_objects_copy_list(custom_object_t **dst, const custom_object_t *src);
custom_object_t *custom_object_add(image_t *img, custom_object_type_t type);
custom_object_t *custom_object_add_to_group(image_t *img, custom_object_t *group,
                                          custom_object_type_t type);
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

void custom_objects_set_list_selected(custom_object_t *obj);
custom_object_t *custom_objects_get_list_selected(void);
void custom_objects_toggle_solo(custom_object_t *obj);
custom_object_t *custom_objects_get_solo(void);

/* Binary blob for .gox CUST chunk. Caller frees *out. */
uint8_t *custom_objects_serialize(const image_t *img, int *out_len);
void custom_objects_deserialize(image_t *img, const uint8_t *data, int len);

void custom_objects_export_log(const image_t *img);
bool custom_objects_export_json(const image_t *img, const char *path);
bool custom_objects_load_template_json(const char *path, image_t *img);

const char *custom_object_type_name(custom_object_type_t type);
bool custom_object_is_spatial(custom_object_type_t type);
/* Map coords (bottom-left block = 0,0,0) <-> internal voxel coords. */
void custom_object_world_to_display(const image_t *img, const int world[3],
                                    int display[3]);
void custom_object_display_to_world(const image_t *img, const int display[3],
                                    int world[3]);
bool custom_object_is_group(custom_object_type_t type);
int custom_object_depth(const custom_object_t *obj);
bool custom_object_effectively_visible(const custom_object_t *obj);
void custom_object_effective_color(const custom_object_t *obj, uint8_t color[4]);

#endif /* METADATA_H */
