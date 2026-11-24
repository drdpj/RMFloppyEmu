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
 * On the Pico 2 only (tests/pico/): the 480Z link driver, pico/link_uart.c, with no wiring. The UART runs in
 * the PL011's internal loopback (UARTCR.LBE), so what we send comes back to us, and the test plays the 480Z's
 * handshake lines by driving the RTS and DTR input pins as outputs: the pad still feeds the input path, so
 * the edge interrupt fires as it would from the transceiver. Timed events (a byte arriving, RTS rising) come
 * from alarms while the driver waits.
 *
 * What this can't check, left to the hardware-in-the-loop tests (docs/testing.md, level 3): parity errors
 * (loopback sends and checks with the same settings), the real pins' levels through the transceiver, and the
 * timing against a real far end.
 */
#include "pico/link_uart.h"

#include <stdint.h>

#include "core/link.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/time.h"
#include "unity.h"

#define CHAR_BITS 11u /* start, 8 data, parity, stop */

static const link_io_t *io;

/* The 480Z's side of a handshake line we play: drive the input pin to the level for asserted or not. */
static void drive_480z(unsigned pin, bool asserted) {
    gpio_put(pin, asserted ? LINK_UART_ASSERTED_LEVEL : !LINK_UART_ASSERTED_LEVEL);
    gpio_set_dir(pin, GPIO_OUT);
}

static bool our_line_asserted(unsigned pin) {
    return gpio_get(pin) == LINK_UART_ASSERTED_LEVEL;
}

static uint32_t actual_baud(void) {
    const uart_hw_t *hw = uart_get_hw(uart1);
    return (uint32_t)((4ull * clock_get_hz(clk_peri)) / (64u * hw->ibrd + hw->fbrd)); /* RP2350 datasheet */
}

void setUp(void) {
    link_uart_init();
    io = link_uart_io();
    io->set_baud(io->ctx, LINK_BAUD_SLOW);
    uart_get_hw(uart1)->cr |= UART_UARTCR_LBE_BITS;
    drive_480z(LINK_UART_PIN_RTS_IN, false);
    drive_480z(LINK_UART_PIN_DTR_IN, true);
}

void tearDown(void) {
    uart_get_hw(uart1)->cr &= ~UART_UARTCR_LBE_BITS;
    gpio_init(LINK_UART_PIN_TX); /* as at power-up: not yet the UART's, and the RX pad's input off */
    gpio_init(LINK_UART_PIN_RX);
    hw_clear_bits(&pads_bank0_hw->io[LINK_UART_PIN_RX], PADS_BANK0_GPIO0_IE_BITS);
    link_uart_init(); /* inputs again, pulled up */
}

/* --- alarms: what the 480Z does while the driver waits ------------------------------------------- */

static volatile bool cts_seen;      /* our CTS, as the alarm found it */
static volatile uint8_t alarm_byte; /* sent into the loopback by the alarm */

static int64_t send_byte_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    cts_seen = our_line_asserted(LINK_UART_PIN_CTS_OUT);
    uart_get_hw(uart1)->dr = alarm_byte;
    return 0;
}

static int64_t raise_rts_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    cts_seen = our_line_asserted(LINK_UART_PIN_CTS_OUT);
    drive_480z(LINK_UART_PIN_RTS_IN, true);
    return 0;
}

static int64_t raise_dtr_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    drive_480z(LINK_UART_PIN_DTR_IN, true);
    return 0;
}

static void after_us(uint32_t us, alarm_callback_t cb) {
    TEST_ASSERT_TRUE(add_alarm_in_us(us, cb, NULL, true) > 0);
}

/* recv() with a byte arriving from the 480Z after a delay */
static link_status_t recv_with_byte_after(uint32_t us, uint8_t b, uint8_t *got, bool *pe) {
    alarm_byte = b;
    cts_seen = false;
    after_us(us, send_byte_alarm);
    return io->recv(io->ctx, got, pe);
}

/* --- framing and baud -------------------------------------------------------------------------------- */

