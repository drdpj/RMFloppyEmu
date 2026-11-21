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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/disk.h"
#include "core/disk_hfe.h"
#include "core/disk_imd.h"
#include "support/testdata.h"
#include "unity.h"

/* A file in memory that can be read, written in place, and written anew through a sink. */
typedef struct {
    uint8_t *p;
    uint32_t n, cap;
} mfile_t;

static bool m_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    mfile_t *f = ctx;
    if (off > f->n || len > f->n - off) return false;
    memcpy(buf, f->p + off, len);
    return true;
}
static bool m_write(void *ctx, uint32_t off, const void *buf, uint32_t len) {
    mfile_t *f = ctx;
    if (off > f->n || len > f->n - off) return false; /* in place only: never grows */
    memcpy(f->p + off, buf, len);
    return true;
}
static bool m_sink(void *ctx, const void *buf, uint32_t len) {
    mfile_t *f = ctx;
    if (len > f->cap - f->n) return false;
    memcpy(f->p + f->n, buf, len);
    f->n += len;
    return true;
}

/* Big enough for a real disc's HFE (about 1 MB). The real discs aren't on the Pico 2 (support/testdata.h),
 * so there the buffers need only hold the synthetic images, and must fit in its RAM. */
#ifdef RMFE_TEST_ON_TARGET
#define FILE_CAP (16u * 1024u)
#else
#define FILE_CAP (1200u * 1024u)
#endif
static uint8_t abuf[FILE_CAP], bbuf[FILE_CAP], cbuf[FILE_CAP];
static mfile_t A = {abuf, 0, sizeof abuf}, B = {bbuf, 0, sizeof bbuf};
static disk_track_t t, t2;
static uint8_t sec[512];

void setUp(void) {
    A.n = B.n = 0;
}
void tearDown(void) {}

static void put(mfile_t *f, const void *p, uint32_t n) {
    TEST_ASSERT_TRUE(m_sink(f, p, n));
}
static void putb(mfile_t *f, uint8_t b) {
    put(f, &b, 1);
}

/* Cylinder 0 (two 256-byte sectors: one stored, one compressed) and cylinder 1 (one compressed). */
static void small_imd(void) {
    const char *h = "IMD 1.18: test\r\n";
    put(&A, h, (uint32_t)strlen(h));
    putb(&A, 0x1A);
    const uint8_t t0[] = {5, 0, 0, 2, 1, 1, 2};
    put(&A, t0, sizeof t0);
    putb(&A, 1);
    for (unsigned i = 0; i < 256; i++)
        putb(&A, (uint8_t)i);
    putb(&A, 2);
    putb(&A, 0xE5);
    const uint8_t t1[] = {5, 1, 0, 1, 1, 1};
    put(&A, t1, sizeof t1);
    putb(&A, 2);
    putb(&A, 0xE5);
}

static void test_track_write_stores_data_and_clears_error_flags(void) {
    disk_track_clear(&t, 0, 0);
    disk_sector_t s = {.r = 1, .n = 0, .size = 128, .flags = DISK_SEC_CRC_ERROR | DISK_SEC_DELETED};
    TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(&t, &s, NULL));
    memset(sec, 0x42, 128);
    disk_track_write(&t, 0, sec);
    TEST_ASSERT_EQUAL_HEX8(0, t.secs[0].flags);
    TEST_ASSERT_EQUAL_HEX8(0x42, t.data[127]);
}

static void test_imd_update_in_place_keeps_the_file_length(void) {
    small_imd();
    uint32_t len = A.n;
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t));
    memset(sec, 0x5A, 256);
    sec[3] = 0x01;
    disk_track_write(&t, 0, sec); /* stored sector: any data */
    memset(sec, 0x00, 256);
    disk_track_write(&t, 1, sec); /* compressed sector: still one repeated byte */
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_update(&img, m_write, &A, &t));
    TEST_ASSERT_EQUAL_UINT32(len, A.n);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t2));
    TEST_ASSERT_EQUAL_HEX8(0x01, t2.data[t2.secs[0].data_off + 3]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, t2.data[t2.secs[0].data_off + 4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, t2.data[t2.secs[1].data_off + 200]);
}

