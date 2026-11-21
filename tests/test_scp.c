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
#include "flux/scp.h"

#include <string.h>

#include "flux/flux_pll.h"
#include "flux/ibm_track.h"
#include "support/flux_synth.h"
#include "support/ibm_encode.h"
#include "unity.h"

/* An SCP file built in memory (Jim Drew's layout), read back through the callback. Sized for the
 * largest test (two revolutions of an MFM track, about 150 KB) so that it fits in the Pico 2's RAM:
 * these tests run there too (tests/pico/). */
static uint8_t file[192 * 1024];
static uint32_t file_len;

static bool mem_read(void *ctx, uint32_t offset, void *dst, uint32_t len) {
    (void)ctx;
    if (offset > file_len || len > file_len - offset) return false;
    memcpy(dst, file + offset, len);
    return true;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void header(uint8_t revs, uint8_t flags, uint8_t cell_width, uint8_t resolution) {
    memset(file, 0, sizeof file);
    memcpy(file, "SCP", 3);
    file[3] = 0x19;
    file[4] = 0x80;
    file[5] = revs;
    file[6] = 0;
    file[7] = 79;
    file[8] = flags;
    file[9] = cell_width;
    file[10] = 0;
    file[11] = resolution;
    file_len = 0x10 + 168 * 4;
}

/* Starts track tn with room for nrevs revolutions; returns the TDH offset. Add them in order with add_rev. */
static uint32_t begin_track(unsigned tn, unsigned nrevs) {
    uint32_t tdh = file_len;
    TEST_ASSERT_TRUE_MESSAGE(tdh + 4 + 12 * nrevs <= sizeof file, "fixture: SCP file buffer too small");
    put32(file + 0x10 + tn * 4, tdh);
    memcpy(file + tdh, "TRK", 3);
    file[tdh + 3] = (uint8_t)tn;
    file_len = tdh + 4 + 12 * nrevs;
    return tdh;
}

/* Appends revolution r of the track at tdh: its flux (ticks) and its index time. */
static void add_rev(uint32_t tdh, unsigned r, const uint32_t *flux, uint32_t nflux, uint32_t index_ticks) {
    uint32_t start = file_len, pos = file_len;
    for (uint32_t i = 0; i < nflux; i++) {
        uint32_t v = flux[i];
        while (v > 0xFFFF) {
            TEST_ASSERT_TRUE_MESSAGE(pos + 2 <= sizeof file, "fixture: SCP file buffer too small");
            file[pos++] = 0;
            file[pos++] = 0;
            v -= 65536;
        }
        TEST_ASSERT_TRUE_MESSAGE(pos + 2 <= sizeof file, "fixture: SCP file buffer too small");
        file[pos++] = (uint8_t)(v >> 8);
        file[pos++] = (uint8_t)v;
    }
    put32(file + tdh + 4 + 12 * r, index_ticks);
    put32(file + tdh + 4 + 12 * r + 4, (pos - start) / 2);
    put32(file + tdh + 4 + 12 * r + 8, start - tdh);
    file_len = pos;
}

/* Appends track tn with the given revolutions' flux (ticks); returns the TDH offset. */
static uint32_t add_track(unsigned tn, unsigned nrevs, const uint32_t *const *revflux, const uint32_t *nflux,
                          const uint32_t *index_ticks) {
    uint32_t tdh = begin_track(tn, nrevs);
    for (unsigned r = 0; r < nrevs; r++)
        add_rev(tdh, r, revflux[r], nflux[r], index_ticks[r]);
    return tdh;
}

static void set_checksum(void) {
    uint32_t sum = 0;
    for (uint32_t i = 0x10; i < file_len; i++)
        sum += file[i];
    put32(file + 12, sum);
}

void setUp(void) {}
void tearDown(void) {}

static void test_open_reads_the_header(void) {
    header(3, SCP_FLAG_INDEX | SCP_FLAG_96TPI, 0, 0);
    scp_image_t img;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL_UINT8(3, img.revolutions);
    TEST_ASSERT_EQUAL_UINT8(79, img.end_track);
    TEST_ASSERT_EQUAL_HEX8(0x03, img.flags);
    TEST_ASSERT_EQUAL_UINT32(40000000u, img.sample_hz);
    header(3, 0, 0, 1); /* resolution 1: 50 ns samples */
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL_UINT32(20000000u, img.sample_hz);
}

static void test_open_rejects_bad_and_unsupported_files(void) {
    scp_image_t img;
    header(1, 0, 0, 0);
    file[2] = 'X';
    TEST_ASSERT_EQUAL(SCP_ERR_SIGNATURE, scp_open(&img, mem_read, NULL));
    header(1, SCP_FLAG_EXTENDED, 0, 0);
    TEST_ASSERT_EQUAL(SCP_ERR_UNSUPPORTED, scp_open(&img, mem_read, NULL));
    header(1, 0, 8, 0);
    TEST_ASSERT_EQUAL(SCP_ERR_UNSUPPORTED, scp_open(&img, mem_read, NULL));
    header(1, 0, 0, 0);
    file_len = 8;
    TEST_ASSERT_EQUAL(SCP_ERR_IO, scp_open(&img, mem_read, NULL));
}

static void test_track_headers_are_read_and_checked(void) {
    static const uint32_t f0[] = {100, 200}, f1[] = {300};
    const uint32_t *revs[] = {f0, f1};
    const uint32_t n[] = {2, 1}, idx[] = {1000, 2000};
    header(2, SCP_FLAG_INDEX, 0, 0);
    uint32_t tdh = add_track(5, 2, revs, n, idx); /* cylinder 2, side 1 */
    scp_image_t img;
    scp_track_t t;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_read_track(&img, 2, 1, &t));
    TEST_ASSERT_EQUAL_UINT8(5, t.track);
    TEST_ASSERT_EQUAL_UINT8(2, t.nrevs);
    TEST_ASSERT_EQUAL_UINT32(2000, t.revs[1].index_ticks);
    TEST_ASSERT_EQUAL_UINT32(1, t.revs[1].nflux);
    TEST_ASSERT_EQUAL_UINT32(tdh + 4 + 24 + 4, t.revs[1].data_offset);
    TEST_ASSERT_EQUAL(SCP_ERR_NO_TRACK, scp_read_track(&img, 2, 0, &t));
    file[tdh + 3] = 6; /* header says the wrong track */
    TEST_ASSERT_EQUAL(SCP_ERR_TRACK, scp_read_track(&img, 2, 1, &t));
    file[tdh] = 'X';
    TEST_ASSERT_EQUAL(SCP_ERR_SIGNATURE, scp_read_track(&img, 2, 1, &t));
}

