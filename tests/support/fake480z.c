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
#include "fake480z.h"

#include <string.h>

static link_status_t f_recv(void *ctx, uint8_t *byte, bool *parity_error) {
    fake480z_t *f = ctx;
    if (f->pos >= f->nin) return LINK_CLOSED;
    const fake_in_t *x = &f->in[f->pos++];
    if (x->kind == FAKE_RESET) return LINK_RESET;
    *byte = x->byte;
    *parity_error = x->kind == FAKE_PARITY_BYTE;
    return LINK_OK;
}

static link_status_t f_send(void *ctx, uint8_t byte) {
    fake480z_t *f = ctx;
    if (f->nout >= FAKE_MAX_OUT) return LINK_CLOSED;
    f->out[f->nout++] = byte;
    return LINK_OK;
}

static link_status_t f_drain(void *ctx) {
    fake480z_t *f = ctx;
    f->drains++;
    return LINK_OK;
}

static void f_set_baud(void *ctx, uint32_t baud) {
    fake480z_t *f = ctx;
    f->baud = baud;
    if (f->nbauds < FAKE_MAX_EVENTS) {
        f->bauds[f->nbauds].out_at = f->nout;
        f->bauds[f->nbauds].in_at = f->pos;
        f->bauds[f->nbauds].baud = baud;
        f->nbauds++;
    }
}

void fake480z_init(fake480z_t *f, const fake_in_t *script, size_t n) {
    memset(f, 0, sizeof *f);
    f->in = script;
    f->nin = n;
    f->io = (link_io_t){f, f_recv, f_send, f_drain, f_set_baud};
}

size_t fake_bytes(fake_in_t *buf, size_t at, const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) buf[at + i] = (fake_in_t){FAKE_BYTE, bytes[i]};
    return at + n;
}
