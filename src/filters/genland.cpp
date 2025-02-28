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

#define SIGNBPL 13
#define SIGNXSIZ 100
#define SIGNYSIZ 19
static unsigned char signfplc[] =
    {
        0x10,0xdd,0xc1,0x15,0xdc,0x45,0xcc,0xdd,0x5d,0x74,0x71,0xe9,0x00,
        0xb0,0x55,0x41,0x15,0x48,0x6d,0x54,0x55,0x55,0x54,0x11,0x45,0x00,
        0x50,0xdd,0xc1,0x1c,0x48,0x55,0x54,0xcd,0x54,0x55,0x71,0x43,0x00,
        0x10,0x55,0x40,0x09,0x48,0x45,0x54,0x55,0xd5,0x56,0x41,0x45,0x00,
        0x10,0x55,0xc0,0x09,0xc8,0x45,0xcc,0x5d,0x5d,0x74,0x77,0xe9,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x75,0x77,0x80,0xc8,0xdd,0xc0,0x15,0x5c,0x40,0xcb,0x5d,0x94,0x0a,
        0x25,0x52,0x42,0x24,0x44,0x41,0x15,0x54,0x20,0x8d,0xd4,0x56,0x0a,
        0x27,0x72,0x20,0xa2,0x4d,0xc1,0x09,0x5c,0x10,0x80,0x54,0x35,0x0e,
        0x25,0x12,0x12,0x21,0x45,0x41,0x15,0x44,0x08,0x80,0x54,0x54,0x0a,
        0x25,0x12,0x88,0xc0,0xdc,0x48,0x95,0xc4,0x05,0x80,0x5c,0x94,0x0a,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x77,0x77,0x70,0x77,0x30,0x77,0x77,0x77,0x75,0x77,0x09,0x00,
        0x00,0x51,0x11,0x10,0x55,0x50,0x12,0x52,0x52,0x25,0x52,0x0b,0x00,
        0x00,0x33,0x33,0x30,0x35,0x50,0x72,0x32,0x32,0x25,0x52,0x0d,0x00,
        0x00,0x51,0x11,0x10,0x55,0x50,0x42,0x52,0x52,0x25,0x52,0x09,0x00,
        0x00,0x51,0x77,0x10,0x57,0x30,0x77,0x52,0x77,0x27,0x77,0x09,0x00,
};

// Disabling this will result in lots of crashing and viruses in your future! :P
void signprint(long x, long y)
{
    long *lptr = (long *)(intptr_t)(((y * VSID + x) << 2) + (intptr_t)buf);
    for (y = 0; y < SIGNYSIZ; y++, lptr += VSID)
        for (x = 0; x < SIGNXSIZ; x++)
            if (signfplc[y * SIGNBPL + (x >> 3)] & (1 << (x & 7)))
                lptr[x] -= 0x01101010;
}

//-----------------------------------------------------------------------------

typedef struct
{
    long f, p, x, y;
} tiletype;
//----------------- Wu's algo (hi quality, fast, big code :/) -----------------
// The following big block of code (up to the genpal* stuff) came from:
//   http://www.ece.mcmaster.ca/~xwu/cq.c on 12/14/2005
// I converted it to my style and added some library calls
//-Ken S.

// Having received many constructive comments and bug reports about my previous
// C implementation of my color quantizer (Graphics Gems vol. II, p. 126-133),
// I am posting the following second version of my program (hopefully 100%
// healthy) as a reply to all those who are interested in the problem.

/**********************************************************************
         C Implementation of Wu's Color Quantizer (v. 2)
         (see Graphics Gems vol. II, pp. 126-133)

Author:   Xiaolin Wu
    Dept. of Computer Science
    Univ. of Western Ontario
    London, Ontario N6A 5B7
    wu@csd.uwo.ca

Algorithm: Greedy orthogonal bipartition of RGB space for variance
        minimization aided by inclusion-exclusion tricks.
        For speed no nearest neighbor search is done. Slightly
        better performance can be expected by more sophisticated
        but more expensive versions.

The author thanks Tom Lane at Tom_Lane@G.GP.CS.CMU.EDU for much of
additional documentation and a cure to a previous bug.

Free to distribute, comments and suggestions are appreciated.
**********************************************************************/

