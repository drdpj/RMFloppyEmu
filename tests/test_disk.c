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
#include "core/disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/disk_hfe.h"
#include "core/disk_imd.h"
#include "support/testdata.h"
#include "unity.h"

static uint8_t file[64 * 1024];
static uint32_t flen;
static disk_track_t t, t2;

static uint32_t bytes_read; /* by mem_read, since a test last cleared it */

static bool mem_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    (void)ctx;
    if (off > flen || len > flen - off) return false;
    memcpy(buf, file + off, len);
    bytes_read += len;
    return true;
}

void setUp(void) {}
void tearDown(void) {}

/* --- the track model ------------------------------------------------------------------------- */

static void add(uint8_t r, uint32_t pos) {
    disk_sector_t s = {.r = r, .n = 0, .size = 128, .pos = pos};
    TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(&t, &s, NULL));
}

static void test_find_returns_the_next_sector_with_that_id_round_the_index(void) {
    disk_track_clear(&t, 2, 0);
    add(5, 2500);
    add(8, 12900); /* two sectors numbered 08, as the 4MATION protection */
    add(4, 23400);
    add(8, 33900);
    TEST_ASSERT_EQUAL(1, disk_track_find(&t, 8, 0));
    TEST_ASSERT_EQUAL(1, disk_track_find(&t, 8, 12900));
    TEST_ASSERT_EQUAL(3, disk_track_find(&t, 8, 12901));
    TEST_ASSERT_EQUAL(1, disk_track_find(&t, 8, 50000)); /* round the index to the first again */
    TEST_ASSERT_EQUAL(2, disk_track_find(&t, 4, 99999));
    TEST_ASSERT_EQUAL(-1, disk_track_find(&t, 9, 0));
}

static void test_add_refuses_what_does_not_fit(void) {
    disk_track_clear(&t, 0, 0);
    disk_sector_t big = {.r = 1, .n = 6, .size = 8192};
    TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(&t, &big, NULL));
    disk_sector_t one = {.r = 2, .n = 0, .size = 128};
    TEST_ASSERT_EQUAL(DISK_ERR_FULL, disk_track_add(&t, &one, NULL));
}

/* --- IMD --------------------------------------------------------------------------------------- */

static void imd_begin(void) {
    const char *h = "IMD 1.18: test\r\ncomment\r\n";
    flen = (uint32_t)strlen(h);
    memcpy(file, h, flen);
    file[flen++] = 0x1A;
}

static void put(const void *p, uint32_t n) {
    memcpy(file + flen, p, n);
    flen += n;
}

static void putb(uint8_t b) {
    file[flen++] = b;
}

static void test_imd_reads_every_record_type(void) {
    imd_begin();
    /* cylinder 1 head 0, MFM 250k, 6 sectors of 256 bytes, IDs 6..1 */
    const uint8_t hdr[] = {5, 1, 0, 6, 1}, ids[] = {6, 5, 4, 3, 2, 1};
    put(hdr, 5);
    put(ids, 6);
    uint8_t d[256];
    for (unsigned i = 0; i < 256; i++)
        d[i] = (uint8_t)i;
    putb(1);
    put(d, 256); /* normal */
    putb(2);
    putb(0xE5); /* compressed */
    putb(3);
    put(d, 256); /* deleted */
    putb(6);
    putb(0x11); /* compressed, data error */
    putb(0);    /* unavailable */
    putb(8);
    putb(0x22); /* compressed, deleted, data error */
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL_UINT8(2, img.cylinders);
    TEST_ASSERT_EQUAL_UINT8(1, img.sides);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 1, 0, &t));
    TEST_ASSERT_FALSE(t.fm);
    TEST_ASSERT_EQUAL_UINT16(250, t.rate_kbps);
    TEST_ASSERT_EQUAL_UINT8(6, t.count);
    const uint8_t want_flags[] = {
        0, 0, DISK_SEC_DELETED, DISK_SEC_CRC_ERROR, DISK_SEC_NO_DATA, DISK_SEC_DELETED | DISK_SEC_CRC_ERROR};
    for (unsigned i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_UINT8(6 - i, t.secs[i].r);
        TEST_ASSERT_EQUAL_UINT8(1, t.secs[i].c);
        TEST_ASSERT_EQUAL_UINT8(1, t.secs[i].n);
        TEST_ASSERT_EQUAL_UINT16(256, t.secs[i].size);
        TEST_ASSERT_EQUAL_HEX8(want_flags[i], t.secs[i].flags);
        /* IBM layout as MAME builds it (flopimg.cpp build_pc_track_mfm): gap 4a 80, sync 12, IAM 4, gap 1 50 =
         * 146 bytes; a sector is 12 sync + 3 A1 before its mark, and 62 + 256 + gap 3 (50) = 368 bytes long; an
         * MFM byte is 16 cells. */
        TEST_ASSERT_EQUAL_UINT32((146u + 15u + 368u * i) * 16u, t.secs[i].pos);
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(d, t.data + t.secs[0].data_off, 256);
    TEST_ASSERT_EQUAL_HEX8(0xE5, t.data[t.secs[1].data_off + 255]);
    TEST_ASSERT_EQUAL_HEX8(0x22, t.data[t.secs[5].data_off]);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t)); /* no record: unformatted */
    TEST_ASSERT_EQUAL_UINT8(0, t.count);
}

