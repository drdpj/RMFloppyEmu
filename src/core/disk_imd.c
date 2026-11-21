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
#include "core/disk_imd.h"

#include <string.h>

#include "core/bytes.h"

#define IMD_HEADER_MAX   65536u /* the comment can't plausibly be longer */
#define IMD_HEAD_CYLMAP  0x80u
#define IMD_HEAD_HEADMAP 0x40u
#define IMD_SIZE_TABLE   0xFFu
#define IMD_MAX_TYPE     8u

/* A track record's fixed part, maps and sizes, as read from the file. Sized for DISK_MAX_SECTORS so it
 * fits comfortably on a Pico stack (the SDK default is 2 KB). */
typedef struct {
    uint8_t mode, cyl, head, nsec, size_code;
    uint8_t ids[DISK_MAX_SECTORS], cmap[DISK_MAX_SECTORS], hmap[DISK_MAX_SECTORS];
    uint16_t sizes[DISK_MAX_SECTORS];
    uint32_t data_at; /* offset of the first sector data record */
} record_t;

static disk_status_t read_record(const disk_imd_t *img, uint32_t at, record_t *r) {
    uint8_t h[5];
    if (at + 5 > img->file_len || !img->read(img->ctx, at, h, 5)) return DISK_ERR_IO;
    r->mode = h[0];
    r->cyl = h[1];
    r->head = h[2];
    r->nsec = h[3];
    r->size_code = h[4];
    if (r->mode > 5 || (r->head & 0x3Eu) || (r->size_code > DISK_MAX_SIZE_CODE && r->size_code != IMD_SIZE_TABLE)) {
        return DISK_ERR_FORMAT;
    }
    if (r->nsec > DISK_MAX_SECTORS) return DISK_ERR_FULL;
    at += 5;
    unsigned n = r->nsec;
    if (n && (at + n > img->file_len || !img->read(img->ctx, at, r->ids, n))) return DISK_ERR_IO;
    at += n;
    if (r->head & IMD_HEAD_CYLMAP) {
        if (at + n > img->file_len || !img->read(img->ctx, at, r->cmap, n)) return DISK_ERR_IO;
        at += n;
    } else {
        memset(r->cmap, r->cyl, n);
    }
    if (r->head & IMD_HEAD_HEADMAP) {
        if (at + n > img->file_len || !img->read(img->ctx, at, r->hmap, n)) return DISK_ERR_IO;
        at += n;
    } else {
        memset(r->hmap, r->head & 1u, n);
    }
    if (r->size_code == IMD_SIZE_TABLE) {
        uint8_t b[2];
        for (unsigned i = 0; i < n; i++, at += 2) {
            if (at + 2 > img->file_len || !img->read(img->ctx, at, b, 2)) return DISK_ERR_IO;
            r->sizes[i] = bytes_get_le16(b);
            if (r->sizes[i] == 0 || r->sizes[i] > DISK_MAX_TRACK_DATA) return DISK_ERR_FORMAT;
        }
    } else {
        for (unsigned i = 0; i < n; i++)
            r->sizes[i] = (uint16_t)(128u << r->size_code);
    }
    r->data_at = at;
    return DISK_OK;
}

/* Walks the sector data records from r->data_at; returns the offset after them. */
static disk_status_t skip_data(const disk_imd_t *img, const record_t *r, uint32_t *end) {
    uint32_t at = r->data_at;
    for (unsigned i = 0; i < r->nsec; i++) {
        uint8_t type;
        if (at >= img->file_len || !img->read(img->ctx, at, &type, 1)) return DISK_ERR_IO;
        at++;
        if (type > IMD_MAX_TYPE) return DISK_ERR_FORMAT;
        if (type == 0) continue;
        at += (type & 1u) ? r->sizes[i] : 1u;
        if (at > img->file_len) return DISK_ERR_IO;
    }
    *end = at;
    return DISK_OK;
}

