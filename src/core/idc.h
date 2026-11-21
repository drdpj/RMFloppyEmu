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
 * The disc unit's command layer: what the IDC firmware does with each command byte from the 480Z
 * (docs/protocol.md, "Command layer"). This is the high-level reimplementation; every behaviour cites
 * the IDC3 ROM address or MAME trace it copies.
 *
 * Every command is implemented except 09 (not decoded) and the aux-port commands 12-15 and 17; 0B is
 * reported, not run (see idc_exec_hook_t). The disc commands are in idc_disk.c. docs/protocol.md,
 * "Implementation status", lists the choices made where the ROM leaves the device free. The MAME comparison
 * tests (tests/mame/) check all of this byte for byte against the real IDC3 firmware.
 */
#ifndef RMFE_CORE_IDC_H
#define RMFE_CORE_IDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/disk.h"
#include "core/link.h"

/* Command bytes [IDC3 jump table 0ac2] */
#define IDC_CMD_MONITOR     0xFEu
#define IDC_CMD_BOOT_BLOCK  0xFFu
#define IDC_CMD_GO_FAST     0x00u
#define IDC_CMD_CONFIGURE   0x05u
#define IDC_CMD_SET_RETRIES 0x07u
#define IDC_CMD_UPLOAD      0x0Au
#define IDC_CMD_EXECUTE     0x0Bu
#define IDC_CMD_VERSION     0x11u
#define IDC_CMD_NOP         0x16u

/* Status bytes [docs/protocol.md "Status / error bytes"] */
#define IDC_STATUS_OK          0x00u
#define IDC_STATUS_BAD_PARAM   0x20u /* IDC3 0ecb, 0edd */
#define IDC_STATUS_UNKNOWN_CMD 0x40u /* IDC3 0a77 */
#define IDC_STATUS_CHECKSUM    0x10u /* IDC3 0ce5: upload checksum wrong */
#define IDC_VERSION_BYTE       0x31u /* IDC3 0d66: '1' */

#define IDC_DRIVES      8u /* logical drives: one side of one physical drive (docs/protocol.md "Parameter bytes") */
#define IDC_PHYS_DRIVES 2u /* drives this device presents (A:, B:); C: and D: are never ready */

#define IDC_CMD_LOGIN      0x01u
#define IDC_CMD_READ       0x02u
#define IDC_CMD_WRITE      0x03u
#define IDC_CMD_WRITE_VER  0x04u
#define IDC_CMD_PROBE      0x06u
#define IDC_CMD_FLUSH      0x08u
#define IDC_CMD_FORMAT     0x0Cu
#define IDC_CMD_VERIFY     0x0Du
#define IDC_CMD_READ_ID    0x0Eu
#define IDC_CMD_WRITE_ID   0x0Fu
#define IDC_CMD_WRITE_ID_V 0x10u

/* Probe (06) format byte [IDC3 0bf9-0c71; traces: 0A for a 40-track disc and 0E for an 80-track one, both in
 * 80-track drives] */
#define IDC_PROBE_MFM      0x02u
#define IDC_PROBE_96TPI    0x04u /* the disc is 80-track */
#define IDC_PROBE_QD_DRIVE 0x08u /* the drive is 80-track */
#define IDC_QD_CYLINDERS   43u   /* an image with more cylinders than this is an 80-track (QD) disc */

/* Disc errors: WD1793 status bits, as the IDC passes them on [IDC3 02c3, 0314] */
#define IDC_STATUS_NOT_READY     0x80u
#define IDC_STATUS_WRITE_PROTECT 0x40u
#define IDC_STATUS_NOT_FOUND     0x10u /* record not found (also: wrong density) */
#define IDC_STATUS_CRC           0x08u
#define IDC_STATUS_WRITE_BACK                                                                                          \
    0x01u /* added to an error writing the cache back (08, or to free a buffer) [IDC3 109f]                            \
           */

/* Drive byte fields, after idc_param_byte() */
#define IDC_PARAM_SIDE  0x01u
#define IDC_PARAM_DRIVE 0x06u
#define IDC_PARAM_MFM   0x20u
#define IDC_PARAM_ALL   0x08u /* command 08: flush every drive */

/*
 * The discs, supplied by the platform (images on the SD card; the tests' in-memory discs).
 * position() is the drive's disc's rotational position now, in cells from the index (0 .. DISK_CELLS_PER_REV-1):
 * it decides which of several sectors with the same ID a read finds, as on a real drive. Each drive spins on
 * its own, so each has its own index.
 */
typedef struct {
    void *ctx;
    bool (*present)(void *ctx, unsigned drive);
    unsigned (*cylinders)(void *ctx, unsigned drive);
    disk_status_t (*load)(void *ctx, unsigned drive, unsigned cyl, unsigned side, disk_track_t *t);
    disk_status_t (*save)(void *ctx, unsigned drive, const disk_track_t *t);
    uint32_t (*position)(void *ctx, unsigned drive);
} idc_drives_t;

