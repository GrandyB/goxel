/* goxel_info - CLI tool to load a .gox file and print scene details as JSON.
 *
 * Usage: goxel_info <file.gox>
 *
 * Prints JSON to stdout describing layers, cameras, materials, and image box.
 */

#include "goxel.h"

static void print_json_string(const char *s) {
    printf("\"");
    for (; *s; s++) {
        switch (*s) {
        case '"':  printf("\\\""); break;
        case '\\': printf("\\\\"); break;
        case '\n': printf("\\n");  break;
        case '\r': printf("\\r");  break;
        case '\t': printf("\\t");  break;
        default:
            if ((unsigned char)*s < 0x20)
                printf("\\u%04x", (unsigned char)*s);
            else
                putchar(*s);
        }
    }
    printf("\"");
}

static void print_mat4(const float m[4][4]) {
    printf("[");
    for (int i = 0; i < 4; i++) {
        printf("[%g,%g,%g,%g]", m[i][0], m[i][1], m[i][2], m[i][3]);
        if (i < 3) printf(",");
    }
    printf("]");
}

static void log_to_stderr(void *user, const char *msg) {
    (void)user;
    fprintf(stderr, "%s\n", msg);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.gox>\n", argv[0]);
        return 1;
    }

    // Redirect all goxel logging to stderr so stdout is clean JSON.
    sys_callbacks.log = log_to_stderr;

    // Minimal init: shapes (for shape layers) + create image.
    shapes_init();
    goxel.image = image_new();

    if (load_from_file(argv[1], true) != 0) {
        fprintf(stderr, "Error: failed to load '%s'\n", argv[1]);
        return 1;
    }

    image_t *img = goxel.image;

    printf("{");

    // Image box
    printf("\"box\":");
    if (!box_is_null(img->box))
        print_mat4(img->box);
    else
        printf("null");

    // Layers
    printf(",\"layers\":[");
    {
        layer_t *layer;
        bool first = true;
        DL_FOREACH(img->layers, layer) {
            if (!first) printf(",");
            first = false;
            printf("{\"name\":");
            print_json_string(layer->name);
            printf(",\"visible\":%s", layer->visible ? "true" : "false");
            printf(",\"id\":%d", layer->id);

            if (layer->volume) {
                int aabb[2][3];
                bool has = volume_get_bbox(layer->volume, aabb, true);
                if (has) {
                    printf(",\"aabb\":[[%d,%d,%d],[%d,%d,%d]]",
                           aabb[0][0], aabb[0][1], aabb[0][2],
                           aabb[1][0], aabb[1][1], aabb[1][2]);
                    printf(",\"size\":[%d,%d,%d]",
                           aabb[1][0] - aabb[0][0],
                           aabb[1][1] - aabb[0][1],
                           aabb[1][2] - aabb[0][2]);
                } else {
                    printf(",\"aabb\":null,\"size\":null");
                }
                printf(",\"tiles\":%d", volume_get_tiles_count(layer->volume));
            } else {
                printf(",\"aabb\":null,\"size\":null,\"tiles\":0");
            }

            if (!box_is_null(layer->box)) {
                printf(",\"box\":");
                print_mat4(layer->box);
            }

            printf(",\"mat\":");
            print_mat4(layer->mat);

            if (layer->base_id)
                printf(",\"base_id\":%d", layer->base_id);
            if (layer->shape)
                printf(",\"shape\":\"%s\"", layer->shape->id);
            if (layer->material) {
                printf(",\"material\":");
                print_json_string(layer->material->name);
            }

            printf("}");
        }
    }
    printf("]");

    // Cameras
    printf(",\"cameras\":[");
    {
        camera_t *cam;
        bool first = true;
        DL_FOREACH(img->cameras, cam) {
            if (!first) printf(",");
            first = false;
            printf("{\"name\":");
            print_json_string(cam->name);
            printf(",\"active\":%s", cam == img->active_camera ? "true" : "false");
            printf(",\"dist\":%g", cam->dist);
            printf(",\"ortho\":%s", cam->ortho ? "true" : "false");
            printf(",\"fovy\":%g", cam->fovy);

            const char *mode_str = "orbit";
            if (cam->mode == CAMERA_MODE_FPV) mode_str = "fpv";
            else if (cam->mode == CAMERA_MODE_PLAYER) mode_str = "player";
            printf(",\"mode\":\"%s\"", mode_str);

            printf(",\"mat\":");
            print_mat4(cam->mat);
            printf("}");
        }
    }
    printf("]");

    // Materials
    printf(",\"materials\":[");
    {
        material_t *mat;
        bool first = true;
        DL_FOREACH(img->materials, mat) {
            if (!first) printf(",");
            first = false;
            printf("{\"name\":");
            print_json_string(mat->name);
            printf(",\"metallic\":%g", mat->metallic);
            printf(",\"roughness\":%g", mat->roughness);
            printf(",\"base_color\":[%g,%g,%g,%g]",
                   mat->base_color[0], mat->base_color[1],
                   mat->base_color[2], mat->base_color[3]);
            printf(",\"emission\":[%g,%g,%g]",
                   mat->emission[0], mat->emission[1], mat->emission[2]);
            printf("}");
        }
    }
    printf("]");

    // Light settings
    printf(",\"light\":{");
    printf("\"pitch\":%g", goxel.rend.light.pitch);
    printf(",\"yaw\":%g", goxel.rend.light.yaw);
    printf(",\"intensity\":%g", goxel.rend.light.intensity);
    printf(",\"fixed\":%s", goxel.rend.light.fixed ? "true" : "false");
    printf(",\"ambient\":%g", goxel.rend.settings.ambient);
    printf(",\"shadow\":%g", goxel.rend.settings.shadow);
    printf("}");

    printf("}\n");

    return 0;
}
