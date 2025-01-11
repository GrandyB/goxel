#ifndef NOISE_H
#define NOISE_H

#include <stdio.h>
#include <math.h>
#include <goxel.h>

float uniform_noise(float x, float y, float z);

void blend_with_noise(int orig[3], float noise_value, float noise_intensity, float noise_saturation, int out[3]);

#endif // NOISE_H
