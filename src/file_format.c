/* Goxel 3D voxels editor
 *
 * copyright (c) 2020 Guillaume Chereau <guillaume@noctua-software.com>
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

#include "file_format.h"
#include "utlist.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// The global hash table of file formats.
file_format_t *file_formats = NULL;
file_format_t *file_formats_import_to_volume = NULL;
file_format_t *file_formats_export_to_volume = NULL;

static bool endswith(const char *str, const char *end)
{
    const char *start;
    if (strlen(str) < strlen(end)) return false;
    start = str + strlen(str) - strlen(end);
    return strcmp(start, end) == 0;
}


static int file_format_name_cmp(file_format_t *a, file_format_t *b)
{
    return strcasecmp(a->name, b->name);
}

void file_format_register(file_format_t *format)
{
    file_format_t *f;

    /* One doubly-linked list only. Formats used to also be linked into
     * file_formats_import_to_volume / file_formats_export_to_volume via the
     * same next/prev, which corrupted walks (e.g. kvx vanishing from placer
     * because it also has export_volume_func). Those heads are convenience
     * pointers to the first matching format in file_formats. */
    DL_APPEND(file_formats, format);
    DL_SORT(file_formats, file_format_name_cmp);

    file_formats_import_to_volume = NULL;
    file_formats_export_to_volume = NULL;
    DL_FOREACH(file_formats, f) {
        if (!file_formats_import_to_volume && f->import_volume_func)
            file_formats_import_to_volume = f;
        if (!file_formats_export_to_volume && f->export_volume_func)
            file_formats_export_to_volume = f;
    }
}

const file_format_t *file_format_for_path(const char *path, const char *name,
                                          const char *mode)
{
    const file_format_t *f;
    bool need_read = strchr(mode, 'r');
    bool need_read_volume = strchr(mode, 'v');
    bool need_write_volume = strchr(mode, 't');
    bool need_write = strchr(mode, 'w');
    const char *ext;

    assert(mode);
    assert(path || name);

    DL_FOREACH(file_formats, f) {
        if (need_read && !f->import_func) continue;
        if (need_write && !f->export_func) continue;
        if (need_read_volume && !f->import_volume_func) continue;
        if (need_write_volume && !f->export_volume_func) continue;
        if (name && strcasecmp(f->name, name) != 0) continue;
        if (path) {
            ext = f->exts[0] + 1; // Pick the string after '*'.
            if (!endswith(path, ext)) continue;
        }
        return f;
    }
    return NULL;
}

const file_format_t *file_format_by_name(const char *name) {
    const file_format_t *f;
    assert(name);
    DL_FOREACH(file_formats, f) {
        if (name && strcasecmp(f->name, name) == 0) return f;
    }
    return NULL;
}

void file_format_iter(const char *mode, void *user,
                      void (*fun)(void *user, file_format_t *f))
{
    assert(mode);
    assert(fun);
    file_format_t *f;
    bool need_read = strchr(mode, 'r');
    bool need_read_volume = strchr(mode, 'v');
    bool need_write = strchr(mode, 'w');
    bool need_write_volume = strchr(mode, 't');
    DL_FOREACH(file_formats, f) {
        if (need_read && !f->import_func) continue;
        if (need_write && !f->export_func) continue;
        if (need_read_volume && !f->import_volume_func) continue;
        if (need_write_volume && !f->export_volume_func) continue;
        fun(user, f);
    }
}