// r0 < col <= r1, etc..
typedef struct
{
    long r0, r1, g0, g1, b0, b1, vol;
} box;

// Histogram is in elements 1..HISTSIZE along each axis,
// element 0 is for base or marginal value. Note:these must start out 0!
static long *wt = 0, *mr = 0, *mg = 0, *mb = 0;
static float *m2 = 0;
static unsigned char *tag;

// build 3-D color histogram of counts, r/g/b, c^2
static void Hist3d(tiletype *tt, long *vwt, long *vmr, long *vmg, long *vmb, float *m2)
{
    long i, x, y, r, g, b, *lptr, sq[256];

    for (i = 0; i < 256; i++)
        sq[i] = i * i;
    lptr = (long *)(intptr_t)tt->f;

    for (y = 0; y < tt->y; y++, lptr = (long *)(intptr_t)(((intptr_t)lptr) + tt->p))
        for (x = 0; x < tt->x; x++)
        {
            r = (lptr[x] >> 16) & 255;
            g = (lptr[x] >> 8) & 255;
            b = (lptr[x]) & 255;
            i = ((r >> 3) + 1) * (33 * 33) + ((g >> 3) + 1) * 33 + ((b >> 3) + 1);
            vwt[i]++;
            vmr[i] += r;
            vmg[i] += g;
            vmb[i] += b;
            m2[i] += (float)(sq[r] + sq[g] + sq[b]);
        }
}

// At end of histogram step, we can interpret
//   wt[r][g][b] = sum over voxel of P(c)
//   mr[r][g][b] = sum over voxel of r*P(c), " for mg, mb
//   m2[r][g][b] = sum over voxel of c^2*P(c)
// Divide by pic.x*pic.y for prob. range {0 to 1}
// Convert histogram to moments for rapid calculation of sums of ^ over any desired box
static void M3d(long *vwt, long *vmr, long *vmg, long *vmb, float *m2)
{
    float line2, area2[33];
    long line, lr, lg, lb, area[33], ar[33], ag[33], ab[33];
    unsigned short ind1, ind2;
    unsigned char i, r, g, b;

    for (r = 1; r <= 32; r++)
    {
        for (i = 0; i <= 32; i++)
            area2[i] = area[i] = ar[i] = ag[i] = ab[i] = 0;
        for (g = 1; g <= 32; g++)
        {
            line2 = line = lr = lg = lb = 0;
            for (b = 1; b <= 32; b++)
            {
                ind1 = r * (33 * 33) + g * 33 + b; //[r][g][b]
                line += vwt[ind1];
                lr += vmr[ind1];
                lg += vmg[ind1];
                lb += vmb[ind1];
                line2 += m2[ind1];
                area[b] += line;
                ar[b] += lr;
                ag[b] += lg;
                ab[b] += lb;
                area2[b] += line2;
                ind2 = ind1 - 33 * 33; //[r-1][g][b]
                vwt[ind1] = vwt[ind2] + area[b];
                vmr[ind1] = vmr[ind2] + ar[b];
                vmg[ind1] = vmg[ind2] + ag[b];
                vmb[ind1] = vmb[ind2] + ab[b];
                m2[ind1] = m2[ind2] + area2[b];
            }
        }
    }
}

// Compute sum over box of any given statistic
static long Vol(box *cube, long *mmt)
{
    return (mmt[cube->r1 * (33 * 33) + cube->g1 * 33 + cube->b1] - mmt[cube->r1 * (33 * 33) + cube->g1 * 33 + cube->b0] - mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b1] + mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b0] - mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b1] + mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b0] + mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b1] - mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b0]);
}

