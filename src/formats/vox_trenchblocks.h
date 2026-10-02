/* Goxel 3D voxels editor
 *
 * copyright (c) 2026
 *
 * Shared MagicaVoxel .vox (Trenchblocks) palette helpers used by the export
 * format and the export-settings window.
 */

#ifndef VOX_TRENCHBLOCKS_H
#define VOX_TRENCHBLOCKS_H

#include "image.h"

#include <stdbool.h>
#include <stdint.h>

#define TB_PAL_META_FIRST 1
#define TB_PAL_META_LAST 8
#define TB_PAL_RESERVED_LAST 16
#define TB_PAL_MAP_FIRST 17

enum {
    TB_REDUCE_MEDIAN_CUT = 0,
    TB_REDUCE_OCTREE = 1,
    TB_REDUCE_WU = 2,
    TB_REDUCE_KMEANS = 3,
    TB_REDUCE_UNIFORM = 4,
};

void tb_palette_ensure_init(image_t *img);
void tb_palette_reset(image_t *img);
bool tb_palette_add_rgb(image_t *img, const uint8_t rgb[4]);
bool tb_palette_set_at(image_t *img, int idx, const uint8_t rgb[4]);
bool tb_palette_remove_at(image_t *img, int idx);

bool tb_forced_map_has_rgb(const image_t *img, const uint8_t c[4]);
int tb_count_forced_map(const image_t *img);
int tb_reduce_available_slots(const image_t *img);

/*
 * Simulate the color-reduction pipeline for Total column display.
 * Writes totals[0] = outstanding after atlas, totals[1..n] after each middle
 * step (global unique count), totals[n+1] after final fill into remaining
 * atlas slots (img->tb_reduce_final_method).  totals_len must be >= n_steps + 2.
 * Returns 0 on success, -1 on failure.
 */
int tb_reduce_simulate_totals(const image_t *img,
                              const tb_reduce_step_t *steps, int n_steps,
                              int *totals, int totals_len);

/*
 * After middle pipeline steps only (not the final remaining-slots fill),
 * write per top-level visible root: layer id and unique colours still
 * outstanding beyond the forced atlas.  If out_global_after is non-NULL,
 * also writes the map-wide outstanding unique count (shared colours once).
 * Returns root count, or -1.
 */
int tb_reduce_per_layer_after_middle(const image_t *img,
                                     const tb_reduce_step_t *steps,
                                     int n_steps,
                                     int *out_layer_ids,
                                     int *out_after,
                                     int max_out,
                                     int *out_global_after);

/*
 * Apply middle pipeline steps to per-root working volumes, merge into a single
 * volume, and fill remaining map palette slots (tb_reduce_final_method,
 * exclude forced).  *out_volume receives a new volume the caller must
 * volume_delete.  palette is 256 entries already seeded with the atlas;
 * n_forced is the count of forced opaque map slots (compact layout assumed).
 * Returns 0 on success, -1 on failure.
 */
int tb_reduce_prepare_export_volume(const image_t *img,
                                    uint8_t (*palette)[4], int n_forced,
                                    volume_t **out_volume);

#endif /* VOX_TRENCHBLOCKS_H */