disk_status_t disk_imd_open(disk_imd_t *img, disk_read_fn read, void *ctx, uint32_t file_len) {
    memset(img, 0, sizeof *img);
    img->read = read;
    img->ctx = ctx;
    img->file_len = file_len;

    uint8_t sig[4];
    if (file_len < 4 || !read(ctx, 0, sig, 4)) return DISK_ERR_IO;
    if (memcmp(sig, "IMD ", 4) != 0) return DISK_ERR_FORMAT;
    uint32_t at = 4;
    for (;;) { /* the header comment ends at 1A */
        uint8_t buf[64];
        uint32_t n = file_len - at < sizeof buf ? file_len - at : (uint32_t)sizeof buf;
        if (n == 0 || at > IMD_HEADER_MAX) return DISK_ERR_FORMAT;
        if (!read(ctx, at, buf, n)) return DISK_ERR_IO;
        const uint8_t *eof = memchr(buf, 0x1A, n);
        if (eof) {
            at += (uint32_t)(eof - buf) + 1u;
            break;
        }
        at += n;
    }
    img->first_track = at;

    record_t r;
    while (at < file_len) {
        disk_status_t st = read_record(img, at, &r);
        if (st != DISK_OK) return st;
        unsigned side = r.head & 1u;
        if (r.cyl >= DISK_IMD_MAX_CYLS) return DISK_ERR_FORMAT;
        if (img->track_off[r.cyl][side]) return DISK_ERR_FORMAT; /* the same track twice */
        img->track_off[r.cyl][side] = at;
        if (r.cyl + 1u > img->cylinders) img->cylinders = (uint8_t)(r.cyl + 1u);
        if (side + 1u > img->sides) img->sides = (uint8_t)(side + 1u);
        st = skip_data(img, &r, &at);
        if (st != DISK_OK) return st;
    }
    return DISK_OK;
}

/*
 * IMD keeps no gaps, so a track is laid out as a formatter lays out an IBM System 34 track, with the gap sizes
 * MAME uses for an IMD (src/lib/formats/flopimg.cpp build_pc_track_fm/mfm, gap 3 from
 * calc_default_pc_gap3_size): MAME's IDC then finds the same sector as we do when a track has two with one ID
 * (the 4MATION protection; MAME comparison). Bytes: */
#define IMD_MFM_START    146u /* gap 4a 80, sync 12, IAM 4, gap 1 50 */
#define IMD_MFM_OVERHEAD 62u  /* sync 12, IDAM 4, CHRN 4, CRC 2, gap 2 22, sync 12, DAM 4, CRC 2 */
#define IMD_MFM_TO_MARK  15u  /* sync 12, A1 A1 A1: then the ID mark */
#define IMD_FM_START     73u  /* gap 4a 40, sync 6, IAM 1, gap 1 26 */
#define IMD_FM_OVERHEAD  33u  /* sync 6, IDAM 1, CHRN 4, CRC 2, gap 2 11, sync 6, DAM 1, CRC 2 */
#define IMD_FM_TO_MARK   6u
#define IMD_GAP3_SMALL   50u /* 5.25", sectors under 512 bytes */
#define IMD_GAP3_LARGE   80u /* 5.25", 512 bytes and up */
#define IMD_REVS_PER_S   5u  /* 300 rpm, as DISK_CELLS_PER_REV */

/* Sets each sector's position (its ID mark) from the layout above. gap 3 shrinks when the track would
 * overflow, as in MAME; a track that can't fit at all is spaced evenly instead. */
