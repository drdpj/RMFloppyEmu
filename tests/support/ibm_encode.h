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
 * Test-only IBM System 34 track encoder: builds FM/MFM bitcells (MSB first) for synthetic tracks, so
 * the decoder (src/flux/ibm_track.c) is tested against tracks built from the format's rules, not
 * from the decoder's own code. Layout (IBM System 34; WD1793 data sheet "IBM format"):
 *   MFM: 80x4E, 12x00, 3xC2*, FC, 50x4E, then per sector: 12x00, 3xA1*, FE C H R N CRC, 22x4E,
 *        12x00, 3xA1*, FB data CRC, gap3 x 4E; 4E to the end of the track. (* missing clock)
 *   FM:  40xFF, 6x00, FC(clock D7), 26xFF, then per sector: 6x00, FE(clock C7) C H R N CRC, 11xFF,
 *        6x00, FB(clock C7) data CRC, gap3 x FF; FF to the end.
 */
#ifndef RMFE_TESTS_IBM_ENCODE_H
#define RMFE_TESTS_IBM_ENCODE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t cap_cells;
    uint32_t pos;     /* cells written */
    unsigned step;    /* cells written per encoded cell (2 = FM at the MFM rate: each cell then a 0) */
    unsigned prev;    /* MFM: the last data bit written */
    bool overflow;
} enc_t;

typedef struct {
    uint8_t c, h, r, n;
    const uint8_t *data; /* 128 << n bytes; NULL = fill with fill_byte */
    uint8_t fill_byte;
    uint8_t mark;        /* data mark; 0 = FB */
    bool no_data;        /* write the ID field only */
    bool bad_id_crc;     /* corrupt the recorded ID CRC */
    bool bad_data_crc;   /* corrupt the recorded data CRC */
} enc_sector_t;

void enc_init(enc_t *e, uint8_t *buf, uint32_t cap_bytes, unsigned step);
void enc_cells(enc_t *e, uint32_t bits, unsigned n); /* the low n bits of bits, MSB first, as encoded cells */
void enc_mfm_byte(enc_t *e, uint8_t b);
void enc_mfm_a1_sync(enc_t *e);                       /* A1 with the missing clock: cells 4489 */
void enc_fm_byte(enc_t *e, uint8_t data, uint8_t clock);

/* A whole track; fills the rest of the buffer with gap bytes. */
void enc_mfm_track(enc_t *e, const enc_sector_t *secs, unsigned count, unsigned gap3);
void enc_fm_track(enc_t *e, const enc_sector_t *secs, unsigned count, unsigned gap3);

/* Rotates the first ncells cells of buf left by k cells (the index moves k cells later). */
void enc_rotate(uint8_t *buf, uint32_t ncells, uint32_t k);

#endif
