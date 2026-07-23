/* Goxel 3D voxels editor
 *
 * copyright (c) 2024-present Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "goxel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Flat 1-block water sheet on the image-box bottom. Colour is painted with a
 * single domain-warped directional FBM field (swells + ripples + foam crests).
 * Always overwrites the bottom voxel of each column.
 */

typedef struct {
    uint8_t color[4];       /* mid water */
    uint8_t deep_color[4];  /* troughs */
    uint8_t foam_color[4];  /* bright crests */
    float scale;            /* feature size in blocks (higher = larger swells) */
    float direction_deg;    /* swell travel direction */
    float stretch;          /* anisotropy along swell (>1 = elongated waves) */
    float warp;             /* domain-warp strength */
    float detail;           /* fine ripple mix 0..1 */
    float foam;             /* crest foam amount / sharpness 0..1 */
    float contrast;         /* overall deep↔foam separation 0..1 */
    int seed;
} water_layer_settings_t;

typedef struct {
    const char *name;
    water_layer_settings_t settings;
} water_layer_preset_t;

typedef struct {
    filter_t filter;
    water_layer_settings_t settings;
    int preset_index;
} filter_water_layer_t;

static const water_layer_settings_t default_settings = {
    .color = {48, 96, 118, 255},
    .deep_color = {28, 58, 78, 255},
    .foam_color = {170, 210, 220, 255},
    .scale = 28.0f,
    .direction_deg = 28.0f,
    .stretch = 2.2f,
    .warp = 0.45f,
    .detail = 0.35f,
    .foam = 0.4f,
    .contrast = 0.75f,
    .seed = 0,
};

static const water_layer_preset_t presets[] = {
    {
        .name = "Open ocean",
        .settings = {
            .color = {40, 85, 115, 255},
            .deep_color = {18, 45, 70, 255},
            .foam_color = {135, 156, 163, 255},
            .scale = 32.0f,
            .direction_deg = 35.0f,
            .stretch = 2.8f,
            .warp = 0.25f,
            .detail = 0.4f,
            .foam = 0.3f,
            .contrast = 0.65f,
            .seed = 42,
        },
    },
    {
        .name = "Calm lake",
        .settings = {
            .color = {55, 105, 125, 255},
            .deep_color = {35, 70, 90, 255},
            .foam_color = {150, 195, 210, 255},
            .scale = 48.0f,
            .direction_deg = 15.0f,
            .stretch = 1.6f,
            .warp = 0.25f,
            .detail = 0.2f,
            .foam = 0.2f,
            .contrast = 0.55f,
            .seed = 11,
        },
    },
    {
        .name = "Choppy sea",
        .settings = {
            .color = {45, 80, 105, 255},
            .deep_color = {22, 42, 60, 255},
            .foam_color = {200, 225, 235, 255},
            .scale = 18.0f,
            .direction_deg = 50.0f,
            .stretch = 1.8f,
            .warp = 0.7f,
            .detail = 0.65f,
            .foam = 0.7f,
            .contrast = 0.9f,
            .seed = 77,
        },
    },
    {
        .name = "Tropical shallows",
        .settings = {
            .color = {55, 145, 155, 255},
            .deep_color = {30, 100, 120, 255},
            .foam_color = {210, 240, 245, 255},
            .scale = 36.0f,
            .direction_deg = 10.0f,
            .stretch = 2.0f,
            .warp = 0.4f,
            .detail = 0.3f,
            .foam = 0.35f,
            .contrast = 0.7f,
            .seed = 3,
        },
    },
    {
        .name = "Murky harbour",
        .settings = {
            .color = {55, 75, 70, 255},
            .deep_color = {35, 48, 42, 255},
            .foam_color = {140, 155, 145, 255},
            .scale = 40.0f,
            .direction_deg = 5.0f,
            .stretch = 1.4f,
            .warp = 0.35f,
            .detail = 0.25f,
            .foam = 0.15f,
            .contrast = 0.5f,
            .seed = 19,
        },
    },
};

static const int preset_count = (int)(sizeof(presets) / sizeof(presets[0]));

static void settings_copy(water_layer_settings_t *dst,
                          const water_layer_settings_t *src)
{
    memcpy(dst, src, sizeof(*dst));
}

static void reset_to_defaults(filter_water_layer_t *filter)
{
    settings_copy(&filter->settings, &default_settings);
    filter->preset_index = 0;
}

static void load_preset(filter_water_layer_t *filter, int index)
{
    if (index < 0 || index >= preset_count)
        return;
    settings_copy(&filter->settings, &presets[index].settings);
    filter->preset_index = index;
}

/* ---- Gradient noise + FBM ------------------------------------------------ */

static unsigned char g_perm[512];

