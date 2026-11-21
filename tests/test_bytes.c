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
#include "core/bytes.h"

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_get_le16_reads_low_byte_first(void) {
    const uint8_t b[] = {0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX16(0x1234, bytes_get_le16(b));
}

static void test_get_le16_high_bit_does_not_sign_extend(void) {
    const uint8_t b[] = {0xff, 0xff};
    TEST_ASSERT_EQUAL_HEX16(0xffff, bytes_get_le16(b));
}

static void test_get_le32_reads_low_byte_first(void) {
    const uint8_t b[] = {0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX32(0x12345678, bytes_get_le32(b));
}

static void test_get_le32_top_byte_above_0x7f(void) {
    /* (p[3] << 24) on a promoted int would overflow; the cast must come first. */
    const uint8_t b[] = {0x00, 0x00, 0x00, 0x80};
    TEST_ASSERT_EQUAL_HEX32(0x80000000u, bytes_get_le32(b));
}

static void test_get_works_on_unaligned_pointer(void) {
    const uint8_t b[] = {0xaa, 0x78, 0x56, 0x34, 0x12};
    TEST_ASSERT_EQUAL_HEX32(0x12345678, bytes_get_le32(b + 1));
}

static void test_put_le16_round_trips(void) {
    uint8_t b[2];
    bytes_put_le16(b, 0xbeef);
    TEST_ASSERT_EQUAL_HEX8(0xef, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0xbe, b[1]);
    TEST_ASSERT_EQUAL_HEX16(0xbeef, bytes_get_le16(b));
}

static void test_put_le32_writes_exactly_four_bytes(void) {
    uint8_t b[6] = {0x11, 0, 0, 0, 0, 0x22};
    bytes_put_le32(b + 1, 0xdeadbeef);
    const uint8_t want[6] = {0x11, 0xef, 0xbe, 0xad, 0xde, 0x22};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, b, 6);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_get_le16_reads_low_byte_first);
    RUN_TEST(test_get_le16_high_bit_does_not_sign_extend);
    RUN_TEST(test_get_le32_reads_low_byte_first);
    RUN_TEST(test_get_le32_top_byte_above_0x7f);
    RUN_TEST(test_get_works_on_unaligned_pointer);
    RUN_TEST(test_put_le16_round_trips);
    RUN_TEST(test_put_le32_writes_exactly_four_bytes);
    return UNITY_END();
}