/* One track of n sectors of 128 << code bytes, IDs 1..n, every sector compressed to one byte. */
static void imd_uniform_track(uint8_t mode, uint8_t n, uint8_t code) {
    imd_begin();
    const uint8_t hdr[] = {mode, 1, 0, n, code};
    put(hdr, 5);
    for (uint8_t i = 1; i <= n; i++)
        putb(i);
    for (uint8_t i = 1; i <= n; i++) {
        putb(2);
        putb(0xE5);
    }
}

static void test_imd_mfm_9x512_sectors_are_654_bytes_apart(void) {
    /* MAME: gap 3 = 80 for 512-byte sectors on 5.25" (calc_default_pc_gap3_size), so 62 + 512 + 80 = 654 bytes,
     * 0.10464 of a turn: the spacing measured between reads in MAME's trace of the 4MATION loader. */
    imd_uniform_track(5, 9, 2);
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 1, 0, &t));
    TEST_ASSERT_EQUAL_UINT8(9, t.count);
    for (unsigned i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_UINT32((146u + 15u + 654u * i) * 16u, t.secs[i].pos);
}

static void test_imd_fm_16x128_gap_3_shrinks_to_fit(void) {
    /* FM: gap 4a 40, sync 6, IAM 1, gap 1 26 = 73 bytes; a sector is 6 sync before its mark and 33 + 128 + gap 3.
     * 3125 bytes a turn at 125 kbit/s; 73 + 16 x 161 = 2649, so gap 3 (50) shrinks to (3125 - 2649) / 15 = 31
     * [MAME build_pc_track_fm]. An FM byte is 32 cells. */
    imd_uniform_track(2, 16, 0);
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 1, 0, &t));
    TEST_ASSERT_TRUE(t.fm);
    TEST_ASSERT_EQUAL_UINT8(16, t.count);
    for (unsigned i = 0; i < 16; i++)
        TEST_ASSERT_EQUAL_UINT32((73u + 6u + 192u * i) * 32u, t.secs[i].pos);
}

static void test_imd_maps_fm_and_size_table(void) {
    imd_begin();
    /* cylinder 0 head 1 with both maps (C 0, H 0 recorded as RM does on side 1), FM 250k, size table */
    const uint8_t hdr[] = {2, 0, 0x80 | 0x40 | 1, 2, 0xFF}, ids[] = {1, 2}, cmap[] = {0, 0}, hmap[] = {0, 0};
    const uint8_t sizes[] = {0x80, 0x00, 0x00, 0x01}; /* 128, 256 */
    put(hdr, 5);
    put(ids, 2);
    put(cmap, 2);
    put(hmap, 2);
    put(sizes, 4);
    putb(2);
    putb(0x01);
    putb(2);
    putb(0x02);
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 1, &t));
    TEST_ASSERT_TRUE(t.fm);
    TEST_ASSERT_EQUAL_UINT16(125, t.rate_kbps);
    TEST_ASSERT_EQUAL_UINT8(2, t.count);
    TEST_ASSERT_EQUAL_UINT8(0, t.secs[1].h);
    TEST_ASSERT_EQUAL_UINT16(128, t.secs[0].size);
    TEST_ASSERT_EQUAL_UINT16(256, t.secs[1].size);
    TEST_ASSERT_EQUAL_UINT8(1, t.secs[1].n);
}

static void test_imd_rejects_bad_files(void) {
    disk_imd_t img;
    imd_begin();
    file[0] = 'X';
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_imd_open(&img, mem_read, NULL, flen));
    imd_begin();
    const uint8_t hdr[] = {5, 0, 0, 1, 2}, id[] = {1};
    put(hdr, 5);
    put(id, 1);
    putb(1); /* 512 bytes promised, none present */
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_imd_open(&img, mem_read, NULL, flen));
    imd_begin();
    put(hdr, 5);
    put(id, 1);
    putb(2);
    putb(0);
    put(hdr, 5); /* the same track again */
    put(id, 1);
    putb(2);
    putb(0);
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_imd_open(&img, mem_read, NULL, flen));
    imd_begin();
    const uint8_t badmode[] = {9, 0, 0, 0, 0};
    put(badmode, 5);
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_imd_open(&img, mem_read, NULL, flen));
}

