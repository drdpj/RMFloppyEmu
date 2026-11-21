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
#include "core/disk_image.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "core/rmfe_assert.h"

static bool ext_is(const char *ext, const char *want) {
    for (; *want; ext++, want++) {
        if (tolower((unsigned char)*ext) != *want) return false;
    }
    return *ext == '\0';
}

bool disk_image_kind_from_name(const char *name, disk_image_kind_t *kind) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    if (ext_is(dot, ".imd")) {
        *kind = DISK_IMAGE_IMD;
        return true;
    }
    if (ext_is(dot, ".hfe")) {
        *kind = DISK_IMAGE_HFE;
        return true;
    }
    return false;
}

disk_status_t disk_image_open_begin(disk_image_t *im, disk_image_kind_t kind, const disk_image_io_t *io,
                                    uint32_t file_len, bool read_only, uint8_t *cells, uint32_t cells_cap) {
    RMFE_ASSERT(im && io && io->read && io->write);
    im->kind = kind;
    im->io = io;
    im->read_only = read_only;
    switch (kind) {
        case DISK_IMAGE_IMD:
            return disk_imd_open_begin(&im->imd, io->read, io->ctx, file_len);
        case DISK_IMAGE_HFE:
            return disk_hfe_open(&im->hfe, io->read, io->ctx, cells, cells_cap); /* the header: one read */
    }
    return DISK_ERR_FORMAT;
}

disk_status_t disk_image_open_step(disk_image_t *im, bool *done) {
    RMFE_ASSERT(im && done);
    switch (im->kind) {
        case DISK_IMAGE_IMD:
            return disk_imd_open_step(&im->imd, done);
        case DISK_IMAGE_HFE:
            *done = true;
            return DISK_OK;
    }
    return DISK_ERR_FORMAT;
}

disk_status_t disk_image_open(disk_image_t *im, disk_image_kind_t kind, const disk_image_io_t *io, uint32_t file_len,
                              bool read_only, uint8_t *cells, uint32_t cells_cap) {
    disk_status_t st = disk_image_open_begin(im, kind, io, file_len, read_only, cells, cells_cap);
    for (bool done = false; st == DISK_OK && !done;)
        st = disk_image_open_step(im, &done);
    return st;
}

unsigned disk_image_cylinders(const disk_image_t *im) {
    switch (im->kind) {
        case DISK_IMAGE_IMD:
            return im->imd.cylinders;
        case DISK_IMAGE_HFE:
            return im->hfe.hfe.cylinders;
    }
    return 0;
}

disk_status_t disk_image_load(disk_image_t *im, unsigned cyl, unsigned side, disk_track_t *out) {
    switch (im->kind) {
        case DISK_IMAGE_IMD:
            return disk_imd_load(&im->imd, cyl, side, out);
        case DISK_IMAGE_HFE:
            return disk_hfe_load(&im->hfe, cyl, side, out);
    }
    return DISK_ERR_FORMAT;
}

static disk_status_t imd_replace(disk_image_t *im, const disk_track_t *t) {
    const disk_image_io_t *io = im->io;
    if (!io->replace_begin || !io->replace_sink || !io->replace_commit || !io->replace_abort) return DISK_ERR_IO;
    if (!io->replace_begin(io->ctx)) return DISK_ERR_IO;
    disk_status_t st = disk_imd_rewrite(&im->imd, t, io->replace_sink, io->ctx);
    if (st != DISK_OK) {
        io->replace_abort(io->ctx);
        return st;
    }
    uint32_t len;
    if (!io->replace_commit(io->ctx, &len)) return DISK_ERR_IO;
    return disk_imd_open(&im->imd, io->read, io->ctx, len);
}

disk_status_t disk_image_save(disk_image_t *im, const disk_track_t *t) {
    if (im->read_only) return DISK_ERR_READ_ONLY;
    switch (im->kind) {
        case DISK_IMAGE_IMD: {
            disk_status_t st = disk_imd_update(&im->imd, im->io->write, im->io->ctx, t);
            return st == DISK_ERR_LAYOUT ? imd_replace(im, t) : st;
        }
        case DISK_IMAGE_HFE:
            return disk_hfe_save(&im->hfe, im->io->write, im->io->ctx, t);
    }
    return DISK_ERR_FORMAT;
}
