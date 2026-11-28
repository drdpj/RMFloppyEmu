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
 * The send side starts a byte only when the transmitter is idle and the 480Z's DTR is asserted. The IDC's SIO,
 * with auto enables, checks its CTS (the 480Z's DTR) as each character starts, so after DTR drops it finishes the
 * one it is sending and no more. A byte left waiting in the PL011's FIFO would go out regardless of DTR: the
 * 480Z's ROS takes two bytes per DTR pulse during the boot block (docs/protocol.md, "Flow control"). Waiting for
 * the shifter costs well under a microsecond a byte.
 */
#include "pico/link_uart.h"

#include "core/rmfe_assert.h"

#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/uart.h"

#define LINK_UART uart1

/* The level on our side for an asserted RS-232 line, and the GPIO edge that asserts it: the one place the
 * transceiver's inversion is handled. */
#define LEVEL(asserted) ((asserted) ? LINK_UART_ASSERTED_LEVEL : !LINK_UART_ASSERTED_LEVEL)
#define ASSERT_EDGE     (LINK_UART_ASSERTED_LEVEL ? GPIO_IRQ_EDGE_RISE : GPIO_IRQ_EDGE_FALL)
#define DEASSERT_EDGE   (LINK_UART_ASSERTED_LEVEL ? GPIO_IRQ_EDGE_FALL : GPIO_IRQ_EDGE_RISE)

/* Set by rts_irq() when the 480Z asserts RTS; read and cleared by take_reset() in the main loop, with
 * interrupts off. */
static volatile bool reset_pending;
static void (*idle_fn)(void); /* set by core 0 before serving; called only from lu_recv() on core 0 */
static bool irq_installed;

/* The PL011's divisors for the rates core/link.c uses, worked out at init by the SDK (uart_set_baudrate) */
typedef struct {
    uint32_t baud, ibrd, fbrd;
} divisors_t;
static divisors_t rates[] = {{LINK_BAUD_SLOW, 0, 0}, {LINK_BAUD_FAST, 0, 0}, {LINK_BAUD_HALF, 0, 0}};

/* The last exchanges, for link_uart_print_log(): what the 480Z and the unit did on the wire, to debug the link on
 * the real machine. An exchange runs from one reset (the 480Z's RTS) to the next. Written by core 0 only; read by
 * the console on core 1, which may see a record mid-update: it is only a diagnostic. */
typedef struct {
    uint32_t start_us, last_us; /* the reset that began it; the last byte in or out */
    uint32_t rx, rx_parity;     /* bytes from the 480Z, and how many had bad parity */
    uint32_t tx, tx_fast;       /* bytes to the 480Z, and how many of them at the fast rate */
    uint32_t fast_baud;         /* that rate */
    uint32_t dtr_waits;         /* times a byte had to wait for the 480Z's DTR */
    uint32_t dtr_wait_max_us;   /* the longest of those waits */
    uint32_t rx_overruns;       /* bytes the 480Z sent that were lost because the receive FIFO was full */
    uint8_t first_rx[4];        /* its first bytes from the 480Z: the command */
} exchange_t;

#define EXCHANGES 8u
static exchange_t exchanges[EXCHANGES];
static volatile unsigned exchange_now; /* the index of the current one */
static uint32_t sending_at;            /* the rate now: lu_set_baud(), core 0 */

/*
 * The link's last bytes, both ways, for link_uart_print_trace(): when each was taken or started, its error flags,
 * and for a byte received, the longest time core 0 was away from the UART while it waited for it with the 480Z's
 * CTS up. That is how long the 480Z could send unheard: more than the FIFO's 32 bytes' worth (2.6 ms at 125000)
 * loses bytes. Written by core 0 only; read by the console on core 1, which may see an entry mid-update.
 */
typedef struct {
    uint32_t t_us;
    uint16_t away_us; /* saturates */
    uint8_t byte;
    uint8_t flags;
} trace_t;

#define TRACE_TX      0x01u
#define TRACE_PE      0x02u /* parity error */
#define TRACE_FE      0x04u /* framing error */
#define TRACE_BE      0x08u /* break */
#define TRACE_OE      0x10u /* bytes were lost just before this one: the FIFO was full */
#define TRACE_RESET   0x20u /* not a byte: the 480Z's RTS reset */
#define TRACE_ENTRIES 512u

static trace_t trace[TRACE_ENTRIES];
static volatile uint32_t trace_count; /* entries ever written */

static void __not_in_flash_func(trace_add)(uint8_t byte, uint8_t flags, uint32_t away_us) {
    trace_t *t = &trace[trace_count % TRACE_ENTRIES];
    t->t_us = time_us_32();
    t->away_us = away_us > UINT16_MAX ? UINT16_MAX : (uint16_t)away_us;
    t->byte = byte;
    t->flags = flags;
    trace_count++;
}

static void __not_in_flash_func(exchange_begin)(void) {
    unsigned k = (exchange_now + 1u) % EXCHANGES;
    exchanges[k] = (exchange_t){0};
    exchanges[k].start_us = exchanges[k].last_us = time_us_32();
    exchange_now = k;
}

static inline void set_line(unsigned pin, bool asserted) {
    gpio_put(pin, LEVEL(asserted));
}

static inline bool line_asserted(unsigned pin) {
    return gpio_get(pin) == LINK_UART_ASSERTED_LEVEL;
}

/*
 * The boot block's pacing (link_set_paced): at most two bytes per DTR pulse, as the ROS takes them. Its receive
 * interrupt drops DTR at each byte, but can run a few hundred microseconds late, and a third or fourth byte started
 * meanwhile overruns its SIO's FIFO (core/link.h). A drop can be over in 37 us, so it is counted by an edge
 * interrupt, not looked for. If no drop comes within PACED_FALLBACK_US, the next byte goes anyway: the real ROS has
 * always dropped DTR within 400 us, but a 480Z that holds it up (a test rig) must still get its block.
 */
#define PACED_BYTES_PER_PULSE 2u    /* docs/protocol.md, "Flow control" */
#define PACED_FALLBACK_US     2000u /* five times the latest drop seen */

static volatile uint32_t dtr_drops; /* written by dtr_irq(), read by lu_send() on core 0 */
static bool paced;                  /* core 0 only: lu_set_paced(), lu_send() */
static unsigned paced_sent;         /* bytes sent since paced_drops was taken */
static uint32_t paced_drops, paced_from_us;

static void __not_in_flash_func(dtr_irq)(void) {
    if (!(gpio_get_irq_event_mask(LINK_UART_PIN_DTR_IN) & DEASSERT_EDGE)) return;
    gpio_acknowledge_irq(LINK_UART_PIN_DTR_IN, DEASSERT_EDGE);
    dtr_drops++;
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
        const divisors_t *r = NULL;
        for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++)
            if (rates[i].baud == baud) r = &rates[i];
        RMFE_ASSERT(r); /* only the rates core/link.c uses */
        hw->ibrd = r->ibrd;
        hw->fbrd = r->fbrd;
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
    trace_add(0, TRACE_RESET, 0);
    exchange_begin();
    reprogram(0, true);
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    while (!(hw->fr & UART_UARTFR_RXFE_BITS))
        (void)hw->dr; /* anything that came in meanwhile */
    return true;
}

