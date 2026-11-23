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
#include "core/link.h"

#include <stddef.h>

#include "core/rmfe_assert.h"

static link_status_t check(link_t *l, link_status_t st) {
    if (st == LINK_RESET && l->baud != LINK_BAUD_SLOW) {
        l->baud = LINK_BAUD_SLOW;
        l->io->set_baud(l->io->ctx, LINK_BAUD_SLOW);
    }
    return st;
}

void link_init(link_t *l, const link_io_t *io) {
    RMFE_ASSERT(l && io && io->recv && io->send && io->drain && io->set_baud);
    l->io = io;
    l->baud = LINK_BAUD_SLOW;
    l->fast_baud = LINK_BAUD_FAST;
    l->boot_baud = LINK_BAUD_FAST;
    l->last_parity_error = false;
    io->set_baud(io->ctx, LINK_BAUD_SLOW);
}

void link_set_boot_baud(link_t *l, uint32_t baud) {
    RMFE_ASSERT(l && (baud == LINK_BAUD_FAST || baud == LINK_BAUD_HALF));
    l->boot_baud = baud;
}

void link_set_fast_baud(link_t *l, uint32_t baud) {
    RMFE_ASSERT(l && (baud == LINK_BAUD_FAST || baud == LINK_BAUD_HALF));
    l->fast_baud = baud;
}

uint8_t link_fast_time_constant(uint32_t baud) {
    RMFE_ASSERT(baud == LINK_BAUD_FAST || baud == LINK_BAUD_HALF);
    return (uint8_t)(LINK_CTC_HZ / LINK_CLOCKS_PER_BIT / baud);
}

link_status_t link_get(link_t *l, uint8_t *byte) {
    bool parity_error = false;
    link_status_t st = check(l, l->io->recv(l->io->ctx, byte, &parity_error));
    l->last_parity_error = st == LINK_OK && parity_error;
    return st;
}

link_status_t link_put(link_t *l, uint8_t byte) {
    return check(l, l->io->send(l->io->ctx, byte));
}

link_status_t link_go_fast(link_t *l) {
    uint32_t fast = l->last_parity_error ? l->boot_baud : l->fast_baud; /* link_set_boot_baud() */
    const uint8_t reply[3] = {LINK_FAST_REPLY_0, LINK_FAST_REPLY_1, link_fast_time_constant(fast)};
    for (size_t i = 0; i < sizeof reply; i++) {
        link_status_t st = link_put(l, reply[i]);
        if (st != LINK_OK) return st;
    }
    link_status_t st = check(l, l->io->drain(l->io->ctx));
    if (st != LINK_OK) return st;
    l->baud = fast;
    l->io->set_baud(l->io->ctx, fast);
    return LINK_OK;
}

link_status_t link_send_record(link_t *l, const uint8_t record[LINK_RECORD]) {
    for (;;) {
        for (size_t i = 0; i < LINK_RECORD; i++) {
            link_status_t st = link_put(l, record[i]);
            if (st != LINK_OK) return st;
        }
        uint8_t ack;
        link_status_t st = link_get(l, &ack);
        if (st != LINK_OK) return st;
        if (ack == LINK_BLOCK_OK) return LINK_OK;
    }
}

link_status_t link_recv_record(link_t *l, uint8_t record[LINK_RECORD]) {
    for (;;) {
        bool any_parity = false;
        for (size_t i = 0; i < LINK_RECORD; i++) {
            bool pe;
            link_status_t st = check(l, l->io->recv(l->io->ctx, &record[i], &pe));
            if (st != LINK_OK) return st;
            any_parity = any_parity || pe;
        }
        link_status_t st = link_put(l, any_parity ? LINK_BLOCK_RETRY : LINK_BLOCK_OK);
        if (st != LINK_OK) return st;
        if (!any_parity) return LINK_OK;
    }
}
