/* Mapmaker - Ace of Spades biome / heightmap utilities
 *
 * Ported from pyspades mapmaker.pyx by James Hofmann (2012), GPLv3.
 * Classicgen / genland excluded (see Genland filter).
 *
 * Used by the Biomes generate effect (Triplefox random.txt recipe).
 */

#ifndef FILTERS_BIOMES_MAPMAKER_H
#define FILTERS_BIOMES_MAPMAKER_H

#include <stdbool.h>
#include <stdint.h>

#define MM_MAP_SIZE 512
#define MM_GRAD_STEPS 64

typedef struct {
    uint32_t state;
} mm_rng_t;

void mm_rng_seed(mm_rng_t *rng, uint32_t seed);
float mm_rng_float(mm_rng_t *rng);
int mm_rng_int(mm_rng_t *rng, int lo, int hi);

/* --- Color / Gradient -------------------------------------------------- */

typedef struct {
    uint8_t r, g, b, a;
} mm_rgba_t;

typedef struct {
    mm_rgba_t steps[MM_GRAD_STEPS];
} mm_gradient_t;

void mm_gradient_init(mm_gradient_t *g);
void mm_gradient_set_step_rgb(mm_gradient_t *g, int step, int r, int g_, int b);
/* HSB as GIMP: H 0-360, S/B 0-100. Interpolates steps [start, end). */
void mm_gradient_hsb(mm_gradient_t *g, int start_pos, float sh, float ss, float sb,
                     int end_pos, float eh, float es, float eb);
/* RGB interpolation between 0-255 colors for steps [start, end). */
void mm_gradient_rgb(mm_gradient_t *g, int start_pos, const uint8_t sc[3],
                     int end_pos, const uint8_t ec[3]);
/* Flatten reversed steps to contiguous RGB triples (64*3 ints). */
void mm_gradient_array(const mm_gradient_t *g, int out[MM_GRAD_STEPS * 3]);

int mm_make_color(int r, int g, int b);
int mm_get_r(int color);
int mm_get_g(int color);
int mm_get_b(int color);
void mm_hsb_to_rgb(float h, float s, float b, int *r, int *g, int *bl);
/* Out: H 0-360, S/B 0-100 (GIMP-style), for GUI edits. */
void mm_rgb_to_hsb(int r, int g, int b, float *h, float *s, float *bri);

/* --- HeightMap --------------------------------------------------------- */

typedef struct {
    int width;
    int height;
    float *hmap; /* width*height */
    int *cmap;   /* width*height, packed colors or biome ids */
} mm_heightmap_t;

bool mm_heightmap_init(mm_heightmap_t *hm, float fill_height);
void mm_heightmap_free(mm_heightmap_t *hm);

float mm_hm_get(const mm_heightmap_t *hm, int x, int y);
float mm_hm_get_repeat(const mm_heightmap_t *hm, int x, int y);
void mm_hm_set(mm_heightmap_t *hm, int x, int y, float val);
void mm_hm_set_repeat(mm_heightmap_t *hm, int x, int y, float val);
void mm_hm_add_repeat(mm_heightmap_t *hm, int x, int y, float val);

int mm_hm_get_col(const mm_heightmap_t *hm, int x, int y);
int mm_hm_get_col_repeat(const mm_heightmap_t *hm, int x, int y);
void mm_hm_set_col_repeat(mm_heightmap_t *hm, int x, int y, int val);

void mm_hm_rect_noise(mm_heightmap_t *hm, mm_rng_t *rng,
                      int x, int y, int w, int h,
                      double jitter, double midpoint);
void mm_hm_rect_color(mm_heightmap_t *hm, int x, int y, int w, int h, int col);

void mm_hm_smoothing(mm_heightmap_t *hm);
/* Median-filter quantized column tops to remove single-block height flecks. */
void mm_hm_despeckle_heights(mm_heightmap_t *hm, int passes);
void mm_hm_midpoint_displace(mm_heightmap_t *hm, mm_rng_t *rng,
                             double jittervalue, double spanscalingmultiplier,
                             int skip);
void mm_hm_jitter_colors(mm_heightmap_t *hm, mm_rng_t *rng, double amount);
void mm_hm_truncate(mm_heightmap_t *hm);
void mm_hm_line_add(mm_heightmap_t *hm, int x, int y, int x2, int y2,
                    int radius, double depth);
void mm_hm_line_set(mm_heightmap_t *hm, int x, int y, int x2, int y2,
                    int radius, double height);
void mm_hm_rewrite_gradient_fill(mm_heightmap_t *hm, mm_rng_t *rng,
                                 const int *const *zcoldefs, int n_gradients);
void mm_hm_rgb_noise_colors(mm_heightmap_t *hm, mm_rng_t *rng, int low, int high);
void mm_hm_smooth_colors(mm_heightmap_t *hm);

/* --- Biome / BiomeMap -------------------------------------------------- */

typedef struct {
    mm_gradient_t *gradient; /* owned elsewhere */
    float height;
    float variation;
    float noise;
    int id;
} mm_biome_t;

#define MM_BIOME_MAP_MAX 64

typedef struct {
    int width;
    int height;
    int twidth;
    int theight;
    int n_biomes;
    mm_biome_t *biomes; /* borrowed, length n_biomes */
    mm_biome_t **tmap;  /* width*height */
    mm_gradient_t **gradients; /* n_biomes */
} mm_biomemap_t;

bool mm_biomemap_init(mm_biomemap_t *bm, mm_biome_t *biomes, int n_biomes,
                      int width, int height);
void mm_biomemap_free(mm_biomemap_t *bm);

mm_biome_t *mm_bm_get_repeat(const mm_biomemap_t *bm, int x, int y);
void mm_bm_set_repeat(mm_biomemap_t *bm, int x, int y, mm_biome_t *val);

typedef struct {
    int x, y;
    mm_biome_t *biome;
} mm_biome_point_t;

/* Append up to max_out points; returns count written. */
int mm_bm_random_points(mm_biomemap_t *bm, mm_rng_t *rng,
                        int qty, mm_biome_t *biome,
                        int x, int y, int w, int h,
                        mm_biome_point_t *out, int max_out);
void mm_bm_point_flood(mm_biomemap_t *bm, const mm_biome_point_t *points,
                       int n_points);
void mm_bm_jitter(mm_biomemap_t *bm, mm_rng_t *rng);

/* Fills hmap from biome tiles; returns false on alloc failure. */
bool mm_bm_create_heightmap(mm_biomemap_t *bm, mm_rng_t *rng,
                            mm_heightmap_t *out_hm);
void mm_bm_rect_of_point(const mm_biomemap_t *bm, int x, int y,
                         int *left, int *top, int *right, int *bottom);

#endif /* FILTERS_BIOMES_MAPMAKER_H */
