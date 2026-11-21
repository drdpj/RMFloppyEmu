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
 * The 480Z link on UART1: see link_uart.h for what it reproduces and where the facts come from.
 *
 * The send side keeps at most one byte waiting in the TX FIFO (behind the one being shifted out), so that
 * a byte is queued only while the 480Z's DTR is asserted, as the SIO's auto enables would have it, and
 * a reset finds little in flight. At 125000 baud this still sends back to back.
 */
#include "pico/link_uart.h"

#include "core/rmfe_assert.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define LINK_UART uart1

/* The level on our side for an asserted RS-232 line, and the GPIO edge that asserts it: the one place the
 * transceiver's inversion is handled. */
#define LEVEL(asserted) ((asserted) ? LINK_UART_ASSERTED_LEVEL : !LINK_UART_ASSERTED_LEVEL)
#define ASSERT_EDGE     (LINK_UART_ASSERTED_LEVEL ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL)

/* Set by rts_irq() when the 480Z asserts RTS; read and cleared by take_reset() in the main loop, with
 * interrupts off. */
static volatile bool reset_pending;
static void (*idle_fn)(void); /* set by core 0 before serving; called only from lu_recv() on core 0 */
static bool irq_installed;

/* The PL011's divisors for the two rates, worked out at init by the SDK (uart_set_baudrate) */
static uint32_t slow_ibrd, slow_fbrd, fast_ibrd, fast_fbrd;

static inline void set_line(unsigned pin, bool asserted) {
    gpio_put(pin, LEVEL(asserted));
}

static inline bool line_asserted(unsigned pin) {
    return gpio_get(pin) == LINK_UART_ASSERTED_LEVEL;
}

/* The IDC's NMI. CTS is dropped here, not later, so the 480Z (which now waits for it to send 00 at 9600)
 * can't send a byte while we are still at 125000. */
static void __not_in_flash_func(rts_irq)(void) {
    uint32_t events = gpio_get_irq_event_mask(LINK_UART_PIN_RTS_IN);
    if (!(events & ASSERT_EDGE)) return;
    gpio_acknowledge_irq(LINK_UART_PIN_RTS_IN, ASSERT_EDGE);
    set_line(LINK_UART_PIN_CTS_OUT, false);
    reset_pending = true;
}

/*
 * Changes the baud rate (unless baud is 0) and/or empties the FIFOs, at once. The SDK's calls for this disable
 * the UART and then wait 15 bit times at the new rate for a character to finish: at 9600 the receiver is off for
 * 1.6 ms. We never need the wait. This runs at a reset, where everything in flight is discarded (and the 480Z
 * waits for CTS), or after go-fast's drain, with nothing in flight; the IDC's CTC switches at once
 * [IDC3 0066 -> 010b]. The PL011 must still be disabled while LCR_H is written, and that write latches the
 * divisors; clearing FEN flushes the TX FIFO (PL011 TRM, "UART operation").
 */
static void __not_in_flash_func(reprogram)(uint32_t baud, bool flush) {
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    uint32_t cr = hw->cr;
    hw->cr = cr & ~(UART_UARTCR_UARTEN_BITS | UART_UARTCR_TXE_BITS | UART_UARTCR_RXE_BITS);
    if (baud) {
        hw->ibrd = baud == LINK_BAUD_FAST ? fast_ibrd : slow_ibrd;
        hw->fbrd = baud == LINK_BAUD_FAST ? fast_fbrd : slow_fbrd;
    }
    uint32_t lcr = hw->lcr_h;
    if (flush) hw->lcr_h = lcr & ~UART_UARTLCR_H_FEN_BITS;
    hw->lcr_h = lcr;
    hw->cr = cr;
}

/* Consumes a pending reset: empties both FIFOs, as resetting the SIO does. The byte being shifted out
 * finishes (the PL011 can't abort it). */
static bool __not_in_flash_func(take_reset)(void) {
    uint32_t irq = save_and_disable_interrupts();
    bool pending = reset_pending;
    reset_pending = false;
    restore_interrupts(irq);
    if (!pending) return false;
    reprogram(0, true);
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    while (!(hw->fr & UART_UARTFR_RXFE_BITS))
        (void)hw->dr; /* anything that came in meanwhile */
    return true;
}