static void test_empty_trailing_revolutions_are_dropped(void) {
    static const uint32_t f0[] = {100};
    const uint32_t *revs[] = {f0, f0, f0};
    const uint32_t n[] = {1, 1, 0}, idx[] = {1000, 1000, 0};
    header(3, SCP_FLAG_INDEX, 0, 0);
    add_track(0, 3, revs, n, idx);
    scp_image_t img;
    scp_track_t t;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_read_track(&img, 0, 0, &t));
    TEST_ASSERT_EQUAL_UINT8(2, t.nrevs);
}

static void test_flux_stream_folds_overflow_words_and_runs_on_through_later_revolutions(void) {
    /* 65537 needs an overflow word; exactly 65536 can't be written in SCP (0000 means "add 65536 to the
     * next value", so a remainder of 0 would read as another overflow word), so no file holds it. */
    static const uint32_t f0[] = {100, 70000, 80}, f1[] = {65537, 5};
    const uint32_t *revs[] = {f0, f1};
    const uint32_t n[] = {3, 2}, idx[] = {70180, 65542};
    header(2, SCP_FLAG_INDEX, 0, 0);
    add_track(0, 2, revs, n, idx);
    scp_image_t img;
    scp_track_t t;
    scp_flux_stream_t s;
    uint32_t v;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_read_track(&img, 0, 0, &t));
    TEST_ASSERT_EQUAL_UINT32(4, t.revs[0].nflux); /* 70000 takes an overflow word */
    TEST_ASSERT_EQUAL(SCP_OK, scp_flux_open(&s, &img, &t, 0));
    const uint32_t want[] = {100, 70000, 80, 65537, 5};
    for (unsigned i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(scp_flux_next(&s, &v));
        TEST_ASSERT_EQUAL_UINT32(want[i], v);
    }
    TEST_ASSERT_FALSE(scp_flux_next(&s, &v));
    TEST_ASSERT_FALSE(s.failed);
    TEST_ASSERT_EQUAL(SCP_OK, scp_flux_open(&s, &img, &t, 1));
    TEST_ASSERT_TRUE(scp_flux_next(&s, &v));
    TEST_ASSERT_EQUAL_UINT32(65537, v);
    TEST_ASSERT_EQUAL(SCP_ERR_RANGE, scp_flux_open(&s, &img, &t, 2));
}

