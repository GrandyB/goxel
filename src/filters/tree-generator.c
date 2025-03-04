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
#include <time.h>

/*
 * Filter to adjust the colors.
 */

static file_format_t *g_current = NULL;

typedef struct tree_model tree_model_t;
struct tree_model
{
    int             ref;
    tree_model_t    *next, *prev;
    const char      *file_name;
    const char      *path;
    volume_t        *volume;
};

typedef struct
{
    filter_t filter;
    int num_trees;
    int max_placement_attempts;

    tree_model_t *models;
    tree_model_t *active_model;
} filter_treegenerator_t;

static bool next_tree_pos(filter_treegenerator_t *filter, int *heights, int dimensions[3], int pos[3], int tree_height, int attempt) {
    int x = random_int(0, dimensions[0]-1);
    int y = random_int(0, dimensions[1]-1);
    int z = heights[y * dimensions[0] + x];
    //LOG_D("Attempt %i: %i/%i/%i", attempt, x, y, z);
    if (attempt > filter->max_placement_attempts) {
        LOG_D("Attempted %i times to acquire suitable placement, stopping", filter->max_placement_attempts);
        return false;
    } else if (z + tree_height >= dimensions[2] || z == -1 || z == 0) {
        attempt += 1;
        return next_tree_pos(filter, heights, dimensions, pos, tree_height, attempt);
    } else {
        pos[0] = x;
        pos[1] = y;
        pos[2] = z+1;
    }
    return true;
}

static tree_model_t* choose_random_tree_model(filter_treegenerator_t *filter) {
    tree_model_t *iter;
    int i, idx, count;
    DL_COUNT(filter->models, iter, count);
    //LOG_D("Count: %i", count);
    
    if (count == 0) return NULL;
    idx = count == 1 ? 0 : random_int(0, count-1);
    i = 0;
    
    DL_FOREACH(filter->models, iter) {
        //LOG_D("Log: %i == %i ? %s", i, idx, iter->file_name);
        if (i == idx) break;
        i++;
    }
    return iter;
}

static int get_tree_height(tree_model_t *model) {
    float box[4][4];
    int dimensions[3];
    volume_get_box(model->volume, false, box);
    box_get_dimensions(box, dimensions);
    return dimensions[2];
}

static void place_trees(filter_treegenerator_t *filter) {
    float box[4][4];
        int dimensions[3], start_pos[3], *heights;

        mat4_copy(goxel.image->box, box);
        box_get_dimensions(box, dimensions);
        box_get_start_pos(box, start_pos);

        if (dimensions[0] == 0 || dimensions[1] == 0) {
            LOG_W("Image has a 0 dimension, not running the script");
            return;
        }

        clock_t start = clock();  // Start timing
        allocate_heights(dimensions, &heights);
        volume_get_heights_in_box(goxel_get_layers_volume(goxel.image), dimensions, start_pos, heights);
        clock_t end = clock();    // End timing
        double elapsed_time = (((double)(end - start)) / CLOCKS_PER_SEC) * 1000;
        printf("Function took %.3fms\n", elapsed_time);

        int treeIndex, tree_pos[3];
        //uint8_t color[4] = { 0, 0, 0, 255 };
        for (treeIndex = 0; treeIndex < filter->num_trees; treeIndex++) {
            tree_model_t *tree = choose_random_tree_model(filter);
            if (!tree) {
                LOG_E("Unable to acquire a tree model from the list");
                return;
            }
            int tree_height = get_tree_height(tree);
            //LOG_D("Random tree: '%s', height: %i", tree->file_name, tree_height);

            if(!next_tree_pos(filter, heights, dimensions, tree_pos, tree_height, 0)) break;
            tree_pos[0] += start_pos[0];
            tree_pos[1] += start_pos[1];
            tree_pos[2] += start_pos[2];
            //LOG_D("Adding tree at %i/%i/%i", tree_pos[0], tree_pos[1], tree_pos[2]);
            volume_t *tree_clone = volume_copy(tree->volume);
            float trans[4][4] = MAT4_IDENTITY;
            trans[3][0] = tree_pos[0];
            trans[3][1] = tree_pos[1];
            trans[3][2] = tree_pos[2];
            volume_move(tree_clone, trans);
            volume_merge(goxel.image->active_layer->volume, tree_clone, MODE_OVER, NULL);
            //volume_set_at(goxel.image->active_layer->volume, NULL, tree_pos, color);
        }
        
}

static bool render_model_list_item(void *item, int idx, bool current)
{
    tree_model_t *model = item;
    
    //char str[12];  // Enough for any 32-bit int
    //sprintf(str, "%d", idx);
    _model_item(idx, &current, model->file_name, sizeof(model->file_name));
    return current;
}

// Copied from placer.c
static void on_file_import(const char *path, const char *file_name, const file_format_t *format)
{
    // Do nothing

}
static void add_model(filter_treegenerator_t *filter, const char *file_name, const char *path, volume_t *vol)
{
    tree_model_t *new_model;
    new_model = calloc(1, sizeof(*new_model));
    *new_model = (tree_model_t) {
        .path = path,
        .file_name = file_name,
        .volume = vol
    };
    DL_APPEND(filter->models, new_model);
    filter->active_model = new_model;
}
static const char *make_label(const file_format_t *f, char *buf, int len)
{
    const char *ext = f->exts[0] + 1;
    snprintf(buf, len, "%s (%s)", f->name, ext);
    return buf;
}
static void on_format(void *user, file_format_t *f)
{
    char label[128];
    make_label(f, label, sizeof(label));
    if (gui_combo_item(label, f == g_current)) {
        g_current = f;
    }
}
static int gui(filter_t *filter_)
{
    filter_treegenerator_t *filter = (void *)filter_;

    gui_list(&(gui_list_t) {
        .items = (void**)&filter->models,
        .current = (void**)&filter->active_model,
        .render = render_model_list_item,
    });

    if (gui_button("Remove selected", 0, 0) && filter->active_model) {
        volume_delete(filter->active_model->volume);
        DL_DELETE(filter->models, filter->active_model);
        filter->active_model = NULL;
    }

    // File importer
    char label[128];
    gui_text("Import as");
    if (!g_current) g_current = file_formats_import_to_volume; // First one.

    make_label(g_current, label, sizeof(label));
    if (gui_combo_begin("Import as", label)) {
        file_format_iter("v", NULL, on_format);
        gui_combo_end();
    }

    if (g_current->import_gui)
        g_current->import_gui(g_current);

    if (gui_button("Import", 1, 0)) {
        const char *path;

        if (!g_current) return -1;
        path = strdup(sys_open_file_dialog("Import", NULL, g_current->exts, g_current->exts_desc));
        if (!path) return -1;
        const char *file_name = strdup(get_file_name_from_path(path));
        volume_t *vol = volume_new();
        goxel_import_file_to_volume(path, g_current->name, vol, on_file_import);
        add_model(filter, file_name, path, vol);
    }

    gui_separator();
    
    gui_group_begin("##settings");
    gui_input_int("# of trees", &filter->num_trees, 0, 9999);
    gui_input_int("Attempt limit", &filter->max_placement_attempts, 0, 999);
    gui_group_end();

    gui_separator();

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        place_trees(filter);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_treegenerator_t *filter = (void *)filter_;
    filter->num_trees = 8;
    filter->max_placement_attempts = 20;
}

FILTER_REGISTER(colors, filter_treegenerator_t,
                .name = "Generation - Tree placement",
                .on_open = on_open,
                .gui_fn = gui, )