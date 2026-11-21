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
#include "flux/hfe.h"

#include <string.h>

#include "flux/ibm_track.h"
#include "support/ibm_encode.h"
#include "unity.h"

/* An HFE file built in memory, read back through the callback. */
static uint8_t file[64 * 1024];
static uint32_t file_len;
static uint8_t buf[16 * 1024];

static bool mem_read(void *ctx, uint32_t offset, void *dst, uint32_t len) {
    (void)ctx;
    if (offset > file_len || len > file_len - offset) return false;
    memcpy(dst, file + offset, len);
    return true;
}

static uint8_t rev(uint8_t b) { /* independent of the library's: bit by bit */
    uint8_t r = 0;
    for (int k = 0; k < 8; k++) r = (uint8_t)(r | (((b >> k) & 1u) << (7 - k)));
    return r;
}

/* Header with the given signature; LUT at block 1; tracks from block 2. */
static void header(const char *sig, uint8_t cyls, uint8_t sides) {
    memset(file, 0xFF, sizeof file);
    memcpy(file, sig, 8);
    file[0x08] = 0;
    file[0x09] = cyls;
    file[0x0A] = sides;
    file[0x0B] = 0x00; /* ISO/IBM MFM */
    file[0x0C] = 0xFA; /* 250 kbit/s */
    file[0x0D] = 0x00;
    file[0x0E] = 0x00;
    file[0x0F] = 0x00;
    file[0x10] = 0x07;
    file[0x11] = 0x00;
    file[0x12] = 0x01; /* track list at block 1 */
    file[0x13] = 0x00;
    file[0x14] = 0xFF;
    file[0x15] = 0xFF;
    file_len = 1024;
}

/* Appends cylinder cyl: side data given in normal (MSB first) cell order, len bytes per side. */
static void add_track(uint8_t cyl, const uint8_t *s0, const uint8_t *s1, uint32_t len) {
    uint32_t block = file_len / 512;
    file[512 + cyl * 4] = (uint8_t)block;
    file[512 + cyl * 4 + 1] = (uint8_t)(block >> 8);
    file[512 + cyl * 4 + 2] = (uint8_t)(len * 2);
    file[512 + cyl * 4 + 3] = (uint8_t)((len * 2) >> 8);
    for (uint32_t done = 0; done < len; done += 256) {
        uint32_t n = len - done < 256 ? len - done : 256;
        for (uint32_t k = 0; k < n; k++) {
            file[file_len + k] = rev(s0[done + k]);
            if (s1) file[file_len + 256 + k] = rev(s1[done + k]);
        }
        file_len += 512;
    }
}

void setUp(void) {}
void tearDown(void) {}

static void test_open_reads_the_header(void) {
    header("HXCPICFE", 40, 2);
    file[0x16] = 0x00; /* track 0 side 0: own encoding, FM */
    file[0x17] = 0x02;
    hfe_image_t img;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL_UINT8(1, img.version);
    TEST_ASSERT_EQUAL_UINT8(40, img.cylinders);
    TEST_ASSERT_EQUAL_UINT8(2, img.sides);
    TEST_ASSERT_EQUAL_UINT16(250, img.bitrate_kbps);
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_MFM, img.encoding);
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_FM, img.track0_encoding[0]);
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_MFM, img.track0_encoding[1]); /* FF: not overridden */
    TEST_ASSERT_TRUE(img.write_allowed);
    TEST_ASSERT_FALSE(img.double_step);
    TEST_ASSERT_EQUAL_UINT8(0x07, img.interface_mode);
}

static void test_open_rejects_bad_files(void) {
    hfe_image_t img;
    header("HXCPICFX", 40, 2);
    TEST_ASSERT_EQUAL(HFE_ERR_SIGNATURE, hfe_open(&img, mem_read, NULL));
    header("HXCPICFE", 0, 2);
    TEST_ASSERT_EQUAL(HFE_ERR_HEADER, hfe_open(&img, mem_read, NULL));
    header("HXCPICFE", 40, 3);
    TEST_ASSERT_EQUAL(HFE_ERR_HEADER, hfe_open(&img, mem_read, NULL));
    header("HXCPICFE", 40, 2);
    file_len = 10;
    TEST_ASSERT_EQUAL(HFE_ERR_IO, hfe_open(&img, mem_read, NULL));
}

static void test_v1_track_sides_come_back_msb_first_across_a_partial_block(void) {
    uint8_t s0[600], s1[600];
    for (unsigned k = 0; k < 600; k++) {
        s0[k] = (uint8_t)(k * 3u + 1u);
        s1[k] = (uint8_t)(0xA5u ^ k);
    }
    header("HXCPICFE", 2, 2);
    add_track(0, s0, s1, 600);
    add_track(1, s1, s0, 600);
    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    uint32_t need;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_track_bytes(&img, 0, 1, &need));
    TEST_ASSERT_EQUAL_UINT32(600, need);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_UINT32(4800, tr.ncells);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s0, buf, 600);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 1, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s1, buf, 600);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 1, 0, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s1, buf, 600);
}

static void test_track0_encoding_override_applies_to_cylinder_0_only(void) {
    uint8_t s[256] = {0};
    header("HXCPICFE", 2, 2);
    file[0x18] = 0x00; /* track 0 side 1: FM */
    file[0x19] = 0x02;
    add_track(0, s, s, 256);
    add_track(1, s, s, 256);
    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 1, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_FM, tr.encoding);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_MFM, tr.encoding);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 1, 1, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_HEX8(HFE_ENC_ISOIBM_MFM, tr.encoding);
}

