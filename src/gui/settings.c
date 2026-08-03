/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
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
#include <errno.h>

#include "utils/ini.h"

/* Keys we do not manage; kept so settings_save does not wipe them. */
typedef struct {
    char *section;
    char *name;
    char *value;
} settings_extra_t;

static settings_extra_t *g_extras = NULL;
static int g_extras_nb = 0;

static void settings_extras_clear(void)
{
    int i;
    for (i = 0; i < g_extras_nb; i++) {
        free(g_extras[i].section);
        free(g_extras[i].name);
        free(g_extras[i].value);
    }
    free(g_extras);
    g_extras = NULL;
    g_extras_nb = 0;
}

static void settings_extras_add(const char *section, const char *name,
                                const char *value)
{
    settings_extra_t *e;
    g_extras = realloc(g_extras, (g_extras_nb + 1) * sizeof(*g_extras));
    e = &g_extras[g_extras_nb++];
    e->section = strdup(section);
    e->name = strdup(name);
    e->value = strdup(value);
}

static void settings_path(char *path, size_t size)
{
    const char *dir = sys_get_user_dir();
    size_t len;

    if (!dir) dir = "";
    snprintf(path, size, "%s", dir);
    len = strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        path[--len] = '\0';
    }
    snprintf(path + len, size - len, "/settings.ini");
}

static int shortcut_callback(action_t *action, void *user)
{
    if (!(action->flags & ACTION_CAN_EDIT_SHORTCUT)) return 0;
    gui_push_id(action->id);
    gui_text("%s: %s", action->id, action->help);
    gui_next_column();
    // XXX: need to check if the inputs are valid!
    gui_input_text("", action->shortcut, sizeof(action->shortcut));
    gui_next_column();
    gui_pop_id();
    return 0;
}


int gui_settings_popup(void *data)
{
    const char **names;
    theme_t *theme;
    int i, nb, current;
    theme_t *themes = theme_get_list();
    int ret = 0;

    if (gui_section_begin("Theme", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        DL_COUNT(themes, theme, nb);
        names = (const char**)calloc(nb, sizeof(*names));
        i = 0;
        DL_FOREACH(themes, theme) {
            if (strcmp(theme->name, theme_get()->name) == 0) current = i;
            names[i++] = theme->name;
        }
        if (gui_combo("##themes", &current, names, nb)) {
            theme_set(names[current]);
        }
        free(names);
    } gui_section_end();


    if (gui_section_begin("Paths", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        gui_text("Palettes: %s/palettes", sys_get_user_dir());
        gui_text("Progs: %s/progs", sys_get_user_dir());
    } gui_section_end();

    if (gui_section_begin("Shortcuts", GUI_SECTION_COLLAPSABLE_CLOSED)) {
        gui_columns(2);
        gui_separator();
        actions_iter(shortcut_callback, NULL);
        gui_separator();
        gui_columns(1);
    } gui_section_end();

    gui_popup_bottom_begin();
    if (gui_button("Save", 0, 0))
        settings_save();
    ret = gui_button("OK", 0, 0);
    gui_popup_bottom_end();
    return ret;
}

static int settings_ini_handler(void *user, const char *section,
                                const char *name, const char *value,
                                int lineno)
{
    action_t *a;
    if (strcmp(section, "ui") == 0) {
        if (strcmp(name, "theme") == 0) {
            theme_set(value);
            return 1;
        }
        if (strcmp(name, "hide_box") == 0) {
            goxel.hide_box = atoi(value) != 0;
            return 1;
        }
    }
    if (strcmp(section, "shortcuts") == 0) {
        if ((a = action_get_by_name(name))) {
            strncpy(a->shortcut, value, sizeof(a->shortcut) - 1);
        } else {
            LOG_W("Cannot set shortcut for unknown action '%s'", name);
        }
        return 1;
    }
    settings_extras_add(section, name, value);
    return 1;
}

void settings_load(void)
{
    char path[1024];
    settings_extras_clear();
    settings_path(path, sizeof(path));
    ini_parse(path, settings_ini_handler, NULL);
}

static int shortcut_save_callback(action_t *a, void *user)
{
    FILE *file = user;
    if (strcmp(a->shortcut, a->default_shortcut ?: "") != 0)
        fprintf(file, "%s=%s\n", a->id, a->shortcut);
    return 0;
}

void settings_save(void)
{
    char path[1024];
    FILE *file;
    int i;
    const char *prev_section = NULL;

    settings_path(path, sizeof(path));
    sys_make_dir(path);
    file = fopen(path, "w");
    if (!file) {
        LOG_E("Cannot save settings to %s: %s", path, strerror(errno));
        return;
    }
    fprintf(file, "[ui]\n");
    fprintf(file, "theme=%s\n", theme_get()->name);
    fprintf(file, "hide_box=%d\n", goxel.hide_box ? 1 : 0);

    fprintf(file, "[shortcuts]\n");
    actions_iter(shortcut_save_callback, file);

    for (i = 0; i < g_extras_nb; i++) {
        if (!prev_section || strcmp(prev_section, g_extras[i].section) != 0) {
            fprintf(file, "[%s]\n", g_extras[i].section);
            prev_section = g_extras[i].section;
        }
        fprintf(file, "%s=%s\n", g_extras[i].name, g_extras[i].value);
    }

    fclose(file);
}