static void imd_lay_out(disk_track_t *t) {
    if (t->count == 0 || t->rate_kbps == 0) return;
    uint32_t bytes_per_rev = (uint32_t)t->rate_kbps * 1000u / 8u / IMD_REVS_PER_S;
    uint32_t cells_per_byte = DISK_CELLS_PER_REV / bytes_per_rev;
    uint32_t start = t->fm ? IMD_FM_START : IMD_MFM_START;
    uint32_t overhead = t->fm ? IMD_FM_OVERHEAD : IMD_MFM_OVERHEAD;
    uint32_t used = start;
    for (unsigned i = 0; i < t->count; i++)
        used += overhead + t->secs[i].size;
    uint32_t gap3 = t->secs[0].size < 512u ? IMD_GAP3_SMALL : IMD_GAP3_LARGE;
    if (used > bytes_per_rev || cells_per_byte == 0) {
        for (unsigned i = 0; i < t->count; i++)
            t->secs[i].pos = (uint32_t)((uint64_t)i * DISK_CELLS_PER_REV / t->count);
        return;
    }
    if (t->count > 1 && used + gap3 * (t->count - 1u) > bytes_per_rev) gap3 = (bytes_per_rev - used) / (t->count - 1u);
    uint32_t at = start + (t->fm ? IMD_FM_TO_MARK : IMD_MFM_TO_MARK);
    for (unsigned i = 0; i < t->count; i++) {
        t->secs[i].pos = at * cells_per_byte;
        at += overhead + t->secs[i].size + gap3;
    }
}

disk_status_t disk_imd_load(const disk_imd_t *img, unsigned cyl, unsigned side, disk_track_t *out) {
    if (cyl >= DISK_IMD_MAX_CYLS || side > 1) return DISK_ERR_RANGE;
    disk_track_clear(out, (uint8_t)cyl, (uint8_t)side);
    uint32_t at = img->track_off[cyl][side];
    if (!at) return DISK_OK; /* unformatted */

    static const uint16_t rates[3] = {500, 300, 250};
    record_t r;
    disk_status_t st = read_record(img, at, &r);
    if (st != DISK_OK) return st;
    out->fm = r.mode < 3;
    out->rate_kbps = (uint16_t)(rates[r.mode % 3] / (out->fm ? 2u : 1u));

    at = r.data_at;
    for (unsigned i = 0; i < r.nsec; i++) {
        uint8_t type;
        if (!img->read(img->ctx, at++, &type, 1)) return DISK_ERR_IO;
        disk_sector_t s = {.c = r.cmap[i], .h = r.hmap[i], .r = r.ids[i], .size = r.sizes[i]}; /* pos: imd_lay_out */
        unsigned n = 0;
        while ((128u << n) < s.size && n < DISK_MAX_SIZE_CODE)
            n++;
        s.n = (uint8_t)n;
        if (type == 0) s.flags = DISK_SEC_NO_DATA;
        if (type == 3 || type == 4 || type == 7 || type == 8) s.flags |= DISK_SEC_DELETED;
        if (type >= 5) s.flags |= DISK_SEC_CRC_ERROR;
        st = disk_track_add(out, &s, NULL);
        if (st != DISK_OK) return st;
        uint8_t *dst = out->data + out->secs[out->count - 1].data_off;
        if (type == 0) continue;
        if (type & 1u) {
            if (!img->read(img->ctx, at, dst, s.size)) return DISK_ERR_IO;
            at += s.size;
        } else {
            uint8_t fill;
            if (!img->read(img->ctx, at++, &fill, 1)) return DISK_ERR_IO;
            memset(dst, fill, s.size);
        }
    }
    imd_lay_out(out);
    return DISK_OK;
}

/* IMD record type for a sector: 0 none, else 1/3/5/7 (normal, deleted, error, deleted error), +1 when
 * compressed to one fill byte. */
static uint8_t base_type(const disk_sector_t *s) {
    if (s->flags & DISK_SEC_NO_DATA) return 0;
    uint8_t t = 1;
    if (s->flags & DISK_SEC_DELETED) t = (uint8_t)(t + 2u);
    if (s->flags & DISK_SEC_CRC_ERROR) t = (uint8_t)(t + 4u);
    return t;
}

static bool uniform(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        if (p[i] != p[0]) return false;
    }
    return true;
}

