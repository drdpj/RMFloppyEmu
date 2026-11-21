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
#include "flux/ibm_track.h"

#include <stddef.h>

#include "flux/crc16.h"

/* The encoded track: a view of the input with cell_step applied. Positions are encoded cells. */
typedef struct {
    const uint8_t *cells;
    uint32_t n;     /* encoded cells in one revolution */
    uint32_t avail; /* encoded cells actually present (>= n when the input runs on past the index) */
    unsigned step; /* input cells per encoded cell */
    ibm_encoding_t enc;
} stream_t;

#define MFM_SYNC_CELLS 0x448944894489ull /* three A1 bytes with the clock between bits 4 and 5 missing */
#define MFM_SYNC_MASK  0xFFFFFFFFFFFFull
#define FM_MARK_CLOCK  0xC7u

static unsigned raw_cell(const uint8_t *cells, uint32_t i) {
    return (cells[i >> 3] >> (7u - (i & 7u))) & 1u;
}

/* Encoded cell i, for any i: real cells where there are any, otherwise round the revolution. */
static unsigned cell(const stream_t *s, int64_t i) {
    int64_t m = i;
    if (m < 0 || m >= (int64_t)s->avail) {
        m %= (int64_t)s->n;
        if (m < 0) m += s->n;
    }
    uint32_t b = (uint32_t)m * s->step;
    unsigned v = raw_cell(s->cells, b);
    for (unsigned k = 1; k < s->step; k++) v |= raw_cell(s->cells, b + k);
    return v;
}

/* The byte whose 16 cells start at pos: clock cell first, so the data bits are the odd cells. */
static uint8_t get_byte(const stream_t *s, int64_t pos) {
    unsigned v = 0;
    for (int k = 0; k < 8; k++) v = (v << 1) | cell(s, pos + 1 + 2 * k);
    return (uint8_t)v;
}

/* 16 cells of an FM byte with the given clock pattern, clock cell first. */
static uint16_t fm_cells(uint8_t data, uint8_t clock) {
    unsigned v = 0;
    for (int k = 7; k >= 0; k--) v = (v << 2) | (((clock >> k) & 1u) << 1) | ((data >> k) & 1u);
    return (uint16_t)v;
}

/* CRC state after the bytes that precede a mark byte: the three A1s for MFM, nothing for FM. */
static uint16_t crc_start(const stream_t *s) {
    static const uint8_t a1[3] = {0xA1, 0xA1, 0xA1};
    return s->enc == IBM_ENC_MFM ? flux_crc16(FLUX_CRC16_INIT, a1, 3) : FLUX_CRC16_INIT;
}

static int is_data_mark(uint8_t m) {
    return m >= 0xF8 && m <= 0xFB;
}

typedef void (*mark_fn)(ibm_track_t *t, const stream_t *s, uint32_t pos, uint8_t mark);

/*
 * Calls fn for every mark byte that starts in [0, n): its position (encoded cells) and value.
 * MFM: after an A1 A1 A1 sync. FM: a byte with clock C7 whose data is FE or F8-FB.
 */
static void scan_marks(ibm_track_t *t, const stream_t *s, mark_fn fn) {
    if (s->enc == IBM_ENC_MFM) {
        uint64_t sr = 0;
        /* Prime with the 48 cells before the index so a sync straddling it is seen. */
        for (int64_t e = -48; e < (int64_t)s->n - 1; e++) {
            sr = ((sr << 1) | cell(s, e)) & MFM_SYNC_MASK;
            if (e >= -1 && sr == MFM_SYNC_CELLS) {
                uint32_t p = (uint32_t)(e + 1);
                fn(t, s, p, get_byte(s, p));
            }
        }
    } else {
        const uint16_t id = fm_cells(IBM_MARK_ID, FM_MARK_CLOCK);
        uint16_t dm[4];
        for (unsigned k = 0; k < 4; k++) dm[k] = fm_cells((uint8_t)(0xF8 + k), FM_MARK_CLOCK);
        uint16_t sr = 0;
        for (int64_t e = 0; e < (int64_t)s->n + 15; e++) {
            sr = (uint16_t)((sr << 1) | cell(s, e));
            if (e < 15) continue;
            uint32_t p = (uint32_t)(e - 15);
            if (sr == id) {
                fn(t, s, p, IBM_MARK_ID);
            } else {
                for (unsigned k = 0; k < 4; k++) {
                    if (sr == dm[k]) fn(t, s, p, (uint8_t)(0xF8 + k));
                }
            }
        }
    }
}

static void on_id(ibm_track_t *t, const stream_t *s, uint32_t pos, uint8_t mark) {
    if (mark != IBM_MARK_ID) return;
    uint8_t f[7];
    uint16_t crc = crc_start(s);
    for (int k = 0; k < 7; k++) {
        f[k] = get_byte(s, (int64_t)pos + 16 * k);
        crc = flux_crc16_byte(crc, f[k]);
    }
    if (t->count >= t->max_sectors) {
        if (t->lost < UINT16_MAX) t->lost++;
        return;
    }
    ibm_sector_t *sec = &t->sectors[t->count++];
    sec->id_pos = pos * s->step;
    sec->data_pos = IBM_NO_POS;
    sec->data_off = 0;
    sec->c = f[1];
    sec->h = f[2];
    sec->r = f[3];
    sec->n = f[4];
    sec->size = sec->n <= IBM_SECTOR_MAX_N ? (uint16_t)(128u << sec->n) : 0;
    sec->id_crc = (uint16_t)((f[5] << 8) | f[6]);
    sec->data_crc = 0;
    sec->mark = 0;
    sec->flags = IBM_SEC_NO_DATA;
    if (crc != 0) sec->flags |= IBM_SEC_ID_CRC_ERROR;
}

