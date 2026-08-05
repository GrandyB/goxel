#include "noise.h"

#include <math.h>
#include <stdbool.h>

// Hash function to create a pseudorandom number based on x, y, z
static int hash(int x, int y, int z) {
    int h = (int)(x * 73856093 ^ y * 19349663 ^ z * 83492791);
    h = (h >> 13) ^ h;
    return h * (h * h * 15731 + 789221) + 1376312589;
}

// Random uniform noise function
float uniform_noise(float x, float y, float z) {
    // Generate a hash for the given position
    int h = hash(x, y, z);

    // Normalize the hash value to [0, 1]
    return (h & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

/* ---- Seeded 2D Perlin (shared by Smooth, water-layer, …) ---------------- */

static unsigned char g_perlin2_perm[512];
static unsigned g_perlin2_seed = 0;
static bool g_perlin2_inited = false;

void perlin2_init_seed(unsigned seed)
{
    int i, j, k;
    unsigned char p[256];
    unsigned s = seed ? seed : 1u;

    if (g_perlin2_inited && g_perlin2_seed == seed)
        return;

    for (i = 0; i < 256; i++)
        p[i] = (unsigned char)i;
    for (i = 255; i > 0; i--) {
        s = s * 1664525u + 1013904223u;
        j = (int)(s % (unsigned)(i + 1));
        k = p[i];
        p[i] = p[j];
        p[j] = (unsigned char)k;
    }
    for (i = 0; i < 256; i++) {
        g_perlin2_perm[i] = p[i];
        g_perlin2_perm[i + 256] = p[i];
    }
    g_perlin2_seed = seed;
    g_perlin2_inited = true;
}

void perlin2_ensure_seeded(unsigned seed)
{
    if (!g_perlin2_inited)
        perlin2_init_seed(seed);
}

static float perlin2_fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float perlin2_grad(int h, float x, float y)
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

float perlin2(float x, float y)
{
    int x0, y0, xi, yi, aa, ab, ba, bb;
    float fx, fy, u, v, x1, x2;

    perlin2_ensure_seeded(1u);

    x0 = (int)floorf(x);
    y0 = (int)floorf(y);
    fx = x - (float)x0;
    fy = y - (float)y0;
    xi = x0 & 255;
    yi = y0 & 255;
    u = perlin2_fade(fx);
    v = perlin2_fade(fy);
    aa = g_perlin2_perm[g_perlin2_perm[xi] + yi];
    ab = g_perlin2_perm[g_perlin2_perm[xi] + yi + 1];
    ba = g_perlin2_perm[g_perlin2_perm[xi + 1] + yi];
    bb = g_perlin2_perm[g_perlin2_perm[xi + 1] + yi + 1];
    x1 = perlin2_grad(aa, fx, fy) +
         (perlin2_grad(ba, fx - 1.0f, fy) - perlin2_grad(aa, fx, fy)) * u;
    x2 = perlin2_grad(ab, fx, fy - 1.0f) +
         (perlin2_grad(bb, fx - 1.0f, fy - 1.0f) -
          perlin2_grad(ab, fx, fy - 1.0f)) * u;
    return x1 + (x2 - x1) * v;
}

float fbm2(float x, float y, int octaves, float persistence, float lacunarity)
{
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;
    int i;
    int oct = octaves < 1 ? 1 : (octaves > 8 ? 8 : octaves);

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

const float HUE_UPPER_LIMIT = 360.0f;

void hsl_to_rgb(double hsl[3], int out[3])
{
    double h = hsl[0];
    double s = hsl[1];
    double l = hsl[2];
    if (h < 0.0 || h > 360.0 || s < 0.0 || s > 1.0 || l < 0.0 || l > 1.0) {
        LOG_D("Invalid HSL values: h=%f, s=%f, l=%f", h, s, l);
        out[0] = out[1] = out[2] = 0; // Default to black
        return;
    }

    double c = 0.0, m = 0.0, x = 0.0;
    c = (1.0 - fabs(2 * l - 1.0)) * s;
    m = 1.0 * (l - 0.5 * c);
    x = c * (1.0 - fabs(fmod(h / 60.0, 2) - 1.0));

    double rgb[3];
    if (h >= 0.0 && h < (HUE_UPPER_LIMIT / 6.0))
    {
        rgb[0] = c + m;
        rgb[1] = x + m;
        rgb[2] = m;
    }
    else if (h >= (HUE_UPPER_LIMIT / 6.0) && h < (HUE_UPPER_LIMIT / 3.0))
    {
        rgb[0] = x + m;
        rgb[1] = c + m;
        rgb[2] = m;
    }
    else if (h < (HUE_UPPER_LIMIT / 3.0) && h < (HUE_UPPER_LIMIT / 2.0))
    {
        rgb[0] = m;
        rgb[1] = c + m;
        rgb[2] = x + m;
    }
    else if (h >= (HUE_UPPER_LIMIT / 2.0)
            && h < (2.0f * HUE_UPPER_LIMIT / 3.0))
    {
        rgb[0] = m;
        rgb[1] = x + m;
        rgb[2] = c + m;
    }
    else if (h >= (2.0 * HUE_UPPER_LIMIT / 3.0)
            && h < (5.0 * HUE_UPPER_LIMIT / 6.0))
    {
        rgb[0] = x + m;
        rgb[1] = m;
        rgb[2] = c + m;
    }
    else if (h >= (5.0 * HUE_UPPER_LIMIT / 6.0) && h < HUE_UPPER_LIMIT)
    {
        rgb[0] = c + m;
        rgb[1] = m;
        rgb[2] = x + m;
    }
    else
    {
        rgb[0] = m;
        rgb[1] = m;
        rgb[2] = m;
    }
    //LOG_D("HSL: %f/%f/%f", h, s, l);
    //LOG_D("RGB: %f/%f/%f", rgb[0], rgb[1], rgb[2]);
    out[0] = (int) (rgb[0] * 255.0f);
    out[1] = (int) (rgb[1] * 255.0f);
    out[2] = (int) (rgb[2] * 255.0f);
    //LOG_D("out: %i/%i/%i", out[0], out[1], out[2]);
}

void blend_alpha_hsl(int orig[3], double noise_hsl[3], float saturation, float intensity, int result[3]) {
    // Normalize saturation and intensity
    saturation = clamp(saturation / 100.0, 0.0, 1.0);
    intensity = clamp(intensity / 100.0, 0.0, 1.0);

    // Convert noise HSL to RGB
    int noise_rgb[3];
    hsl_to_rgb(noise_hsl, noise_rgb);

    // Calculate grayscale luminance of noise
    double noise_luminance = 0.299 * noise_rgb[0] + 0.587 * noise_rgb[1] + 0.114 * noise_rgb[2];

    // Blend noise with its grayscale version based on saturation
    double blended_noise_r = noise_luminance * (1.0 - saturation) + noise_rgb[0] * saturation;
    double blended_noise_g = noise_luminance * (1.0 - saturation) + noise_rgb[1] * saturation;
    double blended_noise_b = noise_luminance * (1.0 - saturation) + noise_rgb[2] * saturation;

    // Blend the original color and the blended noise color based on intensity
    result[0] = (int)((orig[0] * (1.0 - intensity)) + (blended_noise_r * intensity));
    result[1] = (int)((orig[1] * (1.0 - intensity)) + (blended_noise_g * intensity));
    result[2] = (int)((orig[2] * (1.0 - intensity)) + (blended_noise_b * intensity));
}

void blend_with_noise(int orig[3], float noise_value, float noise_intensity, float noise_saturation, int out[3]) {
    double hsl[3];
    hsl[0] = clamp(noise_value, 0.0f, 1.0f) * 360;
    hsl[1] = 1.0f;
    hsl[2] = 0.5f;

    blend_alpha_hsl(orig, hsl, noise_saturation, noise_intensity, orig);
}

void blendColors(int base[3], int overlay[3], double alpha, int result[3]) {
    result[0] = (int)(overlay[0] * alpha + base[0] * (1 - alpha));
    result[1] = (int)(overlay[1] * alpha + base[1] * (1 - alpha));
    result[2] = (int)(overlay[2] * alpha + base[2] * (1 - alpha));
}

void generate_color_rgb(float noise_value, int out[3]) {
    double color_hsl[3];
    color_hsl[0] = clamp(noise_value, 0.0f, 1.0f) * 360;
    color_hsl[1] = 1.0f;
    color_hsl[2] = 0.5f;
    hsl_to_rgb(color_hsl, out);
}

void generate_greyscale_rgb(float noise_value, int out[3]) {
    double value = noise_value * 255.0f;
    out[0] = (int) value;
    out[1] = (int) value;
    out[2] = (int) value;
}

void blend_with_noise_alpha(int orig[3], float noise_value, float noise_intensity, float noise_saturation, int out[3]) {
    // Generate random color
    int color_rgb[3];
    generate_color_rgb(noise_value, color_rgb);

    // Generate greyscale color
    int greyscale_rgb[3];
    generate_greyscale_rgb(noise_value, greyscale_rgb);

    // Blend color onto greyscale, using saturation as the alpha
    double saturation_alpha = clamp(noise_saturation/100.0f, 0.0f, 1.0f);
    int blend_saturation[3];
    blendColors(greyscale_rgb, color_rgb, saturation_alpha, blend_saturation);
    
    // Blend resulting noise back onto the "original"/input color using intensity as the alpha
    double intensity_alpha = clamp(noise_intensity/100.0f, 0.0f, 1.0f);
    blendColors(orig, blend_saturation, intensity_alpha, out);
}