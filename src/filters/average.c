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
#include "palette.h"

#include <string.h>

/*
 * Average: collect map colour picks and offer 25% / 50% / 75% blends between
 * the darkest, mean, and brightest of the selection.
 */

#define AVERAGE_MAX_PICKED 256

typedef struct {
    filter_t filter;
    uint8_t picked[AVERAGE_MAX_PICKED][4];
    int picked_count;
    uint8_t stop_25[4];
    uint8_t stop_50[4];
    uint8_t stop_75[4];
    bool stops_valid;
} filter_average_t;

static int color_luminance(const uint8_t c[4])
{
    return 299 * (int)c[0] + 587 * (int)c[1] + 114 * (int)c[2];
}

static void color_midpoint(const uint8_t a[4], const uint8_t b[4],
                           uint8_t out[4])
{
    out[0] = (uint8_t)(((int)a[0] + (int)b[0] + 1) / 2);
    out[1] = (uint8_t)(((int)a[1] + (int)b[1] + 1) / 2);
    out[2] = (uint8_t)(((int)a[2] + (int)b[2] + 1) / 2);
    out[3] = 255;
}

static void recompute_stops(filter_average_t *filter)
{
    int i;
    int sum[3] = {0, 0, 0};
    int lo_i, hi_i, lum, lum_lo, lum_hi;
    uint8_t mean[4];

    filter->stops_valid = false;
    if (filter->picked_count < 2)
        return;

    lo_i = hi_i = 0;
    lum_lo = lum_hi = color_luminance(filter->picked[0]);
    for (i = 0; i < filter->picked_count; i++) {
        sum[0] += filter->picked[i][0];
        sum[1] += filter->picked[i][1];
        sum[2] += filter->picked[i][2];
        lum = color_luminance(filter->picked[i]);
        if (lum < lum_lo) {
            lum_lo = lum;
            lo_i = i;
        }
        if (lum >= lum_hi) {
            lum_hi = lum;
            hi_i = i;
        }
    }

    mean[0] = (uint8_t)((sum[0] + filter->picked_count / 2) /
                        filter->picked_count);
    mean[1] = (uint8_t)((sum[1] + filter->picked_count / 2) /
                        filter->picked_count);
    mean[2] = (uint8_t)((sum[2] + filter->picked_count / 2) /
                        filter->picked_count);
    mean[3] = 255;

    memcpy(filter->stop_50, mean, 4);
    color_midpoint(filter->picked[lo_i], mean, filter->stop_25);
    color_midpoint(mean, filter->picked[hi_i], filter->stop_75);
    filter->stops_valid = true;
}

static void reset_selection(filter_average_t *filter)
{
    filter->picked_count = 0;
    filter->stops_valid = false;
    memset(filter->picked, 0, sizeof(filter->picked));
    memset(filter->stop_25, 0, 4);
    memset(filter->stop_50, 0, 4);
    memset(filter->stop_75, 0, 4);
}

static void on_open(filter_t *filter_)
{
    reset_selection((filter_average_t *)filter_);
}

static void on_close(filter_t *filter_)
{
    reset_selection((filter_average_t *)filter_);
}

static void on_color_picked(filter_t *filter_, const uint8_t color[4])
{
    filter_average_t *filter = (void *)filter_;
    uint8_t c[4];

    if (!color || filter->picked_count >= AVERAGE_MAX_PICKED)
        return;
    memcpy(c, color, 4);
    c[3] = 255;
    memcpy(filter->picked[filter->picked_count], c, 4);
    filter->picked_count++;
    recompute_stops(filter);
}

static void set_brush_color(const uint8_t color[4])
{
    if (goxel.brush_source_mode == BRUSH_SOURCE_PALETTE) {
        goxel_brush_palette_clear();
        goxel.brush_source_mode = BRUSH_SOURCE_COLOR;
    }
    goxel.painter.color_inherit = false;
    memcpy(goxel.painter.color, color, 4);
    goxel.painter.color[3] = 255;
}

static void add_stop_to_palette(const uint8_t color[4])
{
    int before;

    if (!goxel.palette)
        return;
    if (palette_is_readonly(goxel.palette)) {
        gui_alert("Palette",
                  "Cannot add colours to the In-use colours palette.");
        return;
    }
    before = goxel.palette->size;
    palette_insert(goxel.palette, color, NULL);
    if (goxel.palette->size > before &&
        palette_save_user_gpl(goxel.palette) != 0) {
        gui_alert("Palette",
                  "Could not save the palette to your palettes folder.");
    }
}

static void stop_row(const char *label, const char *swatch_id,
                     const char *add_id, const uint8_t color[4],
                     bool can_add)
{
    int click;

    gui_text("%s", label);
    gui_same_line();
    click = gui_color_swatch(swatch_id, color, 22.f);
    if (click == 1)
        set_brush_color(color);
    gui_tooltip_if_hovered("Set the current brush colour to this swatch.");
    gui_same_line();
    gui_enabled_begin(can_add);
    if (gui_button(add_id, 0, 0))
        add_stop_to_palette(color);
    gui_enabled_end();
    gui_alert_if_disabled_clicked(
        can_add, "Palette",
        !goxel.palette ? "No palette is selected."
                       : "Cannot add colours to the In-use colours palette.");
}

static void picked_section(filter_average_t *filter)
{
    int i;
    int click;
    char id[32];

    if (filter->picked_count <= 0) {
        gui_text("No colours picked yet.");
        return;
    }

    gui_text("%d colour%s picked", filter->picked_count,
             filter->picked_count == 1 ? "" : "s");
    for (i = 0; i < filter->picked_count; i++) {
        if (i)
            gui_same_line();
        snprintf(id, sizeof(id), "##avg_picked_%d", i);
        click = gui_color_swatch(id, filter->picked[i], 18.f);
        if (click == 1)
            set_brush_color(filter->picked[i]);
    }
}

static int gui(filter_t *filter_)
{
    filter_average_t *filter = (void *)filter_;
    bool can_add;

    const char *help_text =
        "Pick colours from the map using Ctrl+Click or the picker tool and "
        "the filter will give you a selection of average colours between the "
        "chosen ones";

    goxel_set_help_text(help_text);

    if (gui_collapsing_header("Hint", true))
        gui_text_wrapped(help_text);

    if (gui_button("Reset selection", -1, 0))
        reset_selection(filter);

    picked_section(filter);
    gui_separator();

    if (filter->picked_count < 2 || !filter->stops_valid)
        return 0;

    can_add = goxel.palette && !palette_is_readonly(goxel.palette);

    stop_row("25%", "##avg_stop_25", "Add to palette##avg_25",
             filter->stop_25, can_add);
    stop_row("50%", "##avg_stop_50", "Add to palette##avg_50",
             filter->stop_50, can_add);
    stop_row("75%", "##avg_stop_75", "Add to palette##avg_75",
             filter->stop_75, can_add);

    return 0;
}

FILTER_REGISTER(average, filter_average_t,
                .name = "Color average",
                .menu = "effects",
                .submenu = "utilities",
                .on_open = on_open,
                .on_close = on_close,
                .on_color_picked = on_color_picked,
                .panel_width = GUI_PANEL_WIDTH_NORMAL + 40,
                .gui_fn = gui, )
