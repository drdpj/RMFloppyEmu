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
#include "flux/ibm_encode.h"

#include <string.h>

#include "flux/crc16.h"

typedef struct {
    uint8_t *cells;
    uint32_t ncells; /* output cells */
    uint32_t pos;    /* encoded cells written */
    unsigned step;
    unsigned prev;   /* MFM: last data bit */
    bool over;
} writer_t;

static void put_cell(writer_t *w, unsigned v) {
    for (unsigned k = 0; k < w->step; k++) {
        uint32_t i = w->pos * w->step + k;
        if (i >= w->ncells) {
            w->over = true;
            return;
        }
        if (v && k == 0) w->cells[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    w->pos++;
}

static void mfm_byte(writer_t *w, uint8_t b) {
    for (int k = 7; k >= 0; k--) {
        unsigned d = (b >> k) & 1u;
        put_cell(w, !(w->prev | d));
        put_cell(w, d);
        w->prev = d;
    }
}

static void mfm_a1(writer_t *w) {
    for (int k = 15; k >= 0; k--) put_cell(w, (0x4489u >> k) & 1u);
    w->prev = 1;
}

static void fm_byte(writer_t *w, uint8_t data, uint8_t clock) {
    for (int k = 7; k >= 0; k--) {
        put_cell(w, (clock >> k) & 1u);
        put_cell(w, (data >> k) & 1u);
    }
}

static void gap_byte(writer_t *w, ibm_encoding_t enc) {
    if (enc == IBM_ENC_MFM) {
        mfm_byte(w, 0x4E);
    } else {
        fm_byte(w, 0xFF, 0xFF);
    }
}

/* Preamble, mark and field (with its CRC, deliberately wrong if bad). */
static void field(writer_t *w, ibm_encoding_t enc, uint8_t mark, const uint8_t *body, uint32_t len, bool bad) {
    static const uint8_t a1[3] = {0xA1, 0xA1, 0xA1};
    uint16_t crc;
    if (enc == IBM_ENC_MFM) {
        for (int i = 0; i < 12; i++) mfm_byte(w, 0x00);
        for (int i = 0; i < 3; i++) mfm_a1(w);
        mfm_byte(w, mark);
        crc = flux_crc16_byte(flux_crc16(FLUX_CRC16_INIT, a1, 3), mark);
    } else {
        for (int i = 0; i < 6; i++) fm_byte(w, 0x00, 0xFF);
        fm_byte(w, mark, 0xC7);
        crc = flux_crc16_byte(FLUX_CRC16_INIT, mark);
    }
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = body ? body[i] : 0;
        crc = flux_crc16_byte(crc, b);
        if (enc == IBM_ENC_MFM) {
            mfm_byte(w, b);
        } else {
            fm_byte(w, b, 0xFF);
        }
    }
    if (bad) crc ^= 0x5A5Au;
    uint8_t c[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
    for (int i = 0; i < 2; i++) {
        if (enc == IBM_ENC_MFM) {
            mfm_byte(w, c[i]);
        } else {
            fm_byte(w, c[i], 0xFF);
        }
    }
}

ibm_status_t ibm_track_encode(const ibm_enc_sector_t *secs, unsigned count, ibm_encoding_t enc, unsigned cell_step,
                              uint8_t *cells, uint32_t ncells) {
    if (!cells || (count && !secs) || cell_step < 1 || cell_step > 2) return IBM_ERR_ARG;
    memset(cells, 0, (ncells + 7u) / 8u);
    writer_t w = {cells, ncells, 0, cell_step, 0, false};
    const uint32_t byte_cells = 16;                                   /* encoded cells per byte */
    const uint32_t pre = (enc == IBM_ENC_MFM ? 15u : 6u) * byte_cells; /* preamble before a mark */
    const uint32_t gap2 = enc == IBM_ENC_MFM ? 22u : 11u;
    const uint32_t total = ncells / cell_step;

    for (unsigned i = 0; i < count; i++) {
        const ibm_enc_sector_t *s = &secs[i];
        uint32_t mark_at = s->pos / cell_step;
        uint32_t start = mark_at >= pre ? mark_at - pre : 0;
        start = (start + byte_cells / 2) / byte_cells * byte_cells; /* to a byte boundary */
        if (start < w.pos) return IBM_ERR_ARG;                      /* overlaps the previous sector */
        while (w.pos + byte_cells <= start) gap_byte(&w, enc);
        const uint8_t id[4] = {s->c, s->h, s->r, s->n};
        field(&w, enc, IBM_MARK_ID, id, 4, false);
        if (!s->no_data) {
            for (uint32_t k = 0; k < gap2; k++) gap_byte(&w, enc);
            field(&w, enc, s->mark ? s->mark : IBM_MARK_DATA, s->data, s->size, s->bad_crc);
        }
        if (w.over || w.pos > total) return IBM_ERR_ARG;
    }
    while (w.pos + byte_cells <= total) gap_byte(&w, enc);
    return IBM_OK;
}
