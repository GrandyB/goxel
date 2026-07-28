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
#include "utils/color.h"

/*
 * Filter to adjust the colors.
 */

typedef struct
{
    filter_t filter;
    float hue;
    float lightness;
    float saturation;
    float contrast;
} filter_colors_t;

static float hue_mod(float x, float y)
{
    while (x < 0)
        x += y;
    return fmod(x, y);
}

// Contrast 0 => grey, 1 => original color.
static void srgb_apply_contrast(uint8_t srgb[3], float contrast)
{
    float rgb[3];
    int i;

    srgb8_to_rgb(srgb, rgb);
    for (i = 0; i < 3; i++)
    {
        rgb[i] = (rgb[i] - 0.5f) * contrast + 0.5f;
        rgb[i] = clamp(rgb[i], 0.0, 1.0);
    }
    rgb_to_srgb8(rgb, srgb);
}

static void on_open(filter_t *filter_)
{
    filter_colors_t *filter = (void *)filter_;
    filter->hue = 0;
    filter->lightness = 0;
    filter->saturation = 0;
    filter->contrast = 0;
}

static void apply_values(void *args, uint8_t color[4])
{
    filter_colors_t *filter = args;
    float hsl[3];

    srgb8_to_hsl(color, hsl);
    hsl[0] = hue_mod(hsl[0] + filter->hue, 360);
    hsl_move_value(&hsl[1], filter->saturation / 100);
    hsl_move_value(&hsl[2], filter->lightness / 100);
    hsl_to_srgb8(hsl, color);
    srgb_apply_contrast(color, filter->contrast / 100 + 1);
}

static int gui(filter_t *filter_)
{
    filter_colors_t *filter = (void *)filter_;
    float hue = filter->hue;
    float lightness = filter->lightness;
    float saturation = filter->saturation;
    float contrast = filter->contrast;
    //bool changed;

    const char* help_text = "Color adjustment filter acts on the current layer as it was when the filter panel was opened, until you hit 're-acquire'. 'Reset' will reset to the state the volume had when this panel was opened. Both will reset the four values to 0.";
    goxel_set_help_text(help_text);
    
    slider_float("Hue", &hue, -180., +180., "%.1f");
    slider_float("Lightness", &lightness, -100., +100., "%.1f");
    slider_float("Saturation", &saturation, -100., +100., "%.1f");
    slider_float("Contrast", &contrast, -100., +100., "%.1f");

    //changed = hue != filter->hue || lightness != filter->lightness ||
    //          saturation != filter->saturation || contrast != filter->contrast;
    filter->hue = hue;
    filter->lightness = lightness;
    filter->saturation = saturation;
    filter->contrast = contrast;

    if (gui_button("Apply", -1, 0))
    {
        image_history_push(goxel.image);
        goxel_apply_color_filter(apply_values, filter);
    }

    if (gui_button("Reset sliders", -1, 0))
    {
        filter->hue = 0;
        filter->lightness = 0;
        filter->saturation = 0;
        filter->contrast = 0;
    }
    return 0;
}

FILTER_REGISTER(colors, filter_colors_t,
                .name = "Colors - H/S/L/C for layer",
                .on_open = on_open,
                .gui_fn = gui, )