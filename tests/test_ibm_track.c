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
#include "flux/ibm_track.h"

#include <string.h>

#include "support/ibm_encode.h"
#include "unity.h"

/* A 300 rpm, 250 kbit/s track: 100000 cells (2 us each) = 12500 bytes. */
#define TRACK_BYTES 12500u
#define MAX_SECS    32u

static uint8_t track[40000];
static ibm_sector_t secs[MAX_SECS];
static uint8_t data[64 * 1024];
static ibm_track_t t;
static uint8_t pattern[16][1024];

void setUp(void) {
    ibm_track_init(&t, secs, MAX_SECS, data, sizeof data);
    for (unsigned r = 0; r < 16; r++) {
        for (unsigned k = 0; k < sizeof pattern[r]; k++) pattern[r][k] = (uint8_t)(r * 7u + k);
    }
}
void tearDown(void) {}

/* RM 480Z data track shape: 9 x 512, IDs 1-9, cylinder 5 head 1. */
static void make_mfm_9x512(enc_sector_t *s) {
    memset(s, 0, 9 * sizeof *s);
    for (unsigned i = 0; i < 9; i++) {
        s[i].c = 5;
        s[i].h = 1;
        s[i].r = (uint8_t)(i + 1);
        s[i].n = 2;
        s[i].data = pattern[i + 1];
    }
}

static uint32_t encode_mfm(const enc_sector_t *s, unsigned count) {
    enc_t e;
    enc_init(&e, track, TRACK_BYTES, 1);
    enc_mfm_track(&e, s, count, 54);
    TEST_ASSERT_FALSE_MESSAGE(e.overflow, "test track too long");
    return TRACK_BYTES * 8u;
}

static void assert_sector(const ibm_sector_t *s, uint8_t c, uint8_t h, uint8_t r, uint8_t n, uint8_t flags) {
    TEST_ASSERT_EQUAL_HEX8(c, s->c);
    TEST_ASSERT_EQUAL_HEX8(h, s->h);
    TEST_ASSERT_EQUAL_HEX8(r, s->r);
    TEST_ASSERT_EQUAL_HEX8(n, s->n);
    TEST_ASSERT_EQUAL_HEX8(flags, s->flags);
}

static void test_mfm_9x512_track_decodes_every_sector_in_order(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    uint32_t n = encode_mfm(s, 9);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, n, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    TEST_ASSERT_EQUAL_UINT16(0, t.lost);
    for (unsigned i = 0; i < 9; i++) {
        assert_sector(&secs[i], 5, 1, (uint8_t)(i + 1), 2, 0);
        TEST_ASSERT_EQUAL_UINT16(512, secs[i].size);
        TEST_ASSERT_EQUAL_HEX8(IBM_MARK_DATA, secs[i].mark);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[i + 1], data + secs[i].data_off, 512);
        TEST_ASSERT_TRUE(secs[i].data_pos > secs[i].id_pos);
        if (i) TEST_ASSERT_TRUE(secs[i].id_pos > secs[i - 1].id_pos);
    }
    TEST_ASSERT_EQUAL_UINT32(9 * 512, t.data_used);
}

static void test_fm_16x128_at_double_rate_is_found_by_auto(void) {
    /* RM track 0: FM, 16 x 128, stored at the MFM cell rate as HFE does. */
    enc_sector_t s[16];
    memset(s, 0, sizeof s);
    for (unsigned i = 0; i < 16; i++) {
        s[i].r = (uint8_t)(i + 1);
        s[i].data = pattern[i];
    }
    enc_t e;
    enc_init(&e, track, TRACK_BYTES, 2);
    enc_fm_track(&e, s, 16, 16);
    TEST_ASSERT_FALSE(e.overflow);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_auto(&t, track, TRACK_BYTES * 8u));
    TEST_ASSERT_EQUAL(IBM_ENC_FM, t.encoding);
    TEST_ASSERT_EQUAL_UINT(2, t.cell_step);
    TEST_ASSERT_EQUAL_UINT16(16, t.count);
    for (unsigned i = 0; i < 16; i++) {
        assert_sector(&secs[i], 0, 0, (uint8_t)(i + 1), 0, 0);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[i], data + secs[i].data_off, 128);
        TEST_ASSERT_EQUAL_UINT32(0, secs[i].id_pos % 2); /* positions are input cells */
    }
}

