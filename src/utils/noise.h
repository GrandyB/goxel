#ifndef NOISE_H
#define NOISE_H

#include <stdio.h>
#include <math.h>

float uniform_noise(float x, float y, float z);

void generate_random_color(float noise_value, float noise_intensity, float noise_saturation, float out[3]);

#endif // NOISE_H