static void test_the_link_is_8e1_with_fifos(void) {
    uint32_t lcr = uart_get_hw(uart1)->lcr_h;
    TEST_ASSERT_EQUAL_HEX32(3u << UART_UARTLCR_H_WLEN_LSB, lcr & UART_UARTLCR_H_WLEN_BITS); /* 8 bits */
    TEST_ASSERT_TRUE(lcr & UART_UARTLCR_H_PEN_BITS);                                        /* parity */
    TEST_ASSERT_TRUE(lcr & UART_UARTLCR_H_EPS_BITS);                                        /* even */
    TEST_ASSERT_FALSE(lcr & UART_UARTLCR_H_STP2_BITS);                                      /* 1 stop bit */
    TEST_ASSERT_FALSE(lcr & UART_UARTLCR_H_SPS_BITS);                                       /* not stick parity */
    TEST_ASSERT_TRUE(lcr & UART_UARTLCR_H_FEN_BITS);
    TEST_ASSERT_FALSE(uart_get_hw(uart1)->cr & (UART_UARTCR_CTSEN_BITS | UART_UARTCR_RTSEN_BITS));
}

static void test_init_is_9600_baud(void) {
    link_uart_init();
    TEST_ASSERT_UINT32_WITHIN(10, 9600, actual_baud()); /* the IDC's is 9615 (docs/protocol.md) */
}

static void test_fast_is_exactly_125000_baud(void) {
    io->set_baud(io->ctx, LINK_BAUD_FAST);
    TEST_ASSERT_EQUAL_UINT32(125000, actual_baud());
}

/* fast-baud = 62500: 2 MHz / 16 / 2, the 480Z's rate for a go-fast reply of 5A 00 02 (core/link.h) */
static void test_half_is_exactly_62500_baud(void) {
    io->set_baud(io->ctx, LINK_BAUD_HALF);
    TEST_ASSERT_EQUAL_UINT32(62500, actual_baud());
}

static void test_dcd_is_held_asserted_and_cts_starts_deasserted(void) {
    TEST_ASSERT_TRUE(our_line_asserted(LINK_UART_PIN_DCD_OUT));
    TEST_ASSERT_FALSE(our_line_asserted(LINK_UART_PIN_CTS_OUT));
}

static void test_undriven_inputs_read_idle(void) {
    /* With no 480Z (or no transceiver): RX must idle at mark, not float low into a stream of breaks read as
     * 00 (go fast), which the unit then tries to answer; RTS and DTR must read deasserted. */
    link_uart_init();
    TEST_ASSERT_TRUE(gpio_is_pulled_up(LINK_UART_PIN_RX));
    TEST_ASSERT_TRUE(gpio_is_pulled_up(LINK_UART_PIN_RTS_IN));
    TEST_ASSERT_TRUE(gpio_is_pulled_up(LINK_UART_PIN_DTR_IN));
}

/* --- receiving ----------------------------------------------------------------------------------------- */

static void test_cts_is_asserted_only_while_recv_waits(void) {
    uint8_t b = 0;
    bool pe = true;
    TEST_ASSERT_EQUAL(LINK_OK, recv_with_byte_after(3000, 0xA5, &b, &pe));
    TEST_ASSERT_TRUE(cts_seen); /* while waiting */
    TEST_ASSERT_EQUAL_HEX8(0xA5, b);
    TEST_ASSERT_FALSE(pe);
    TEST_ASSERT_FALSE(our_line_asserted(LINK_UART_PIN_CTS_OUT)); /* and not after */
}

static void round_trip_every_byte_value(uint32_t baud) {
    io->set_baud(io->ctx, baud);
    for (unsigned v = 0; v < 256; v++) {
        uint8_t b;
        bool pe = true;
        TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, (uint8_t)v));
        TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe));
        TEST_ASSERT_EQUAL_HEX8(v, b);
        TEST_ASSERT_FALSE(pe);
    }
}

static void test_every_byte_value_round_trips_at_9600(void) {
    round_trip_every_byte_value(LINK_BAUD_SLOW);
}

static void test_every_byte_value_round_trips_at_125000(void) {
    round_trip_every_byte_value(LINK_BAUD_FAST);
}

static void test_every_byte_value_round_trips_at_62500(void) {
    round_trip_every_byte_value(LINK_BAUD_HALF);
}

/* --- sending -------------------------------------------------------------------------------------------- */