static void test_imd_update_refuses_and_changes_nothing_when_a_record_would_grow(void) {
    small_imd();
    memcpy(cbuf, abuf, A.n);
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t));
    for (unsigned i = 0; i < 256; i++)
        sec[i] = (uint8_t)(i * 7u);
    disk_track_write(&t, 0, sec);
    disk_track_write(&t, 1, sec); /* compressed sector, no longer one repeated byte */
    TEST_ASSERT_EQUAL(DISK_ERR_LAYOUT, disk_imd_update(&img, m_write, &A, &t));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(cbuf, abuf, A.n); /* not even the first sector was written */
}

static void test_imd_rewrite_replaces_one_track_and_copies_the_rest_uncompressed(void) {
    small_imd();
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t));
    for (unsigned i = 0; i < 256; i++)
        sec[i] = (uint8_t)(i * 7u);
    disk_track_write(&t, 1, sec);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_rewrite(&img, &t, m_sink, &B));
    disk_imd_t img2;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img2, m_read, &B, B.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img2, 0, 0, &t2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sec, t2.data + t2.secs[1].data_off, 256);
    TEST_ASSERT_EQUAL_HEX8(255, t2.data[t2.secs[0].data_off + 255]);
    /* Cylinder 1's record: the same 6-byte header and ID, then its compressed sector (02 E5) stored in full
     * (01 and 256 x E5), as a rewrite leaves every sector (core/disk_imd.h, owner 2026-11-21). */
    uint32_t a1 = img.track_off[1][0], b1 = img2.track_off[1][0];
    TEST_ASSERT_EQUAL_UINT32(8, A.n - a1);
    TEST_ASSERT_EQUAL_UINT32(6 + 1 + 256, B.n - b1);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(abuf + a1, bbuf + b1, 6);
    TEST_ASSERT_EQUAL_HEX8(0x01, bbuf[b1 + 6]);
    for (unsigned k = 0; k < 256; k++)
        TEST_ASSERT_EQUAL_HEX8(0xE5, bbuf[b1 + 7 + k]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(abuf, bbuf, img.first_track); /* the header byte for byte */
    /* and the changed track: both sectors stored (type 1), the second given varied data */
    uint32_t b0 = img2.track_off[0][0];
    TEST_ASSERT_EQUAL_HEX8(0x01, bbuf[b0 + 7]);
    TEST_ASSERT_EQUAL_HEX8(0x01, bbuf[b0 + 7 + 1 + 256]);
}

/* Cylinder 0: sector 1 compressed deleted (4, fill 11), 2 compressed with a CRC error (6, fill 22), 3 compressed
 * deleted with an error (8, fill 33), 4 unavailable (0), 5 stored (1). ImageDisk 1.18's data record types. */
static void typed_imd(void) {
    const char *h = "IMD 1.18: typed\r\n";
    put(&A, h, (uint32_t)strlen(h));
    putb(&A, 0x1A);
    const uint8_t t0[] = {5, 0, 0, 5, 1, 1, 2, 3, 4, 5};
    put(&A, t0, sizeof t0);
    const uint8_t recs[] = {4, 0x11, 6, 0x22, 8, 0x33, 0};
    put(&A, recs, sizeof recs);
    putb(&A, 1);
    for (unsigned i = 0; i < 256; i++)
        putb(&A, (uint8_t)~i);
    const uint8_t t1[] = {5, 1, 0, 1, 1, 1, 2, 0xE5};
    put(&A, t1, sizeof t1);
}