static void test_truncated_flux_data_is_reported(void) {
    static const uint32_t f0[] = {100, 200, 300};
    const uint32_t *revs[] = {f0};
    const uint32_t n[] = {3}, idx[] = {600};
    header(1, SCP_FLAG_INDEX, 0, 0);
    add_track(0, 1, revs, n, idx);
    file_len -= 4;
    scp_image_t img;
    scp_track_t t;
    scp_flux_stream_t s;
    uint32_t v;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_read_track(&img, 0, 0, &t));
    TEST_ASSERT_EQUAL(SCP_OK, scp_flux_open(&s, &img, &t, 0));
    while (scp_flux_next(&s, &v)) {}
    TEST_ASSERT_TRUE(s.failed);
}

static void test_checksum_is_verified(void) {
    static const uint32_t f0[] = {100, 200};
    const uint32_t *revs[] = {f0};
    const uint32_t n[] = {2}, idx[] = {300};
    header(1, SCP_FLAG_INDEX, 0, 0);
    add_track(0, 1, revs, n, idx);
    set_checksum();
    scp_image_t img;
    bool ok = false;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_verify_checksum(&img, file_len, &ok));
    TEST_ASSERT_TRUE(ok);
    file[file_len - 1] ^= 1;
    TEST_ASSERT_EQUAL(SCP_OK, scp_verify_checksum(&img, file_len, &ok));
    TEST_ASSERT_FALSE(ok);
}

static void test_mfm_track_round_trips_through_scp_and_the_pll(void) {
    static uint8_t track[12500], cells[3 * 12500], pattern[9][512], data[8192];
    static uint32_t flux[40000]; /* one revolution (about 38000) at a time, into the file before the next */
    static ibm_sector_t secs[16];
    enc_sector_t s[9];
    memset(s, 0, sizeof s);
    for (unsigned i = 0; i < 9; i++) {
        for (unsigned k = 0; k < 512; k++)
            pattern[i][k] = (uint8_t)(k ^ (i * 17u));
        s[i] = (enc_sector_t){.c = 12, .h = 1, .r = (uint8_t)(i + 1), .n = 2, .data = pattern[i]};
    }
    enc_t e;
    enc_init(&e, track, sizeof track, 1);
    enc_mfm_track(&e, s, 9, 54);
    header(2, SCP_FLAG_INDEX | SCP_FLAG_96TPI, 0, 0);
    uint32_t tdh = begin_track(12 * 2 + 1, 2);
    const uint32_t speed_permille[2] = {1000, 1005};
    for (unsigned r = 0; r < 2; r++) {
        uint32_t idx;
        uint32_t n =
            synth_flux(track, sizeof track * 8u, 1, 80, speed_permille[r], 5, flux, sizeof flux / sizeof flux[0], &idx);
        TEST_ASSERT_TRUE(n > 0);
        add_rev(tdh, r, flux, n, idx);
    }

    scp_image_t img;
    scp_track_t t;
    scp_flux_stream_t fs;
    TEST_ASSERT_EQUAL(SCP_OK, scp_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(SCP_OK, scp_read_track(&img, 12, 1, &t));
    for (unsigned r = 0; r < 2; r++) {
        TEST_ASSERT_EQUAL(SCP_OK, scp_flux_open(&fs, &img, &t, r));
        flux_pll_params_t p = {img.sample_hz, 2000u, t.revs[r].index_ticks, 0, 0};
        flux_pll_result_t pr;
        flux_pll_decode(&p, scp_flux_next, &fs, cells, sizeof cells * 8u, &pr);
        ibm_track_t tr;
        ibm_track_init(&tr, secs, 16, data, sizeof data);
        TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_span_auto(&tr, cells, pr.ncells_rev, pr.ncells));
        TEST_ASSERT_EQUAL_UINT16(9, tr.count);
        for (unsigned i = 0; i < 9; i++) {
            TEST_ASSERT_EQUAL_HEX8(0, secs[i].flags);
            TEST_ASSERT_EQUAL_HEX8(12, secs[i].c);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(pattern[secs[i].r - 1], data + secs[i].data_off, 512);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_reads_the_header);
    RUN_TEST(test_open_rejects_bad_and_unsupported_files);
    RUN_TEST(test_track_headers_are_read_and_checked);
    RUN_TEST(test_empty_trailing_revolutions_are_dropped);
    RUN_TEST(test_flux_stream_folds_overflow_words_and_runs_on_through_later_revolutions);
    RUN_TEST(test_truncated_flux_data_is_reported);
    RUN_TEST(test_checksum_is_verified);
    RUN_TEST(test_mfm_track_round_trips_through_scp_and_the_pll);
    return UNITY_END();
}
