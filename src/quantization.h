/* Goxel 3D voxels editor
 *
 * copyright (c) 2016 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef QUANTIZATION_H
#define QUANTIZATION_H

#include "volume.h"

#include <stdint.h>

// Generate a palette of up to `nb` colors from a volume.
// Exact colours if unique count <= nb; otherwise median-cut quantization.
// `exclude` / `n_exclude`: optional opaque RGBs to omit (NULL / 0 if none).
void quantization_gen_palette(const volume_t *volume, int nb,
                              uint8_t (*palette)[4],
                              const uint8_t (*exclude)[4], int n_exclude);

// Nearest opaque palette index by Manhattan RGB distance.  Skips slots with
// alpha != 255.  Returns -1 if no usable slot.
int quantization_nearest(const uint8_t c[4],
                         const uint8_t (*palette)[4], int n);

// Remap every opaque voxel in `volume` to the nearest palette colour.
void quantization_remap_volume(volume_t *volume,
                               const uint8_t (*palette)[4], int n);

// Snap RGB channels to a uniform grid (step in 1..255; 1 leaves channels unchanged).
void quantization_uniform_snap(const uint8_t in[4], int step, uint8_t out[4]);

// Remap every opaque voxel by uniform RGB snapping.
void quantization_remap_volume_uniform(volume_t *volume, int step);

#endif // QUANTIZATION_H
