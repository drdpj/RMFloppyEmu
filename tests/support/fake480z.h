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
 * Test-only: a scripted 480Z on the far end of a link_io_t. The script is what the 480Z sends,
 * including bytes with parity errors and RTS resets; everything the unit sends is recorded, with
 * each baud change and drain noted at its position in the output, and which bytes went out paced.
 */
#ifndef RMFE_TESTS_FAKE480Z_H
#define RMFE_TESTS_FAKE480Z_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/link.h"

typedef enum { FAKE_BYTE, FAKE_PARITY_BYTE, FAKE_RESET } fake_kind_t;

typedef struct {
    fake_kind_t kind;
    uint8_t byte;
} fake_in_t;

#define FB(x)  {FAKE_BYTE, (uint8_t)(x)}
#define FP(x)  {FAKE_PARITY_BYTE, (uint8_t)(x)}
#define FRESET {FAKE_RESET, 0}

#define FAKE_MAX_OUT    16384
#define FAKE_MAX_EVENTS 64

typedef struct {
    const fake_in_t *in;
    size_t nin, pos;
    uint8_t out[FAKE_MAX_OUT];
    size_t nout;
    struct {
        size_t out_at; /* bytes sent before the change */
        size_t in_at;  /* script items consumed before the change */
        uint32_t baud;
    } bauds[FAKE_MAX_EVENTS];
    size_t nbauds;
    size_t drains;
    uint32_t baud;
    bool paced;         /* link_set_paced: now */
    size_t paced_out;   /* bytes sent while paced */
    size_t paced_since; /* bytes sent before the last change to pacing */
    link_io_t io;
} fake480z_t;

void fake480z_init(fake480z_t *f, const fake_in_t *script, size_t n);

/* Appends a block of plain bytes to a script being built in buf (for long records). */
size_t fake_bytes(fake_in_t *buf, size_t at, const uint8_t *bytes, size_t n);

#endif
