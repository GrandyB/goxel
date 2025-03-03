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

#include <string.h>
#include <stddef.h>
#include "goxel.h"
#include "file_format.h"

#ifndef GUI_CUSTOM_EXPORT_PANEL

#if 0

Keep this here as a reference until I fix file format names and order.

    {"glTF (.gltf)", "export_as_gltf"},
    {"Wavefront (.obj)", "export_as_obj"},
    {"Stanford (.pny)", "export_as_ply"},
    {"Png", "export_as_png"},
    {"Magica voxel (.vox)", "export_as_vox"},
    {"Qubicle (.qb)", "export_as_qubicle"},
    {"Slab (.kvx)", "export_as_kvx"},
    {"Spades (.vxl)", "export_as_vxl"},
    {"Png slices (.png)", "export_as_png_slices"},
    {"Plain text (.txt)", "export_as_txt"},

#endif

static file_format_t *g_current = NULL;

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

static const char* dumb_basename(const char* path)
{
    size_t length = strlen(path);
    size_t offset = 0;

    for (size_t i = 0; i < length; i++) {
        int is_separator = path[i] == '/' || path[i] == '\\';

        if (is_separator && i != length - 1) {
            offset = i + 1;
        }
    }

    return path + offset;
}

static int choose_export_path()
{
    const file_format_t *f = file_format_for_path(NULL, g_current->name, "w");
    const char* chosen_path = sys_get_save_path("", f->exts, f->exts_desc);
    if (!chosen_path)
        return 1;

    free((void*) goxel.last_export_panel_path);
    goxel.last_export_panel_path = strdup(chosen_path);

    return 0;
}

void gui_export_panel(void)
{
    char label[128];
    gui_text("Export as");
    if (!g_current) g_current = file_formats; // First one.

    make_label(g_current, label, sizeof(label));
    if (gui_combo_begin("Export as", label)) {
        file_format_iter("w", NULL, on_format);
        gui_combo_end();
    }

    if (g_current->export_gui)
        g_current->export_gui(g_current);

    if (gui_button("Export As...", 1, 0)) {
        if (choose_export_path() == 0) {
            goxel_export_to_file(goxel.last_export_panel_path, g_current->name);
        }
    }

    char export_label[128];
    if (goxel.last_export_panel_path) {
        snprintf(export_label, 128, "Export %s", dumb_basename(goxel.last_export_panel_path));
    } else {
        strcpy(export_label, "Export");
    }

    if (gui_button(export_label, 1, 0)) {
        if (!goxel.last_export_panel_path) {
            if (choose_export_path() == 1) {
                return;
            }
        }

        goxel_export_to_file(goxel.last_export_panel_path, g_current->name);
    }
}

#endif // GUI_CUSTOM_EXPORT_PANEL