// Bot&Top allow faster calculation of Vol() for proposed subbox of given box.
// Sum of Top&Bot is Vol() of subbox split in given direction & w/specified new upper bound.
// Compute part of Vol(cube,mmt) that doesn't depend on r1/g1/b1 (depending on dir)
static long Bot(box *cube, unsigned char dir, long *mmt)
{
    switch (dir)
    {
    case 2:
        return (-mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b1] + mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b0] + mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b1] - mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b0]);
        break;
    case 1:
        return (-mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b1] + mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b0] + mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b1] - mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b0]);
        break;
    case 0:
        return (-mmt[cube->r1 * (33 * 33) + cube->g1 * 33 + cube->b0] + mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b0] + mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b0] - mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b0]);
        break;
    default:
        __builtin_unreachable(); // tells MSVC default can't be reached
    }
    return 1;
}

// Compute rest of Vol(cube,mmt), substituting pos for r1/g1/b1 (depending on dir)
static long Top(box *cube, unsigned char dir, long pos, long *mmt)
{
    switch (dir)
    {
    case 2:
        return (+mmt[pos * (33 * 33) + cube->g1 * 33 + cube->b1] - mmt[pos * (33 * 33) + cube->g1 * 33 + cube->b0] - mmt[pos * (33 * 33) + cube->g0 * 33 + cube->b1] + mmt[pos * (33 * 33) + cube->g0 * 33 + cube->b0]);
        break;
    case 1:
        return (+mmt[cube->r1 * (33 * 33) + pos * 33 + cube->b1] - mmt[cube->r1 * (33 * 33) + pos * 33 + cube->b0] - mmt[cube->r0 * (33 * 33) + pos * 33 + cube->b1] + mmt[cube->r0 * (33 * 33) + pos * 33 + cube->b0]);
        break;
    case 0:
        return (+mmt[cube->r1 * (33 * 33) + cube->g1 * 33 + pos] - mmt[cube->r1 * (33 * 33) + cube->g0 * 33 + pos] - mmt[cube->r0 * (33 * 33) + cube->g1 * 33 + pos] + mmt[cube->r0 * (33 * 33) + cube->g0 * 33 + pos]);
        break;
    default:
        __builtin_unreachable(); // tells MSVC default can't be reached
    }
    return 1;
}

// Compute weighted variance of box. NB: as w/raw statistics, this is really variance * pic.x*pic.y
static float Var(box *cube)
{
    float dr, dg, db, xx;

    xx = +m2[cube->r1 * (33 * 33) + cube->g1 * 33 + cube->b1] - m2[cube->r1 * (33 * 33) + cube->g1 * 33 + cube->b0] - m2[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b1] + m2[cube->r1 * (33 * 33) + cube->g0 * 33 + cube->b0] - m2[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b1] + m2[cube->r0 * (33 * 33) + cube->g1 * 33 + cube->b0] + m2[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b1] - m2[cube->r0 * (33 * 33) + cube->g0 * 33 + cube->b0];
    dr = Vol(cube, mr);
    dg = Vol(cube, mg);
    db = Vol(cube, mb);
    return (xx - (dr * dr + dg * dg + db * db) / (float)Vol(cube, wt));
}

// Minimize sum of variances of 2 subboxes. The sum(c^2) terms can be ignored since their sum
// over both subboxes is the same (sum for whole box) no matter where we split.
// The remaining terms have - sign in variance formula; we drop - and MAXIMIZE sum of the 2 terms.
static float Maximize(box *cube, unsigned char dir, long first, long last, long *cut,
                      long wr, long wg, long wb, long ww)
{
    float f, fmax;
    long i, hr, hg, hb, hw, br, bg, bb, bw;

    br = Bot(cube, dir, mr);
    bg = Bot(cube, dir, mg);
    bb = Bot(cube, dir, mb);
    bw = Bot(cube, dir, wt);
    fmax = 0.0;
    *cut = -1;
    for (i = first; i < last; i++)
    {
        hr = br + Top(cube, dir, i, mr);
        hg = bg + Top(cube, dir, i, mg);
        hb = bb + Top(cube, dir, i, mb);
        hw = bw + Top(cube, dir, i, wt);
        // now hx is sum over lower half of box, if split at i
        if (!hw)
            continue; // subbox could be empty of pixels! never split into an empty box
        f = ((float)hr * hr + (float)hg * hg + (float)hb * hb) / hw;

        hr = wr - hr;
        hg = wg - hg;
        hb = wb - hb;
        hw = ww - hw;
        if (!hw)
            continue; // subbox could be empty of pixels! never split into an empty box
        f += ((float)hr * hr + (float)hg * hg + (float)hb * hb) / hw;
        if (f > fmax)
        {
            fmax = f;
            *cut = i;
        }
    }
    return (fmax);
}

