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

// Function to generate an RGB color based on a float input [0, 1]
void generate_random_color(float noise_value, float noise_intensity, float noise_saturation, float out[3]) {
    // Ensure the value is clamped between 0.0 and 1.0
    // if (value < 0.0f) value = 0.0f;
    // if (value > 1.0f) value = 1.0f;

    // // Scale the value to a hue value in the range [0, 360]
    // float hue = value * 360.0f;

    // // Convert hue to RGB using the HSV to RGB conversion formula
    // float c = noise_saturation;                       // Chroma: full intensity modulated by saturation
    // float x = c * (1 - fabs(fmod(hue / 60.0f, 2) - 1));
    // float m = noise_intensity * (1 - c);              // Adjust intensity (adds "grayness")

    // float r_prime = 0.0f, g_prime = 0.0f, b_prime = 0.0f;

    // if (hue < 60) {
    //     r_prime = c;
    //     g_prime = x;
    //     b_prime = 0;
    // } else if (hue < 120) {
    //     r_prime = x;
    //     g_prime = c;
    //     b_prime = 0;
    // } else if (hue < 180) {
    //     r_prime = 0;
    //     g_prime = c;
    //     b_prime = x;
    // } else if (hue < 240) {
    //     r_prime = 0;
    //     g_prime = x;
    //     b_prime = c;
    // } else if (hue < 300) {
    //     r_prime = x;
    //     g_prime = 0;
    //     b_prime = c;
    // } else {
    //     r_prime = c;
    //     g_prime = 0;
    //     b_prime = x;
    // }

    // // Convert from [0, 1] to [0, 255] and return as an RGB struct
    // RGB color = {
    //     .r = (int)((r_prime + m) * 255.0f),
    //     .g = (int)((g_prime + m) * 255.0f),
    //     .b = (int)((b_prime + m) * 255.0f)
    // };

    //return color;
}