disk_status_t disk_imd_update(const disk_imd_t *img, disk_write_fn write, void *ctx, const disk_track_t *t) {
    if (t->cyl >= DISK_IMD_MAX_CYLS || t->side > 1) return DISK_ERR_RANGE;
    uint32_t at = img->track_off[t->cyl][t->side];
    if (!at) return DISK_ERR_LAYOUT;
    record_t r;
    disk_status_t st = read_record(img, at, &r);
    if (st != DISK_OK) return st;
    if (r.nsec != t->count) return DISK_ERR_LAYOUT;
    for (unsigned i = 0; i < r.nsec; i++) {
        const disk_sector_t *s = &t->secs[i];
        if (r.ids[i] != s->r || r.cmap[i] != s->c || r.hmap[i] != s->h || r.sizes[i] != s->size) {
            return DISK_ERR_LAYOUT;
        }
    }
    /* First pass: check every record keeps its length. Second pass: write. */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t p = r.data_at;
        for (unsigned i = 0; i < r.nsec; i++) {
            const disk_sector_t *s = &t->secs[i];
            const uint8_t *d = t->data + s->data_off;
            uint8_t old;
            if (!img->read(img->ctx, p, &old, 1)) return DISK_ERR_IO;
            uint8_t want = base_type(s);
            uint32_t old_len = old == 0 ? 0 : (old & 1u) ? s->size : 1u;
            if (old == 0 || want == 0) {
                if (old != want) return DISK_ERR_LAYOUT;
            } else if (old & 1u) {
                if (pass && (!write(ctx, p, &want, 1) || !write(ctx, p + 1, d, s->size))) return DISK_ERR_IO;
            } else {
                if (!uniform(d, s->size)) return DISK_ERR_LAYOUT;
                uint8_t rec[2] = {(uint8_t)(want + 1u), d[0]};
                if (pass && !write(ctx, p, rec, 2)) return DISK_ERR_IO;
            }
            p += 1u + old_len;
        }
    }
    return DISK_OK;
}

static bool copy_range(const disk_imd_t *img, uint32_t from, uint32_t to, disk_sink_fn sink, void *ctx) {
    uint8_t buf[128];
    while (from < to) {
        uint32_t n = to - from < sizeof buf ? to - from : (uint32_t)sizeof buf;
        if (!img->read(img->ctx, from, buf, n) || !sink(ctx, buf, n)) return false;
        from += n;
    }
    return true;
}

static disk_status_t encode_track(const disk_track_t *t, disk_sink_fn sink, void *ctx) {
    uint8_t mode;
    if (t->fm) {
        mode = t->rate_kbps == 125 ? 2 : t->rate_kbps == 150 ? 1 : t->rate_kbps == 250 ? 0 : 0xFF;
    } else {
        mode = t->rate_kbps == 250 ? 5 : t->rate_kbps == 300 ? 4 : t->rate_kbps == 500 ? 3 : 0xFF;
    }
    if (mode == 0xFF) return DISK_ERR_FORMAT;
    bool maps = false, same_size = true;
    for (unsigned i = 0; i < t->count; i++) {
        const disk_sector_t *s = &t->secs[i];
        if (s->c != t->cyl || s->h != t->side) maps = true;
        if (s->size != (128u << s->n) || s->n != t->secs[0].n) same_size = false;
    }
    uint8_t hdr[5] = {mode, t->cyl, (uint8_t)(t->side | (maps ? 0xC0u : 0u)), t->count,
                      same_size ? t->secs[0].n : 0xFFu};
    if (!sink(ctx, hdr, 5)) return DISK_ERR_IO;
    for (unsigned i = 0; i < t->count; i++) {
        if (!sink(ctx, &t->secs[i].r, 1)) return DISK_ERR_IO;
    }
    if (maps) {
        for (unsigned i = 0; i < t->count; i++) {
            if (!sink(ctx, &t->secs[i].c, 1)) return DISK_ERR_IO;
        }
        for (unsigned i = 0; i < t->count; i++) {
            if (!sink(ctx, &t->secs[i].h, 1)) return DISK_ERR_IO;
        }
    }
    if (!same_size) {
        for (unsigned i = 0; i < t->count; i++) {
            uint8_t b[2];
            bytes_put_le16(b, t->secs[i].size);
            if (!sink(ctx, b, 2)) return DISK_ERR_IO;
        }
    }
    for (unsigned i = 0; i < t->count; i++) {
        const disk_sector_t *s = &t->secs[i];
        const uint8_t *d = t->data + s->data_off;
        uint8_t type = base_type(s);
        if (type == 0) {
            if (!sink(ctx, &type, 1)) return DISK_ERR_IO;
        } else if (!sink(ctx, &type, 1) || !sink(ctx, d, s->size)) {
            return DISK_ERR_IO;
        }
    }
    return DISK_OK;
}

