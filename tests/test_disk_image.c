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
#include "core/disk_image.h"

#include <string.h>

#include "unity.h"

/*
 * An image file in memory, with the replacement file an IMD rewrite goes through, and switches to make each of
 * the replacement's steps fail.
 */
typedef struct {
    uint8_t data[8192];
    uint32_t n;
    uint8_t repl[8192];
    uint32_t repl_n;
    bool repl_open, fail_begin, fail_sink, fail_commit;
    unsigned writes, begins, commits, aborts;
} fake_file_t;

static fake_file_t F;
static uint8_t before[8192];
static disk_track_t t, t2;
static uint8_t sec[512];

static bool f_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    fake_file_t *f = ctx;
    if (off > f->n || len > f->n - off) return false;
    memcpy(buf, f->data + off, len);
    return true;
}
static bool f_write(void *ctx, uint32_t off, const void *buf, uint32_t len) {
    fake_file_t *f = ctx;
    if (off > f->n || len > f->n - off) return false;
    memcpy(f->data + off, buf, len);
    f->writes++;
    return true;
}
static bool f_begin(void *ctx) {
    fake_file_t *f = ctx;
    f->begins++;
    if (f->fail_begin) return false;
    f->repl_open = true;
    f->repl_n = 0;
    return true;
}
static bool f_sink(void *ctx, const void *buf, uint32_t len) {
    fake_file_t *f = ctx;
    TEST_ASSERT_TRUE_MESSAGE(f->repl_open, "sink without replace_begin");
    if (f->fail_sink || len > sizeof f->repl - f->repl_n) return false;
    memcpy(f->repl + f->repl_n, buf, len);
    f->repl_n += len;
    return true;
}
static bool f_commit(void *ctx, uint32_t *new_len) {
    fake_file_t *f = ctx;
    TEST_ASSERT_TRUE_MESSAGE(f->repl_open, "commit without replace_begin");
    f->repl_open = false;
    if (f->fail_commit) return false; /* the platform keeps the old file */
    memcpy(f->data, f->repl, f->repl_n);
    f->n = f->repl_n;
    *new_len = f->n;
    f->commits++;
    return true;
}
static void f_abort(void *ctx) {
    fake_file_t *f = ctx;
    f->repl_open = false;
    f->aborts++;
}

static const disk_image_io_t io = {&F, f_read, f_write, f_begin, f_sink, f_commit, f_abort};
static const disk_image_io_t io_no_replace = {&F, f_read, f_write, NULL, NULL, NULL, NULL};

void setUp(void) {
    memset(&F, 0, sizeof F);
}
void tearDown(void) {}

static void put(const void *p, uint32_t n) {
    TEST_ASSERT_TRUE(n <= sizeof F.data - F.n);
    memcpy(F.data + F.n, p, n);
    F.n += n;
}
static void putb(uint8_t b) {
    put(&b, 1);
}

/* The IMD of test_disk_write.c: cylinder 0 (two 256-byte sectors: one stored, one compressed to E5) and
 * cylinder 1 (one compressed). */
static void small_imd(void) {
    const char *h = "IMD 1.18: test\r\n";
    put(h, (uint32_t)strlen(h));
    putb(0x1A);
    const uint8_t t0[] = {5, 0, 0, 2, 1, 1, 2};
    put(t0, sizeof t0);
    putb(1);
    for (unsigned i = 0; i < 256; i++)
        putb((uint8_t)i);
    putb(2);
    putb(0xE5);
    const uint8_t t1[] = {5, 1, 0, 1, 1, 1};
    put(t1, sizeof t1);
    putb(2);
    putb(0xE5);
}

static void open_small_imd(disk_image_t *im, const disk_image_io_t *with, bool read_only) {
    small_imd();
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_open(im, DISK_IMAGE_IMD, with, F.n, read_only, NULL, 0));
}

/* Cylinder 0 with its compressed sector given varied data: no longer one byte, so its record must grow. */
static void grow_cylinder_0(disk_image_t *im) {
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(im, 0, 0, &t));
    for (unsigned i = 0; i < 256; i++)
        sec[i] = (uint8_t)(i * 7u);
    disk_track_write(&t, 1, sec);
}

/* --- kinds ----------------------------------------------------------------------------------------- */

static void test_kind_comes_from_the_extension_in_any_case(void) {
    disk_image_kind_t k;
    TEST_ASSERT_TRUE(disk_image_kind_from_name("SYSTEM.IMD", &k));
    TEST_ASSERT_EQUAL(DISK_IMAGE_IMD, k);
    TEST_ASSERT_TRUE(disk_image_kind_from_name("a.imd", &k));
    TEST_ASSERT_EQUAL(DISK_IMAGE_IMD, k);
    TEST_ASSERT_TRUE(disk_image_kind_from_name("games/Dragon.Hfe", &k));
    TEST_ASSERT_EQUAL(DISK_IMAGE_HFE, k);
}

