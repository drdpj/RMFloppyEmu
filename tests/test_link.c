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

#include <string.h>

#include "support/fake480z.h"
#include "unity.h"

static fake480z_t f;
static link_t l;
static fake_in_t script[1024];
static uint8_t rec[LINK_RECORD], got[LINK_RECORD];

void setUp(void) {
    for (unsigned i = 0; i < LINK_RECORD; i++)
        rec[i] = (uint8_t)(i * 3u + 7u);
}
void tearDown(void) {}

static void start(size_t n) {
    fake480z_init(&f, script, n);
    link_init(&l, &f.io);
}

static void test_link_starts_at_9600(void) {
    start(0);
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, f.baud);
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, l.baud);
}

static void test_go_fast_replies_5a_00_01_drains_then_switches(void) {
    start(0);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x01};
    TEST_ASSERT_EQUAL_size_t(3, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_size_t(1, f.drains);
    TEST_ASSERT_EQUAL_size_t(2, f.nbauds); /* init, then fast */
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_FAST, f.bauds[1].baud);
    TEST_ASSERT_EQUAL_size_t(3, f.bauds[1].out_at); /* only after the whole reply */
}

/* The 480Z loads the reply's third byte into CTC0 as its time constant: 2 MHz / 16 / 2 = 62500 baud
 * [480Z ROS 1.2 1f11-1f17 -> 3cae; docs/protocol.md, "Framing and baud rate"]. */
static void test_go_fast_at_62500_replies_5a_00_02_then_switches_to_62500(void) {
    start(0);
    link_set_fast_baud(&l, 62500u);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x02};
    TEST_ASSERT_EQUAL_size_t(3, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_size_t(2, f.nbauds);
    TEST_ASSERT_EQUAL_UINT32(62500u, f.bauds[1].baud);
    TEST_ASSERT_EQUAL_size_t(3, f.bauds[1].out_at);
    TEST_ASSERT_EQUAL_UINT32(62500u, l.baud);
}

static void test_the_fast_rate_survives_a_reset(void) {
    script[0] = (fake_in_t)FRESET;
    start(1);
    link_set_fast_baud(&l, 62500u);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_RESET, link_get(&l, &b));
    TEST_ASSERT_EQUAL_UINT32(9600u, f.baud);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x02, 0x5A, 0x00, 0x02};
    TEST_ASSERT_EQUAL_size_t(6, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 6);
    TEST_ASSERT_EQUAL_UINT32(62500u, f.baud);
}

static void test_the_fast_rate_can_be_set_back_to_125000(void) {
    start(0);
    link_set_fast_baud(&l, 62500u);
    link_set_fast_baud(&l, 125000u);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_UINT32(125000u, f.baud);
}

/* The 480Z's ROS sends its 00 at 8N2 (WR4 4C) [480Z ROS 1.2 1cb5]: to an 8E1 receiver it has bad parity. */
static void test_a_00_with_bad_parity_goes_fast_at_the_boot_rate(void) {
    script[0] = (fake_in_t)FP(0x00);
    start(1);
    link_set_boot_baud(&l, 62500u);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x02};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_UINT32(62500u, f.baud);
}

static void test_a_clean_00_goes_fast_at_the_fast_rate_whatever_the_boot_rate(void) {
    script[0] = (fake_in_t)FB(0x00);
    start(1);
    link_set_boot_baud(&l, 62500u);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_UINT32(125000u, f.baud);
}

static void test_only_the_last_byte_s_parity_counts(void) {
    script[0] = (fake_in_t)FP(0x00);
    script[1] = (fake_in_t)FB(0x00);
    start(2);
    link_set_boot_baud(&l, 62500u);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    TEST_ASSERT_EQUAL_HEX8(0x01, f.out[2]);
    TEST_ASSERT_EQUAL_UINT32(125000u, f.baud);
}

/* Without link_set_boot_baud the link is the IDC's: 5A 00 01 whatever the parity [IDC3 0a1f, 0b07]. */
static void test_by_default_a_00_with_bad_parity_goes_fast_at_125000(void) {
    script[0] = (fake_in_t)FP(0x00);
    start(1);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    const uint8_t want[] = {0x5A, 0x00, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_UINT32(125000u, f.baud);
}

static void test_reset_drops_back_to_9600_and_is_reported(void) {
    script[0] = (fake_in_t)FRESET;
    start(1);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_RESET, link_get(&l, &b));
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, f.baud);
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, l.baud);
}

static void test_reset_at_9600_sets_no_new_baud(void) {
    script[0] = (fake_in_t)FRESET;
    start(1);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_RESET, link_get(&l, &b));
    TEST_ASSERT_EQUAL_size_t(1, f.nbauds);
}