static void test_send_waits_for_the_480zs_dtr(void) {
    drive_480z(LINK_UART_PIN_DTR_IN, false);
    uint64_t t0 = time_us_64(); /* before the alarm is set, which then fires at least 5 ms later */
    after_us(5000, raise_dtr_alarm);
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0x3C));
    TEST_ASSERT_TRUE(time_us_64() - t0 >= 5000);
    uint8_t b;
    bool pe;
    TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0x3C, b);
}

static volatile unsigned landed_before_dtr; /* bytes the loopback had received when the alarm raised DTR */

static int64_t count_then_raise_dtr_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    unsigned n = 0;
    while (uart_is_readable(uart1)) {
        (void)uart_get_hw(uart1)->dr;
        n++;
    }
    landed_before_dtr = n;
    drive_480z(LINK_UART_PIN_DTR_IN, true);
    return 0;
}

/* The IDC's SIO checks CTS (the 480Z's DTR) as each character starts [IDC3 012c: WR3 E1, auto enables]: once the
 * 480Z drops DTR, the character being sent finishes and the next waits, even if the unit has already handed it
 * over. During the boot block the ROS takes two bytes per DTR pulse (docs/protocol.md, "Flow control"). At 9600 a
 * character takes 1.15 ms, so the second send() comes while the first is still on the wire. */
static void test_a_byte_handed_over_while_dtr_is_down_waits_for_dtr(void) {
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0xA1));
    drive_480z(LINK_UART_PIN_DTR_IN, false);
    landed_before_dtr = 99;
    after_us(5000, count_then_raise_dtr_alarm);
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0xB2));
    TEST_ASSERT_EQUAL_UINT(1, landed_before_dtr); /* 0xA1 only */
    uint8_t b;
    bool pe;
    TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0xB2, b);
}

/* --- the boot block's pacing (link_set_paced): two bytes per DTR pulse ------------------------------- */

#define PACED_FALLBACK_US 2000u /* link_uart.c: a third byte goes anyway this long after the first */

static int64_t drop_dtr_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    drive_480z(LINK_UART_PIN_DTR_IN, false);
    return 0;
}

/* Sends three bytes paced at 125000, DTR up throughout, or dropped for 40 us 500 us in (pulse); returns the
 * microseconds from just before the alarms are set to the third send()'s return. */
static uint32_t send_three_paced(bool pulse) {
    io->set_baud(io->ctx, LINK_BAUD_FAST);
    io->set_paced(io->ctx, true);
    uint64_t t0 = time_us_64(); /* before the alarms are set, which then fire at least that long after it */
    if (pulse) {
        after_us(500, drop_dtr_alarm);
        after_us(540, raise_dtr_alarm);
    }
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0x11));
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0x22));
    TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, 0x33));
    uint32_t us = (uint32_t)(time_us_64() - t0);
    io->set_paced(io->ctx, false);
    TEST_ASSERT_EQUAL(LINK_OK, io->drain(io->ctx));
    static const uint8_t want[] = {0x11, 0x22, 0x33};
    for (unsigned i = 0; i < sizeof want; i++) {
        uint8_t b;
        bool pe;
        TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe));
        TEST_ASSERT_EQUAL_HEX8(want[i], b);
    }
    return us;
}

/* The ROS's receive interrupt drops DTR at each byte, sometimes hundreds of microseconds late; a third byte
 * started before then overruns its SIO (core/link.h). Its shortest drop on a real 480Z was 37 us: one of 40 us,
 * 500 us in, must be seen, and the third byte go when DTR is back. */
static void test_paced_a_third_byte_waits_for_a_short_dtr_drop(void) {
    uint32_t us = send_three_paced(true);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(540u, us);
    TEST_ASSERT_LESS_THAN_UINT32(PACED_FALLBACK_US, us);
}

/* A 480Z that holds DTR up (a test rig) still gets every byte, the third after the fallback. */
static void test_paced_with_dtr_held_up_a_third_byte_goes_after_the_fallback(void) {
    uint32_t us = send_three_paced(false);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(PACED_FALLBACK_US, us);
    TEST_ASSERT_LESS_THAN_UINT32(PACED_FALLBACK_US + 500u, us);
}

