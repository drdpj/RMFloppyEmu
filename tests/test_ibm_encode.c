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
#include "flux/ibm_encode.h"

#include <string.h>

#include "flux/ibm_track.h"
#include "unity.h"

/* The encoder is checked against the decoder, which test_ibm_track checks against an independent
 * test encoder (tests/support/ibm_encode.c): no shared code on either side. */

#define NCELLS 100000u
static uint8_t cells[NCELLS / 8];
static uint8_t pat[9][512];
static ibm_sector_t secs[32];
static uint8_t data[16384];
static ibm_track_t t;

void setUp(void) {
    for (unsigned i = 0; i < 9; i++) {
        for (unsigned k = 0; k < 512; k++) pat[i][k] = (uint8_t)(i * 13u + k);
    }
    ibm_track_init(&t, secs, 32, data, sizeof data);
}
void tearDown(void) {}

static void layout(ibm_enc_sector_t *s, unsigned n, unsigned step, unsigned size_code) {
    memset(s, 0, n * sizeof *s);
    for (unsigned i = 0; i < n; i++) {
        s[i] = (ibm_enc_sector_t){.c = 4, .h = 0, .r = (uint8_t)(i + 1), .n = (uint8_t)size_code, .data = pat[i],
                                  .size = (uint16_t)(128u << size_code),
                                  .pos = 2500u * step / 2u + i * (NCELLS - 6000u) / n};
    }
}

static void test_mfm_track_decodes_back_with_positions_kept(void) {
    ibm_enc_sector_t s[9];
    layout(s, 9, 1, 2);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_encode(s, 9, IBM_ENC_MFM, 1, cells, NCELLS));
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, cells, NCELLS, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    for (unsigned i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, secs[i].flags);
        TEST_ASSERT_EQUAL_HEX8(i + 1, secs[i].r);
        TEST_ASSERT_UINT32_WITHIN(8, s[i].pos, secs[i].id_pos);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pat[i], data + secs[i].data_off, 512);
    }
}

static void test_fm_at_double_rate_decodes_back(void) {
    ibm_enc_sector_t s[9];
    layout(s, 9, 2, 0); /* 9 x 128 to stay well inside an FM revolution */
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_encode(s, 9, IBM_ENC_FM, 2, cells, NCELLS));
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_auto(&t, cells, NCELLS));
    TEST_ASSERT_EQUAL(IBM_ENC_FM, t.encoding);
    TEST_ASSERT_EQUAL_UINT(2, t.cell_step);
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    for (unsigned i = 0; i < 9; i++) {
        TEST_ASSERT_UINT32_WITHIN(16, s[i].pos, secs[i].id_pos);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pat[i], data + secs[i].data_off, 128);
    }
}

static void test_deleted_bad_crc_no_data_and_duplicate_ids_survive(void) {
    ibm_enc_sector_t s[9];
    layout(s, 9, 1, 2);
    s[1].mark = IBM_MARK_DELETED;
    s[2].bad_crc = true;
    s[3].no_data = true;
    s[5].r = 2; /* a second sector 2 */
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_encode(s, 9, IBM_ENC_MFM, 1, cells, NCELLS));
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, cells, NCELLS, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DELETED, secs[1].flags);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DATA_CRC_ERROR, secs[2].flags);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_NO_DATA, secs[3].flags);
    TEST_ASSERT_EQUAL_HEX8(2, secs[5].r);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pat[5], data + secs[5].data_off, 512);
}

static void test_overlapping_or_overlong_layouts_are_refused(void) {
    ibm_enc_sector_t s[9];
    layout(s, 9, 1, 2);
    s[4].pos = s[3].pos + 100; /* on top of the previous sector */
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_encode(s, 9, IBM_ENC_MFM, 1, cells, NCELLS));
    layout(s, 9, 1, 2);
    s[8].pos = NCELLS - 1000; /* runs past the end of the revolution */
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_encode(s, 9, IBM_ENC_MFM, 1, cells, NCELLS));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mfm_track_decodes_back_with_positions_kept);
    RUN_TEST(test_fm_at_double_rate_decodes_back);
    RUN_TEST(test_deleted_bad_crc_no_data_and_duplicate_ids_survive);
    RUN_TEST(test_overlapping_or_overlong_layouts_are_refused);
    return UNITY_END();
}
