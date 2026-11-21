/*
 * flux - floppy track decoding and image containers (RMFloppyEmu, Disk Wrangler)
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
 * Reader for SuperCard Pro (.scp) flux images, as written by the SuperCard Pro, Greaseweazle,
 * FluxEngine and others. Specification: Jim Drew's scp_image_specs.txt; details cross-checked with
 * Greaseweazle's src/greaseweazle/image/scp.py.
 *
 *   0x00  "SCP", version, disk type, revolutions, start track, end track,
 *         flags (bit 0 index-cued, 1 96 tpi, 2 360 rpm, 3 normalised, 4 read/write, 5 footer,
 *         6 extended mode), cell width (0 = 16 bits), heads (0 both, 1 side 0 only, 2 side 1 only),
 *         resolution (sample period = 25 ns * (resolution + 1)), u32 checksum of bytes 0x10 onward
 *   0x10  168 u32 track data header offsets (0 = no track); track number = cylinder * 2 + side
 *   TDH   "TRK", track number, then per revolution: u32 index time (ticks), u32 flux count,
 *         u32 offset of its flux data from the TDH
 *   flux  big-endian u16 intervals in ticks; 0000 means "add 65536 to the next value"
 *
 * Revolutions are read as one stream from the start of the chosen one to the end of the last, so a
 * PLL can run on past the index (flux_pll.h). Reading is through a callback and a small buffer in
 * scp_flux_stream_t: an 18 MB image is never held in memory. Nothing is allocated.
 *
 * Not supported: cell widths other than 16 bits and extended mode (scp_open returns
 * SCP_ERR_UNSUPPORTED). Legacy single-sided images that number their tracks consecutively are read
 * as if they were cylinder * 2 + side, which is wrong for them; none have been seen here yet.
 */
#ifndef FLUX_SCP_H
#define FLUX_SCP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCP_OK = 0,
    SCP_ERR_IO,          /* read failed, or the file is shorter than its tables say */
    SCP_ERR_SIGNATURE,   /* not an SCP file, or a track header without "TRK" */
    SCP_ERR_UNSUPPORTED, /* cell width, extended mode */
    SCP_ERR_NO_TRACK,    /* no data for this cylinder/side */
    SCP_ERR_RANGE,       /* revolution number out of range */
    SCP_ERR_TRACK,       /* the track header is inconsistent */
} scp_status_t;

#define SCP_MAX_TRACKS 168u
#define SCP_MAX_REVS   16u
#define SCP_FLAG_INDEX 0x01u
#define SCP_FLAG_96TPI 0x02u
#define SCP_FLAG_360RPM 0x04u
#define SCP_FLAG_NORMALISED 0x08u
#define SCP_FLAG_FOOTER 0x20u
#define SCP_FLAG_EXTENDED 0x40u

typedef bool (*scp_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);

typedef struct {
    scp_read_fn read;
    void *ctx;
    uint8_t version, disk_type, revolutions, start_track, end_track, flags, cell_width, heads, resolution;
    uint32_t checksum;
    uint32_t sample_hz; /* 40 MHz / (resolution + 1) */
} scp_image_t;

typedef struct {
    uint32_t index_ticks; /* duration of the revolution, index to index */
    uint32_t nflux;       /* flux values (u16 words, counting 0000 overflow words) */
    uint32_t data_offset; /* file offset of its flux data */
} scp_rev_t;

typedef struct {
    uint8_t track;    /* cylinder * 2 + side */
    uint8_t nrevs;    /* revolutions with data (trailing empty ones dropped) */
    uint32_t tdh_offset;
    scp_rev_t revs[SCP_MAX_REVS];
} scp_track_t;

/* A stream of flux intervals from a file, through a small buffer. */
typedef struct {
    const scp_image_t *img;
    uint32_t pos, end; /* file offsets */
    uint8_t buf[256];
    uint32_t buf_pos, buf_len;
    bool failed;       /* a read failed */
} scp_flux_stream_t;

scp_status_t scp_open(scp_image_t *img, scp_read_fn read, void *ctx);

/* Reads the track header for cylinder/side. SCP_ERR_NO_TRACK if the image has none. */
scp_status_t scp_read_track(const scp_image_t *img, unsigned cyl, unsigned side, scp_track_t *out);

/* Starts a flux stream at revolution rev, running to the end of the track's last revolution. */
scp_status_t scp_flux_open(scp_flux_stream_t *s, const scp_image_t *img, const scp_track_t *t, unsigned rev);

/* flux_next_fn for flux_pll_decode(): the next interval in ticks, overflow words folded in. */
bool scp_flux_next(void *stream, uint32_t *ticks);

/* Sums bytes 0x10 to the end (len = file size) and compares with the header's checksum. */
scp_status_t scp_verify_checksum(const scp_image_t *img, uint32_t file_len, bool *ok);

const char *scp_status_name(scp_status_t st);

#ifdef __cplusplus
}
#endif

#endif
