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
#include "flux/hfe.h"

#include <string.h>

#define HFE_BLOCK        512u
#define HFE_HALF_BLOCK   256u
#define HFE_HEADER_BYTES 0x1Au

/* v3 opcodes, as they read after bit reversal */
#define V3_NOP      0xF0u
#define V3_INDEX    0xF1u
#define V3_BITRATE  0xF2u
#define V3_SKIPBITS 0xF3u
#define V3_RAND     0xF4u

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

hfe_status_t hfe_open(hfe_image_t *img, hfe_read_fn read, void *ctx) {
    uint8_t h[HFE_HEADER_BYTES];
    if (!img || !read) return HFE_ERR_IO;
    memset(img, 0, sizeof *img);
    if (!read(ctx, 0, h, sizeof h)) return HFE_ERR_IO;

    if (memcmp(h, "HXCPICFE", 8) == 0) {
        img->version = 1;
    } else if (memcmp(h, "HXCHFEV3", 8) == 0) {
        img->version = 3;
    } else {
        return HFE_ERR_SIGNATURE;
    }
    if (h[0x08] > 1 || h[0x09] == 0 || h[0x0A] < 1 || h[0x0A] > 2) return HFE_ERR_HEADER;

    img->read = read;
    img->ctx = ctx;
    img->cylinders = h[0x09];
    img->sides = h[0x0A];
    img->encoding = h[0x0B];
    img->bitrate_kbps = le16(h + 0x0C);
    img->rpm = le16(h + 0x0E);
    img->interface_mode = h[0x10];
    img->track_list_block = le16(h + 0x12);
    img->write_allowed = h[0x14] != 0x00;
    img->double_step = h[0x15] == 0x00;
    img->track0_encoding[0] = h[0x16] == 0x00 ? h[0x17] : img->encoding;
    img->track0_encoding[1] = h[0x18] == 0x00 ? h[0x19] : img->encoding;
    return HFE_OK;
}

/* The track list entry for a cylinder: data offset (bytes) and length (bytes, both sides). */
static hfe_status_t track_entry(const hfe_image_t *img, unsigned cyl, unsigned side, uint32_t *offset,
                                uint32_t *side_bytes) {
    if (!img || !img->read) return HFE_ERR_IO;
    if (cyl >= img->cylinders || side >= img->sides) return HFE_ERR_RANGE;
    uint8_t e[4];
    uint32_t at = (uint32_t)img->track_list_block * HFE_BLOCK + cyl * 4u;
    if (!img->read(img->ctx, at, e, sizeof e)) return HFE_ERR_IO;
    *offset = (uint32_t)le16(e) * HFE_BLOCK;
    *side_bytes = le16(e + 2) / 2u;
    return HFE_OK;
}

hfe_status_t hfe_track_bytes(const hfe_image_t *img, unsigned cyl, unsigned side, uint32_t *bytes) {
    uint32_t offset;
    return track_entry(img, cyl, side, &offset, bytes);
}

/* Writes cells MSB first into buf, at a bit position that never passes the byte being read. */
typedef struct {
    uint8_t *buf;
    uint32_t pos;
} bitwriter_t;

static void put_bits(bitwriter_t *w, uint8_t v, unsigned n) { /* the top n bits of v */
    for (unsigned k = 0; k < n; k++, w->pos++) {
        uint8_t mask = (uint8_t)(0x80u >> (w->pos & 7u));
        if (v & (0x80u >> k)) {
            w->buf[w->pos >> 3] |= mask;
        } else {
            w->buf[w->pos >> 3] &= (uint8_t)~mask;
        }
    }
}

/* v3: interprets the opcodes in place. Output never overtakes input: every opcode or data byte read
 * produces at most 8 cells. */
