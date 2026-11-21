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
 * ImageDisk (.IMD) images (ImageDisk 1.18 documentation, Dave Dunfield): an ASCII header ending in 1A,
 * then one record per track: mode (0-2 FM, 3-5 MFM at 500/300/250 kbit/s), cylinder, head (bit 7: a
 * cylinder map follows, bit 6: a head map follows), sector count, size code (FF: a table of 16-bit
 * sizes follows), the sector numbering map, the optional maps, then per sector a data record:
 * 0 unavailable; 1/3/5/7 data (normal/deleted/error/deleted+error); 2/4/6/8 the same, compressed to
 * one fill byte.
 *
 * disk_imd_open() indexes the file once (where each track's record is); disk_imd_load() reads one
 * track into a disk_track_t, sectors in recorded order, placed with the IBM gaps MAME uses for an IMD
 * (disk_imd.c, imd_lay_out). Nothing is allocated.
 */
#ifndef RMFE_CORE_DISK_IMD_H
#define RMFE_CORE_DISK_IMD_H

#include <stdbool.h>
#include <stdint.h>

#include "core/disk.h"

#define DISK_IMD_MAX_CYLS 84u

typedef struct {
    disk_read_fn read;
    void *ctx;
    uint32_t file_len;
    uint32_t first_track;                     /* offset of the first track record */
    uint32_t track_off[DISK_IMD_MAX_CYLS][2]; /* 0: no record for that cylinder/side */
    uint8_t cylinders, sides;                 /* 1 + highest cylinder / side with a record */
    uint32_t scan_at;                         /* while opening in steps: the next track record to index */
} disk_imd_t;

/* Opens an image: checks the header and indexes every track record. The same as disk_imd_open_begin() followed by
 * disk_imd_open_step() until done. */
disk_status_t disk_imd_open(disk_imd_t *img, disk_read_fn read, void *ctx, uint32_t file_len);

/* Opening in steps, so the platform can open an image a little at a time between other work (docs/plan.md,
 * milestone 6: the 480Z must not wait while an image is opened). begin checks the header; each step indexes one
 * track record and sets *done once the whole file is indexed. The image is usable only once done; an error at any
 * step means it isn't an image (as disk_imd_open would have said). */
disk_status_t disk_imd_open_begin(disk_imd_t *img, disk_read_fn read, void *ctx, uint32_t file_len);
disk_status_t disk_imd_open_step(disk_imd_t *img, bool *done);

/* Loads one track. A cylinder/side with no record loads as an empty (unformatted) track. */
disk_status_t disk_imd_load(const disk_imd_t *img, unsigned cyl, unsigned side, disk_track_t *out);

/*
 * Writes a changed track back into the file in place, when every sector's record keeps its length:
 * the same sectors (IDs, order, sizes, recorded C/H), an uncompressed sector staying uncompressed, a
 * compressed one whose new data is still one repeated byte. Nothing is written unless all of it fits;
 * otherwise DISK_ERR_LAYOUT, and the caller uses disk_imd_rewrite().
 */
disk_status_t disk_imd_update(const disk_imd_t *img, disk_write_fn write, void *ctx, const disk_track_t *t);

/*
 * Writes the whole image anew to sink, with track t in place of (or added to) the image's record for
 * its cylinder/side; an empty t leaves that track out (unformatted). The caller writes to a new file and
 * renames it over the old one, then reopens it with disk_imd_open().
 * The new image is uncompressed (owner, 2026-11-21): every sector stored in full, the changed track's and
 * every other track's, whose compressed sector records (one fill byte) are expanded. The header, and every
 * other part of every other track, are copied byte for byte. So after one rewrite, every later write goes in
 * place (disk_imd_update) unless it changes a track's layout; a rewrite costs over a second on the SD card,
 * an in-place write very little. An image that is only ever read, or written in place, is never changed.
 */
disk_status_t disk_imd_rewrite(const disk_imd_t *img, const disk_track_t *t, disk_sink_fn sink, void *ctx);

#endif
