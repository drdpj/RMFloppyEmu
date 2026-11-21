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
 * The 480Z link on the Pico 2: link_io_t (core/link.h) on UART1 and four GPIOs, through the RS-232
 * transceiver (docs/hardware.md, "The 480Z link"). The IDC's side of the link, as this reproduces it:
 *   - 8E1, 9600 or 125000 baud (docs/protocol.md, "Framing and baud rate"); the PL011's divisors give both
 *     exactly from the 150 MHz peripheral clock.
 *   - The 480Z's CTS (the IDC's DTR) is asserted only while recv() waits for a byte [IDC3 0a36].
 *   - The 480Z's DCD (the IDC's RTS) is held asserted [IDC3 0123: WR5 EA].
 *   - The 480Z's RTS (the IDC's DCD and NMI) going asserted is a reset: the next recv(), send() or drain()
 *     returns LINK_RESET, once, with both FIFOs emptied, as the NMI resets the SIO [IDC3 0066].
 *   - A byte is sent only while the 480Z's DTR (the IDC's CTS) is asserted: the IDC's SIO has auto enables
 *     on [IDC3 012c: WR3 E1], so its transmitter waits for CTS.
 * There is one link, so the state is static.
 */
#ifndef RMFE_PICO_LINK_UART_H
#define RMFE_PICO_LINK_UART_H

#include "core/link.h"

/* docs/hardware.md, "Pin plan" */
#define LINK_UART_PIN_TX      4u /* UART1 TX -> 480Z RxD */
#define LINK_UART_PIN_RX      5u /* UART1 RX <- 480Z TxD */
#define LINK_UART_PIN_CTS_OUT 6u /* -> 480Z CTS: "send me a byte" */
#define LINK_UART_PIN_DCD_OUT 7u /* -> 480Z DCD */
#define LINK_UART_PIN_RTS_IN  8u /* <- 480Z RTS: reset (edge interrupt) */
#define LINK_UART_PIN_DTR_IN  9u /* <- 480Z DTR: we may send */

/* The transceiver inverts (RS-232 asserted = positive volts = logic 0 on our side), and the firmware
 * otherwise works in "asserted" terms. Undriven inputs are pulled up, so an unplugged 480Z reads as
 * deasserted. */
#define LINK_UART_ASSERTED_LEVEL 0u

/* Sets up UART1 (8E1, 9600 baud), the pins and the reset interrupt. May be called again to start afresh. */
void link_uart_init(void);

/* The link_io_t for core/link.h and core/idc.h. Its recv() and send() wait as long as they need to. */
const link_io_t *link_uart_io(void);

/* fn is called over and over while recv() waits for a byte and none has come, with the 480Z's CTS asserted.
 * NULL: nothing. It must return quickly: a byte arriving meanwhile waits in the FIFO until it does. The firmware
 * uses it to do the menu's work on the SD card between commands (src/pico/main.c). Core 0 only. */
void link_uart_set_idle(void (*fn)(void));

#endif