/* Three MFM tracks of 9 x 512, cylinders 0-2 on side 0, uncompressed: each track record is 5 + 9 + 9 x 513 bytes. */
#define STEP_TRACK_BYTES (5u + 9u + 9u * 513u)
static void imd_three_tracks(void) {
    imd_begin();
    for (uint8_t c = 0; c < 3; c++) {
        const uint8_t hdr[] = {5, c, 0, 9, 2};
        put(hdr, 5);
        for (uint8_t i = 1; i <= 9; i++)
            putb(i);
        for (uint8_t i = 1; i <= 9; i++) {
            putb(1);
            memset(file + flen, (int)(c * 16u + i), 512);
            flen += 512;
        }
    }
}

static void test_imd_open_in_steps_indexes_one_track_record_a_step(void) {
    imd_three_tracks();
    disk_imd_t whole, stepped;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&whole, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_begin(&stepped, mem_read, NULL, flen));
    bool done = false;
    for (unsigned step = 1; step <= 3; step++) {
        bytes_read = 0;
        TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_step(&stepped, &done));
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(STEP_TRACK_BYTES, bytes_read); /* one track record's worth, at most */
        TEST_ASSERT_EQUAL_UINT8(step, stepped.cylinders);
        TEST_ASSERT_EQUAL(step == 3, done);
    }
    TEST_ASSERT_EQUAL_UINT8(whole.cylinders, stepped.cylinders);
    TEST_ASSERT_EQUAL_UINT8(whole.sides, stepped.sides);
    TEST_ASSERT_EQUAL_MEMORY(whole.track_off, stepped.track_off, sizeof whole.track_off);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&stepped, 2, 0, &t));
    TEST_ASSERT_EQUAL_UINT8(9, t.count);
    TEST_ASSERT_EQUAL_HEX8(2u * 16u + 1u, t.data[t.secs[0].data_off]);
}

static void test_imd_open_step_after_done_stays_done(void) {
    imd_begin(); /* a header and no tracks */
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_begin(&img, mem_read, NULL, flen));
    bool done = false;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_step(&img, &done));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT8(0, img.cylinders);
    imd_three_tracks();
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, mem_read, NULL, flen));
    bytes_read = 0;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_step(&img, &done));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(0, bytes_read);
}

static void test_imd_open_step_finds_the_errors_open_finds(void) {
    disk_imd_t img;
    bool done = false;
    imd_begin();
    file[0] = 'X';
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_imd_open_begin(&img, mem_read, NULL, flen));
    imd_begin();
    const uint8_t hdr[] = {5, 0, 0, 1, 2}, id[] = {1};
    for (int k = 0; k < 2; k++) { /* the same track twice: the second step finds it */
        put(hdr, 5);
        put(id, 1);
        putb(2);
        putb(0);
    }
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_begin(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_step(&img, &done));
    TEST_ASSERT_FALSE(done);
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_imd_open_step(&img, &done));
    imd_begin();
    put(hdr, 5);
    put(id, 1);
    putb(1); /* 512 bytes promised, none present */
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open_begin(&img, mem_read, NULL, flen));
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_imd_open_step(&img, &done));
}

/* --- real discs (testdata/, never committed; missing data fails) -------------------------------- */

static uint8_t *slurp(const char *name, uint32_t *len) {
    RMFE_NEEDS_TESTDATA();
    const char *dir = getenv("RMFE_TESTDATA");
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "RMFE_TESTDATA not set");
    char path[1024];
    snprintf(path, sizeof path, "%s/images/%s", dir, name);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_size_t((size_t)n, fread(buf, 1, (size_t)n, fp));
    fclose(fp);
    *len = (uint32_t)n;
    return buf;
}

typedef struct {
    const uint8_t *p;
    uint32_t n;
} blob_t;

static bool blob_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    const blob_t *b = ctx;
    if (off > b->n || len > b->n - off) return false;
    memcpy(buf, b->p + off, len);
    return true;
}

/* The IMD and HFE of the same disc must load the same sectors, in the same order, with the same data:
 * two backends sharing no code (docs/disk-survey.md: the two images agree sector for sector). */