static void test_other_names_are_not_images(void) {
    disk_image_kind_t k;
    TEST_ASSERT_FALSE(disk_image_kind_from_name("IDC.ROM", &k));
    TEST_ASSERT_FALSE(disk_image_kind_from_name("imd", &k));
    TEST_ASSERT_FALSE(disk_image_kind_from_name("a.imd.txt", &k));
    TEST_ASSERT_FALSE(disk_image_kind_from_name("a.im", &k));
    TEST_ASSERT_FALSE(disk_image_kind_from_name("a.imdx", &k));
    TEST_ASSERT_FALSE(disk_image_kind_from_name("", &k));
}

/* --- IMD ------------------------------------------------------------------------------------------- */

static void test_imd_opens_with_its_cylinders_and_loads_tracks(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    TEST_ASSERT_EQUAL_UINT(2, disk_image_cylinders(&im));
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 0, 0, &t));
    TEST_ASSERT_EQUAL_UINT8(2, t.count);
    TEST_ASSERT_EQUAL_HEX8(200, t.data[t.secs[0].data_off + 200]);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 5, 0, &t)); /* beyond the image */
    TEST_ASSERT_EQUAL_UINT8(0, t.count);
}

static void test_imd_save_goes_in_place_when_every_record_keeps_its_length(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    uint32_t len = F.n;
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 0, 0, &t));
    memset(sec, 0x5A, 256);
    disk_track_write(&t, 0, sec); /* the stored sector: any data fits */
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(0, F.begins);
    TEST_ASSERT_TRUE(F.writes > 0);
    TEST_ASSERT_EQUAL_UINT32(len, F.n);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 0, 0, &t2));
    TEST_ASSERT_EQUAL_HEX8(0x5A, t2.data[t2.secs[0].data_off + 100]);
}

static void test_imd_save_that_grows_a_record_replaces_the_file_and_reopens_it(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    uint32_t len = F.n;
    grow_cylinder_0(&im);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(1, F.begins);
    TEST_ASSERT_EQUAL_UINT(1, F.commits);
    TEST_ASSERT_EQUAL_UINT(0, F.writes); /* nothing was written over the old file */
    /* The whole image is now uncompressed (core/disk_imd.h): cylinder 0's changed sector and cylinder 1's
     * untouched one each went from one fill byte to 256 data bytes. */
    TEST_ASSERT_EQUAL_UINT32(len + 255 + 255, F.n);
    /* The image now reads the new file: the change, and the untouched cylinder 1. */
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 0, 0, &t2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sec, t2.data + t2.secs[1].data_off, 256);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 1, 0, &t2));
    TEST_ASSERT_EQUAL_UINT8(1, t2.count);
    TEST_ASSERT_EQUAL_HEX8(0xE5, t2.data[t2.secs[0].data_off + 255]);
}

static void test_after_one_rewrite_a_write_that_would_grow_a_record_goes_in_place(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    grow_cylinder_0(&im);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(1, F.begins);
    uint32_t len = F.n;
    /* cylinder 1's sector was compressed (E5): varied data no longer needs a rewrite */
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 1, 0, &t));
    for (unsigned i = 0; i < 256; i++)
        sec[i] = (uint8_t)(i * 3u);
    disk_track_write(&t, 0, sec);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(1, F.begins);
    TEST_ASSERT_TRUE(F.writes > 0);
    TEST_ASSERT_EQUAL_UINT32(len, F.n);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 1, 0, &t2));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sec, t2.data + t2.secs[0].data_off, 256);
}

static void check_untouched(disk_image_t *im, uint32_t len) {
    TEST_ASSERT_EQUAL_UINT32(len, F.n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, F.data, len);
    TEST_ASSERT_EQUAL_UINT(0, F.writes);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(im, 0, 0, &t2)); /* and the image still reads it */
    TEST_ASSERT_EQUAL_HEX8(0xE5, t2.data[t2.secs[1].data_off + 9]);
}

static void test_a_failed_replacement_write_is_aborted_and_the_image_is_untouched(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    uint32_t len = F.n;
    memcpy(before, F.data, len);
    grow_cylinder_0(&im);
    F.fail_sink = true;
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(1, F.aborts);
    TEST_ASSERT_EQUAL_UINT(0, F.commits);
    check_untouched(&im, len);
}

static void test_a_replacement_that_cannot_start_writes_nothing(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    uint32_t len = F.n;
    memcpy(before, F.data, len);
    grow_cylinder_0(&im);
    F.fail_begin = true;
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(0, F.commits);
    check_untouched(&im, len);
}

