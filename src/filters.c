/* Goxel 3D voxels editor
 *
 * copyright (c) 2024 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along
 * with goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"
#include "../ext_src/stb/stb_ds.h"
#include <stdlib.h> // For qsort
#include <string.h> // For strcasecmp (POSIX) / _stricmp (Windows)

// stb array of registered filters.
static filter_t **g_filters = NULL;

static void a_filter_toggle(void *data)
{
    filter_t *filter = data;
    LOG_D("Toggle filter %s", filter->name);
    filter->is_open = !filter->is_open;

    if (filter->is_open && filter->on_open)
    {
        filter->on_open(filter);
    }

    if (!filter->is_open && filter->on_close)
    {
        filter->on_close(filter);
    }
}

void filter_register_(filter_t *filter)
{
    LOG_D("registering action: %s", filter->action_id);
    action_t action;
    action = (action_t){
        .id = filter->action_id,
        .default_shortcut = filter->default_shortcut,
        .cfunc_data = a_filter_toggle,
        .data = (void *)filter,
        .flags = ACTION_CAN_EDIT_SHORTCUT,
    };
    action_register(&action, 0);
    arrput(g_filters, filter);
}

// Comparison function for sorting filters alphabetically (case-insensitive)
int compare_filters(const void *a, const void *b) {
    const filter_t *filterA = *(const filter_t **)a;
    const filter_t *filterB = *(const filter_t **)b;

#ifdef _WIN32
    return _stricmp(filterA->name, filterB->name);  // Windows case-insensitive comparison
#else
    return strcasecmp(filterA->name, filterB->name);  // POSIX (Linux/macOS)
#endif
}

// Iterate through filters, sorted alphabetically (ignoring case)
void filters_iter_all(void *arg, void (*f)(void *arg, filter_t *filter)) {
    int i;
    
    // Sort filters alphabetically (ignoring case)
    qsort(g_filters, arrlen(g_filters), sizeof(filter_t *), compare_filters);

    // Iterate over sorted filters
    for (i = 0; i < arrlen(g_filters); i++) {
        f(arg, g_filters[i]);
    }
}