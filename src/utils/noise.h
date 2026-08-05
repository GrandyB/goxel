#ifndef NOISE_H
#define NOISE_H

#ifndef NOISE_TEXTURE_SIZE
#   define NOISE_TEXTURE_SIZE 512
#endif

#include <stdio.h>
#include <math.h>
#include <goxel.h>

float uniform_noise(float x, float y, float z);

/* Seeded 2D classic Perlin (~[-1, 1]).  One shared permutation table;
 * call perlin2_init_seed when the seed changes.  Seed 0 is treated as 1. */
void perlin2_init_seed(unsigned seed);
/* Like init, but only if the table has never been built (keeps stroke seeds). */
void perlin2_ensure_seeded(unsigned seed);
float perlin2(float x, float y);
float fbm2(float x, float y, int octaves, float persistence, float lacunarity);

void blend_with_noise(int orig[3], float noise_value, float noise_intensity, float noise_saturation, int out[3]);

void blend_with_noise_alpha(int orig[3], float noise_value, float noise_intensity, float noise_saturation, int out[3]);

#endif // NOISE_H