static link_status_t __not_in_flash_func(lu_recv)(void *ctx, uint8_t *byte, bool *parity_error) {
    (void)ctx;
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    uint32_t last_poll = time_us_32(), away = 0;
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
        uint32_t now = time_us_32();
        if (now - last_poll > away) away = now - last_poll;
        if (!(hw->fr & UART_UARTFR_RXFE_BITS)) {
            set_line(LINK_UART_PIN_CTS_OUT, false);
            uint32_t dr = hw->dr;
            *byte = (uint8_t)(dr & UART_UARTDR_DATA_BITS);
            *parity_error = (dr & UART_UARTDR_PE_BITS) != 0;
            exchange_t *x = &exchanges[exchange_now];
            uint8_t flags =
                (uint8_t)((*parity_error ? TRACE_PE : 0u) | (dr & UART_UARTDR_FE_BITS ? TRACE_FE : 0u) |
                          (dr & UART_UARTDR_BE_BITS ? TRACE_BE : 0u) | (dr & UART_UARTDR_OE_BITS ? TRACE_OE : 0u));
            if (hw->rsr & UART_UARTRSR_OE_BITS) {
                flags |= TRACE_OE;
                hw->rsr = 0; /* clears it, so each overrun is seen once */
            }
            if (flags & TRACE_OE) x->rx_overruns++;
            trace_add(*byte, flags, away);
            if (x->rx < sizeof x->first_rx) x->first_rx[x->rx] = *byte;
            x->rx++;
            if (*parity_error) x->rx_parity++;
            x->last_us = time_us_32();
            return LINK_OK;
        }
        if (idle_fn) idle_fn();   /* with CTS up: a byte arriving meanwhile waits in the FIFO */
        last_poll = time_us_32(); /* the idle work is counted out: it runs only between commands */
    }
}

