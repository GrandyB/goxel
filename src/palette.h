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

// XXX: probably need to redo the code here.

#ifndef PALETTE_H
#define PALETTE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  color[4];
    char     name[256];
} palette_entry_t;

typedef struct palette palette_t;
struct palette {
    palette_t *next, *prev; // For the global list of palettes.
    char    name[128];
    int     columns;
    int     size;
    int     allocated;
    palette_entry_t *entries;
};

// Load all the available palettes into a list.
void palette_load_all(palette_t **list);

/*
 * Free every palette in *list, reload from disk/assets, then optionally find
 * one whose name equals prefer_name. Returns that palette or NULL (caller may
 * fall back to the list head).
 */
palette_t *palette_reload_all(palette_t **list, const char *prefer_name);

/*
 * Function: palette_search
 * Search a given color in a palette
 *
 * Parameters:
 *   palette    - A palette.
 *   col        - The color we are looking for.
 *   exact      - If set to true, return -1 if no color is found, else
 *                return the closest color.
 *
 * Return:
 *   The index of the color in the palette.
 */
int palette_search(const palette_t *palette, const uint8_t col[4],
                   bool exact);

void palette_insert(palette_t *p, const uint8_t col[4], const char *name);

/* Remove one entry by index; shifts later entries down. */
void palette_remove_at(palette_t *p, int idx);

/* Remove all entries; keeps the entries buffer allocated for reuse. */
void palette_clear(palette_t *p);

void palette_free(palette_t *p);
palette_t *palette_clone(const palette_t *src, const char *new_name);
palette_t *palette_new_empty(const char *name);
void palette_list_remove(palette_t **list_head, palette_t *p);
bool palette_name_in_use(const palette_t *list, const char *name,
                         const palette_t *except);

/*
 * Write palette as GIMP .gpl under sys_get_user_dir()/palettes/.
 * Returns 0 on success, -1 if no user dir, -2 on I/O error.
 */
int palette_save_user_gpl(const palette_t *p);

/*
 * Remove the palette's .gpl under sys_get_user_dir()/palettes/ using the same
 * basename rule as palette_save_user_gpl. Missing file is OK.
 * Returns 0 on success or nothing to remove, -2 if deletion failed.
 */
int palette_delete_user_gpl(const palette_t *p);

/*
 * Same as palette_delete_user_gpl, but basename is derived from display name
 * (e.g. the previous palette name after a rename).
 */
int palette_delete_user_gpl_named(const char *palette_display_name);

/*
 * After a palette rename once the new content is saved under the new name:
 * removes the obsolete .gpl for old_display_name when it would map to a
 * different filename than new_display_name (same basename ⇒ no deletion).
 */
void palette_remove_obsolete_gpl_after_rename(const char *old_display_name,
                                              const char *new_display_name);

#endif // PALETTE_H