static void test_fm_at_its_own_rate_is_found_by_auto(void) {
    enc_sector_t s[8];
    memset(s, 0, sizeof s);
    for (unsigned i = 0; i < 8; i++) {
        s[i].r = (uint8_t)(i + 1);
        s[i].fill_byte = (uint8_t)(0x10 + i);
    }
    enc_t e;
    enc_init(&e, track, TRACK_BYTES / 2, 1);
    enc_fm_track(&e, s, 8, 27);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_auto(&t, track, TRACK_BYTES * 4u));
    TEST_ASSERT_EQUAL(IBM_ENC_FM, t.encoding);
    TEST_ASSERT_EQUAL_UINT(1, t.cell_step);
    TEST_ASSERT_EQUAL_UINT16(8, t.count);
    TEST_ASSERT_EQUAL_HEX8(0x17, data[secs[7].data_off + 127]);
}

static void test_deleted_data_mark_is_flagged(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[3].mark = IBM_MARK_DELETED;
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DELETED, secs[3].flags);
    TEST_ASSERT_EQUAL_HEX8(IBM_MARK_DELETED, secs[3].mark);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[4], data + secs[3].data_off, 512);
    TEST_ASSERT_EQUAL_HEX8(0, secs[4].flags);
}

static void test_bad_data_crc_is_flagged_and_data_still_returned(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[0].bad_data_crc = true;
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DATA_CRC_ERROR, secs[0].flags);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[1], data + secs[0].data_off, 512);
}

static void test_bad_id_crc_is_flagged_and_find_skips_it(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[2].bad_id_crc = true;
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_TRUE(secs[2].flags & IBM_SEC_ID_CRC_ERROR);
    TEST_ASSERT_EQUAL(-1, ibm_track_find(&t, 3));
    TEST_ASSERT_EQUAL(3, ibm_track_find(&t, 4));
}

static void test_id_without_data_is_flagged_and_does_not_steal_the_next_data(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[4].no_data = true;
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_NO_DATA, secs[4].flags);
    TEST_ASSERT_EQUAL_UINT32(IBM_NO_POS, secs[4].data_pos);
    TEST_ASSERT_EQUAL_HEX8(0, secs[5].flags);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[6], data + secs[5].data_off, 512);
}

static void test_duplicate_ids_are_all_kept(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[8].r = 1; /* a second sector 1, with different data */
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    TEST_ASSERT_EQUAL_HEX8(1, secs[0].r);
    TEST_ASSERT_EQUAL_HEX8(1, secs[8].r);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[9], data + secs[8].data_off, 512);
    TEST_ASSERT_EQUAL(0, ibm_track_find(&t, 1)); /* the first one round from the index */
}

/* Encodes the 9 x 512 track, then moves the index to `cut` cells after sector 5's data mark (a
 * negative cut lands between its ID and its data mark), and decodes again. */
static void decode_with_index_moved(int32_t cut, uint32_t *ncells) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    uint32_t n = encode_mfm(s, 9);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, n, IBM_ENC_MFM, 1));
    enc_rotate(track, n, (uint32_t)((int32_t)secs[4].data_pos + cut));
    ibm_track_init(&t, secs, MAX_SECS, data, sizeof data);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, n, IBM_ENC_MFM, 1));
    *ncells = n;
}

static void test_data_field_straddling_the_index_is_decoded_once(void) {
    uint32_t n;
    decode_with_index_moved(16 * 200, &n); /* index 200 bytes into sector 5's data */
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    int i5 = ibm_track_find(&t, 5);
    TEST_ASSERT_EQUAL(8, i5); /* last round from the index */
    TEST_ASSERT_EQUAL_HEX8(0, secs[i5].flags);
    TEST_ASSERT_TRUE(secs[i5].data_pos + 16u * (1 + 512 + 2) > n); /* the field runs past the index */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[5], data + secs[i5].data_off, 512);
    TEST_ASSERT_EQUAL_HEX8(6, secs[0].r); /* first after the index is now sector 6 */
}

