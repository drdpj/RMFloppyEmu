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
 * The disc as the unit's command layer sees it: one track at a time, its sectors in rotational order
 * with their IDs, status, data and position in the revolution. Every image format (disk_imd.h,
 * disk_hfe.h) loads into this model, so the commands never see the format.
 *
 * Positions are in 2 us cells, 100000 to the revolution (300 rpm). They matter: a track may hold
 * several sectors with the same ID, and which one a read gets depends on where the disc is when the
 * command arrives (the 4MATION protection, docs/protocol.md). IMD has no positions, so its sectors are
 * placed in their recorded order with standard IBM gaps, as MAME lays an IMD out; HFE positions are real.
 */
#ifndef RMFE_CORE_DISK_H
#define RMFE_CORE_DISK_H

#include <stdbool.h>
#include <stdint.h>

#define DISK_CELLS_PER_REV  100000u /* 2 us cells at 300 rpm */
#define DISK_MAX_SECTORS    32u
#define DISK_MAX_TRACK_DATA 8192u /* bytes of sector data per track (RM formats use up to 4608) */
#define DISK_MAX_SIZE_CODE  6u    /* 128 << 6 = 8192 */

typedef enum {
    DISK_OK = 0,
    DISK_ERR_IO,        /* read/write callback failed, or the file is shorter than its structure says */
    DISK_ERR_FORMAT,    /* not a valid image of this kind */
    DISK_ERR_RANGE,     /* cylinder/side not in the image */
    DISK_ERR_FULL,      /* the track has more sectors or data than disk_track_t holds */
    DISK_ERR_LAYOUT,    /* (IMD) the change can't be made in place: rewrite the file (disk_imd_rewrite) */
    DISK_ERR_READ_ONLY, /* the image can't be written (an HFE v3, or one marked read-only) */
} disk_status_t;

/* disk_sector_t.flags */
#define DISK_SEC_CRC_ERROR 0x01u /* data read with a CRC error (the data is as recorded) */
#define DISK_SEC_NO_DATA   0x02u /* ID only: no data field */
#define DISK_SEC_DELETED   0x04u /* deleted-data mark */

typedef struct {
    uint8_t c, h, r, n; /* the ID as recorded (RM discs record head 0 on side 1) */
    uint8_t flags;      /* DISK_SEC_* */
    uint16_t size;      /* data bytes (128 << n, or as the image says) */
    uint32_t data_off;  /* into disk_track_t.data */
    uint32_t pos;       /* start of the ID field, in cells from the index (0 .. DISK_CELLS_PER_REV-1) */
} disk_sector_t;

typedef struct {
    uint8_t cyl, side;
    bool fm;            /* FM (single density) rather than MFM */
    uint16_t rate_kbps; /* nominal data rate: 125 (FM), 250 (MFM), ... */
    uint8_t count;      /* sectors, in rotational order */
    disk_sector_t secs[DISK_MAX_SECTORS];
    uint32_t data_used;
    uint8_t data[DISK_MAX_TRACK_DATA];
} disk_track_t;

/* Empties a track (an unformatted track has no sectors). */
void disk_track_clear(disk_track_t *t, uint8_t cyl, uint8_t side);

/* Appends a sector; data may be NULL (zeros). Returns DISK_ERR_FULL if it doesn't fit. */
disk_status_t disk_track_add(disk_track_t *t, const disk_sector_t *s, const uint8_t *data);

/*
 * The sector a read for ID r would find with the disc at rotational position pos: the first sector
 * with that R at or after pos, going round the index.
 * Returns its index, or -1 if the track has no such sector. Only R is matched: the 480Z asks by
 * record, and RM discs record head 0 on side 1, so H can't be checked (docs/protocol.md).
 */
int disk_track_find(const disk_track_t *t, uint8_t r, uint32_t pos);

/*
 * Stores data into sector idx as a controller's Write Sector does: normal data mark, good CRC, the ID
 * and position unchanged. data holds the sector's size in bytes.
 */
void disk_track_write(disk_track_t *t, unsigned idx, const uint8_t *data);

/* As disk_track_write, for len bytes at offset within the sector (a 128-byte record of a larger
 * sector). The rest of the sector keeps its data. */
void disk_track_write_part(disk_track_t *t, unsigned idx, uint32_t offset, const uint8_t *data, uint32_t len);

/* I/O callbacks shared by the image backends (FatFs file, host FILE*, memory). */
typedef bool (*disk_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);
typedef bool (*disk_write_fn)(void *ctx, uint32_t offset, const void *buf, uint32_t len);
/* Appends len bytes to a new file being written from the start (for a whole-file rewrite). */
typedef bool (*disk_sink_fn)(void *ctx, const void *buf, uint32_t len);

#endif