static long Cut(box *set1, box *set2)
{
    float maxr, maxg, maxb;
    long cutr, cutg, cutb, wr, wg, wb, ww;
    unsigned char dir;

    wr = Vol(set1, mr);
    wg = Vol(set1, mg);
    wb = Vol(set1, mb);
    ww = Vol(set1, wt);

    maxr = Maximize(set1, 2, set1->r0 + 1, set1->r1, &cutr, wr, wg, wb, ww);
    maxg = Maximize(set1, 1, set1->g0 + 1, set1->g1, &cutg, wr, wg, wb, ww);
    maxb = Maximize(set1, 0, set1->b0 + 1, set1->b1, &cutb, wr, wg, wb, ww);

    if ((maxr >= maxg) && (maxr >= maxb))
    {
        dir = 2;
        if (cutr < 0)
            return (0);
    } // can't split box
    else if ((maxg >= maxr) && (maxg >= maxb))
        dir = 1;
    else
        dir = 0;

    set2->r1 = set1->r1;
    set2->g1 = set1->g1;
    set2->b1 = set1->b1;

    switch (dir)
    {
    case 2:
        set2->r0 = set1->r1 = cutr;
        set2->g0 = set1->g0;
        set2->b0 = set1->b0;
        break;
    case 1:
        set2->g0 = set1->g1 = cutg;
        set2->r0 = set1->r0;
        set2->b0 = set1->b0;
        break;
    case 0:
        set2->b0 = set1->b1 = cutb;
        set2->r0 = set1->r0;
        set2->g0 = set1->g0;
        break;
    }
    set1->vol = (set1->r1 - set1->r0) * (set1->g1 - set1->g0) * (set1->b1 - set1->b0);
    set2->vol = (set2->r1 - set2->r0) * (set2->g1 - set2->g0) * (set2->b1 - set2->b0);
    return (1);
}

static void Mark(box *cube, long label, unsigned char *tag)
{
    long r, g, b;

    for (r = cube->r0 + 1; r <= cube->r1; r++)
        for (g = cube->g0 + 1; g <= cube->g1; g++)
            for (b = cube->b0 + 1; b <= cube->b1; b++)
                tag[(r << 10) + (r << 6) + r + (g << 5) + g + b] = label;
}

void genpal_free()
{
    if (tag)
    {
        free(tag);
        tag = 0;
    }
    if (m2)
    {
        free(m2);
        m2 = 0;
    }
    if (mb)
    {
        free(mb);
        mb = 0;
    }
    if (mg)
    {
        free(mg);
        mg = 0;
    }
    if (mr)
    {
        free(mr);
        mr = 0;
    }
    if (wt)
    {
        free(wt);
        wt = 0;
    }
}