static hfe_status_t v3_decode(uint8_t *buf, uint32_t len, hfe_track_t *out) {
    bitwriter_t w = {buf, 0};
    uint32_t i = 0;
    uint8_t first_rate = 0;
    bool index_seen = false;
    while (i < len) {
        uint8_t x = buf[i++];
        switch (x) {
            case V3_NOP:
                break;
            case V3_INDEX:
                if (!index_seen) out->index_cell = w.pos;
                index_seen = true;
                break;
            case V3_BITRATE:
                if (i >= len) return HFE_ERR_TRACK;
                if (!first_rate) first_rate = buf[i];
                i++;
                break;
            case V3_SKIPBITS: {
                if (i + 2 > len) return HFE_ERR_TRACK;
                uint8_t n = buf[i], v = buf[i + 1];
                i += 2;
                if (n == 0 || n > 7) return HFE_ERR_TRACK;
                if (v == V3_RAND) {
                    out->weak_cells += 8u - n;
                    v = 0;
                } else if ((unsigned)v << n > 0xFFu) {
                    return HFE_ERR_TRACK;
                }
                put_bits(&w, (uint8_t)(v << n), 8u - n);
                break;
            }
            case V3_RAND:
                out->weak_cells += 8;
                put_bits(&w, 0, 8);
                break;
            default:
                if ((x & 0xF0u) == 0xF0u) return HFE_ERR_TRACK;
                put_bits(&w, x, 8);
                break;
        }
    }
    out->ncells = w.pos;
    if (first_rate) out->bitrate_kbps = (uint16_t)(18000u / first_rate);
    return HFE_OK;
}

hfe_status_t hfe_read_track(const hfe_image_t *img, unsigned cyl, unsigned side, uint8_t *buf, uint32_t cap,
                            hfe_track_t *out) {
    uint32_t offset, len;
    hfe_status_t st = track_entry(img, cyl, side, &offset, &len);
    if (st != HFE_OK) return st;
    if (!buf || !out) return HFE_ERR_BUFFER;
    if (cap < len) return HFE_ERR_BUFFER;

    memset(out, 0, sizeof *out);
    out->encoding = cyl == 0 ? img->track0_encoding[side] : img->encoding;
    out->bitrate_kbps = img->bitrate_kbps;

    /* Side s of each 512-byte block is at +256*s; the last block may be part-filled. */
    for (uint32_t done = 0; done < len; done += HFE_HALF_BLOCK, offset += HFE_BLOCK) {
        uint32_t n = len - done < HFE_HALF_BLOCK ? len - done : HFE_HALF_BLOCK;
        if (!img->read(img->ctx, offset + side * HFE_HALF_BLOCK, buf + done, n)) return HFE_ERR_IO;
    }
    for (uint32_t k = 0; k < len; k++) buf[k] = reverse8(buf[k]);

    if (img->version == 3) return v3_decode(buf, len, out);
    out->ncells = len * 8u;
    return HFE_OK;
}

hfe_status_t hfe_write_track(const hfe_image_t *img, hfe_write_fn write, void *ctx, unsigned cyl, unsigned side,
                             const uint8_t *cells, uint32_t ncells) {
    uint32_t offset, len;
    hfe_status_t st = track_entry(img, cyl, side, &offset, &len);
    if (st != HFE_OK) return st;
    if (img->version != 1 || !img->write_allowed || !write) return HFE_ERR_READ_ONLY;
    if (!cells || ncells != len * 8u) return HFE_ERR_BUFFER;
    uint8_t buf[HFE_HALF_BLOCK];
    for (uint32_t done = 0; done < len; done += HFE_HALF_BLOCK, offset += HFE_BLOCK) {
        uint32_t n = len - done < HFE_HALF_BLOCK ? len - done : HFE_HALF_BLOCK;
        for (uint32_t k = 0; k < n; k++) buf[k] = reverse8(cells[done + k]);
        if (!write(ctx, offset + side * HFE_HALF_BLOCK, buf, n)) return HFE_ERR_IO;
    }
    return HFE_OK;
}

const char *hfe_status_name(hfe_status_t st) {
    switch (st) {
        case HFE_OK:
            return "ok";
        case HFE_ERR_IO:
            return "read error or file too short";
        case HFE_ERR_SIGNATURE:
            return "not an HFE file";
        case HFE_ERR_HEADER:
            return "bad HFE header";
        case HFE_ERR_RANGE:
            return "track not in image";
        case HFE_ERR_BUFFER:
            return "buffer too small";
        case HFE_ERR_TRACK:
            return "bad HFE v3 track data";
        case HFE_ERR_READ_ONLY:
            return "HFE image is read-only";
    }
    return "unknown";
}