static void noise_init(int seed)
{
    int i, j, k;
    unsigned char p[256];

    srand((unsigned)seed);
    for (i = 0; i < 256; i++)
        p[i] = (unsigned char)i;
    for (i = 255; i > 0; i--) {
        j = rand() % (i + 1);
        k = p[i];
        p[i] = p[j];
        p[j] = (unsigned char)k;
    }
    for (i = 0; i < 256; i++) {
        g_perm[i] = p[i];
        g_perm[i + 256] = p[i];
    }
}

static float fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float grad2(int h, float x, float y)
{
    switch (h & 7) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x;
    case 5: return -x;
    case 6: return y;
    default: return -y;
    }
}

static float perlin2(float x, float y)
{
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    int xi = x0 & 255;
    int yi = y0 & 255;
    float u = fade(fx);
    float v = fade(fy);
    int aa = g_perm[g_perm[xi] + yi];
    int ab = g_perm[g_perm[xi] + yi + 1];
    int ba = g_perm[g_perm[xi + 1] + yi];
    int bb = g_perm[g_perm[xi + 1] + yi + 1];
    float x1 = grad2(aa, fx, fy) +
               (grad2(ba, fx - 1.0f, fy) - grad2(aa, fx, fy)) * u;
    float x2 = grad2(ab, fx, fy - 1.0f) +
               (grad2(bb, fx - 1.0f, fy - 1.0f) - grad2(ab, fx, fy - 1.0f)) * u;
    return x1 + (x2 - x1) * v; /* roughly [-1, 1] */
}

static float fbm2(float x, float y, int octaves, float persistence, float lacunarity)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;
    int i;
    int oct = clamp(octaves, 1, 8);

    for (i = 0; i < oct; i++) {
        sum += amp * perlin2(x * freq, y * freq);
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    if (norm <= 1e-6f)
        return 0.0f;
    return sum / norm;
}

/*
 * Height field in [0, 1]:
 *  1. Anisotropic UV from swell direction / stretch
 *  2. Domain warp for organic flow
 *  3. Large-scale FBM swells
 *  4. Fine ripple layer
 *  5. Soft troughs + sharp foam crests
 */
static float water_field(const water_layer_settings_t *s, float x, float y)
{
    float scale = max(s->scale, 1.0f);
    float ang = s->direction_deg * (float)(M_PI / 180.0);
    float ca = cosf(ang);
    float sa = sinf(ang);
    float stretch = max(s->stretch, 1.0f);
    float u = (x * ca + y * sa) / scale;
    float v = (-x * sa + y * ca) / (scale * stretch);
    float warp = clamp(s->warp, 0.0f, 2.0f);
    float detail = clamp(s->detail, 0.0f, 1.0f);
    float foam = clamp(s->foam, 0.0f, 1.0f);
    float wu, wv, swell, ripples, h, crest, t;

    wu = u + warp * fbm2(u * 0.7f + 3.1f, v * 0.7f + 1.7f, 3, 0.5f, 2.0f);
    wv = v + warp * fbm2(u * 0.7f + 8.4f, v * 0.7f + 5.2f, 3, 0.5f, 2.0f);

    swell = fbm2(wu, wv, 5, 0.52f, 2.05f);
    ripples = fbm2(wu * 4.5f + 12.0f, wv * 4.5f - 7.0f, 3, 0.45f, 2.2f);

    h = swell * (1.0f - detail * 0.55f) + ripples * detail * 0.55f;
    h = h * 0.5f + 0.5f; /* [-1,1] → [0,1] */

    /* Soften lows, sharpen highs into foam streaks. */
    t = h * h * (3.0f - 2.0f * h);
    crest = smoothstep(1.0f - foam * 0.55f - 0.2f, 1.0f, h);
    h = t * (1.0f - foam * 0.35f) + crest * foam;
    return clamp(h, 0.0f, 1.0f);
}

static void lerp_color(const uint8_t a[4], const uint8_t b[4], float t,
                       uint8_t out[4])
{
    float u = clamp(t, 0.0f, 1.0f);
    out[0] = (uint8_t)clamp((int)lroundf(a[0] + (b[0] - a[0]) * u), 0, 255);
    out[1] = (uint8_t)clamp((int)lroundf(a[1] + (b[1] - a[1]) * u), 0, 255);
    out[2] = (uint8_t)clamp((int)lroundf(a[2] + (b[2] - a[2]) * u), 0, 255);
    out[3] = 255;
}

static void field_to_color(const water_layer_settings_t *s, float h,
                           uint8_t out[4])
{
    float c = clamp(s->contrast, 0.0f, 1.0f);
    float mid = 0.5f + (h - 0.5f) * c;
    uint8_t tmp[4];

    mid = clamp(mid, 0.0f, 1.0f);
    if (mid < 0.5f) {
        lerp_color(s->deep_color, s->color, mid * 2.0f, out);
    } else {
        lerp_color(s->color, s->foam_color, (mid - 0.5f) * 2.0f, tmp);
        /* Extra foam push on the brightest crests. */
        lerp_color(tmp, s->foam_color,
                   smoothstep(0.75f, 1.0f, h) * clamp(s->foam, 0.0f, 1.0f),
                   out);
    }
    out[3] = s->color[3] ? s->color[3] : 255;
}

