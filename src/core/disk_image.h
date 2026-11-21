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
 * One disc image file of either kind (IMD or HFE), as a drive presents it: open, the number of cylinders, load a
 * track, save a track. The platform supplies the file operations (disk_image_io_t); this decides how a save goes:
 *   - HFE: the track re-encoded and written in place (disk_hfe_save);
 *   - IMD: in place when every sector record keeps its length (disk_imd_update), otherwise the whole image written
 *     anew to a replacement file, which the platform then puts in place of the old one (disk_imd_rewrite). The old
 *     image stays whole until the new one is complete, so a failed write never leaves a half-written image
 *     (docs/hardware.md, "writes supported for both formats, safely").
 * Shared by the firmware (src/pico) and the MAME comparison (tests/mame/mamecmp.c). Nothing is allocated.
 */
#ifndef RMFE_CORE_DISK_IMAGE_H
#define RMFE_CORE_DISK_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/disk.h"
#include "core/disk_hfe.h"
#include "core/disk_imd.h"

typedef enum {
    DISK_IMAGE_IMD,
    DISK_IMAGE_HFE,
} disk_image_kind_t;

/* The image file, as the platform keeps it. ctx is passed to every call. */
typedef struct {
    void *ctx;
    disk_read_fn read;   /* from the image file */
    disk_write_fn write; /* into the image file, in place (it never grows) */
    /* An IMD rewrite: start a new, empty replacement file; write it through sink; then commit, which closes it,
     * puts it in place of the image file (the old one is gone only once the new one is complete), reopens it for
     * read and write, and gives its length. abort removes the unfinished replacement. */
    bool (*replace_begin)(void *ctx);
    disk_sink_fn replace_sink;
    bool (*replace_commit)(void *ctx, uint32_t *new_len);
    void (*replace_abort)(void *ctx);
} disk_image_io_t;

typedef struct {
    disk_image_kind_t kind;
    const disk_image_io_t *io;
    bool read_only;
    union {
        disk_imd_t imd;
        disk_hfe_t hfe;
    };
} disk_image_t;

/*
 * The kind from a file name's extension (.imd or .hfe, any case). Returns false for anything else.
 */
bool disk_image_kind_from_name(const char *name, disk_image_kind_t *kind);

/*
 * Opens an image of file_len bytes. read_only: refuse saves (a write-protected image). cells/cells_cap: an HFE's
 * buffer for one track side's cells (16 KB holds any 5.25" track); unused for IMD, and may be shared between
 * images that are never loaded or saved at the same time. io must outlive the image.
 */
disk_status_t disk_image_open(disk_image_t *im, disk_image_kind_t kind, const disk_image_io_t *io, uint32_t file_len,
                              bool read_only, uint8_t *cells, uint32_t cells_cap);

/* The same in steps (disk_imd_open_begin/step): begin, then step until *done; each step reads a track record's
 * worth of the file at most. An HFE is done at begin. The image is usable only once done. */
disk_status_t disk_image_open_begin(disk_image_t *im, disk_image_kind_t kind, const disk_image_io_t *io,
                                    uint32_t file_len, bool read_only, uint8_t *cells, uint32_t cells_cap);
disk_status_t disk_image_open_step(disk_image_t *im, bool *done);

/* Cylinders in the image (1 + the highest with a record, for IMD; the header's, for HFE). */
unsigned disk_image_cylinders(const disk_image_t *im);

/* Loads one track side. Beyond the image's cylinders, or with no record: an empty track. */
disk_status_t disk_image_load(disk_image_t *im, unsigned cyl, unsigned side, disk_track_t *out);

/* Saves one changed track: DISK_ERR_READ_ONLY for a write-protected image or an HFE v3; DISK_ERR_IO if a file
 * operation fails (an unfinished IMD replacement is aborted, and the image is as it was). */
disk_status_t disk_image_save(disk_image_t *im, const disk_track_t *t);

#endif
