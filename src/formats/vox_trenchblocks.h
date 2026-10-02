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

void tb_palette_ensure_init(image_t *img);
void tb_palette_reset(image_t *img);
bool tb_palette_add_rgb(image_t *img, const uint8_t rgb[4]);
bool tb_palette_set_at(image_t *img, int idx, const uint8_t rgb[4]);
bool tb_palette_remove_at(image_t *img, int idx);

#endif /* VOX_TRENCHBLOCKS_H */