static void test_a_record_at_125000_goes_back_to_back(void) {
    io->set_baud(io->ctx, LINK_BAUD_FAST);
    uint64_t t0 = time_us_64();
    for (unsigned i = 0; i < LINK_RECORD; i++)
        TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, (uint8_t)i));
    TEST_ASSERT_EQUAL(LINK_OK, io->drain(io->ctx));
    uint32_t us = (uint32_t)(time_us_64() - t0);
    uint32_t want = LINK_RECORD * CHAR_BITS * 1000000u / LINK_BAUD_FAST; /* 11264 us */
    TEST_ASSERT_UINT32_WITHIN(want / 100u, want, us);                    /* within 1% */
}

static void test_go_fast_sends_5a_00_01_at_9600_then_switches(void) {
    link_t l;
    link_init(&l, io);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    TEST_ASSERT_EQUAL_UINT32(125000, actual_baud());
    const uint8_t want[] = {LINK_FAST_REPLY_0, LINK_FAST_REPLY_1, LINK_FAST_REPLY_2};
    for (unsigned i = 0; i < sizeof want; i++) {
        uint8_t b;
        bool pe = true;
        TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe)); /* came back at 9600, before the switch */
        TEST_ASSERT_EQUAL_HEX8(want[i], b);
        TEST_ASSERT_FALSE(pe);
    }
}

/* --- RTS: the reset ------------------------------------------------------------------------------------- */