static link_status_t __not_in_flash_func(lu_recv)(void *ctx, uint8_t *byte, bool *parity_error) {
    (void)ctx;
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    for (;;) {
        /* CTS goes up only if no reset is pending: rts_irq() can't then be overtaken. */
        uint32_t irq = save_and_disable_interrupts();
        bool pending = reset_pending;
        if (!pending) set_line(LINK_UART_PIN_CTS_OUT, true);
        restore_interrupts(irq);
        if (pending) {
            (void)take_reset();
            return LINK_RESET;
        }
        if (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
            set_line(LINK_UART_PIN_CTS_OUT, false);
            uint32_t dr = hw->dr;
            *byte = (uint8_t)(dr & UART_UARTDR_DATA_BITS);
            *parity_error = (dr & UART_UARTDR_PE_BITS) != 0;
            return LINK_OK;
        }
        if (idle_fn) idle_fn(); /* with CTS up: a byte arriving meanwhile waits in the FIFO */
    }
}

void link_uart_set_idle(void (*fn)(void)) {
    idle_fn = fn;
}

static link_status_t __not_in_flash_func(lu_send)(void *ctx, uint8_t byte) {
    (void)ctx;
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    for (;;) {
        if (take_reset()) return LINK_RESET;
        if (line_asserted(LINK_UART_PIN_DTR_IN) && (hw->fr & UART_UARTFR_TXFE_BITS)) {
            hw->dr = byte;
            return LINK_OK;
        }
    }
}

static link_status_t __not_in_flash_func(lu_drain)(void *ctx) {
    (void)ctx;
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    for (;;) {
        if (take_reset()) return LINK_RESET;
        if (!(hw->fr & UART_UARTFR_BUSY_BITS)) return LINK_OK; /* BUSY covers the FIFO and the shifter */
    }
}

static void __not_in_flash_func(lu_set_baud)(void *ctx, uint32_t baud) {
    (void)ctx;
    RMFE_ASSERT(baud == LINK_BAUD_SLOW || baud == LINK_BAUD_FAST); /* the only rates core/link.c uses */
    reprogram(baud, false);
}

static const link_io_t io = {NULL, lu_recv, lu_send, lu_drain, lu_set_baud};

const link_io_t *link_uart_io(void) {
    return &io;
}

static void init_output(unsigned pin, bool asserted) {
    gpio_init(pin);
    set_line(pin, asserted);
    gpio_set_dir(pin, GPIO_OUT);
}

static void init_input(unsigned pin) {
    gpio_init(pin);
    gpio_pull_up(pin); /* an unplugged 480Z reads as deasserted (link_uart.h) */
}

void link_uart_init(void) {
    gpio_set_irq_enabled(LINK_UART_PIN_RTS_IN, ASSERT_EDGE, false);
    reset_pending = false;

    init_output(LINK_UART_PIN_CTS_OUT, false);
    init_output(LINK_UART_PIN_DCD_OUT, true); /* [IDC3 0123: WR5 EA, RTS on] */
    init_input(LINK_UART_PIN_RTS_IN);
    init_input(LINK_UART_PIN_DTR_IN);

    /* The pins first: until RX is the UART's, the UART's input reads low, which is a break, which arrives as a
     * 00 (go fast) that the unit would answer with 5A 00 01 at power-up (seen on the level-3 tests). RX idles at
     * mark when nothing drives it: a floating RX would read as a stream of the same. */
    gpio_pull_up(LINK_UART_PIN_RX);
    gpio_set_function(LINK_UART_PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(LINK_UART_PIN_RX, GPIO_FUNC_UART);

    /* The SDK works out the divisors; both rates are exact at 150 MHz (link_uart.h). Ends at 9600. */
    (void)uart_init(LINK_UART, LINK_BAUD_FAST);
    fast_ibrd = uart_get_hw(LINK_UART)->ibrd;
    fast_fbrd = uart_get_hw(LINK_UART)->fbrd;
    (void)uart_set_baudrate(LINK_UART, LINK_BAUD_SLOW);
    slow_ibrd = uart_get_hw(LINK_UART)->ibrd;
    slow_fbrd = uart_get_hw(LINK_UART)->fbrd;
    uart_set_format(LINK_UART, 8, 1, UART_PARITY_EVEN); /* 8E1 [IDC3 0123: WR4 47] */
    uart_set_hw_flow(LINK_UART, false, false);          /* the handshake lines are GPIOs */
    uart_set_fifo_enabled(LINK_UART, true);
    /* And anything that arrived while the UART was being set up is not a command. */
    while (uart_is_readable(LINK_UART))
        (void)uart_get_hw(LINK_UART)->dr;
    uart_get_hw(LINK_UART)->rsr = 0; /* clears the error flags */

    if (!irq_installed) {
        gpio_add_raw_irq_handler(LINK_UART_PIN_RTS_IN, rts_irq);
        irq_set_enabled(IO_IRQ_BANK0, true);
        irq_installed = true;
    }
    gpio_acknowledge_irq(LINK_UART_PIN_RTS_IN, ASSERT_EDGE);
    gpio_set_irq_enabled(LINK_UART_PIN_RTS_IN, ASSERT_EDGE, true);
}