void link_uart_set_idle(void (*fn)(void)) {
    idle_fn = fn;
}

static link_status_t __not_in_flash_func(lu_send)(void *ctx, uint8_t byte) {
    (void)ctx;
    uart_hw_t *hw = uart_get_hw(LINK_UART);
    exchange_t *x = &exchanges[exchange_now];
    bool waited = false;
    uint32_t wait_from = 0;
    if (paced && paced_sent >= PACED_BYTES_PER_PULSE) {
        while (dtr_drops == paced_drops && time_us_32() - paced_from_us < PACED_FALLBACK_US)
            if (take_reset()) return LINK_RESET;
        paced_sent = 0;
    }
    for (;;) {
        if (take_reset()) return LINK_RESET;
        bool dtr = line_asserted(LINK_UART_PIN_DTR_IN);
        if (dtr && !(hw->fr & UART_UARTFR_BUSY_BITS)) {
            hw->dr = byte;
            trace_add(byte, TRACE_TX, 0);
            if (paced && paced_sent++ == 0) {
                paced_drops = dtr_drops;
                paced_from_us = time_us_32();
            }
            x->last_us = time_us_32();
            if (waited && x->last_us - wait_from > x->dtr_wait_max_us) x->dtr_wait_max_us = x->last_us - wait_from;
            x->tx++;
            if (sending_at != LINK_BAUD_SLOW) {
                x->tx_fast++;
                x->fast_baud = sending_at;
            }
            return LINK_OK;
        }
        if (!dtr && !waited) {
            waited = true;
            wait_from = time_us_32();
            x->dtr_waits++;
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
    reprogram(baud, false);
    sending_at = baud;
}

static void __not_in_flash_func(lu_set_paced)(void *ctx, bool on) {
    (void)ctx;
    paced = on;
    paced_sent = 0;
}

static const link_io_t io = {NULL, lu_recv, lu_send, lu_drain, lu_set_baud, lu_set_paced};

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
    sending_at = LINK_BAUD_SLOW;
    exchange_begin();

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

    /* The SDK works out the divisors; every rate is exact at 150 MHz (link_uart.h). The last is 9600, the
     * first: the link starts slow. */
    (void)uart_init(LINK_UART, LINK_BAUD_SLOW);
    for (unsigned i = sizeof rates / sizeof rates[0]; i-- > 0;) {
        (void)uart_set_baudrate(LINK_UART, rates[i].baud);
        rates[i].ibrd = uart_get_hw(LINK_UART)->ibrd;
        rates[i].fbrd = uart_get_hw(LINK_UART)->fbrd;
    }
    uart_set_format(LINK_UART, 8, 1, UART_PARITY_EVEN); /* 8E1 [IDC3 0123: WR4 47] */
    uart_set_hw_flow(LINK_UART, false, false);          /* the handshake lines are GPIOs */
    uart_set_fifo_enabled(LINK_UART, true);
    /* And anything that arrived while the UART was being set up is not a command. */
    while (uart_is_readable(LINK_UART))
        (void)uart_get_hw(LINK_UART)->dr;
    uart_get_hw(LINK_UART)->rsr = 0; /* clears the error flags */

    if (!irq_installed) {
        gpio_add_raw_irq_handler(LINK_UART_PIN_RTS_IN, rts_irq);
        gpio_add_raw_irq_handler(LINK_UART_PIN_DTR_IN, dtr_irq);
        irq_set_enabled(IO_IRQ_BANK0, true);
        irq_installed = true;
    }
    gpio_acknowledge_irq(LINK_UART_PIN_RTS_IN, ASSERT_EDGE);
    gpio_set_irq_enabled(LINK_UART_PIN_RTS_IN, ASSERT_EDGE, true);
    paced = false;
    paced_sent = 0;
    gpio_acknowledge_irq(LINK_UART_PIN_DTR_IN, DEASSERT_EDGE);
    gpio_set_irq_enabled(LINK_UART_PIN_DTR_IN, DEASSERT_EDGE, true);
}

void link_uart_print_log(void) {
    uint32_t now = time_us_32();
    unsigned newest = exchange_now;
    for (unsigned i = 1; i <= EXCHANGES; i++) {
        const exchange_t *x = &exchanges[(newest + i) % EXCHANGES];
        if (!x->start_us) continue; /* never used */
        bool current = (newest + i) % EXCHANGES == newest;
        uint32_t end = current ? now : exchanges[(newest + i + 1u) % EXCHANGES].start_us;
        printf("LINK %s: in %lu (%lu bad parity, %lu overruns) [", current ? "now " : "past", (unsigned long)x->rx,
               (unsigned long)x->rx_parity, (unsigned long)x->rx_overruns);
        for (unsigned k = 0; k < x->rx && k < sizeof x->first_rx; k++)
            printf(k ? " %02X" : "%02X", x->first_rx[k]);
        printf("] out %lu (%lu fast, at %lu), DTR waits %lu (longest %lu us), lasted %lu ms, quiet %lu ms at the %s\n",
               (unsigned long)x->tx, (unsigned long)x->tx_fast, (unsigned long)x->fast_baud,
               (unsigned long)x->dtr_waits, (unsigned long)x->dtr_wait_max_us,
               (unsigned long)((end - x->start_us) / 1000u), (unsigned long)((end - x->last_us) / 1000u),
               current ? "moment" : "end");
    }
}

void link_uart_print_trace(void) {
    uint32_t end = trace_count;
    uint32_t first = end > TRACE_ENTRIES ? end - TRACE_ENTRIES : 0u;
    printf("TRACE: the link's last %lu bytes (< from the 480Z, > to it); +us since the one before; away: the longest "
           "time core 0 was away from the UART, with CTS up, while it waited for that byte\n",
           (unsigned long)(end - first));
    uint32_t prev = 0;
    for (uint32_t i = first; i < end; i++) {
        const trace_t *t = &trace[i % TRACE_ENTRIES];
        uint32_t dt = i == first ? 0u : t->t_us - prev;
        prev = t->t_us;
        if (t->flags & TRACE_RESET) {
            printf("%5lu +%8lu RESET\n", (unsigned long)i, (unsigned long)dt);
        } else if (t->flags & TRACE_TX) {
            printf("%5lu +%8lu >%02X\n", (unsigned long)i, (unsigned long)dt, t->byte);
        } else {
            printf("%5lu +%8lu <%02X away %5u%s%s%s%s\n", (unsigned long)i, (unsigned long)dt, t->byte, t->away_us,
                   t->flags & TRACE_OE ? " OVERRUN" : "", t->flags & TRACE_PE ? " PARITY" : "",
                   t->flags & TRACE_FE ? " FRAMING" : "", t->flags & TRACE_BE ? " BREAK" : "");
        }
    }
    printf("TRACE: end; now %lu us after the last\n", (unsigned long)(time_us_32() - prev));
}