/* A track's record as it is, except that each compressed sector record (type 2, 4, 6 or 8: one fill byte) becomes
 * the uncompressed one (type 1, 3, 5 or 7) with the fill repeated over the sector. The header and maps are copied
 * byte for byte. */
static disk_status_t copy_record_expanded(const disk_imd_t *img, uint32_t at, disk_sink_fn sink, void *ctx) {
    record_t r;
    disk_status_t st = read_record(img, at, &r);
    if (st != DISK_OK) return st;
    if (!copy_range(img, at, r.data_at, sink, ctx)) return DISK_ERR_IO;
    uint32_t p = r.data_at;
    for (unsigned i = 0; i < r.nsec; i++) {
        uint8_t type;
        if (p >= img->file_len || !img->read(img->ctx, p, &type, 1)) return DISK_ERR_IO;
        p++;
        if (type > IMD_MAX_TYPE) return DISK_ERR_FORMAT;
        if (type == 0 || (type & 1u)) { /* no data, or stored: as it is */
            uint32_t n = type ? r.sizes[i] : 0u;
            if (p + n > img->file_len || !sink(ctx, &type, 1) || !copy_range(img, p, p + n, sink, ctx)) {
                return DISK_ERR_IO;
            }
            p += n;
            continue;
        }
        uint8_t fill[128];
        if (p >= img->file_len || !img->read(img->ctx, p, fill, 1)) return DISK_ERR_IO;
        p++;
        memset(fill, fill[0], sizeof fill);
        uint8_t stored = (uint8_t)(type - 1u);
        if (!sink(ctx, &stored, 1)) return DISK_ERR_IO;
        for (uint32_t left = r.sizes[i]; left;) {
            uint32_t n = left < sizeof fill ? left : (uint32_t)sizeof fill;
            if (!sink(ctx, fill, n)) return DISK_ERR_IO;
            left -= n;
        }
    }
    return DISK_OK;
}

disk_status_t disk_imd_rewrite(const disk_imd_t *img, const disk_track_t *t, disk_sink_fn sink, void *ctx) {
    if (t->cyl >= DISK_IMD_MAX_CYLS || t->side > 1) return DISK_ERR_RANGE;
    if (!copy_range(img, 0, img->first_track, sink, ctx)) return DISK_ERR_IO;
    unsigned cyls = img->cylinders > t->cyl ? img->cylinders : t->cyl + 1u;
    for (unsigned c = 0; c < cyls; c++) {
        for (unsigned h = 0; h < 2; h++) {
            if (c == t->cyl && h == t->side) {
                if (t->count) {
                    disk_status_t st = encode_track(t, sink, ctx);
                    if (st != DISK_OK) return st;
                }
                continue;
            }
            uint32_t at = img->track_off[c][h];
            if (!at) continue;
            disk_status_t st = copy_record_expanded(img, at, sink, ctx);
            if (st != DISK_OK) return st;
        }
    }
    return DISK_OK;
}