static void test_data_mark_after_the_index_joins_the_id_before_it(void) {
    uint32_t n;
    decode_with_index_moved(-16 * 20, &n); /* index in the gap between sector 5's ID and data */
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    int i5 = ibm_track_find(&t, 5);
    TEST_ASSERT_EQUAL(8, i5);
    TEST_ASSERT_EQUAL_HEX8(0, secs[i5].flags);
    TEST_ASSERT_TRUE(secs[i5].data_pos < secs[i5].id_pos); /* data mark comes round after the index */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[5], data + secs[i5].data_off, 512);
}

static void test_more_ids_than_room_are_counted_as_lost(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    ibm_track_init(&t, secs, 4, data, sizeof data);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(4, t.count);
    TEST_ASSERT_EQUAL_UINT16(5, t.lost);
}

static void test_data_that_does_not_fit_is_dropped_but_crc_checked(void) {
    enc_sector_t s[9];
    make_mfm_9x512(s);
    s[1].bad_data_crc = true;
    ibm_track_init(&t, secs, MAX_SECS, data, 512);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, encode_mfm(s, 9), IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_HEX8(0, secs[0].flags);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DATA_DROPPED | IBM_SEC_DATA_CRC_ERROR, secs[1].flags);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_DATA_DROPPED, secs[2].flags);
    TEST_ASSERT_EQUAL_UINT32(512, t.data_used);
}

static void test_size_code_above_6_is_flagged_bad_size(void) {
    enc_sector_t s[1] = {{.c = 0, .h = 0, .r = 1, .n = 7, .fill_byte = 0xE5}};
    enc_t e;
    enc_init(&e, track, sizeof track, 1);
    enc_mfm_track(&e, s, 1, 54);
    TEST_ASSERT_FALSE(e.overflow);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode(&t, track, (uint32_t)sizeof track * 8u, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL_UINT16(1, t.count);
    TEST_ASSERT_EQUAL_HEX8(IBM_SEC_BAD_SIZE, secs[0].flags);
    TEST_ASSERT_EQUAL_UINT16(0, secs[0].size);
    TEST_ASSERT_EQUAL_UINT32(0, t.data_used);
}

static void test_blank_track_finds_nothing(void) {
    memset(track, 0, TRACK_BYTES);
    TEST_ASSERT_EQUAL(IBM_ERR_NOTHING, ibm_track_decode_auto(&t, track, TRACK_BYTES * 8u));
    TEST_ASSERT_EQUAL_UINT16(0, t.count);
}

static void test_bad_arguments_are_rejected(void) {
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_decode(&t, NULL, 1000, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_decode(&t, track, 1000, IBM_ENC_MFM, 3));
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_decode(&t, track, 100, IBM_ENC_MFM, 1));
    TEST_ASSERT_EQUAL(IBM_ERR_ARG, ibm_track_decode(NULL, track, 1000, IBM_ENC_MFM, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mfm_9x512_track_decodes_every_sector_in_order);
    RUN_TEST(test_fm_16x128_at_double_rate_is_found_by_auto);
    RUN_TEST(test_fm_at_its_own_rate_is_found_by_auto);
    RUN_TEST(test_deleted_data_mark_is_flagged);
    RUN_TEST(test_bad_data_crc_is_flagged_and_data_still_returned);
    RUN_TEST(test_bad_id_crc_is_flagged_and_find_skips_it);
    RUN_TEST(test_id_without_data_is_flagged_and_does_not_steal_the_next_data);
    RUN_TEST(test_duplicate_ids_are_all_kept);
    RUN_TEST(test_data_field_straddling_the_index_is_decoded_once);
    RUN_TEST(test_data_mark_after_the_index_joins_the_id_before_it);
    RUN_TEST(test_more_ids_than_room_are_counted_as_lost);
    RUN_TEST(test_data_that_does_not_fit_is_dropped_but_crc_checked);
    RUN_TEST(test_size_code_above_6_is_flagged_bad_size);
    RUN_TEST(test_blank_track_finds_nothing);
    RUN_TEST(test_bad_arguments_are_rejected);
    return UNITY_END();
}
