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

#include "goxel.h"

static void sync_name_field(const palette_t *cur, palette_t **synced,
                            char *name_buf, int name_buf_size)
{
    if (cur != *synced) {
        *synced = (palette_t *)cur;
        snprintf(name_buf, name_buf_size, "%s", cur->name);
    }
}

static void palette_persist_or_alert(void)
{
    if (palette_save_user_gpl(goxel.palette) != 0) {
        gui_alert("Palette",
                  "Could not save the palette to your palettes folder.");
    }
}

void gui_palette_panel(void)
{
    int nb, i;
    int pl_combo;
    int swatch_idx;
    const palette_t *p;
    const char **names;
    gui_icon_info_t *grid;
    static char name_buf[128];
    static palette_t *name_sync_palette;

    sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                    (int)sizeof(name_buf));

    DL_COUNT(goxel.palettes, p, nb);
    names = (const char **)calloc(nb, sizeof(*names));

    pl_combo = 0;
    i = 0;
    DL_FOREACH(goxel.palettes, p) {
        if (p == goxel.palette)
            pl_combo = i;
        names[i++] = p->name;
    }

    gui_row_begin(2);
    if (gui_combo("##palettes", &pl_combo, names, nb)) {
        goxel.palette = goxel.palettes;
        for (i = 0; i < pl_combo; i++)
            goxel.palette = goxel.palette->next;
        name_sync_palette = NULL;
        sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                        (int)sizeof(name_buf));
    }
    if (gui_button("Delete palette", -1, 0)) {
        palette_t *victim = goxel.palette;
        palette_t *next_sel;
        palette_t *cnt_it;
        int pal_count;

        DL_COUNT(goxel.palettes, cnt_it, pal_count);
        if (pal_count <= 1) {
            gui_alert("Palette", "Cannot delete the last palette.");
        } else {
            next_sel = victim->next ? victim->next : victim->prev;
            palette_list_remove(&goxel.palettes, victim);
            goxel.palette = next_sel;
            name_sync_palette = NULL;
            sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                            (int)sizeof(name_buf));
        }
    }
    gui_tooltip_if_hovered(
            "Delete the whole palette from the list (not a file on disk).");
    gui_row_end();
    free(names);

    gui_input_text("##palette_name", name_buf, sizeof(name_buf));

    gui_row_begin(3);
    if (gui_button("Rename", -1, 0)) {
        if (name_buf[0] == '\0') {
            gui_alert("Palette", "Enter a palette name.");
        } else if (palette_name_in_use(goxel.palettes, name_buf,
                                       goxel.palette)) {
            gui_alert("Palette", "A palette with that name already exists.");
        } else {
            snprintf(goxel.palette->name, sizeof(goxel.palette->name), "%s",
                     name_buf);
        }
    }
    if (gui_button("Copy", -1, 0)) {
        palette_t *copy;

        if (name_buf[0] == '\0') {
            gui_alert("Palette", "Enter a name for the new palette.");
        } else if (palette_name_in_use(goxel.palettes, name_buf, NULL)) {
            gui_alert("Palette", "A palette with that name already exists.");
        } else {
            copy = palette_clone(goxel.palette, name_buf);
            DL_APPEND(goxel.palettes, copy);
            goxel.palette = copy;
            name_sync_palette = NULL;
            sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                            (int)sizeof(name_buf));
            palette_persist_or_alert();
        }
    }
    if (gui_button("New", -1, 0)) {
        palette_t *fresh;

        if (name_buf[0] == '\0') {
            gui_alert("Palette", "Enter a name for the new palette.");
        } else if (palette_name_in_use(goxel.palettes, name_buf, NULL)) {
            gui_alert("Palette", "A palette with that name already exists.");
        } else {
            fresh = palette_new_empty(name_buf);
            DL_APPEND(goxel.palettes, fresh);
            goxel.palette = fresh;
            name_sync_palette = NULL;
            sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                            (int)sizeof(name_buf));
            palette_persist_or_alert();
        }
    }
    gui_row_end();

    p = goxel.palette;
    {
        int psz = p->size;

        if (psz < 0)
            psz = 0;

        swatch_idx = 0;
        if (psz > 0) {
            swatch_idx = palette_search(p, goxel.painter.color, true);
            if (swatch_idx < 0)
                swatch_idx = 0;
        } else {
            swatch_idx = -1;
        }

        {
            size_t n = (size_t)(unsigned)psz;

            grid = n ? calloc(n, sizeof(*grid)) : NULL;
        }
        for (i = 0; i < psz; i++) {
            grid[i] = (gui_icon_info_t) {
                .label = p->entries[i].name,
                .icon = 0,
                .color = {VEC4_SPLIT(p->entries[i].color)},
            };
            if (memcmp(goxel.painter.color, p->entries[i].color, 4) == 0)
                swatch_idx = i;
        }
        if (gui_icons_grid(psz, grid, &swatch_idx)) {
            memcpy(goxel.painter.color, p->entries[swatch_idx].color, 4);
        }
        free(grid);
    }

    gui_row_begin(2);
    if (gui_button("Add current color", -1, 0)) {
        int n_before = goxel.palette->size;

        palette_insert(goxel.palette, goxel.painter.color, NULL);
        if (goxel.palette->size > n_before)
            palette_persist_or_alert();
    }
    if (gui_button("Remove selected", -1, 0)) {
        uint8_t removed[4];

        if (goxel.palette->size <= 0) {
            gui_alert("Palette", "This palette has no colors to remove.");
        } else if (swatch_idx < 0 || swatch_idx >= goxel.palette->size) {
            gui_alert("Palette", "No swatch is selected.");
        } else {
            memcpy(removed, goxel.palette->entries[swatch_idx].color, 4);
            palette_remove_at(goxel.palette, swatch_idx);
            if (memcmp(goxel.painter.color, removed, 4) == 0 &&
                goxel.palette->size > 0) {
                int ni = swatch_idx;
                if (ni >= goxel.palette->size)
                    ni = goxel.palette->size - 1;
                memcpy(goxel.painter.color,
                       goxel.palette->entries[ni].color, 4);
            }
            palette_persist_or_alert();
        }
    }
    gui_tooltip_if_hovered("Remove the highlighted color swatch from the "
                           "current palette.");
    gui_row_end();
}

void gui_palette_floating(void)
{
    if (!gui_palette_window_begin(280.f, 400.f))
        return;
    gui_palette_panel();
    gui_palette_window_end();
}
