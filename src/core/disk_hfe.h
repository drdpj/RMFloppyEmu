/*
 * RMFloppyEmu - a Raspberry Pi Pico 2 replacement for the RML 480Z's disk unit
 * Copyright (C) 2026 Daniel Jameson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * HFE images as discs: each track side is read with src/flux (hfe_read_track) and decoded
 * (ibm_track_decode_auto) into a disk_track_t, with each sector's real position. Sectors whose ID
 * field has a bad CRC are left out: no controller can find them by ID.
 *
 * The caller provides the cell buffer (at least the largest track side: about 12.5 KB for a
 * 250 kbit/s 300 rpm disc; hfe_track_bytes() says exactly), so nothing is allocated.
 */
#ifndef RMFE_CORE_DISK_HFE_H
#define RMFE_CORE_DISK_HFE_H

#include <stdint.h>

#include "core/disk.h"
#include "flux/hfe.h"
#include "flux/ibm_encode.h"
#include "flux/ibm_track.h"

typedef struct {
    hfe_image_t hfe;
    uint8_t *cells;
    uint32_t cells_cap;
    ibm_sector_t secs[DISK_MAX_SECTORS + 8]; /* room for ID-CRC-error sectors that are then dropped */
    ibm_enc_sector_t enc[DISK_MAX_SECTORS];   /* for disk_hfe_save (here, not on a small Pico stack) */
} disk_hfe_t;

disk_status_t disk_hfe_open(disk_hfe_t *img, disk_read_fn read, void *ctx, uint8_t *cells, uint32_t cells_cap);

/* Loads one track side. Beyond the image's cylinders: an empty track. */
disk_status_t disk_hfe_load(disk_hfe_t *img, unsigned cyl, unsigned side, disk_track_t *out);

/*
 * Writes a track back: re-encoded as FM/MFM (flux/ibm_encode.h) with every sector at its position,
 * then written in place over the track side (v1 images; v3 ones are DISK_ERR_READ_ONLY). The track's
 * length in the file never changes. Uses the cell buffer given to disk_hfe_open().
 */
disk_status_t disk_hfe_save(disk_hfe_t *img, disk_write_fn write, void *ctx, const disk_track_t *t);

#endif
