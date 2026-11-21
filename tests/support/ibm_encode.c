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
#include "ibm_encode.h"

#include <stdlib.h>
#include <string.h>

#include "flux/crc16.h"

void enc_init(enc_t *e, uint8_t *buf, uint32_t cap_bytes, unsigned step) {
    memset(buf, 0, cap_bytes);
    e->buf = buf;
    e->cap_cells = cap_bytes * 8u;
    e->pos = 0;
    e->step = step;
    e->prev = 0;
    e->overflow = false;
}

static void put_cell(enc_t *e, unsigned v) {
    for (unsigned k = 0; k < e->step; k++) {
        if (e->pos >= e->cap_cells) {
            e->overflow = true;
            return;
        }
        if (v && k == 0) e->buf[e->pos >> 3] |= (uint8_t)(0x80u >> (e->pos & 7u));
        e->pos++;
    }
}

void enc_cells(enc_t *e, uint32_t bits, unsigned n) {
    while (n--) put_cell(e, (bits >> n) & 1u);
}

void enc_mfm_byte(enc_t *e, uint8_t b) {
    for (int k = 7; k >= 0; k--) {
        unsigned d = (b >> k) & 1u;
        put_cell(e, !(e->prev | d)); /* clock: 1 only between two 0 data bits */
        put_cell(e, d);
        e->prev = d;
    }
}

void enc_mfm_a1_sync(enc_t *e) {
    enc_cells(e, 0x4489u, 16);
    e->prev = 1; /* A1 ends in a 1 */
}

void enc_fm_byte(enc_t *e, uint8_t data, uint8_t clock) {
    for (int k = 7; k >= 0; k--) {
        put_cell(e, (clock >> k) & 1u);
        put_cell(e, (data >> k) & 1u);
    }
}

static void mfm_run(enc_t *e, uint8_t b, unsigned n) {
    while (n--) enc_mfm_byte(e, b);
}

static void fm_run(enc_t *e, uint8_t b, unsigned n) {
    while (n--) enc_fm_byte(e, b, 0xFF);
}

/* The field bytes after the mark, with its CRC (corrupted on request). */
static void field(uint8_t *out, uint8_t mark, const uint8_t *body, unsigned len, bool mfm, bool corrupt) {
    static const uint8_t a1[3] = {0xA1, 0xA1, 0xA1};
    uint16_t crc = mfm ? flux_crc16(FLUX_CRC16_INIT, a1, 3) : FLUX_CRC16_INIT;
    crc = flux_crc16_byte(crc, mark);
    crc = flux_crc16(crc, body, len);
    if (corrupt) crc ^= 0x5A5Au;
    memcpy(out, body, len);
    out[len] = (uint8_t)(crc >> 8);
    out[len + 1] = (uint8_t)crc;
}

static void sector(enc_t *e, const enc_sector_t *s, unsigned gap3, bool mfm) {
    uint8_t id[4] = {s->c, s->h, s->r, s->n}, idf[6];
    unsigned size = 128u << s->n;
    uint8_t *body = malloc(size + 2);
    field(idf, 0xFE, id, 4, mfm, s->bad_id_crc);
    if (mfm) {
        mfm_run(e, 0x00, 12);
        for (int k = 0; k < 3; k++) enc_mfm_a1_sync(e);
        enc_mfm_byte(e, 0xFE);
        for (int k = 0; k < 6; k++) enc_mfm_byte(e, idf[k]);
        mfm_run(e, 0x4E, 22);
    } else {
        fm_run(e, 0x00, 6);
        enc_fm_byte(e, 0xFE, 0xC7);
        for (int k = 0; k < 6; k++) enc_fm_byte(e, idf[k], 0xFF);
        fm_run(e, 0xFF, 11);
    }
    if (!s->no_data) {
        uint8_t mark = s->mark ? s->mark : 0xFB;
        uint8_t *data = malloc(size);
        if (s->data) {
            memcpy(data, s->data, size);
        } else {
            memset(data, s->fill_byte, size);
        }
        field(body, mark, data, size, mfm, s->bad_data_crc);
        free(data);
        if (mfm) {
            mfm_run(e, 0x00, 12);
            for (int k = 0; k < 3; k++) enc_mfm_a1_sync(e);
            enc_mfm_byte(e, mark);
            for (unsigned k = 0; k < size + 2; k++) enc_mfm_byte(e, body[k]);
        } else {
            fm_run(e, 0x00, 6);
            enc_fm_byte(e, mark, 0xC7);
            for (unsigned k = 0; k < size + 2; k++) enc_fm_byte(e, body[k], 0xFF);
        }
    }
    free(body);
    if (mfm) {
        mfm_run(e, 0x4E, gap3);
    } else {
        fm_run(e, 0xFF, gap3);
    }
}

void enc_mfm_track(enc_t *e, const enc_sector_t *secs, unsigned count, unsigned gap3) {
    mfm_run(e, 0x4E, 80);
    mfm_run(e, 0x00, 12);
    for (int k = 0; k < 3; k++) {
        enc_cells(e, 0x5224u, 16); /* C2 with the missing clock */
        e->prev = 0;
    }
    enc_mfm_byte(e, 0xFC);
    mfm_run(e, 0x4E, 50);
    for (unsigned i = 0; i < count; i++) sector(e, &secs[i], gap3, true);
    while (!e->overflow && e->pos + 16 * e->step <= e->cap_cells) enc_mfm_byte(e, 0x4E);
}

void enc_fm_track(enc_t *e, const enc_sector_t *secs, unsigned count, unsigned gap3) {
    fm_run(e, 0xFF, 40);
    fm_run(e, 0x00, 6);
    enc_fm_byte(e, 0xFC, 0xD7);
    fm_run(e, 0xFF, 26);
    for (unsigned i = 0; i < count; i++) sector(e, &secs[i], gap3, false);
    while (!e->overflow && e->pos + 16 * e->step <= e->cap_cells) enc_fm_byte(e, 0xFF, 0xFF);
}

void enc_rotate(uint8_t *buf, uint32_t ncells, uint32_t k) {
    uint8_t *tmp = calloc((ncells + 7) / 8, 1);
    for (uint32_t i = 0; i < ncells; i++) {
        uint32_t from = (i + k) % ncells;
        if (buf[from >> 3] & (0x80u >> (from & 7u))) tmp[i >> 3] |= (uint8_t)(0x80u >> (i & 7u));
    }
    memcpy(buf, tmp, (ncells + 7) / 8);
    free(tmp);
}