static void test_get_ignores_parity_errors_on_single_bytes(void) {
    script[0] = (fake_in_t)FP(0x42);
    start(1);
    uint8_t b = 0;
    TEST_ASSERT_EQUAL(LINK_OK, link_get(&l, &b));
    TEST_ASSERT_EQUAL_HEX8(0x42, b);
}

static void test_end_of_input_is_closed(void) {
    start(0);
    uint8_t b;
    TEST_ASSERT_EQUAL(LINK_CLOSED, link_get(&l, &b));
}

static void test_send_record_accepted_first_time(void) {
    script[0] = (fake_in_t)FB(0x00);
    start(1);
    TEST_ASSERT_EQUAL(LINK_OK, link_send_record(&l, rec));
    TEST_ASSERT_EQUAL_size_t(LINK_RECORD, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rec, f.out, LINK_RECORD);
}

static void test_send_record_repeats_until_acknowledged_with_00(void) {
    script[0] = (fake_in_t)FB(0xFF);
    script[1] = (fake_in_t)FB(0x01);
    script[2] = (fake_in_t)FB(0x00);
    start(3);
    TEST_ASSERT_EQUAL(LINK_OK, link_send_record(&l, rec));
    TEST_ASSERT_EQUAL_size_t(3 * LINK_RECORD, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rec, f.out + 2 * LINK_RECORD, LINK_RECORD);
}

static void test_recv_record_clean_is_acknowledged_00(void) {
    size_t n = fake_bytes(script, 0, rec, LINK_RECORD);
    start(n);
    TEST_ASSERT_EQUAL(LINK_OK, link_recv_record(&l, got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rec, got, LINK_RECORD);
    TEST_ASSERT_EQUAL_size_t(1, f.nout);
    TEST_ASSERT_EQUAL_HEX8(LINK_BLOCK_OK, f.out[0]);
}

static void test_recv_record_with_a_parity_error_asks_again_with_ff(void) {
    uint8_t bad[LINK_RECORD];
    memset(bad, 0xEE, sizeof bad);
    size_t n = fake_bytes(script, 0, bad, LINK_RECORD);
    script[57] = (fake_in_t)FP(0xEE); /* one byte with a parity error */
    n = fake_bytes(script, n, rec, LINK_RECORD);
    start(n);
    TEST_ASSERT_EQUAL(LINK_OK, link_recv_record(&l, got));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rec, got, LINK_RECORD); /* the second, clean copy */
    const uint8_t want[] = {LINK_BLOCK_RETRY, LINK_BLOCK_OK};
    TEST_ASSERT_EQUAL_size_t(2, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 2);
}

static void test_reset_in_the_middle_of_a_record_abandons_it(void) {
    size_t n = fake_bytes(script, 0, rec, 40);
    script[n++] = (fake_in_t)FRESET;
    start(n);
    TEST_ASSERT_EQUAL(LINK_OK, link_go_fast(&l));
    f.nout = 0;
    TEST_ASSERT_EQUAL(LINK_RESET, link_recv_record(&l, got));
    TEST_ASSERT_EQUAL_size_t(0, f.nout); /* no acknowledgement sent */
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, f.baud);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_link_starts_at_9600);
    RUN_TEST(test_go_fast_replies_5a_00_01_drains_then_switches);
    RUN_TEST(test_go_fast_at_62500_replies_5a_00_02_then_switches_to_62500);
    RUN_TEST(test_the_fast_rate_survives_a_reset);
    RUN_TEST(test_the_fast_rate_can_be_set_back_to_125000);
    RUN_TEST(test_a_00_with_bad_parity_goes_fast_at_the_boot_rate);
    RUN_TEST(test_a_clean_00_goes_fast_at_the_fast_rate_whatever_the_boot_rate);
    RUN_TEST(test_only_the_last_byte_s_parity_counts);
    RUN_TEST(test_by_default_a_00_with_bad_parity_goes_fast_at_125000);
    RUN_TEST(test_reset_drops_back_to_9600_and_is_reported);
    RUN_TEST(test_reset_at_9600_sets_no_new_baud);
    RUN_TEST(test_get_ignores_parity_errors_on_single_bytes);
    RUN_TEST(test_end_of_input_is_closed);
    RUN_TEST(test_send_record_accepted_first_time);
    RUN_TEST(test_send_record_repeats_until_acknowledged_with_00);
    RUN_TEST(test_recv_record_clean_is_acknowledged_00);
    RUN_TEST(test_recv_record_with_a_parity_error_asks_again_with_ff);
    RUN_TEST(test_reset_in_the_middle_of_a_record_abandons_it);
    return UNITY_END();
}