static void generate_water_layer(volume_t *volume,
                                 const water_layer_settings_t *settings)
{
    float box[4][4];
    int dimensions[3], start_pos[3];
    int x, y, pos[3], bottom_z;
    uint8_t color[4];
    volume_iterator_t iter;
    float h;

    if (!volume || !goxel.image)
        return;

    mat4_copy(goxel.image->box, box);
    if (box_is_null(box))
        volume_get_box(volume, true, box);
    if (box_is_null(box))
        return;

    box_get_dimensions(box, dimensions);
    box_get_start_pos(box, start_pos);
    if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[2] <= 0)
        return;

    noise_init(settings->seed);
    bottom_z = start_pos[2];
    iter = volume_get_iterator(volume, VOLUME_ITER_VOXELS);

    for (x = 0; x < dimensions[0]; x++) {
        for (y = 0; y < dimensions[1]; y++) {
            pos[0] = start_pos[0] + x;
            pos[1] = start_pos[1] + y;
            pos[2] = bottom_z;
            h = water_field(settings, (float)x, (float)y);
            field_to_color(settings, h, color);
            volume_set_at(volume, &iter, pos, color);
        }
    }
}

/* ---- GUI ----------------------------------------------------------------- */

static int gui(filter_t *filter_)
{
    filter_water_layer_t *filter = (void *)filter_;
    water_layer_settings_t *s = &filter->settings;
    const char *preset_names[16];
    int i;
    const char *help_text =
        "Paints a flat 1-block water sheet on the image-box bottom.\n"
        "Uses domain-warped directional noise for swells, ripples, and foam.\n"
        "Always overwrites the bottom block of each column.";

    goxel_set_help_text(help_text);
    if (gui_collapsing_header("Hint", false))
        gui_text_wrapped(help_text);

    for (i = 0; i < preset_count && i < 16; i++)
        preset_names[i] = presets[i].name;

    gui_group_begin("Presets");
    gui_combo("Preset", &filter->preset_index, preset_names, preset_count);
    gui_row_begin(2);
    if (gui_button("Load", 0, 0))
        load_preset(filter, filter->preset_index);
    if (gui_button("Reset", 0, 0))
        reset_to_defaults(filter);
    gui_row_end();
    gui_group_end();

    gui_group_begin("Colors");
    gui_color_small("Mid", s->color);
    gui_color_small("Deep", s->deep_color);
    gui_color_small("Foam", s->foam_color);
    gui_group_end();

    gui_group_begin("Waves");
    gui_input_float("Scale", &s->scale, 1.0f, 1.0f, 256.0f, "%.0f");
    gui_tooltip_if_hovered("Size of the large swells in blocks (higher = broader).");
    gui_input_float("Direction", &s->direction_deg, 1.0f, 0.0f, 360.0f, "%.0f");
    gui_tooltip_if_hovered("Swell travel direction in degrees.");
    gui_input_float("Stretch", &s->stretch, 0.05f, 1.0f, 6.0f, "%.2f");
    gui_tooltip_if_hovered(
        "Elongates waves along the swell direction (1 = round cells).");
    gui_input_float("Warp", &s->warp, 0.01f, 0.0f, 2.0f, "%.2f");
    gui_tooltip_if_hovered("Domain warp — bends the pattern into flowing shapes.");
    gui_input_float("Detail", &s->detail, 0.01f, 0.0f, 1.0f, "%.2f");
    gui_tooltip_if_hovered("Amount of fine ripple noise on top of the swells.");
    gui_input_float("Foam", &s->foam, 0.01f, 0.0f, 1.0f, "%.2f");
    gui_tooltip_if_hovered("How strongly bright foam appears on wave crests.");
    gui_input_float("Contrast", &s->contrast, 0.01f, 0.0f, 1.0f, "%.2f");
    gui_tooltip_if_hovered("Separation between deep, mid, and foam colours.");
    gui_input_int("Seed", &s->seed, 0, RAND_MAX);
    if (gui_button("Randomize seed", -1, 0)) {
        srand((unsigned)time(NULL));
        s->seed = rand();
    }
    gui_group_end();

    if (gui_button("Generate", -1, 0)) {
        if (!goxel.image || !goxel.image->active_layer ||
            !goxel.image->active_layer->volume)
            return 0;
        image_history_push(goxel.image);
        generate_water_layer(goxel.image->active_layer->volume, s);
    }
    return 0;
}

static void on_open(filter_t *filter_)
{
    filter_water_layer_t *filter = (void *)filter_;
    reset_to_defaults(filter);
}

FILTER_REGISTER(water_layer, filter_water_layer_t,
                .name = "Generation - Water layer",
                .on_open = on_open,
                .panel_width = 275,
                .gui_fn = gui, )
