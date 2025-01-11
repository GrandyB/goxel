#include "noise.h"

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