static void on_data(ibm_track_t *t, const stream_t *s, uint32_t pos, uint8_t mark) {
    if (!is_data_mark(mark) || t->count == 0) return;

    /* The data belongs to the nearest ID before it (going back round the index if need be). */
    ibm_sector_t *best = NULL;
    uint32_t best_d = UINT32_MAX;
    for (uint16_t i = 0; i < t->count; i++) {
        int64_t id_end = (int64_t)(t->sectors[i].id_pos / s->step) + 7 * 16;
        int64_t d = ((int64_t)pos - id_end) % (int64_t)s->n;
        if (d < 0) d += s->n;
        if ((uint32_t)d < best_d) {
            best_d = (uint32_t)d;
            best = &t->sectors[i];
        }
    }
    if (best_d > IBM_DATA_WINDOW_CELLS || !(best->flags & IBM_SEC_NO_DATA)) return;

    best->flags &= (uint8_t)~IBM_SEC_NO_DATA;
    best->data_pos = pos * s->step;
    best->mark = mark;
    if (mark == 0xF8 || mark == 0xF9) best->flags |= IBM_SEC_DELETED;
    if (best->size == 0) {
        best->flags |= IBM_SEC_BAD_SIZE;
        return;
    }

    int store = t->data_used + best->size <= t->data_cap;
    if (store) {
        best->data_off = t->data_used;
        t->data_used += best->size;
    } else {
        best->flags |= IBM_SEC_DATA_DROPPED;
    }
    uint16_t crc = flux_crc16_byte(crc_start(s), mark);
    int64_t p = (int64_t)pos + 16;
    for (uint32_t k = 0; k < best->size; k++, p += 16) {
        uint8_t b = get_byte(s, p);
        crc = flux_crc16_byte(crc, b);
        if (store) t->data[best->data_off + k] = b;
    }
    uint8_t hi = get_byte(s, p), lo = get_byte(s, p + 16);
    crc = flux_crc16_byte(flux_crc16_byte(crc, hi), lo);
    best->data_crc = (uint16_t)((hi << 8) | lo);
    if (crc != 0) best->flags |= IBM_SEC_DATA_CRC_ERROR;
}

void ibm_track_init(ibm_track_t *t, ibm_sector_t *sectors, uint16_t max_sectors, uint8_t *data, uint32_t data_cap) {
    t->sectors = sectors;
    t->max_sectors = max_sectors;
    t->data = data;
    t->data_cap = data_cap;
    t->encoding = IBM_ENC_MFM;
    t->cell_step = 1;
    t->count = 0;
    t->lost = 0;
    t->data_used = 0;
}

ibm_status_t ibm_track_decode(ibm_track_t *t, const uint8_t *cells, uint32_t ncells, ibm_encoding_t enc,
                              unsigned cell_step) {
    return ibm_track_decode_span(t, cells, ncells, ncells, enc, cell_step);
}

ibm_status_t ibm_track_decode_span(ibm_track_t *t, const uint8_t *cells, uint32_t ncells_rev, uint32_t ncells,
                                   ibm_encoding_t enc, unsigned cell_step) {
    if (!t || !cells || cell_step < 1 || cell_step > 2 || ncells < ncells_rev) return IBM_ERR_ARG;
    if (t->max_sectors && !t->sectors) return IBM_ERR_ARG;
    if (t->data_cap && !t->data) return IBM_ERR_ARG;
    stream_t s = {cells, ncells_rev / cell_step, ncells / cell_step, cell_step, enc};
    if (s.n < 16 * 8) return IBM_ERR_ARG;

    t->encoding = enc;
    t->cell_step = cell_step;
    t->count = 0;
    t->lost = 0;
    t->data_used = 0;
    scan_marks(t, &s, on_id);
    scan_marks(t, &s, on_data);
    return IBM_OK;
}

static int has_good_id(const ibm_track_t *t) {
    for (uint16_t i = 0; i < t->count; i++) {
        if (!(t->sectors[i].flags & IBM_SEC_ID_CRC_ERROR)) return 1;
    }
    return 0;
}

ibm_status_t ibm_track_decode_auto(ibm_track_t *t, const uint8_t *cells, uint32_t ncells) {
    return ibm_track_decode_span_auto(t, cells, ncells, ncells);
}

ibm_status_t ibm_track_decode_span_auto(ibm_track_t *t, const uint8_t *cells, uint32_t ncells_rev, uint32_t ncells) {
    static const struct {
        ibm_encoding_t enc;
        unsigned step;
    } tries[] = {{IBM_ENC_MFM, 1}, {IBM_ENC_FM, 2}, {IBM_ENC_FM, 1}};
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        ibm_status_t st = ibm_track_decode_span(t, cells, ncells_rev, ncells, tries[i].enc, tries[i].step);
        if (st != IBM_OK) return st;
        if (has_good_id(t)) return IBM_OK;
    }
    ibm_status_t st = ibm_track_decode_span(t, cells, ncells_rev, ncells, IBM_ENC_MFM, 1);
    return st != IBM_OK ? st : IBM_ERR_NOTHING;
}

int ibm_track_find(const ibm_track_t *t, uint8_t r) {
    for (uint16_t i = 0; i < t->count; i++) {
        if (t->sectors[i].r == r && !(t->sectors[i].flags & IBM_SEC_ID_CRC_ERROR)) return i;
    }
    return -1;
}