static void test_imd_rewrite_expands_deleted_and_error_sectors_keeping_their_type(void) {
    typed_imd();
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    disk_track_clear(&t, 1, 0); /* rewrite cylinder 1 (any change will do): cylinder 0 is the one copied */
    t.rate_kbps = 250;
    disk_sector_t s = {.c = 1, .h = 0, .r = 1, .n = 1, .size = 256};
    memset(sec, 0x44, 256);
    TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(&t, &s, sec));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_rewrite(&img, &t, m_sink, &B));
    /* cylinder 0's record: header and IDs as they were, then 3, 256 x 11; 5, 256 x 22; 7, 256 x 33; 0; 1, data */
    uint32_t at = img.track_off[0][0], hdr = 5 + 5;
    TEST_ASSERT_EQUAL_HEX8_ARRAY(abuf + at, bbuf + at, hdr);
    const uint8_t want_type[3] = {3, 5, 7}, fill[3] = {0x11, 0x22, 0x33};
    uint32_t p = at + hdr;
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_HEX8(want_type[i], bbuf[p++]);
        for (unsigned k = 0; k < 256; k++)
            TEST_ASSERT_EQUAL_HEX8(fill[i], bbuf[p++]);
    }
    TEST_ASSERT_EQUAL_HEX8(0, bbuf[p++]);
    TEST_ASSERT_EQUAL_HEX8(1, bbuf[p++]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, bbuf[p]);
    /* and it loads the same: data, deleted marks and CRC errors */
    disk_imd_t img2;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img2, m_read, &B, B.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 0, 0, &t));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img2, 0, 0, &t2));
    TEST_ASSERT_EQUAL_UINT8(t.count, t2.count);
    for (unsigned i = 0; i < t.count; i++) {
        TEST_ASSERT_EQUAL_HEX8(t.secs[i].flags, t2.secs[i].flags);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(t.data + t.secs[i].data_off, t2.data + t2.secs[i].data_off, t.secs[i].size);
    }
    TEST_ASSERT_EQUAL_HEX8(DISK_SEC_DELETED, t2.secs[0].flags);
    TEST_ASSERT_EQUAL_HEX8(DISK_SEC_CRC_ERROR, t2.secs[1].flags);
    TEST_ASSERT_EQUAL_HEX8(DISK_SEC_DELETED | DISK_SEC_CRC_ERROR, t2.secs[2].flags);
    TEST_ASSERT_EQUAL_HEX8(DISK_SEC_NO_DATA, t2.secs[3].flags);
}

static void test_imd_rewrite_adds_and_removes_tracks(void) {
    small_imd();
    disk_imd_t img;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    disk_track_clear(&t, 3, 1); /* a newly formatted track: FM 16 x 128 */
    t.fm = true;
    t.rate_kbps = 125;
    for (unsigned i = 0; i < 16; i++) {
        disk_sector_t s = {.c = 3, .h = 0 /* RM style */, .r = (uint8_t)(i + 1), .n = 0, .size = 128};
        memset(sec, 0xE5, 128);
        TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(&t, &s, sec));
    }
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_rewrite(&img, &t, m_sink, &B));
    disk_imd_t img2;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img2, m_read, &B, B.n));
    TEST_ASSERT_EQUAL_UINT8(4, img2.cylinders);
    TEST_ASSERT_EQUAL_UINT8(2, img2.sides);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img2, 3, 1, &t2));
    TEST_ASSERT_TRUE(t2.fm);
    TEST_ASSERT_EQUAL_UINT16(125, t2.rate_kbps);
    TEST_ASSERT_EQUAL_UINT8(16, t2.count);
    TEST_ASSERT_EQUAL_UINT8(0, t2.secs[15].h);
    /* and removing: an empty track drops cylinder 1 */
    mfile_t C = {cbuf, 0, sizeof cbuf};
    disk_track_clear(&t, 1, 0);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_rewrite(&img2, &t, m_sink, &C));
    disk_imd_t img3;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img3, m_read, &C, C.n));
    TEST_ASSERT_EQUAL_UINT32(0, img3.track_off[1][0]);
    TEST_ASSERT_NOT_EQUAL(0, img3.track_off[3][1]);
}

/* --- real discs (testdata/, never committed) --------------------------------------------------- */

static void load_file(const char *name, mfile_t *f) {
    RMFE_NEEDS_TESTDATA();
    const char *dir = getenv("RMFE_TESTDATA");
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "RMFE_TESTDATA not set");
    char path[1024];
    snprintf(path, sizeof path, "%s/images/%s", dir, name);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    f->n = (uint32_t)fread(f->p, 1, f->cap, fp);
    fclose(fp);
    TEST_ASSERT_TRUE(f->n > 0 && f->n < f->cap);
}