/*
 * What kind of drive each of ours is: in the IDC this comes from links on its board (docs/protocol.md,
 * open questions), and it decides the drive bit of the 06 probe. FOLLOW_IMAGE (the default) makes an image
 * with more than IDC_QD_CYLINDERS cylinders an 80-track drive and anything else a 40-track one. MAME's
 * MQ2 has 80-track drives (525qd), so the MAME comparison sets IDC_DRIVE_80_TRACK.
 */
typedef enum {
    IDC_DRIVE_FOLLOW_IMAGE = 0,
    IDC_DRIVE_40_TRACK,
    IDC_DRIVE_80_TRACK,
} idc_drive_type_t;

/* One cached track per drive, like the IDC's write-back cache [IDC3 108f-11c8]. */
/*
 * The IDC caches in 512-byte buffers of four records, with a state per record [IDC3 1221-126e, IX+7]. A buffer
 * whose four records have all been written goes to the disc at once [10a6-10b2]; the rest wait for 08 or for
 * the buffer to be needed. We keep the whole track, and per buffer which records were written (a bit each)
 * and whether its sector is missing from the track, so errors come out where the IDC's do.
 */
#define IDC_CACHE_BUFFERS 9u /* per track: 36 MFM records / 4 */
typedef struct {
    bool valid, dirty;                  /* dirty: the track differs from the disc */
    uint8_t written[IDC_CACHE_BUFFERS]; /* records written since the buffer was last on the disc */
    uint16_t missing;                   /* buffers written whose sector the track doesn't have (bit each) */
    disk_track_t t;
} idc_cache_t;
#define IDC_RAM_BASE  0xE000u /* 2 KB of RAM at E000, mirrored up to FFFF [MAME rm_mq2] */
#define IDC_RAM_BYTES 0x0800u

/*
 * Called when the 480Z asks the unit to run code it uploaded (command 0B). The reimplementation has no
 * Z80, so the code is not run: the platform shows it and saves ram (the unit's 2 KB, as uploaded) for
 * analysis. Nothing traced has ever used 0A/0B (docs/protocol.md); this is how we would find out.
 */
typedef void (*idc_exec_hook_t)(void *ctx, uint16_t address, const uint8_t ram[IDC_RAM_BYTES]);

typedef struct {
    link_t link;
    const uint8_t *boot_block; /* sent for command FF (from the user's IDC ROM: idc_find_boot_block) */
    size_t boot_len;
    uint8_t retries;               /* FE9D */
    uint8_t drive_map[IDC_DRIVES]; /* logical -> physical drive, FEA4-FEAB [IDC3 007f, 0b8d] */
    uint8_t sticky;                /* first error of the current command, echoed as status (FEA3) */
    uint8_t ram[IDC_RAM_BYTES];    /* the unit's RAM, as far as uploads (0A) are concerned */
    idc_exec_hook_t exec_hook;     /* optional */
    void *exec_ctx;
    const idc_drives_t *drives; /* NULL: no drives (every disc command answers not ready) */
    idc_cache_t cache[IDC_PHYS_DRIVES];
    idc_drive_type_t drive_type[IDC_PHYS_DRIVES];
} idc_t;

/* Sets up the unit: link at 9600 baud, identity drive map, retry count 1. boot_block may be NULL
 * (command FF is then answered as unknown). */
void idc_init(idc_t *d, const link_io_t *io, const uint8_t *boot_block, size_t boot_len);

/* Connects the discs (before idc_run). */
void idc_set_drives(idc_t *d, const idc_drives_t *drives);

/* Sets a drive's type (the IDC's board links). drive is 0 (A:) or 1 (B:). */
void idc_set_drive_type(idc_t *d, unsigned drive, idc_drive_type_t type);

/* Writes every changed cached track back (the platform calls this before swapping an image). Returns
 * the first error as an IDC status byte, or IDC_STATUS_OK. */
uint8_t idc_flush_all(idc_t *d);

/* Waits for one command and serves it. LINK_RESET: the 480Z interrupted (the link is at 9600 again);
 * LINK_CLOSED: stop. */
link_status_t idc_serve_one(idc_t *d);

/* Serves commands until the link closes, starting afresh after every reset. */
void idc_run(idc_t *d);

/*
 * The drive/flags parameter byte as the IDC reads it [IDC3 0f77]: if the low two bits are 01 or 10
 * they are swapped (a parity trick); 00 and 11 are unchanged. Other bits pass through.
 */
uint8_t idc_param_byte(uint8_t raw);

/*
 * Finds the boot block (what command FF sends) in an IDC ROM image, by locating the FF handler
 * (ld bc,(len) / ld hl,block / ld a,(hl) / call send) and following its operands, so it works for any
 * revision laid out that way (IDC3: 1049 bytes at 1B2A; IDC5: 1181 bytes at 1B2A). Returns false if
 * the ROM isn't recognised. The block stays in the caller's ROM buffer; nothing is copied.
 */
bool idc_find_boot_block(const uint8_t *rom, size_t rom_len, const uint8_t **block, size_t *len);

#endif
