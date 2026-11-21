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
#include "flux/crc16.h"

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_check_value_of_123456789_is_29b1(void) {
    /* The catalogue check value for CRC-16/CCITT-FALSE (aka CRC-16/IBM-3740). */
    const uint8_t s[] = "123456789";
    TEST_ASSERT_EQUAL_HEX16(0x29B1, flux_crc16(FLUX_CRC16_INIT, s, 9));
}

static void test_three_a1_sync_bytes_give_cdb4(void) {
    /* The well-known MFM preset after A1 A1 A1 (WD279x/uPD765 documentation). */
    const uint8_t a1[] = {0xA1, 0xA1, 0xA1};
    TEST_ASSERT_EQUAL_HEX16(0xCDB4, flux_crc16(FLUX_CRC16_INIT, a1, 3));
}

static void test_field_with_its_crc_appended_gives_zero(void) {
    uint8_t f[7] = {0xFE, 0x05, 0x01, 0x09, 0x02, 0, 0};
    uint16_t crc = flux_crc16(FLUX_CRC16_INIT, f, 5);
    f[5] = (uint8_t)(crc >> 8);
    f[6] = (uint8_t)crc;
    TEST_ASSERT_EQUAL_HEX16(0, flux_crc16(FLUX_CRC16_INIT, f, 7));
}

static void test_byte_and_block_forms_agree(void) {
    const uint8_t s[] = {0x00, 0xFF, 0x4E, 0xA1, 0x80};
    uint16_t a = FLUX_CRC16_INIT;
    for (unsigned i = 0; i < sizeof s; i++) a = flux_crc16_byte(a, s[i]);
    TEST_ASSERT_EQUAL_HEX16(flux_crc16(FLUX_CRC16_INIT, s, sizeof s), a);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_check_value_of_123456789_is_29b1);
    RUN_TEST(test_three_a1_sync_bytes_give_cdb4);
    RUN_TEST(test_field_with_its_crc_appended_gives_zero);
    RUN_TEST(test_byte_and_block_forms_agree);
    return UNITY_END();
}