static void imd_and_hfe_agree(const char *stem) {
    char n1[256], n2[256];
    snprintf(n1, sizeof n1, "%s.IMD", stem);
    snprintf(n2, sizeof n2, "%s.hfe", stem);
    blob_t a, b;
    a.p = slurp(n1, &a.n);
    b.p = slurp(n2, &b.n);
    disk_imd_t imd;
    static disk_hfe_t hfe;
    static uint8_t cells[16384];
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&imd, blob_read, &a, a.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_open(&hfe, blob_read, &b, cells, sizeof cells));
    unsigned sectors = 0;
    for (unsigned c = 0; c < 40; c++) {
        for (unsigned s = 0; s < 2; s++) {
            TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&imd, c, s, &t));
            TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&hfe, c, s, &t2));
            TEST_ASSERT_EQUAL_UINT8(t.count, t2.count);
            TEST_ASSERT_EQUAL(t.fm, t2.fm);
            TEST_ASSERT_EQUAL_UINT16(t.rate_kbps, t2.rate_kbps);
            for (unsigned i = 0; i < t.count; i++) {
                TEST_ASSERT_EQUAL_UINT8(t.secs[i].r, t2.secs[i].r);
                TEST_ASSERT_EQUAL_UINT8(t.secs[i].c, t2.secs[i].c);
                TEST_ASSERT_EQUAL_UINT8(t.secs[i].h, t2.secs[i].h);
                TEST_ASSERT_EQUAL_HEX8(t.secs[i].flags, t2.secs[i].flags);
                TEST_ASSERT_EQUAL_UINT16(t.secs[i].size, t2.secs[i].size);
                TEST_ASSERT_EQUAL_HEX8_ARRAY(t.data + t.secs[i].data_off, t2.data + t2.secs[i].data_off,
                                             t.secs[i].size);
                if (i) TEST_ASSERT_TRUE(t2.secs[i].pos > t2.secs[i - 1].pos);
                sectors++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(730, sectors);
    free((void *)a.p);
    free((void *)b.p);
}

static void test_real_dragon_world_imd_and_hfe_load_the_same(void) {
    imd_and_hfe_agree("480Z_4MATION_Dragon_World_1985");
}

static void test_real_flowers_of_crystal_imd_and_hfe_load_the_same(void) {
    imd_and_hfe_agree("480Z_4MATION_Flowers_of_Crystal_1985");
}

static void test_real_rebuilt_imd_has_both_sector_08s_and_find_picks_by_rotation(void) {
    blob_t a;
    a.p = slurp("480Z_4MATION_Flowers_of_Crystal_1985_rebuilt.IMD", &a.n);
    disk_imd_t imd;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&imd, blob_read, &a, a.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&imd, 2, 0, &t));
    const uint8_t want[] = {0x05, 0x08, 0x04, 0x08, 0x03, 0x07, 0x02, 0x06, 0x01};
    TEST_ASSERT_EQUAL_UINT8(9, t.count);
    for (unsigned i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_HEX8(want[i], t.secs[i].r);
    int first = disk_track_find(&t, 8, 0), second = disk_track_find(&t, 8, t.secs[1].pos + 1);
    TEST_ASSERT_EQUAL(1, first);
    TEST_ASSERT_EQUAL(3, second);
    /* the two copies differ at byte 2 (docs/protocol.md) */
    TEST_ASSERT_NOT_EQUAL(t.data[t.secs[1].data_off + 2], t.data[t.secs[3].data_off + 2]);
    free((void *)a.p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_find_returns_the_next_sector_with_that_id_round_the_index);
    RUN_TEST(test_add_refuses_what_does_not_fit);
    RUN_TEST(test_imd_reads_every_record_type);
    RUN_TEST(test_imd_mfm_9x512_sectors_are_654_bytes_apart);
    RUN_TEST(test_imd_fm_16x128_gap_3_shrinks_to_fit);
    RUN_TEST(test_imd_maps_fm_and_size_table);
    RUN_TEST(test_imd_rejects_bad_files);
    RUN_TEST(test_imd_open_in_steps_indexes_one_track_record_a_step);
    RUN_TEST(test_imd_open_step_after_done_stays_done);
    RUN_TEST(test_imd_open_step_finds_the_errors_open_finds);
    RUN_TEST(test_real_dragon_world_imd_and_hfe_load_the_same);
    RUN_TEST(test_real_flowers_of_crystal_imd_and_hfe_load_the_same);
    RUN_TEST(test_real_rebuilt_imd_has_both_sector_08s_and_find_picks_by_rotation);
    return UNITY_END();
}
