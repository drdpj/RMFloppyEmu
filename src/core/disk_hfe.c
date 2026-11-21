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
#include "core/disk_hfe.h"

#include <string.h>

disk_status_t disk_hfe_open(disk_hfe_t *img, disk_read_fn read, void *ctx, uint8_t *cells, uint32_t cells_cap) {
    memset(img, 0, sizeof *img);
    img->cells = cells;
    img->cells_cap = cells_cap;
    switch (hfe_open(&img->hfe, read, ctx)) {
        case HFE_OK:
            return DISK_OK;
        case HFE_ERR_IO:
            return DISK_ERR_IO;
        case HFE_ERR_SIGNATURE:
        case HFE_ERR_HEADER:
        case HFE_ERR_RANGE:
        case HFE_ERR_BUFFER:
        case HFE_ERR_TRACK:
        case HFE_ERR_READ_ONLY:
            return DISK_ERR_FORMAT;
    }
    return DISK_ERR_FORMAT;
}

disk_status_t disk_hfe_load(disk_hfe_t *img, unsigned cyl, unsigned side, disk_track_t *out) {
    if (side > 1) return DISK_ERR_RANGE;
    disk_track_clear(out, (uint8_t)cyl, (uint8_t)side);
    if (cyl >= img->hfe.cylinders || side >= img->hfe.sides) return DISK_OK; /* unformatted */

    hfe_track_t tr;
    switch (hfe_read_track(&img->hfe, cyl, side, img->cells, img->cells_cap, &tr)) {
        case HFE_OK:
            break;
        case HFE_ERR_IO:
            return DISK_ERR_IO;
        case HFE_ERR_BUFFER:
            return DISK_ERR_FULL;
        case HFE_ERR_SIGNATURE:
        case HFE_ERR_HEADER:
        case HFE_ERR_RANGE:
        case HFE_ERR_TRACK:
        case HFE_ERR_READ_ONLY:
            return DISK_ERR_FORMAT;
    }
    /* Decode straight into the track's data area; the sector table is rebuilt below. */
    ibm_track_t t;
    ibm_track_init(&t, img->secs, (uint16_t)(sizeof img->secs / sizeof img->secs[0]), out->data, sizeof out->data);
    if (ibm_track_decode_auto(&t, img->cells, tr.ncells) == IBM_ERR_NOTHING) return DISK_OK; /* no sectors */
    out->fm = t.encoding == IBM_ENC_FM;
    out->rate_kbps = (uint16_t)(tr.bitrate_kbps / t.cell_step);
    out->data_used = t.data_used;

    for (unsigned i = 0; i < t.count; i++) {
        const ibm_sector_t *x = &img->secs[i];
        if (x->flags & IBM_SEC_ID_CRC_ERROR) continue;
        if (x->flags & IBM_SEC_DATA_DROPPED) return DISK_ERR_FULL;
        if (out->count >= DISK_MAX_SECTORS) return DISK_ERR_FULL;
        disk_sector_t *s = &out->secs[out->count++];
        *s = (disk_sector_t){.c = x->c, .h = x->h, .r = x->r, .n = x->n, .size = x->size, .data_off = x->data_off,
                             .pos = (uint32_t)((uint64_t)x->id_pos * DISK_CELLS_PER_REV / tr.ncells)};
        if (x->flags & (IBM_SEC_NO_DATA | IBM_SEC_BAD_SIZE)) s->flags |= DISK_SEC_NO_DATA;
        if (x->flags & IBM_SEC_DATA_CRC_ERROR) s->flags |= DISK_SEC_CRC_ERROR;
        if (x->flags & IBM_SEC_DELETED) s->flags |= DISK_SEC_DELETED;
        if (s->flags & DISK_SEC_NO_DATA) s->size = (uint16_t)(x->n <= DISK_MAX_SIZE_CODE ? 128u << x->n : 0u);
    }
    return DISK_OK;
}

disk_status_t disk_hfe_save(disk_hfe_t *img, disk_write_fn write, void *ctx, const disk_track_t *t) {
    if (t->cyl >= img->hfe.cylinders || t->side >= img->hfe.sides) return DISK_ERR_RANGE;
    if (img->hfe.version != 1 || !img->hfe.write_allowed) return DISK_ERR_READ_ONLY;
    uint32_t bytes;
    if (hfe_track_bytes(&img->hfe, t->cyl, t->side, &bytes) != HFE_OK) return DISK_ERR_IO;
    if (bytes > img->cells_cap) return DISK_ERR_FULL;
    uint32_t ncells = bytes * 8u;

    /* FM is stored at the MFM cell rate of the image (two cells per FM cell) when the image runs at
     * 250 kbit/s or more; the same rule the decoder's FM-at-double-rate case undoes. */
    unsigned step = (t->fm && img->hfe.bitrate_kbps >= 2u * t->rate_kbps) ? 2u : 1u;
    ibm_enc_sector_t *secs = img->enc;
    for (unsigned i = 0; i < t->count; i++) {
        const disk_sector_t *s = &t->secs[i];
        secs[i] = (ibm_enc_sector_t){.c = s->c, .h = s->h, .r = s->r, .n = s->n,
                                     .mark = (s->flags & DISK_SEC_DELETED) ? IBM_MARK_DELETED : IBM_MARK_DATA,
                                     .no_data = (s->flags & DISK_SEC_NO_DATA) != 0,
                                     .bad_crc = (s->flags & DISK_SEC_CRC_ERROR) != 0,
                                     .data = t->data + s->data_off, .size = s->size,
                                     .pos = (uint32_t)((uint64_t)s->pos * ncells / DISK_CELLS_PER_REV)};
    }
    if (ibm_track_encode(secs, t->count, t->fm ? IBM_ENC_FM : IBM_ENC_MFM, step, img->cells, ncells) != IBM_OK) {
        return DISK_ERR_LAYOUT;
    }
    switch (hfe_write_track(&img->hfe, write, ctx, t->cyl, t->side, img->cells, ncells)) {
        case HFE_OK:
            return DISK_OK;
        case HFE_ERR_IO:
            return DISK_ERR_IO;
        case HFE_ERR_READ_ONLY:
            return DISK_ERR_READ_ONLY;
        case HFE_ERR_BUFFER:
            return DISK_ERR_FULL;
        case HFE_ERR_SIGNATURE:
        case HFE_ERR_HEADER:
        case HFE_ERR_RANGE:
        case HFE_ERR_TRACK:
            return DISK_ERR_FORMAT;
    }
    return DISK_ERR_FORMAT;
}
