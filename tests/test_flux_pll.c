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
#include "flux/flux_pll.h"

#include <string.h>

#include "flux/ibm_track.h"
#include "support/flux_synth.h"
#include "support/ibm_encode.h"
#include "unity.h"

#define TRACK_BYTES 12500u /* 100000 cells: 300 rpm at 2 us */

static uint8_t track[TRACK_BYTES], cells[3 * TRACK_BYTES], pattern[9][512];
/* Two revolutions of an MFM track are about 76000 intervals; this fits in the Pico 2's RAM, where these
 * tests also run (tests/pico/). synth_flux returns 0 if it is too small, which fails the test. */
static uint32_t flux[80000];
static ibm_sector_t secs[32];
static uint8_t data[16384];

typedef struct {
    const uint32_t *flux;
    uint32_t n, i;
} feed_t;

static bool feed_next(void *ctx, uint32_t *ticks) {
    feed_t *f = ctx;
    if (f->i >= f->n) return false;
    *ticks = f->flux[f->i++];
    return true;
}

void setUp(void) {
    enc_sector_t s[9];
    memset(s, 0, sizeof s);
    for (unsigned i = 0; i < 9; i++) {
        for (unsigned k = 0; k < 512; k++)
            pattern[i][k] = (uint8_t)(i * 31u + k * 7u);
        s[i] = (enc_sector_t){.c = 7, .h = 0, .r = (uint8_t)(i + 1), .n = 2, .data = pattern[i]};
    }
    enc_t e;
    enc_init(&e, track, TRACK_BYTES, 1);
    enc_mfm_track(&e, s, 9, 54);
}
void tearDown(void) {}

/* 40 MHz sampling, 2 us cells: 80 ticks per cell. */
static void decode_and_check(uint32_t speed_permille, uint32_t jitter) {
    uint32_t rev_ticks;
    uint32_t n = synth_flux(track, TRACK_BYTES * 8u, 2, 80, speed_permille, jitter, flux, sizeof flux / sizeof flux[0],
                            &rev_ticks);
    TEST_ASSERT_TRUE(n > 0);
    feed_t f = {flux, n, 0};
    flux_pll_params_t p = {40000000u, 2000u, rev_ticks, 0, 0};
    flux_pll_result_t r;
    flux_pll_decode(&p, feed_next, &f, cells, sizeof cells * 8u, &r);
    TEST_ASSERT_FALSE(r.truncated);
    TEST_ASSERT_UINT32_WITHIN(40, TRACK_BYTES * 8u, r.ncells_rev); /* one revolution of cells */
    TEST_ASSERT_TRUE(r.ncells > r.ncells_rev);                     /* and the run-on past the index */

    ibm_track_t t;
    ibm_track_init(&t, secs, 32, data, sizeof data);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_span_auto(&t, cells, r.ncells_rev, r.ncells));
    TEST_ASSERT_EQUAL(IBM_ENC_MFM, t.encoding);
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    for (unsigned i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, secs[i].flags);
        TEST_ASSERT_EQUAL_HEX8(i + 1, secs[i].r);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[i], data + secs[i].data_off, 512);
    }
}

static void test_clean_flux_decodes_to_the_original_sectors(void) {
    decode_and_check(1000, 0);
}

static void test_jitter_of_ten_percent_of_a_cell_is_tolerated(void) {
    decode_and_check(1000, 8);
}

static void test_a_drive_two_percent_slow_is_tracked(void) {
    decode_and_check(1020, 6);
}

static void test_a_drive_three_percent_fast_is_tracked(void) {
    decode_and_check(970, 6);
}

static void test_cells_are_exact_for_clean_flux(void) {
    uint32_t rev_ticks;
    uint32_t n = synth_flux(track, TRACK_BYTES * 8u, 1, 80, 1000, 0, flux, sizeof flux / sizeof flux[0], &rev_ticks);
    TEST_ASSERT_TRUE(n > 0);
    feed_t f = {flux, n, 0};
    flux_pll_params_t p = {40000000u, 2000u, rev_ticks, 0, 0};
    flux_pll_result_t r;
    flux_pll_decode(&p, feed_next, &f, cells, sizeof cells * 8u, &r);
    /* The flux ends at the last 1, so the trailing 0s of the track are not recreated. */
    TEST_ASSERT_TRUE(r.ncells <= TRACK_BYTES * 8u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(track, cells, r.ncells / 8u);
}

static void test_a_full_buffer_stops_and_says_so(void) {
    uint32_t rev_ticks;
    uint32_t n = synth_flux(track, TRACK_BYTES * 8u, 1, 80, 1000, 0, flux, sizeof flux / sizeof flux[0], &rev_ticks);
    TEST_ASSERT_TRUE(n > 0);
    feed_t f = {flux, n, 0};
    flux_pll_params_t p = {40000000u, 2000u, rev_ticks, 0, 0};
    flux_pll_result_t r;
    flux_pll_decode(&p, feed_next, &f, cells, 1000, &r);
    TEST_ASSERT_TRUE(r.truncated);
    TEST_ASSERT_TRUE(r.ncells <= 1000);
    TEST_ASSERT_EQUAL_UINT32(r.ncells, r.ncells_rev); /* never reached the index */
}

static void test_bad_parameters_produce_nothing(void) {
    feed_t f = {flux, 0, 0};
    flux_pll_params_t p = {0, 2000u, 0, 0, 0};
    flux_pll_result_t r;
    flux_pll_decode(&p, feed_next, &f, cells, sizeof cells * 8u, &r);
    TEST_ASSERT_EQUAL_UINT32(0, r.ncells);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_flux_decodes_to_the_original_sectors);
    RUN_TEST(test_jitter_of_ten_percent_of_a_cell_is_tolerated);
    RUN_TEST(test_a_drive_two_percent_slow_is_tracked);
    RUN_TEST(test_a_drive_three_percent_fast_is_tracked);
    RUN_TEST(test_cells_are_exact_for_clean_flux);
    RUN_TEST(test_a_full_buffer_stops_and_says_so);
    RUN_TEST(test_bad_parameters_produce_nothing);
    return UNITY_END();
}