static void test_out_of_range_and_small_buffer_are_refused(void) {
    uint8_t s[300] = {0};
    header("HXCPICFE", 1, 1);
    add_track(0, s, NULL, 300);
    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_ERR_RANGE, hfe_read_track(&img, 1, 0, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL(HFE_ERR_RANGE, hfe_read_track(&img, 0, 1, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL(HFE_ERR_BUFFER, hfe_read_track(&img, 0, 0, buf, 299, &tr));
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 0, buf, 300, &tr));
}

static void test_truncated_track_data_is_an_io_error(void) {
    uint8_t s[600] = {0};
    header("HXCPICFE", 1, 2);
    add_track(0, s, s, 600);
    file_len -= 512; /* lose the last block */
    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_ERR_IO, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));
}

static void test_v3_opcodes_are_interpreted(void) {
    /* As they read after bit reversal: rate 72 (250 kbit/s); AA; nop; index; 55; skip 3 of 1F
       (cells 11111); random byte (8 weak cells); skip 2 then random (6 weak cells). */
    const uint8_t v3[] = {0xF2, 72, 0xAA, 0xF0, 0xF1, 0x55, 0xF3, 3, 0x1F, 0xF4, 0xF3, 2, 0xF4};
    header("HXCHFEV3", 1, 1);
    add_track(0, v3, NULL, sizeof v3);
    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL_UINT8(3, img.version);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));
    TEST_ASSERT_EQUAL_UINT32(8 + 8 + 5 + 8 + 6, tr.ncells);
    TEST_ASSERT_EQUAL_UINT32(8, tr.index_cell);
    TEST_ASSERT_EQUAL_UINT32(14, tr.weak_cells);
    TEST_ASSERT_EQUAL_UINT16(250, tr.bitrate_kbps);
    /* AA 55 11111 00000000 000000 -> AA 55 F8 00 00 (35 cells) */
    const uint8_t want[] = {0xAA, 0x55, 0xF8, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, buf, 4);
    TEST_ASSERT_EQUAL_HEX8(0, buf[4] & 0xE0u);
}

static void test_v3_bad_opcodes_are_refused(void) {
    hfe_image_t img;
    hfe_track_t tr;
    const uint8_t unknown[] = {0xAA, 0xF7, 0x55};
    header("HXCHFEV3", 1, 1);
    add_track(0, unknown, NULL, sizeof unknown);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_ERR_TRACK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));

    const uint8_t truncated[] = {0xAA, 0xF3, 3};
    header("HXCHFEV3", 1, 1);
    add_track(0, truncated, NULL, sizeof truncated);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_ERR_TRACK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));

    const uint8_t overflow[] = {0xF3, 4, 0x1F}; /* 1F << 4 doesn't fit a byte */
    header("HXCHFEV3", 1, 1);
    add_track(0, overflow, NULL, sizeof overflow);
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_ERR_TRACK, hfe_read_track(&img, 0, 0, buf, sizeof buf, &tr));
}

static void test_mfm_track_round_trips_through_an_hfe_file(void) {
    static uint8_t cells[12500], d[9][512];
    enc_sector_t s[9];
    memset(s, 0, sizeof s);
    for (unsigned i = 0; i < 9; i++) {
        for (unsigned k = 0; k < 512; k++) d[i][k] = (uint8_t)(i ^ k);
        s[i].c = 3;
        s[i].h = 1;
        s[i].r = (uint8_t)(i + 1);
        s[i].n = 2;
        s[i].data = d[i];
    }
    enc_t e;
    enc_init(&e, cells, sizeof cells, 1);
    enc_mfm_track(&e, s, 9, 54);
    header("HXCPICFE", 1, 2);
    static uint8_t blank[12500];
    add_track(0, blank, cells, sizeof cells);

    hfe_image_t img;
    hfe_track_t tr;
    TEST_ASSERT_EQUAL(HFE_OK, hfe_open(&img, mem_read, NULL));
    TEST_ASSERT_EQUAL(HFE_OK, hfe_read_track(&img, 0, 1, buf, sizeof buf, &tr));
    static ibm_sector_t secs[16];
    static uint8_t data[8192];
    ibm_track_t t;
    ibm_track_init(&t, secs, 16, data, sizeof data);
    TEST_ASSERT_EQUAL(IBM_OK, ibm_track_decode_auto(&t, buf, tr.ncells));
    TEST_ASSERT_EQUAL_UINT16(9, t.count);
    for (unsigned i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(0, secs[i].flags);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(d[i], data + secs[i].data_off, 512);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_open_reads_the_header);
    RUN_TEST(test_open_rejects_bad_files);
    RUN_TEST(test_v1_track_sides_come_back_msb_first_across_a_partial_block);
    RUN_TEST(test_track0_encoding_override_applies_to_cylinder_0_only);
    RUN_TEST(test_out_of_range_and_small_buffer_are_refused);
    RUN_TEST(test_truncated_track_data_is_an_io_error);
    RUN_TEST(test_v3_opcodes_are_interpreted);
    RUN_TEST(test_v3_bad_opcodes_are_refused);
    RUN_TEST(test_mfm_track_round_trips_through_an_hfe_file);
    return UNITY_END();
}