static void test_a_failed_commit_is_an_io_error_and_the_old_file_stays(void) {
    disk_image_t im;
    open_small_imd(&im, &io, false);
    uint32_t len = F.n;
    memcpy(before, F.data, len);
    grow_cylinder_0(&im);
    F.fail_commit = true;
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_image_save(&im, &t));
    check_untouched(&im, len);
}

static void test_without_a_way_to_replace_the_file_a_growing_save_is_an_io_error(void) {
    disk_image_t im;
    open_small_imd(&im, &io_no_replace, false);
    uint32_t len = F.n;
    memcpy(before, F.data, len);
    grow_cylinder_0(&im);
    TEST_ASSERT_EQUAL(DISK_ERR_IO, disk_image_save(&im, &t));
    check_untouched(&im, len);
}

static void test_a_write_protected_image_refuses_saves_and_writes_nothing(void) {
    disk_image_t im;
    open_small_imd(&im, &io, true);
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_load(&im, 0, 0, &t));
    memset(sec, 0x5A, 256);
    disk_track_write(&t, 0, sec);
    TEST_ASSERT_EQUAL(DISK_ERR_READ_ONLY, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(0, F.writes);
    TEST_ASSERT_EQUAL_UINT(0, F.begins);
}

static void test_a_bad_imd_is_a_format_error(void) {
    disk_image_t im;
    put("not an image", 12);
    TEST_ASSERT_EQUAL(DISK_ERR_FORMAT, disk_image_open(&im, DISK_IMAGE_IMD, &io, F.n, false, NULL, 0));
}

/* --- HFE (its loading and saving are tested in test_hfe.c and test_disk_write.c) -------------------- */

/* A v1 header (HFE format, HxC): 40 cylinders, 2 sides, MFM at 250 kbit/s, the track list at block 1. */
static void hfe_header(void) {
    memset(F.data, 0xFF, 1024);
    memcpy(F.data, "HXCPICFE", 8);
    F.data[0x08] = 0;    /* revision */
    F.data[0x09] = 40;   /* cylinders */
    F.data[0x0A] = 2;    /* sides */
    F.data[0x0B] = 0x00; /* ISO/IBM MFM */
    F.data[0x0C] = 0xFA; /* 250 kbit/s */
    F.data[0x0D] = 0x00;
    F.data[0x0E] = 0x00;
    F.data[0x0F] = 0x00;
    F.data[0x10] = 0x07;
    F.data[0x11] = 0x00;
    F.data[0x12] = 0x01; /* track list at block 1 */
    F.data[0x13] = 0x00;
    F.n = 1024;
}

static void test_hfe_opens_with_the_headers_cylinders(void) {
    static uint8_t cells[16384];
    disk_image_t im;
    hfe_header();
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_open(&im, DISK_IMAGE_HFE, &io, F.n, false, cells, sizeof cells));
    TEST_ASSERT_EQUAL_UINT(40, disk_image_cylinders(&im));
}

static void test_a_write_protected_hfe_refuses_saves(void) {
    static uint8_t cells[16384];
    disk_image_t im;
    hfe_header();
    TEST_ASSERT_EQUAL(DISK_OK, disk_image_open(&im, DISK_IMAGE_HFE, &io, F.n, true, cells, sizeof cells));
    disk_track_clear(&t, 5, 0);
    TEST_ASSERT_EQUAL(DISK_ERR_READ_ONLY, disk_image_save(&im, &t));
    TEST_ASSERT_EQUAL_UINT(0, F.writes);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_kind_comes_from_the_extension_in_any_case);
    RUN_TEST(test_other_names_are_not_images);
    RUN_TEST(test_imd_opens_with_its_cylinders_and_loads_tracks);
    RUN_TEST(test_imd_save_goes_in_place_when_every_record_keeps_its_length);
    RUN_TEST(test_imd_save_that_grows_a_record_replaces_the_file_and_reopens_it);
    RUN_TEST(test_after_one_rewrite_a_write_that_would_grow_a_record_goes_in_place);
    RUN_TEST(test_a_failed_replacement_write_is_aborted_and_the_image_is_untouched);
    RUN_TEST(test_a_replacement_that_cannot_start_writes_nothing);
    RUN_TEST(test_a_failed_commit_is_an_io_error_and_the_old_file_stays);
    RUN_TEST(test_without_a_way_to_replace_the_file_a_growing_save_is_an_io_error);
    RUN_TEST(test_a_write_protected_image_refuses_saves_and_writes_nothing);
    RUN_TEST(test_a_bad_imd_is_a_format_error);
    RUN_TEST(test_hfe_opens_with_the_headers_cylinders);
    RUN_TEST(test_a_write_protected_hfe_refuses_saves);
    return UNITY_END();
}