static void test_rts_while_recv_waits_is_one_reset_and_drops_cts(void) {
    after_us(3000, raise_rts_alarm);
    uint8_t b;
    bool pe;
    TEST_ASSERT_EQUAL(LINK_RESET, io->recv(io->ctx, &b, &pe));
    TEST_ASSERT_TRUE(cts_seen);
    TEST_ASSERT_FALSE(our_line_asserted(LINK_UART_PIN_CTS_OUT));
    /* Only once: the next call waits for a byte as usual. RTS stays up through the exchange. */
    TEST_ASSERT_EQUAL(LINK_OK, recv_with_byte_after(3000, 0x00, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0x00, b);
}

static void test_rts_dropping_is_not_a_reset(void) {
    drive_480z(LINK_UART_PIN_RTS_IN, true);
    uint8_t b;
    bool pe;
    TEST_ASSERT_EQUAL(LINK_RESET, io->recv(io->ctx, &b, &pe)); /* the rise */
    drive_480z(LINK_UART_PIN_RTS_IN, false);
    TEST_ASSERT_EQUAL(LINK_OK, recv_with_byte_after(3000, 0x77, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0x77, b);
}

static void test_a_reset_discards_bytes_already_received(void) {
    for (unsigned i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL(LINK_OK, io->send(io->ctx, (uint8_t)(0xE0 + i)));
    TEST_ASSERT_EQUAL(LINK_OK, io->drain(io->ctx)); /* all four are in the RX FIFO now */
    drive_480z(LINK_UART_PIN_RTS_IN, true);
    uint8_t b;
    bool pe;
    TEST_ASSERT_EQUAL(LINK_RESET, io->recv(io->ctx, &b, &pe));
    TEST_ASSERT_EQUAL(LINK_OK, recv_with_byte_after(3000, 0x00, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0x00, b); /* the new exchange's byte, not E0 */
}

static void test_a_reset_while_sending_discards_what_is_queued(void) {
    /* Bytes queued at 9600 (about 1.1 ms each): RTS rises one byte in. At most the byte being shifted out
     * and the one queued behind it get through, as the PL011 can't abort a character. */
    drive_480z(LINK_UART_PIN_RTS_IN, false);
    after_us(1500, raise_rts_alarm);
    unsigned sent = 0;
    link_status_t st = LINK_OK;
    while (st == LINK_OK && sent < 32) {
        st = io->send(io->ctx, (uint8_t)(0x80 + sent));
        if (st == LINK_OK) sent++;
    }
    TEST_ASSERT_EQUAL(LINK_RESET, st);
    TEST_ASSERT_TRUE(sent >= 2 && sent <= 4);
    busy_wait_us(5000); /* anything still in flight lands */
    unsigned landed = 0;
    while (uart_is_readable(uart1)) {
        (void)uart_get_hw(uart1)->dr;
        landed++;
    }
    TEST_ASSERT_TRUE_MESSAGE(landed <= 1, "more than the byte in the shifter got through the reset");
}

static void test_rts_while_send_waits_for_dtr_is_a_reset(void) {
    drive_480z(LINK_UART_PIN_DTR_IN, false);
    after_us(3000, raise_rts_alarm);
    TEST_ASSERT_EQUAL(LINK_RESET, io->send(io->ctx, 0x11));
}

static void test_the_link_layer_returns_to_9600_on_a_reset(void) {
    link_t l;
    link_init(&l, io);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    for (unsigned i = 0; i < 3; i++) {
        uint8_t b;
        bool pe;
        TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe)); /* 5A 00 01 back from the loopback */
    }
    after_us(3000, raise_rts_alarm);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_RESET, link_get(&l, &b));
    TEST_ASSERT_UINT32_WITHIN(10, 9600, actual_baud());
}

static volatile uint64_t rts_at; /* when the alarm raised RTS */

static int64_t raise_rts_timed_alarm(alarm_id_t id, void *user) {
    (void)id;
    (void)user;
    rts_at = time_us_64();
    drive_480z(LINK_UART_PIN_RTS_IN, true);
    return 0;
}

static void test_a_reset_is_back_at_9600_within_microseconds(void) {
    /* The IDC's NMI reloads its CTC at once [IDC3 0066 -> 010b]. The SDK's baud and FIFO calls leave the
     * receiver off for 15 bit times (1.6 ms at 9600), long enough to garble a 00 the 480Z sends soon after RTS:
     * the level-3 tests caught that. */
    link_t l;
    link_init(&l, io);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    for (unsigned i = 0; i < 3; i++) {
        uint8_t b;
        bool pe;
        TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe)); /* 5A 00 01 back from the loopback */
    }
    after_us(3000, raise_rts_timed_alarm);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_RESET, link_get(&l, &b));
    uint32_t us = (uint32_t)(time_us_64() - rts_at);
    TEST_ASSERT_UINT32_WITHIN(10, 9600, actual_baud());
    TEST_ASSERT_LESS_THAN_UINT32(50, us);
    uart_get_hw(uart1)->dr = 0x00; /* the 480Z's 00, sent straight away */
    bool pe = true;
    TEST_ASSERT_EQUAL(LINK_OK, io->recv(io->ctx, &b, &pe));
    TEST_ASSERT_EQUAL_HEX8(0x00, b);
    TEST_ASSERT_FALSE(pe);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_the_link_is_8e1_with_fifos);
    RUN_TEST(test_init_is_9600_baud);
    RUN_TEST(test_fast_is_exactly_125000_baud);
    RUN_TEST(test_half_is_exactly_62500_baud);
    RUN_TEST(test_dcd_is_held_asserted_and_cts_starts_deasserted);
    RUN_TEST(test_undriven_inputs_read_idle);
    RUN_TEST(test_cts_is_asserted_only_while_recv_waits);
    RUN_TEST(test_every_byte_value_round_trips_at_9600);
    RUN_TEST(test_every_byte_value_round_trips_at_125000);
    RUN_TEST(test_every_byte_value_round_trips_at_62500);
    RUN_TEST(test_send_waits_for_the_480zs_dtr);
    RUN_TEST(test_a_byte_handed_over_while_dtr_is_down_waits_for_dtr);
    RUN_TEST(test_paced_a_third_byte_waits_for_a_short_dtr_drop);
    RUN_TEST(test_paced_with_dtr_held_up_a_third_byte_goes_after_the_fallback);
    RUN_TEST(test_a_record_at_125000_goes_back_to_back);
    RUN_TEST(test_go_fast_sends_5a_00_01_at_9600_then_switches);
    RUN_TEST(test_rts_while_recv_waits_is_one_reset_and_drops_cts);
    RUN_TEST(test_rts_dropping_is_not_a_reset);
    RUN_TEST(test_a_reset_discards_bytes_already_received);
    RUN_TEST(test_a_reset_while_sending_discards_what_is_queued);
    RUN_TEST(test_rts_while_send_waits_for_dtr_is_a_reset);
    RUN_TEST(test_the_link_layer_returns_to_9600_on_a_reset);
    RUN_TEST(test_a_reset_is_back_at_9600_within_microseconds);
    return UNITY_END();
}
