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
 * The byte link between the 480Z and the disc unit (docs/protocol.md: "Framing and baud rate",
 * "Flow control", read/write record exchanges).
 *
 * The platform supplies link_io_t: blocking receive and send of single bytes (8E1 framing is the
 * platform's job), waiting for the transmitter to drain, and setting the baud rate. Two duties of
 * the platform are implied by recv():
 *   - the unit's DTR (the 480Z's CTS, "send me a byte") is asserted only while recv() waits, as the
 *     IDC firmware does [IDC3 0a36], so the 480Z never sends a byte nobody is waiting for;
 *   - a rising RTS from the 480Z (the IDC's NMI) makes any pending or later call return LINK_RESET
 *     until the link layer has handled it.
 * On LINK_RESET the link layer drops to 9600 baud itself [IDC3 0066 -> 010b]; the caller abandons
 * whatever exchange it was in and waits for a new command.
 *
 * Deterministic and allocation-free: the link keeps only the current baud rate.
 */
#ifndef RMFE_CORE_LINK_H
#define RMFE_CORE_LINK_H

#include <stdbool.h>
#include <stdint.h>

/* Both ends clock their SIO from CTC0, counting a 2 MHz clock, at 16 clocks a bit: baud = 2 MHz / 16 / the time
 * constant [IDC3 0116, 0b1d]. */
#define LINK_CTC_HZ         2000000u
#define LINK_CLOCKS_PER_BIT 16u
#define LINK_BAUD_SLOW      9600u   /* CTC0 time constant 0D: 2 MHz / 13 / 16 = 9615 [IDC3 0116] */
#define LINK_BAUD_FAST      125000u /* CTC0 time constant 01: 2 MHz / 16, the IDC's [IDC3 0b1d] */
#define LINK_BAUD_HALF      62500u  /* CTC0 time constant 02: not the IDC's; for an interface that can't do 125000 */
#define LINK_RECORD         128u    /* bytes per record transfer [IDC3 0f3e, 0f59] */

/* "Go fast" reply to command 00: 5A 00, then the time constant the 480Z loads into its CTC0 for the fast rate
 * [IDC3 0b07, trace; 480Z ROS 1.2 1f04-1f17 -> 3cae, ROS 2.2 3f04-3f17 -> 7cae]. The IDC sends 01. */
#define LINK_FAST_REPLY_0 0x5Au
#define LINK_FAST_REPLY_1 0x00u
#define LINK_FAST_REPLY_2 0x01u

/* Block acknowledgements sent by the unit after receiving a record [IDC3 0f52-0f72]. */
#define LINK_BLOCK_OK    0x00u
#define LINK_BLOCK_RETRY 0xFFu

typedef enum {
    LINK_OK = 0,
    LINK_RESET,  /* the 480Z raised RTS: abandon the exchange (the link is back at 9600 baud) */
    LINK_CLOSED, /* the link has gone (host builds and tests: end of input); stop serving */
} link_status_t;

typedef struct {
    void *ctx;
    /* Waits for the next byte, the 480Z's CTS asserted only while waiting. *parity_error is set if
     * the byte arrived with a parity error. */
    link_status_t (*recv)(void *ctx, uint8_t *byte, bool *parity_error);
    link_status_t (*send)(void *ctx, uint8_t byte);
    /* Waits until the last byte sent has left the transmitter completely. */
    link_status_t (*drain)(void *ctx);
    void (*set_baud)(void *ctx, uint32_t baud);
} link_io_t;

typedef struct {
    const link_io_t *io;
    uint32_t baud;
    uint32_t fast_baud;     /* the rate command 00 switches to: LINK_BAUD_FAST or LINK_BAUD_HALF */
    uint32_t boot_baud;     /* the same, when that 00 came from the 480Z's ROS (link_set_boot_baud) */
    bool last_parity_error; /* the last byte link_get() returned had a parity error */
} link_t;

/* Starts the link at 9600 baud, going fast at 125000 every time, as the IDC does. */
void link_init(link_t *l, const link_io_t *io);

/* Sets the rate command 00 switches to, from the next 00: LINK_BAUD_FAST (the IDC's) or LINK_BAUD_HALF. */
void link_set_fast_baud(link_t *l, uint32_t baud);

/*
 * Sets the rate for a 00 from the 480Z's ROS, which then loads the boot block (command FF): LINK_BAUD_FAST (as the
 * IDC, and the default) or LINK_BAUD_HALF. The ROS sets its SIO to 8N2 (WR4 4C) [480Z ROS 1.2 1cb5, ROS 2.2 3cb5],
 * so its 00 reaches the unit's 8E1 receiver with a stop bit where the parity bit should be: a parity error, every
 * time, as 00 has even parity. The boot code and the CP/M BIOS send 8E1, so their 00s arrive clean. On a real 480Z
 * (ROS 1.2) the ROS lost bytes of the boot block at 125000 though everything after it worked there, and loaded it at
 * 62500 (docs/protocol.md, "Framing and baud rate").
 */
void link_set_boot_baud(link_t *l, uint32_t baud);

/* The time constant go-fast sends for a fast rate (the third byte of its reply). */
uint8_t link_fast_time_constant(uint32_t baud);

/* One byte each way. Parity errors on single bytes are ignored, as the IDC ignores them [IDC3 0a1f]. */
link_status_t link_get(link_t *l, uint8_t *byte);
link_status_t link_put(link_t *l, uint8_t byte);

/* Command 00: replies 5A 00 and the fast rate's time constant (01 for 125000), waits for it to leave, then
 * switches to the fast rate: the boot rate if the 00 (the last byte link_get() returned) had a parity error. */
link_status_t link_go_fast(link_t *l);

/* Sends a 128-byte record, then reads the 480Z's acknowledgement: 00 accepts it, anything else
 * asks for it again (no limit, as the IDC; an RTS reset ends it) [IDC3 0f3d]. */
link_status_t link_send_record(link_t *l, const uint8_t record[LINK_RECORD]);

/* Receives a 128-byte record. If any byte had a parity error the unit answers FF and takes the
 * whole record again; otherwise it answers 00 [IDC3 0f58]. */
link_status_t link_recv_record(link_t *l, uint8_t record[LINK_RECORD]);

#endif
