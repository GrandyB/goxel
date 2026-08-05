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
    if (palette_is_readonly(goxel.palette))
        return;
    if (palette_save_user_gpl(goxel.palette) != 0) {
        gui_alert("Palette",
                  "Could not save the palette to your palettes folder.");
    }
}

static void palette_panel_new_user(char *name_buf, int name_buf_size,
                                   palette_t **name_sync_palette)
{
    palette_t *fresh;
    char unique[128];

    if (palette_is_readonly(goxel.palette)) {
        palette_make_unique_name(goxel.palettes, "Palette", unique,
                                 (int)sizeof(unique));
        snprintf(name_buf, name_buf_size, "%s", unique);
    } else if (name_buf[0] == '\0') {
        gui_alert("Palette", "Enter a name for the new palette.");
        return;
    } else if (palette_name_in_use(goxel.palettes, name_buf, NULL)) {
        gui_alert("Palette", "A palette with that name already exists.");
        return;
    }

    fresh = palette_new_empty(name_buf);
    DL_APPEND(goxel.palettes, fresh);
    goxel.palette = fresh;
    *name_sync_palette = NULL;
    sync_name_field(goxel.palette, name_sync_palette, name_buf, name_buf_size);
    palette_persist_or_alert();
}

void gui_palette_panel(void)
{
    int nb, i;
    int swatch_idx;
    int click;
    bool readonly;
    bool in_palette_mode;
    const palette_t *p;
    const palette_t *it;
    gui_icon_info_t *grid = NULL;
    bool *multi_sel = NULL;
    static char name_buf[128];
    static palette_t *name_sync_palette;
    const char *preview;

    if (!goxel.palette)
        return;

    sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                    (int)sizeof(name_buf));
    readonly = palette_is_readonly(goxel.palette);
    in_palette_mode = (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE &&
                       goxel.brush_palette_count > 0);

    DL_COUNT(goxel.palettes, it, nb);
    preview = goxel.palette->name;

    gui_row_begin(2);
    if (gui_combo_begin("##palettes", preview)) {
        i = 0;
        DL_FOREACH(goxel.palettes, it) {
            if (i == 1 && nb > 1 &&
                strcmp(goxel.palettes->name, PALETTE_IN_USE_NAME) == 0)
                gui_combo_separator();
            if (gui_combo_item(it->name, it == goxel.palette)) {
                goxel.palette = (palette_t *)it;
                name_sync_palette = NULL;
                sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                                (int)sizeof(name_buf));
            }
            i++;
        }
        gui_combo_end();
    }
    gui_same_line();
    gui_enabled_begin(!readonly);
    if (gui_button("Delete palette", -1, 0)) {
        palette_t *victim = goxel.palette;
        palette_t *next_sel;
        palette_t *cnt_it;
        int pal_count;

        DL_COUNT(goxel.palettes, cnt_it, pal_count);
        if (palette_is_readonly(victim)) {
            gui_alert("Palette", "Cannot delete this palette.");
        } else if (pal_count <= 1) {
            gui_alert("Palette", "Cannot delete the last palette.");
        } else {
            next_sel = victim->next ? victim->next : victim->prev;
            if (palette_delete_user_gpl(victim) != 0) {
                gui_alert("Palette",
                          "Could not delete the palette file from your palettes "
                          "folder.");
            } else {
                palette_list_remove(&goxel.palettes, victim);
                goxel.palette = next_sel;
                name_sync_palette = NULL;
                sync_name_field(goxel.palette, &name_sync_palette, name_buf,
                                (int)sizeof(name_buf));
            }
        }
    }
    gui_enabled_end();
    gui_tooltip_if_hovered("Remove this palette from the list and delete its "
                           ".gpl file from your palettes folder.");
    gui_row_end();

    if (readonly) {
        char count_buf[32];
        float count_w, name_w;
        int n = goxel.palette->size;

        if (n < 0)
            n = 0;
        snprintf(count_buf, sizeof(count_buf), "(%d)", n);
        count_w = gui_calc_text_width(count_buf);
        name_w = gui_content_avail_x() - count_w - 8.f;
        if (name_w < 40.f)
            name_w = 40.f;
        gui_enabled_begin(false);
        gui_input_text_row("##palette_name", name_buf, (int)sizeof(name_buf),
                           name_w, 0.f);
        gui_enabled_end();
        gui_same_line();
        gui_text("%s", count_buf);
        gui_tooltip_if_hovered(
                "Number of unique colours currently used in the scene.");
    } else {
        gui_input_text("##palette_name", name_buf, sizeof(name_buf));
    }

    gui_row_begin(3);
    gui_enabled_begin(!readonly);
    if (gui_button("Rename", -1, 0)) {
        if (name_buf[0] == '\0') {
            gui_alert("Palette", "Enter a palette name.");
        } else if (palette_name_in_use(goxel.palettes, name_buf,
                                       goxel.palette)) {
            gui_alert("Palette", "A palette with that name already exists.");
        } else {
            char old_name[sizeof(goxel.palette->name)];

            snprintf(old_name, sizeof(old_name), "%s", goxel.palette->name);
            snprintf(goxel.palette->name, sizeof(goxel.palette->name), "%s",
                     name_buf);
            if (palette_save_user_gpl(goxel.palette) != 0) {
                snprintf(goxel.palette->name, sizeof(goxel.palette->name), "%s",
                         old_name);
                snprintf(name_buf, sizeof(name_buf), "%s", old_name);
                gui_alert("Palette",
                          "Could not save the palette to your palettes folder.");
            } else {
                palette_remove_obsolete_gpl_after_rename(old_name, name_buf);
            }
        }
    }
    gui_enabled_end();
    gui_enabled_begin(!readonly);
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
    gui_enabled_end();
    /* New stays enabled on the readonly In-use palette. */
    if (gui_button("New", -1, 0)) {
        palette_panel_new_user(name_buf, (int)sizeof(name_buf),
                               &name_sync_palette);
    }
    gui_row_end();

    p = goxel.palette;
    {
        int psz = p->size;

        if (psz < 0)
            psz = 0;

        swatch_idx = -1;
        if (psz > 0 && !in_palette_mode) {
            swatch_idx = palette_search(p, goxel.painter.color, true);
        }

        if (psz > 0) {
            grid = calloc((size_t)psz, sizeof(*grid));
            multi_sel = calloc((size_t)psz, sizeof(*multi_sel));
        }
        for (i = 0; i < psz; i++) {
            grid[i] = (gui_icon_info_t) {
                .label = p->entries[i].name,
                .icon = 0,
                .color = {VEC4_SPLIT(p->entries[i].color)},
            };
            if (in_palette_mode)
                multi_sel[i] = goxel_brush_palette_contains(p->entries[i].color);
            else if (memcmp(goxel.painter.color, p->entries[i].color, 4) == 0)
                swatch_idx = i;
        }
        click = gui_color_swatches_grid(psz, grid,
                                        in_palette_mode ? multi_sel : NULL,
                                        &swatch_idx);
        if (click == 2 && swatch_idx >= 0 && swatch_idx < psz) {
            goxel_brush_palette_shift_click(p->entries[swatch_idx].color);
        } else if (click == 1 && swatch_idx >= 0 && swatch_idx < psz) {
            goxel_brush_palette_clear();
            goxel.brush_source_mode = BRUSH_SOURCE_COLOR;
            if (gui_pick_rgb_keep_alpha()) {
                painter_color_apply_rgb_keep_alpha(
                        goxel.painter.color, p->entries[swatch_idx].color);
            } else {
                memcpy(goxel.painter.color, p->entries[swatch_idx].color, 4);
            }
        }
        free(grid);
        free(multi_sel);
    }

    gui_row_begin(2);
    gui_enabled_begin(!readonly);
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
        } else if (in_palette_mode) {
            gui_alert("Palette",
                      "Exit multi-colour mode before removing a swatch.");
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
    gui_enabled_end();
    gui_tooltip_if_hovered("Remove the highlighted color swatch from the "
                           "current palette.");
    gui_row_end();

    gui_row_begin(1);
    gui_enabled_begin(!readonly);
    if (gui_button("Clear all colours", -1, 0)) {
        if (goxel.palette->size > 0) {
            palette_clear(goxel.palette);
            palette_persist_or_alert();
        }
    }
    gui_enabled_end();
    gui_tooltip_if_hovered("Remove every color swatch from the current "
                           "palette.");
    gui_row_end();
}

void gui_palette_floating(void)
{
    if (!gui_palette_window_begin(280.f, 400.f))
        return;
    gui_palette_panel();
    gui_palette_window_end();
}
