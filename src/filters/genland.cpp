// Genland - procedural landscape generator
// by Tom Dobrowolski (http://ged.ax.pl/~tomkh) (heightmap generator)
// and Ken Silverman (http://advsys.net/ken) (DTA/PNG/VXL writers)

// This file has been modified from Ken Silverman's original release

// If you do something cool, feel free to write us
// (contact info can be found at our websites)

// License for this code:
// 	* No commercial exploitation please
// 	* Do not remove our names from the code or credits
// 	* You may distribute modified code/executables,
// 	  but please make it clear that it is modified.

// History:
// 	2005-12-24: Released GENLAND.EXE with Ken's GROUDRAW demos.
// 	2006-03-10: Released GENLAND.CPP source code
//  ---
//  2025-02-28: Ported to a state where it can be integrated into Goxel

#include <memory.h>
#include <math.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <x86intrin.h> // Provides _rdtsc()

extern "C"
{
#include "goxel.h"
#include "genland.h"
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic pop

static inline int64_t rdtsc64()
{
    return __rdtsc(); // Works on modern compilers (MSVC, GCC, Clang)
}

#pragma pack(1)
typedef struct
{
    double x, y, z;
} dpoint3d;
typedef struct
{
    unsigned char b, g, r, a;
} vcol;

// Anything less than 9/512 seems to crash
#if 1
#define VSHL 9
#define VSID 512
#else // nice for testing:
#define VSHL 8
#define VSID 256
#endif

static void process_voxel_data(volume_t *volume, genland_settings_t *settings, vcol *argb)
{
    int pos[3];
    // Loop over each (x, y) coordinate in the buffer grid
    for (int y = 0; y < VSID; y++)
    {
        for (int x = 0; x < VSID; x++, argb++)
        {
            // The alpha channel holds the height value, but with 0 at the top.
            // Convert it so that 0 becomes the bottom of the volume.
            // settings->max_height - 1 is the maximum z index in the volume.
            int voxelTopZ = clamp((settings->max_height - 1) - (int)argb->a, 0, settings->max_height);
            
            // Only process columns with a valid height.
            if (voxelTopZ >= 0)
            {
                // Fill the column from the bottom (z = 0) up to voxelTopZ with the same color.
                for (int z = 0; z <= voxelTopZ; z++)
                {
                    pos[0] = x;
                    pos[1] = y;
                    pos[2] = z;
                    uint8_t color[4] = {argb->r, argb->g, argb->b, 255};
                    volume_set_at(volume, NULL, pos, color);
                }
            }
        }
    }
}


long savevxl(char *filnam, vcol *argb)
{
    dpoint3d ipo, ist, ihe, ifo;
    long i, j, x, y, z, zz;
    FILE *fil;

    if (!(fil = fopen(filnam, "wb")))
        return (-1);

    i = 0x09072000;
    fwrite(&i, 4, 1, fil); // Version
    i = VSID;
    fwrite(&i, 4, 1, fil);
    i = VSID;
    fwrite(&i, 4, 1, fil);

    ipo.x = ipo.y = VSID * .5;
    ipo.z = argb[(VSID >> 1) * VSID + (VSID >> 1)].a - 64;
    ist.x = 1;
    ist.y = 0;
    ist.z = 0;
    ihe.x = 0;
    ihe.y = 0;
    ihe.z = 1;
    ifo.x = 0;
    ifo.y = -1;
    ifo.z = 0;
    fwrite(&ipo, 24, 1, fil);
    fwrite(&ist, 24, 1, fil);
    fwrite(&ihe, 24, 1, fil);
    fwrite(&ifo, 24, 1, fil);

    for (y = 0; y < VSID; y++)
        for (x = 0; x < VSID; x++, argb++)
        {
            z = (long)argb[0].a;
            zz = z + 1;
            for (i = -1; i <= 1; i += 2)
            {
                if ((unsigned long)(x + i) < VSID)
                {
                    j = (long)argb[i].a;
                    if (j > zz)
                        zz = j;
                }
                if ((unsigned long)(y + i) < VSID)
                {
                    j = (long)argb[i * VSID].a;
                    if (j > zz)
                        zz = j;
                }
            }

            // Write slab header for column(x,y)
            fputc(0, fil);
            fputc(z, fil);
            fputc((zz - 1) & 255, fil);
            fputc(0, fil);

            i = (((*(long *)argb) & 0xffffff) | 0x80000000);
            // Write list of colors on column(x,y)
            for (; z < zz; z++)
                fwrite(&i, 4, 1, fil);
        }

    fclose(fil);
    return (0);
}

//----------------------------------------------------------------------------
// Noise algo based on "Improved Perlin Noise" by Ken Perlin
// http://mrl.nyu.edu/~perlin/

static __forceinline float fgrad(long h, float x, float y, float z)
{
    switch (h) // h masked before call (h&15)
    {
    case 0:
        return (x + y);
    case 1:
        return (-x + y);
    case 2:
        return (x - y);
    case 3:
        return (-x - y);
    case 4:
        return (x + z);
    case 5:
        return (-x + z);
    case 6:
        return (x - z);
    case 7:
        return (-x - z);
    case 8:
        return (y + z);
    case 9:
        return (-y + z);
    case 10:
        return (y - z);
    case 11:
        return (-y - z);
    case 12:
        return (x + y);
    case 13:
        return (-x + y);
        // case 12: return(   y+z);
        // case 13: return(  -y+z);
    case 14:
        return (y - z);
    case 15:
        return (-y - z);
    }
    return (0);
}

static unsigned char noisep[512], noisep15[512];
static void noiseinit()
{
    long i, j, k;
    float f;

    for (i = 256 - 1; i >= 0; i--)
        noisep[i] = i;
    for (i = 256 - 1; i > 0; i--)
    {
        j = ((rand() * (i + 1)) >> 15);
        k = noisep[i];
        noisep[i] = noisep[j];
        noisep[j] = k;
    }
    for (i = 256 - 1; i >= 0; i--)
        noisep[i + 256] = noisep[i];
    for (i = 512 - 1; i >= 0; i--)
        noisep15[i] = noisep[i] & 15;
}

double noise3d(double fx, double fy, double fz, long mask)
{
    long i, l[6], a[4];
    float p[3], f[8];

    // if (mask > 255) mask = 255; //Checked before call
    l[0] = floor(fx);
    p[0] = fx - ((float)l[0]);
    l[0] &= mask;
    l[3] = (l[0] + 1) & mask;
    l[1] = floor(fy);
    p[1] = fy - ((float)l[1]);
    l[1] &= mask;
    l[4] = (l[1] + 1) & mask;
    l[2] = floor(fz);
    p[2] = fz - ((float)l[2]);
    l[2] &= mask;
    l[5] = (l[2] + 1) & mask;
    i = noisep[l[0]];
    a[0] = noisep[i + l[1]];
    a[2] = noisep[i + l[4]];
    i = noisep[l[3]];
    a[1] = noisep[i + l[1]];
    a[3] = noisep[i + l[4]];
    f[0] = fgrad(noisep15[a[0] + l[2]], p[0], p[1], p[2]);
    f[1] = fgrad(noisep15[a[1] + l[2]], p[0] - 1, p[1], p[2]);
    f[2] = fgrad(noisep15[a[2] + l[2]], p[0], p[1] - 1, p[2]);
    f[3] = fgrad(noisep15[a[3] + l[2]], p[0] - 1, p[1] - 1, p[2]);
    p[2]--;
    f[4] = fgrad(noisep15[a[0] + l[5]], p[0], p[1], p[2]);
    f[5] = fgrad(noisep15[a[1] + l[5]], p[0] - 1, p[1], p[2]);
    f[6] = fgrad(noisep15[a[2] + l[5]], p[0], p[1] - 1, p[2]);
    f[7] = fgrad(noisep15[a[3] + l[5]], p[0] - 1, p[1] - 1, p[2]);
    p[2]++;
    p[2] = (3.0 - 2.0 * p[2]) * p[2] * p[2];
    p[1] = (3.0 - 2.0 * p[1]) * p[1] * p[1];
    p[0] = (3.0 - 2.0 * p[0]) * p[0] * p[0];
    f[0] = (f[4] - f[0]) * p[2] + f[0];
    f[1] = (f[5] - f[1]) * p[2] + f[1];
    f[2] = (f[6] - f[2]) * p[2] + f[2];
    f[3] = (f[7] - f[3]) * p[2] + f[3];
    f[0] = (f[2] - f[0]) * p[1] + f[0];
    f[1] = (f[3] - f[1]) * p[1] + f[1];
    return ((f[1] - f[0]) * p[0] + f[0]);
}

//--------------------------------------------------------------------------------------------------

vcol buf[VSID * VSID];
vcol amb[VSID * VSID]; // ambient
float hgt[VSID * VSID];
unsigned char sh[VSID * VSID];

#define PI 3.141592653589793

//-----------------------------------------------------------------------------

#define EPS 0.1

extern "C" void generate_tomland_terrain(volume_t *volume, genland_settings_t *settings)
{
    // Variables for noise sampling, blending, and color computations
    double sampleX, sampleY, tempValue, grassBlend, secondaryBlend, riverNoise;
    // Array storing amplitude for each octave of noise
    double octaveAmplitudes[settings->num_octaves];
    // Base height samples and corrected height samples (for normal calculation)
    double baseSamples[3], correctedSamples[3];
    double unusedDot, normalX, normalY, normalZ;
    // Ground color components (red, green, blue)
    double groundRed, groundGreen, groundBlue;
    // Temporary variable used in shadow calculations
    float shadowCheckValue;
    // Loop indices and temporary variables
    long octaveIndex, shadowIter, pixelX, pixelY, globalIndex, octave, progressPercent, maxAmbient, colorIndex;
    long palette[256]; // Unused palette array
    // Lookup table for noise mask values per octave
    long maskLUT[settings->num_octaves];

    printf("Heightmap generator by Tom Dobrowoski (http://ged.ax.pl/~tomkh)\n");
    printf("Output formats by Ken Silverman (http://advsys.net/ken)\n");

    noiseinit();

    // Tom's algorithm from 12/04/2005
    printf("Generating landscape\n");
    // Initialize octave amplitudes and mask lookup table
    tempValue = 1.0;
    for (octaveIndex = 0; octaveIndex < settings->num_octaves; octaveIndex++)
    {
        octaveAmplitudes[octaveIndex] = tempValue;
        tempValue *= settings->amp_octave_mult;  // Reduce amplitude per octave
        maskLUT[octaveIndex] = min((1 << (octaveIndex + 2)) - 1, 255);
    }
    globalIndex = 0;
    // Loop over each pixel in the terrain grid (VSID is the grid size)
    for (pixelY = 0; pixelY < VSID; pixelY++)
    {
        for (pixelX = 0; pixelX < VSID; pixelX++, globalIndex++)
        {
            // Get 3 height samples with slight offsets: (0,0), (EPS,0), (0,EPS)
            for (octaveIndex = 0; octaveIndex < 3; octaveIndex++)
            {
                // Convert grid coordinates to noise space, adding offset based on sample index
                sampleX = (pixelX * (256.0 / (double)VSID) + (double)(octaveIndex & 1) * EPS) * (1.0 / 64.0);
                sampleY = (pixelY * (256.0 / (double)VSID) + (double)(octaveIndex >> 1) * EPS) * (1.0 / 64.0);
                tempValue = 0;       // Reset noise accumulator for this sample
                riverNoise = 0;      // Reset river noise accumulator
                // Accumulate multi-fractal noise over all octaves
                for (octave = 0; octave < settings->num_octaves; octave++)
                {
                    tempValue += noise3d(sampleX, sampleY, 9.5, maskLUT[octave])
                                 * octaveAmplitudes[octave] * (tempValue * 1.6 + 1.0);
                    riverNoise += noise3d(sampleX, sampleY, 13.2, maskLUT[octave])
                                  * octaveAmplitudes[octave];
                    sampleX *= 2;  // Increase frequency for next octave
                    sampleY *= 2;
                }
                // Compute base height for the sample
                baseSamples[octaveIndex] = (tempValue * - settings->variety) + settings->offset;
                // Modulate height using sine to simulate river effect (.02 approximates river width)
                tempValue = sin(pixelX * (PI / 256.0) + riverNoise * 4.0) * (0.5 + settings->river_width) + (0.5 - settings->river_width);
                if (tempValue > 1)
                    tempValue = 1;
                correctedSamples[octaveIndex] = baseSamples[octaveIndex] * tempValue;
                if (tempValue < 0)
                    tempValue = 0;
                baseSamples[octaveIndex] *= tempValue;
                // If the corrected sample is lower than the base, adjust to simulate water surface normal
                if (correctedSamples[octaveIndex] < baseSamples[octaveIndex])
                    correctedSamples[octaveIndex] = -log(1.0 - correctedSamples[octaveIndex]);
            }
            // Compute surface normal using cross-product from height differences
            normalX = correctedSamples[1] - correctedSamples[0];
            normalY = correctedSamples[2] - correctedSamples[0];
            normalZ = -EPS;  // Approximate vertical change
            tempValue = 1.0 / sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
            normalX *= tempValue;
            normalY *= tempValue;
            normalZ *= tempValue;

            // Initialize ground color from terrain settings
            groundRed = settings->color_ground[0];
            groundGreen = settings->color_ground[1];
            groundBlue = settings->color_ground[2];

            // Calculate blend factor for grass using normal and additional noise
            grassBlend = min(max(max(-normalZ, 0.0) * 1.4 - correctedSamples[0] / 32.0 +
                                noise3d(pixelX * (1.0 / 64.0), pixelY * (1.0 / 64.0), 0.3, 15) * 0.3, 0.0), 1.0);
            // Blend towards first grass color
            groundRed += (settings->color_grass1[0] - groundRed) * grassBlend;
            groundGreen += (settings->color_grass1[1] - groundGreen) * grassBlend;
            groundBlue += (settings->color_grass1[2] - groundBlue) * grassBlend;

            // Compute secondary blend factor for a second grass tone
            secondaryBlend = (1 - fabs(grassBlend - 0.5) * 2) * 0.7;
            groundRed += (settings->color_grass2[0] - groundRed) * secondaryBlend;
            groundGreen += (settings->color_grass2[1] - groundGreen) * secondaryBlend;
            groundBlue += (settings->color_grass2[2] - groundBlue) * secondaryBlend;

            // Calculate water blend factor based on difference between base and corrected height
            secondaryBlend = max(min((baseSamples[0] - correctedSamples[0]) * 1.5, 1.0), 0.0);
            // Adjust water intensity factor
            grassBlend = 1 - secondaryBlend * 0.2;
            groundRed += ((settings->color_water[0] * grassBlend) - groundRed) * secondaryBlend;
            groundGreen += ((settings->color_water[1] * grassBlend) - groundGreen) * secondaryBlend;
            groundBlue += ((settings->color_water[2] * grassBlend) - groundBlue) * secondaryBlend;

            // Compute ambient color contribution using a fixed ambient factor
            tempValue = 0.3;  // Ambient factor
            amb[globalIndex].r = (unsigned char)min(max(groundRed * tempValue, 0), 255);
            amb[globalIndex].g = (unsigned char)min(max(groundGreen * tempValue, 0), 255);
            amb[globalIndex].b = (unsigned char)min(max(groundBlue * tempValue, 0), 255);
            maxAmbient = max(max(amb[globalIndex].r, amb[globalIndex].g), amb[globalIndex].b);

            // Lighting: adjust color based on surface normal and fixed light direction
            tempValue = (normalX * 0.5 + normalY * 0.25 - normalZ)
                        / sqrt(0.5 * 0.5 + 0.25 * 0.25 + 1.0 * 1.0);
            tempValue *= 1.2;
            buf[globalIndex].a = (unsigned char)(settings->max_height - baseSamples[0]);// * ((double)VSID / 256.0));
            buf[globalIndex].r = (unsigned char)min(max(groundRed * tempValue, 0), 255 - maxAmbient);
            buf[globalIndex].g = (unsigned char)min(max(groundGreen * tempValue, 0), 255 - maxAmbient);
            buf[globalIndex].b = (unsigned char)min(max(groundBlue * tempValue, 0), 255 - maxAmbient);

            // Save the primary corrected height sample for shadow computation
            hgt[globalIndex] = correctedSamples[0];
        }
        // Update and display progress percentage
        progressPercent = ((pixelY + 1) * 100) / VSID;
        if (progressPercent > (pixelY * 100) / VSID)
            printf("\r%ld%%", progressPercent);
    }
    printf("\r");

    printf("Applying shadows\n");

    // Initialize shadow map to zero
    memset(sh, 0, sizeof(sh));
    globalIndex = 0;
    // Compute shadows by checking if neighboring heights block light
    for (pixelY = 0; pixelY < VSID; pixelY++)
    {
        for (pixelX = 0; pixelX < VSID; pixelX++, globalIndex++)
        {
            shadowCheckValue = hgt[globalIndex] + 0.44;
            // Step through offsets to determine if shadow should be cast
            for (shadowIter = 1, octaveIndex = 1;
                 octaveIndex < (VSID >> 2);
                 shadowIter++, octaveIndex++, shadowCheckValue += 0.44)
            {
                if (hgt[(((pixelY - (shadowIter >> 1)) & (VSID - 1)) << VSHL)
                        + ((pixelX - octaveIndex) & (VSID - 1))] > shadowCheckValue)
                {
                    sh[globalIndex] = 32;
                    break;
                }
            }
        }
    }
    // Smooth the shadow map by averaging with neighboring pixels
    globalIndex = 0;
    for (pixelY = 0; pixelY < VSID; pixelY++)
    {
        for (pixelX = 0; pixelX < VSID; pixelX++, globalIndex++)
        {
            sh[globalIndex] = (sh[globalIndex] +
                     sh[(((pixelY + 1) & (VSID - 1)) << VSHL) + pixelX] +
                     sh[(pixelY << VSHL) + ((pixelX + 1) & (VSID - 1))] +
                     sh[(((pixelY + 1) & (VSID - 1)) << VSHL) + ((pixelX + 1) & (VSID - 1))]
                     + 2) >> 2;
        }
    }
    // Apply shadow effect to final color buffer by darkening colors
    globalIndex = 0;
    for (pixelY = 0; pixelY < VSID; pixelY++)
    {
        for (pixelX = 0; pixelX < VSID; pixelX++, globalIndex++)
        {
            colorIndex = 256 - (sh[globalIndex] << 2);
            buf[globalIndex].r = ((buf[globalIndex].r * colorIndex) >> 8) + amb[globalIndex].r;
            buf[globalIndex].g = ((buf[globalIndex].g * colorIndex) >> 8) + amb[globalIndex].g;
            buf[globalIndex].b = ((buf[globalIndex].b * colorIndex) >> 8) + amb[globalIndex].b;
        }
    }

    // Process and integrate the voxel data into the volume structure
    process_voxel_data(volume, settings, buf);

    printf("Done!\n");
}


#if 0
!endif
#endif