long genpal_init()
{
    // Eats 33*33*33*4*5 = 718740 bytes total
    wt = (long *)malloc(33 * 33 * 33 * sizeof(long));
    if (!wt)
    {
        genpal_free();
        return (-1);
    }
    mr = (long *)malloc(33 * 33 * 33 * sizeof(long));
    if (!mr)
    {
        genpal_free();
        return (-1);
    }
    mg = (long *)malloc(33 * 33 * 33 * sizeof(long));
    if (!mg)
    {
        genpal_free();
        return (-1);
    }
    mb = (long *)malloc(33 * 33 * 33 * sizeof(long));
    if (!mb)
    {
        genpal_free();
        return (-1);
    }
    m2 = (float *)malloc(33 * 33 * 33 * sizeof(float));
    if (!m2)
    {
        genpal_free();
        return (-1);
    }
    memset(wt, 0, 33 * 33 * 33 * sizeof(long));
    memset(mr, 0, 33 * 33 * 33 * sizeof(long));
    memset(mg, 0, 33 * 33 * 33 * sizeof(long));
    memset(mb, 0, 33 * 33 * 33 * sizeof(long));
    memset(m2, 0, 33 * 33 * 33 * sizeof(float));
    return (0);
}

void genpal_addhist(tiletype *tt)
{
    Hist3d(tt, (long *)wt, (long *)mr, (long *)mg, (long *)mb, (float *)m2); // printf("Histogram done\n");
}

void genpal(long *pal)
{
    box cube[256];
    float f, vv[256];
    long i, k, n, w, colsiz;

    M3d((long *)wt, (long *)mr, (long *)mg, (long *)mb, (float *)m2); // printf("Moments done\n");
    colsiz = 256;
    cube[0].r0 = cube[0].g0 = cube[0].b0 = 0;
    cube[0].r1 = cube[0].g1 = cube[0].b1 = 32;
    n = 0;
    for (i = 1; i < colsiz; i++)
    {
        if (Cut(&cube[n], &cube[i]))
        { // volume test ensures we won't try to cut one-cell box
            vv[n] = (cube[n].vol > 1) ? Var(&cube[n]) : 0.0;
            vv[i] = (cube[i].vol > 1) ? Var(&cube[i]) : 0.0;
        }
        else
        {
            vv[n] = 0.0; // don't try to split this box again
            i--;         // didn't create box i
        }
        n = 0;
        f = vv[0];
        for (k = 1; k <= i; k++)
            if (vv[k] > f)
            {
                f = vv[k];
                n = k;
            }
        if (f <= 0.0)
        {
            colsiz = i + 1;
            // fprintf(stderr,"Only got %d boxes\n",colsiz);
            break;
        }
    }
    // printf("Partition done\n");
    if (m2)
    {
        free(m2);
        m2 = 0;
    }

    tag = (unsigned char *)malloc(33 * 33 * 33);
    if (!tag)
    {
        printf("Not enough space\n");
        exit(1);
    }
    for (k = 0; k < colsiz; k++)
    {
        Mark(&cube[k], k, tag);
        w = Vol(&cube[k], wt);
        if (!w)
        {
            pal[k] = 0;
            continue;
        }
        pal[k] = ((Vol(&cube[k], mr) / w) << 16) +
                 ((Vol(&cube[k], mg) / w) << 8) +
                 (Vol(&cube[k], mb) / w);
    }
}

void genpal_32to8(tiletype *rt, tiletype *wt)
{
    long i, x, y, xe, ye, *lptr;
    unsigned char *cptr;

    cptr = (unsigned char *)(intptr_t)wt->f;
    i = 0;
    lptr = (long *)(intptr_t)rt->f;
    xe = rt->x;
    if (xe < wt->x)
        xe = wt->x;
    ye = rt->y;
    if (ye < wt->y)
        ye = wt->y;
    for (y = 0; y < ye; y++, cptr += wt->p, lptr = (long *)(intptr_t)(((intptr_t)lptr) + rt->p))
        for (x = 0; x < xe; x++, i++)
            cptr[x] = tag[((((lptr[x] >> 16) & 255) >> 3) + 1) * (33 * 33) +
                          ((((lptr[x] >> 8) & 255) >> 3) + 1) * 33 +
                          ((((lptr[x]) & 255) >> 3) + 1)];
}

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

    // Print sign at fixed coordinates (for debugging or watermark)
    signprint(426, 982);

    // Process and integrate the voxel data into the volume structure
    process_voxel_data(volume, settings, buf);

    printf("Done!\n");
}


#if 0
!endif
#endif
