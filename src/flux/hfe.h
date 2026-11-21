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
 * Reader for HxC Floppy Emulator HFE images, v1 ("HXCPICFE") and v3 ("HXCHFEV3").
 *
 * An HFE file holds each track side as a stream of bitcells at a fixed rate (twice the nominal
 * data bit rate in the header: 250 kbit/s -> 2 us cells). hfe_read_track() returns one side as
 * bitcells, MSB first, ready for ibm_track_decode(). Format, as implemented here (cross-checked
 * with the HxC documentation as quoted in MAME's lib/formats/hxchfe_dsk.cpp and with
 * Greaseweazle's image/hfe.py):
 *
 *   0x000  header, 512 bytes (little-endian):
 *          00 signature[8]   "HXCPICFE" (v1) or "HXCHFEV3" (v3)
 *          08 format revision (0 or 1)     09 cylinders     0A sides (1 or 2)
 *          0B track encoding (0 ISO/IBM MFM, 1 Amiga MFM, 2 ISO/IBM FM, 3 EMU FM, FF unknown)
 *          0C bit rate, kbit/s (u16)       0E RPM (u16, often 0)
 *          10 floppy interface mode        11 (unused)
 *          12 track list offset, in 512-byte blocks (u16)
 *          14 write allowed (FF = yes)     15 single step (FF = yes; 00 = double step)
 *          16 track 0 side 0 uses its own encoding (00 = yes, FF = no)   17 that encoding
 *          18 track 0 side 1 uses its own encoding (00 = yes, FF = no)   19 that encoding
 *   track list: per cylinder, u16 offset (512-byte blocks), u16 length (bytes, both sides)
 *   track data: 512-byte blocks of 256 bytes side 0 then 256 bytes side 1; the last block may be
 *          partly used (side 1 still starts at +256). Within each byte the first cell is bit 0.
 *   v3 only: after bit reversal a byte Fx is an opcode - F0 nop; F1 index; F2 r: set the cell
 *          time to r/36 MHz; F3 n x: x << n gives 8-n cells (the top 8-n bits); F4 random (weak)
 *          cells, 8 of them (or, as the byte after F3 n, 8-n). Cells returned for weak areas are 0.
 *
 * I/O is by callback, so the file can be in memory, on a host file system or on an SD card.
 * No memory is allocated: hfe_read_track() works in the caller's buffer.
 */
#ifndef FLUX_HFE_H
#define FLUX_HFE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HFE_OK = 0,
    HFE_ERR_IO,        /* the read callback failed or the file is shorter than the header/table say */
    HFE_ERR_SIGNATURE, /* not an HFE file */
    HFE_ERR_HEADER,    /* header values out of range (revision, cylinders, sides) */
    HFE_ERR_RANGE,     /* cylinder or side not in the image */
    HFE_ERR_BUFFER,    /* the caller's buffer is too small for the track */
    HFE_ERR_TRACK,     /* bad v3 opcode stream */
    HFE_ERR_READ_ONLY, /* writing a v3 image, or one whose header forbids writes */
} hfe_status_t;

/* Header track encodings. */
#define HFE_ENC_ISOIBM_MFM 0x00
#define HFE_ENC_AMIGA_MFM  0x01
#define HFE_ENC_ISOIBM_FM  0x02
#define HFE_ENC_EMU_FM     0x03
#define HFE_ENC_UNKNOWN    0xFF

/* Reads len bytes at offset into buf. Returns true only if all len bytes were read. */
typedef bool (*hfe_read_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);

typedef struct {
    hfe_read_fn read;
    void *ctx;
    uint8_t version; /* 1 or 3 */
    uint8_t cylinders;
    uint8_t sides;
    uint8_t encoding; /* HFE_ENC_* for every track except an overridden track 0 side */
    uint16_t bitrate_kbps;
    uint16_t rpm;
    uint8_t interface_mode;
    bool write_allowed;
    bool double_step;
    uint8_t track0_encoding[2]; /* per side: the encoding track 0 uses (the header's own if not overridden) */
    uint16_t track_list_block;
} hfe_image_t;

typedef struct {
    uint32_t ncells;        /* bitcells in the buffer, MSB first */
    uint8_t encoding;       /* HFE_ENC_* the header gives for this track */
    uint16_t bitrate_kbps;  /* nominal: header rate, or (v3) from the first Bitrate opcode */
    uint32_t index_cell;    /* v3: cell of the first Index opcode; 0 otherwise */
    uint32_t weak_cells;    /* v3: cells marked random (returned as 0) */
} hfe_track_t;

/* Writes len bytes at offset. Returns true only if all were written. */
typedef bool (*hfe_write_fn)(void *ctx, uint32_t offset, const void *buf, uint32_t len);

/* Reads and checks the header. Keeps read/ctx for later calls. */
hfe_status_t hfe_open(hfe_image_t *img, hfe_read_fn read, void *ctx);

/* Bytes of buffer hfe_read_track() needs for this track side. */
hfe_status_t hfe_track_bytes(const hfe_image_t *img, unsigned cyl, unsigned side, uint32_t *bytes);

/* Reads one track side into buf (cap bytes) as bitcells, MSB first. */
hfe_status_t hfe_read_track(const hfe_image_t *img, unsigned cyl, unsigned side, uint8_t *buf, uint32_t cap,
                            hfe_track_t *out);

/*
 * Writes one track side in place (v1 images only), from bitcells MSB first. ncells must be the
 * side's full length (hfe_track_bytes() * 8): the image's track layout is never changed.
 */
hfe_status_t hfe_write_track(const hfe_image_t *img, hfe_write_fn write, void *ctx, unsigned cyl, unsigned side,
                             const uint8_t *cells, uint32_t ncells);

const char *hfe_status_name(hfe_status_t st);

#ifdef __cplusplus
}
#endif

#endif