static void test_real_hfe_write_changes_one_sector_and_nothing_else(void) {
    static uint8_t orig[FILE_CAP];
    load_file("480Z_4MATION_Dragon_World_1985.hfe", &A);
    memcpy(orig, abuf, A.n);
    mfile_t O = {orig, A.n, A.n};
    static disk_hfe_t img, ref;
    static uint8_t cells[16384], cells2[16384];
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_open(&img, m_read, &A, cells, sizeof cells));
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&img, 5, 1, &t));
    int i3 = disk_track_find(&t, 3, 0);
    TEST_ASSERT_TRUE(i3 >= 0);
    for (unsigned k = 0; k < 512; k++)
        sec[k] = (uint8_t)(k ^ 0xA5u);
    disk_track_write(&t, (unsigned)i3, sec);
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_save(&img, m_write, &A, &t));
    /* Every track of the written image decodes the same as the original, except that sector. */
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_open(&ref, m_read, &O, cells2, sizeof cells2));
    for (unsigned c = 0; c < 40; c++) {
        for (unsigned s = 0; s < 2; s++) {
            TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&img, c, s, &t));
            TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&ref, c, s, &t2));
            TEST_ASSERT_EQUAL_UINT8(t2.count, t.count);
            for (unsigned i = 0; i < t.count; i++) {
                TEST_ASSERT_EQUAL_UINT8(t2.secs[i].r, t.secs[i].r);
                TEST_ASSERT_EQUAL_HEX8(0, t.secs[i].flags);
                TEST_ASSERT_UINT32_WITHIN(20, t2.secs[i].pos, t.secs[i].pos);
                const uint8_t *want = (c == 5 && s == 1 && t.secs[i].r == 3) ? sec : t2.data + t2.secs[i].data_off;
                TEST_ASSERT_EQUAL_HEX8_ARRAY(want, t.data + t.secs[i].data_off, t.secs[i].size);
            }
        }
    }
}

static void test_real_hfe_fm_track_0_round_trips(void) {
    load_file("480Z_4MATION_Flowers_of_Crystal_1985.hfe", &A);
    static disk_hfe_t img;
    static uint8_t cells[16384];
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_open(&img, m_read, &A, cells, sizeof cells));
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&img, 0, 0, &t));
    TEST_ASSERT_TRUE(t.fm);
    memcpy(&t2, &t, sizeof t);
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_save(&img, m_write, &A, &t)); /* unchanged data, re-encoded */
    TEST_ASSERT_EQUAL(DISK_OK, disk_hfe_load(&img, 0, 0, &t));
    TEST_ASSERT_TRUE(t.fm);
    TEST_ASSERT_EQUAL_UINT8(16, t.count);
    for (unsigned i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_UINT8(t2.secs[i].r, t.secs[i].r);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(t2.data + t2.secs[i].data_off, t.data + t.secs[i].data_off, 128);
    }
}

static void test_real_imd_rewrite_round_trips_the_whole_disc(void) {
    load_file("480Z_4MATION_Dragon_World_1985.IMD", &A);
    disk_imd_t img, img2;
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img, m_read, &A, A.n));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, 12, 0, &t));
    for (unsigned k = 0; k < 512; k++)
        sec[k] = (uint8_t)k;
    disk_track_write(&t, 4, sec);
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_rewrite(&img, &t, m_sink, &B));
    TEST_ASSERT_EQUAL(DISK_OK, disk_imd_open(&img2, m_read, &B, B.n));
    for (unsigned c = 0; c < 40; c++) {
        for (unsigned s = 0; s < 2; s++) {
            TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img, c, s, &t));
            TEST_ASSERT_EQUAL(DISK_OK, disk_imd_load(&img2, c, s, &t2));
            TEST_ASSERT_EQUAL_UINT8(t.count, t2.count);
            for (unsigned i = 0; i < t.count; i++) {
                TEST_ASSERT_EQUAL_UINT8(t.secs[i].r, t2.secs[i].r);
                TEST_ASSERT_EQUAL_HEX8(t.secs[i].flags, t2.secs[i].flags);
                const uint8_t *want = (c == 12 && s == 0 && i == 4) ? sec : t.data + t.secs[i].data_off;
                TEST_ASSERT_EQUAL_HEX8_ARRAY(want, t2.data + t2.secs[i].data_off, t.secs[i].size);
            }
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_track_write_stores_data_and_clears_error_flags);
    RUN_TEST(test_imd_update_in_place_keeps_the_file_length);
    RUN_TEST(test_imd_update_refuses_and_changes_nothing_when_a_record_would_grow);
    RUN_TEST(test_imd_rewrite_replaces_one_track_and_copies_the_rest_uncompressed);
    RUN_TEST(test_imd_rewrite_expands_deleted_and_error_sectors_keeping_their_type);
    RUN_TEST(test_imd_rewrite_adds_and_removes_tracks);
    RUN_TEST(test_real_hfe_write_changes_one_sector_and_nothing_else);
    RUN_TEST(test_real_hfe_fm_track_0_round_trips);
    RUN_TEST(test_real_imd_rewrite_round_trips_the_whole_disc);
    return UNITY_END();
}
