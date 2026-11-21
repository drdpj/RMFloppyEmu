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
#include "flux/scp.h"

#include <string.h>

#define SCP_HEADER_BYTES 0x10u
#define SCP_TDH_BYTES    4u
#define SCP_REV_BYTES    12u
#define SCP_BASE_HZ      40000000u

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

scp_status_t scp_open(scp_image_t *img, scp_read_fn read, void *ctx) {
    uint8_t h[SCP_HEADER_BYTES];
    if (!img || !read) return SCP_ERR_IO;
    memset(img, 0, sizeof *img);
    if (!read(ctx, 0, h, sizeof h)) return SCP_ERR_IO;
    if (memcmp(h, "SCP", 3) != 0) return SCP_ERR_SIGNATURE;
    img->read = read;
    img->ctx = ctx;
    img->version = h[3];
    img->disk_type = h[4];
    img->revolutions = h[5];
    img->start_track = h[6];
    img->end_track = h[7];
    img->flags = h[8];
    img->cell_width = h[9];
    img->heads = h[10];
    img->resolution = h[11];
    img->checksum = le32(h + 12);
    img->sample_hz = SCP_BASE_HZ / ((uint32_t)img->resolution + 1u);
    if ((img->cell_width != 0 && img->cell_width != 16) || (img->flags & SCP_FLAG_EXTENDED)) {
        return SCP_ERR_UNSUPPORTED;
    }
    return SCP_OK;
}

scp_status_t scp_read_track(const scp_image_t *img, unsigned cyl, unsigned side, scp_track_t *out) {
    if (!img || !img->read || !out) return SCP_ERR_IO;
    unsigned tnr = cyl * 2u + side;
    if (side > 1 || tnr >= SCP_MAX_TRACKS) return SCP_ERR_NO_TRACK;
    memset(out, 0, sizeof *out);

    uint8_t e[4];
    if (!img->read(img->ctx, SCP_HEADER_BYTES + tnr * 4u, e, 4)) return SCP_ERR_IO;
    uint32_t tdh = le32(e);
    if (tdh == 0) return SCP_ERR_NO_TRACK;

    unsigned nrevs = img->revolutions;
    if (nrevs == 0) return SCP_ERR_NO_TRACK;
    if (nrevs > SCP_MAX_REVS) nrevs = SCP_MAX_REVS;
    uint8_t hdr[SCP_TDH_BYTES + SCP_REV_BYTES * SCP_MAX_REVS];
    if (!img->read(img->ctx, tdh, hdr, SCP_TDH_BYTES + SCP_REV_BYTES * nrevs)) return SCP_ERR_IO;
    if (memcmp(hdr, "TRK", 3) != 0) return SCP_ERR_SIGNATURE;
    if (hdr[3] != tnr) return SCP_ERR_TRACK;

    out->track = (uint8_t)tnr;
    out->tdh_offset = tdh;
    for (unsigned r = 0; r < nrevs; r++) {
        const uint8_t *p = hdr + SCP_TDH_BYTES + SCP_REV_BYTES * r;
        out->revs[r].index_ticks = le32(p);
        out->revs[r].nflux = le32(p + 4);
        out->revs[r].data_offset = tdh + le32(p + 8);
    }
    /* Drop empty trailing revolutions (some tools write them). */
    while (nrevs && (out->revs[nrevs - 1].nflux == 0 || out->revs[nrevs - 1].index_ticks == 0)) nrevs--;
    if (nrevs == 0) return SCP_ERR_NO_TRACK;
    for (unsigned r = 1; r < nrevs; r++) {
        if (out->revs[r].data_offset < out->revs[r - 1].data_offset) return SCP_ERR_TRACK;
    }
    out->nrevs = (uint8_t)nrevs;
    return SCP_OK;
}

scp_status_t scp_flux_open(scp_flux_stream_t *s, const scp_image_t *img, const scp_track_t *t, unsigned rev) {
    if (!s || !img || !t) return SCP_ERR_IO;
    if (rev >= t->nrevs) return SCP_ERR_RANGE;
    memset(s, 0, sizeof *s);
    s->img = img;
    s->pos = t->revs[rev].data_offset;
    const scp_rev_t *last = &t->revs[t->nrevs - 1];
    s->end = last->data_offset + last->nflux * 2u;
    return SCP_OK;
}

static bool next_word(scp_flux_stream_t *s, uint16_t *w) {
    if (s->buf_pos + 2 > s->buf_len) {
        if (s->pos >= s->end) return false;
        uint32_t n = s->end - s->pos < sizeof s->buf ? s->end - s->pos : (uint32_t)sizeof s->buf;
        n &= ~1u;
        if (n == 0) return false;
        if (!s->img->read(s->img->ctx, s->pos, s->buf, n)) {
            s->failed = true;
            return false;
        }
        s->pos += n;
        s->buf_pos = 0;
        s->buf_len = n;
    }
    *w = (uint16_t)((s->buf[s->buf_pos] << 8) | s->buf[s->buf_pos + 1]);
    s->buf_pos += 2;
    return true;
}

bool scp_flux_next(void *stream, uint32_t *ticks) {
    scp_flux_stream_t *s = stream;
    uint32_t acc = 0;
    uint16_t w;
    while (next_word(s, &w)) {
        if (w == 0) {
            acc += 65536u;
            continue;
        }
        *ticks = acc + w;
        return true;
    }
    return false;
}

scp_status_t scp_verify_checksum(const scp_image_t *img, uint32_t file_len, bool *ok) {
    uint8_t buf[512];
    uint32_t sum = 0;
    if (!img || !img->read || !ok) return SCP_ERR_IO;
    for (uint32_t pos = SCP_HEADER_BYTES; pos < file_len;) {
        uint32_t n = file_len - pos < sizeof buf ? file_len - pos : (uint32_t)sizeof buf;
        if (!img->read(img->ctx, pos, buf, n)) return SCP_ERR_IO;
        for (uint32_t k = 0; k < n; k++) sum += buf[k];
        pos += n;
    }
    *ok = sum == img->checksum;
    return SCP_OK;
}

const char *scp_status_name(scp_status_t st) {
    switch (st) {
        case SCP_OK:
            return "ok";
        case SCP_ERR_IO:
            return "read error or file too short";
        case SCP_ERR_SIGNATURE:
            return "not an SCP file";
        case SCP_ERR_UNSUPPORTED:
            return "unsupported SCP variant";
        case SCP_ERR_NO_TRACK:
            return "track not in image";
        case SCP_ERR_RANGE:
            return "revolution not in track";
        case SCP_ERR_TRACK:
            return "bad SCP track header";
    }
    return "unknown";
}
