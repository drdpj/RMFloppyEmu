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
#include <string.h>

#include "core/idc.h"
#include "support/fake480z.h"
#include "unity.h"

/*
 * Two in-memory discs shaped like the RM 480Z system disc: 40 cylinders, two sides, track 0 FM 16 x 128,
 * the rest MFM 9 x 512. Every data byte says where it is: drive, cylinder, side, sector and offset.
 *
 * A track is made from that pattern when it is needed, and only the tracks a test changes (directly, or
 * through f_save) are kept, so the fixture fits in the Pico 2's RAM: these tests run there too
 * (tests/pico/). No test changes more than a few tracks.
 */
#define CYLS        40u
#define KEPT_TRACKS 8u
static disk_track_t kept[KEPT_TRACKS];
static unsigned kept_where[KEPT_TRACKS][3]; /* drive, cylinder, side of kept[i] */
static unsigned nkept;
static bool present[IDC_PHYS_DRIVES], read_only[IDC_PHYS_DRIVES];
static uint32_t rot; /* the rotational position the tests choose */
static unsigned saves;

static uint8_t pattern(unsigned drv, unsigned c, unsigned s, unsigned r, unsigned k) {
    return (uint8_t)(drv * 101u + c * 7u + s * 50u + r * 13u + k);
}

static void make_track(disk_track_t *t, unsigned drv, unsigned c, unsigned s) {
    disk_track_clear(t, (uint8_t)c, (uint8_t)s);
    bool fm = c == 0;
    t->fm = fm;
    t->rate_kbps = fm ? 125 : 250;
    unsigned n = fm ? 16 : 9, size = fm ? 128 : 512;
    for (unsigned i = 0; i < n; i++) {
        uint8_t buf[512];
        for (unsigned k = 0; k < size; k++)
            buf[k] = pattern(drv, c, s, i + 1, k);
        disk_sector_t sec = {.c = (uint8_t)c,
                             .h = 0,
                             .r = (uint8_t)(i + 1),
                             .n = fm ? 0 : 2,
                             .size = (uint16_t)size,
                             .pos = i * DISK_CELLS_PER_REV / n};
        TEST_ASSERT_EQUAL(DISK_OK, disk_track_add(t, &sec, buf));
    }
}

static disk_track_t *find_kept(unsigned drv, unsigned c, unsigned s) {
    for (unsigned i = 0; i < nkept; i++) {
        if (kept_where[i][0] == drv && kept_where[i][1] == c && kept_where[i][2] == s) return &kept[i];
    }
    return NULL;
}

/* The disc's track: kept from now on, so a test can change it and see what the commands did to it. */
static disk_track_t *disc(unsigned drv, unsigned c, unsigned s) {
    TEST_ASSERT_TRUE(drv < IDC_PHYS_DRIVES && c < CYLS && s < 2);
    disk_track_t *t = find_kept(drv, c, s);
    if (t) return t;
    TEST_ASSERT_TRUE_MESSAGE(nkept < KEPT_TRACKS, "fixture: the test changes more tracks than KEPT_TRACKS");
    kept_where[nkept][0] = drv;
    kept_where[nkept][1] = c;
    kept_where[nkept][2] = s;
    t = &kept[nkept++];
    make_track(t, drv, c, s);
    return t;
}

static bool f_present(void *ctx, unsigned drv) {
    (void)ctx;
    return present[drv];
}
static unsigned f_cyls(void *ctx, unsigned drv) {
    (void)ctx;
    (void)drv;
    return CYLS;
}
static disk_status_t f_load(void *ctx, unsigned drv, unsigned c, unsigned s, disk_track_t *t) {
    (void)ctx;
    if (c >= CYLS) { /* past the disc's last cylinder: blank, as the image backends load it */
        disk_track_clear(t, (uint8_t)c, (uint8_t)s);
        return DISK_OK;
    }
    const disk_track_t *k = find_kept(drv, c, s);
    if (k) {
        memcpy(t, k, sizeof *t);
    } else {
        make_track(t, drv, c, s);
    }
    return DISK_OK;
}
static disk_status_t f_save(void *ctx, unsigned drv, const disk_track_t *t) {
    (void)ctx;
    if (read_only[drv]) return DISK_ERR_READ_ONLY;
    memcpy(disc(drv, t->cyl, t->side), t, sizeof *t);
    saves++;
    return DISK_OK;
}
static uint32_t f_position(void *ctx, unsigned drv) {
    (void)ctx;
    (void)drv;
    return rot;
}

static const idc_drives_t drives = {NULL, f_present, f_cyls, f_load, f_save, f_position};
static fake480z_t f;
static idc_t d;
static fake_in_t script[2048];
static idc_drive_type_t type_a; /* drive A's type for run() */

void setUp(void) {
    nkept = 0;
    present[0] = present[1] = true;
    read_only[0] = read_only[1] = false;
    rot = 0;
    saves = 0;
    type_a = IDC_DRIVE_FOLLOW_IMAGE;
}
void tearDown(void) {}

static void run(size_t n) {
    fake480z_init(&f, script, n);
    idc_init(&d, &f.io, NULL, 0);
    idc_set_drives(&d, &drives);
    idc_set_drive_type(&d, 0, type_a);
    idc_run(&d);
}

static size_t cmd(size_t at, uint8_t c, uint8_t a, uint8_t b, uint8_t e) {
    script[at++] = (fake_in_t)FB(c);
    script[at++] = (fake_in_t)FB(a);
    script[at++] = (fake_in_t)FB(b);
    script[at++] = (fake_in_t)FB(e);
    return at;
}

static void expect_record(size_t at, unsigned drv, unsigned c, unsigned s, unsigned sector, unsigned offset) {
    for (unsigned k = 0; k < 128; k++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(pattern(drv, c, s, sector, offset + k), f.out[at + k], "record byte");
    }
}

static void test_login_ready_and_not_ready(void) {
    present[1] = false;
    size_t n = 0;
    script[n++] = (fake_in_t)FB(0x01);
    script[n++] = (fake_in_t)FB(0x00); /* drive A */
    script[n++] = (fake_in_t)FB(0x01);
    script[n++] = (fake_in_t)FB(0x01); /* drive B: raw 01 is drive 1 side 0 */
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, IDC_STATUS_NOT_READY};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_read_mfm_record_maps_to_sector_and_offset(void) {
    /* record 7: logical sector 1, which the 2:1 skew puts at ID 6 [IDC3 09c7; MAME boot trace], bytes 256-383 */
    size_t n = cmd(0, 0x02, 0x20, 5, 7);
    script[n++] = (fake_in_t)FB(0x00); /* ack */
    run(n);
    TEST_ASSERT_EQUAL_size_t(3 + 128, f.nout);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.out[0]); /* accept */
    TEST_ASSERT_EQUAL_HEX8(0x00, f.out[1]); /* status */
    TEST_ASSERT_EQUAL_HEX8(0x00, f.out[2]); /* result */
    expect_record(3, 0, 5, 0, 6, 256);
}

static void test_read_side_1_and_fm_track_0(void) {
    size_t n = cmd(0, 0x02, 0x22, 30, 36); /* side 1: raw 22 -> 21; last record: logical sector 8 = ID 5, offset 384 */
    script[n++] = (fake_in_t)FB(0x00);
    n = cmd(n, 0x02, 0x00, 0, 16); /* FM track 0, record 16 = sector 16 */
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    TEST_ASSERT_EQUAL_size_t(2 * (3 + 128), f.nout);
    expect_record(3, 0, 30, 1, 5, 384);
    expect_record(3 + 128 + 3, 0, 0, 0, 16, 0);
}

static void test_cpm_drives_c_and_d_are_the_second_sides_of_drives_a_and_b(void) {
    /* Each side is a CP/M drive: the 480Z's low two bits are A: 00, B: 01, C: 10, D: 11, which the IDC's parity swap
     * makes drive 0 side 0, drive 1 side 0, drive 0 side 1, drive 1 side 1 (Link 480Z Disc System User Guide,
     * pp. 5.5-5.7; docs/protocol.md "The drive byte"). Each side has its own FM track 0. */
    size_t n = cmd(0, 0x02, 0x02, 0, 5); /* C:, track 0 FM, record 5 = sector 5 */
    script[n++] = (fake_in_t)FB(0x00);
    n = cmd(n, 0x02, 0x03, 0, 16); /* D:, track 0 FM, record 16 = sector 16 */
    script[n++] = (fake_in_t)FB(0x00);
    n = cmd(n, 0x02, 0x23, 7, 7); /* D:, track 7 MFM, record 7: logical sector 1 = ID 6, offset 256 */
    script[n++] = (fake_in_t)FB(0x00);
    n = cmd(n, 0x02, 0x21, 7, 7); /* B: the same record on the first side */
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    TEST_ASSERT_EQUAL_size_t(4 * (3 + 128), f.nout);
    expect_record(3, 0, 0, 1, 5, 0);
    expect_record(3 + 131, 1, 0, 1, 16, 0);
    expect_record(3 + 2 * 131, 1, 7, 1, 6, 256);
    expect_record(3 + 3 * 131, 1, 7, 0, 6, 256);
}

static void test_wrong_density_is_record_not_found(void) {
    cmd(0, 0x02, 0x20, 0, 1); /* MFM asked of the FM track 0 */
    run(4);
    const uint8_t want[] = {0x00, 0x00, IDC_STATUS_NOT_FOUND};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_size_t(3, f.nout);
}

static void test_bad_track_record_and_drive(void) {
    present[1] = false;
    size_t n = cmd(0, 0x02, 0x20, 40, 1); /* track 40 of 40 */
    n = cmd(n, 0x02, 0x20, 5, 0);         /* record 0 */
    n = cmd(n, 0x02, 0x20, 5, 37);        /* record 37 of 36 */
    n = cmd(n, 0x02, 0x21, 5, 1);         /* drive B, absent */
    run(n);
    const uint8_t want[] = {0x00, 0x20, 0x00, 0x20, 0x00, 0x20, 0x00, IDC_STATUS_NOT_READY};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_an_80_track_drive_takes_tracks_past_a_40_track_disc(void) {
    /* The IDC's limit is the drive's: 50h tracks once it has found an 80-track drive [IDC3 028b, 0f14]. In
     * MAME, FORMAT's quad density formats tracks 28h-4Fh of a 40-track image. Track 4F reads as blank (10);
     * 50 is out of range (20). */
    type_a = IDC_DRIVE_80_TRACK;
    size_t n = cmd(0, 0x02, 0x20, 0x4F, 1);
    n = cmd(n, 0x02, 0x20, 0x50, 1);
    run(n);
    const uint8_t want[] = {0x00, 0x00, IDC_STATUS_NOT_FOUND, 0x00, IDC_STATUS_BAD_PARAM};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_write_goes_to_the_cache_and_reaches_the_disc_on_flush(void) {
    size_t n = cmd(0, 0x04, 0x20, 9, 6); /* logical sector 1 = ID 6 (index 5), offset 128 */
    uint8_t rec[128];
    memset(rec, 0xAB, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    n = cmd(n, 0x02, 0x20, 9, 6); /* read it back (from the cache) */
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.out[2]); /* block ack */
    TEST_ASSERT_EQUAL_HEX8(0x00, f.out[3]); /* write result */
    TEST_ASSERT_EQUAL_HEX8(0xAB, f.out[4 + 3]);
    TEST_ASSERT_EQUAL_UINT(0, saves); /* not yet on the disc */
    TEST_ASSERT_EQUAL_HEX8(pattern(0, 9, 0, 6, 128), disc(0, 9, 0)->data[disc(0, 9, 0)->secs[5].data_off + 128]);

    /* flush (08 20, as CP/M sends it) */
    script[0] = (fake_in_t)FB(0x08);
    script[1] = (fake_in_t)FB(0x20);
    fake480z_init(&f, script, 2);
    idc_run(&d);
    const uint8_t want[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
    TEST_ASSERT_EQUAL_UINT(1, saves);
    const disk_track_t *t = disc(0, 9, 0);
    TEST_ASSERT_EQUAL_HEX8(0xAB, t->data[t->secs[5].data_off + 128]);
    TEST_ASSERT_EQUAL_HEX8(pattern(0, 9, 0, 6, 0), t->data[t->secs[5].data_off]); /* rest of the sector kept */
    TEST_ASSERT_EQUAL_HEX8(pattern(0, 9, 0, 6, 300), t->data[t->secs[5].data_off + 300]);
}

static void test_moving_to_another_track_writes_the_changed_one_back(void) {
    size_t n = cmd(0, 0x04, 0x20, 9, 1);
    uint8_t rec[128];
    memset(rec, 0x11, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    n = cmd(n, 0x02, 0x20, 10, 1);
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    TEST_ASSERT_EQUAL_UINT(1, saves);
    TEST_ASSERT_EQUAL_HEX8(0x11, disc(0, 9, 0)->data[disc(0, 9, 0)->secs[0].data_off]);
}

static void test_write_protected_disc_reports_41_on_flush(void) {
    read_only[0] = true;
    size_t n = cmd(0, 0x04, 0x20, 9, 1);
    uint8_t rec[128];
    memset(rec, 0x22, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    script[n++] = (fake_in_t)FB(0x08);
    script[n++] = (fake_in_t)FB(0x20);
    run(n);
    /* accept, status, block ack, result, then flush: accept, status, result 41: write protect, with bit 0 for
     * an error writing the cache back [IDC3 109f; MAME trace write_protect: <08 >00 <21 >00 >41] */
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_WRITE_PROTECT | IDC_STATUS_WRITE_BACK};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_a_failed_write_back_loses_the_change(void) {
    /* The IDC marks the buffer clean even when writing it fails [IDC3 1198]; the MAME trace write_protect then
     * reads the disc's own data back. */
    read_only[0] = true;
    size_t n = cmd(0, 0x04, 0x20, 9, 1);
    uint8_t rec[128];
    memset(rec, 0x22, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    script[n++] = (fake_in_t)FB(0x08);
    script[n++] = (fake_in_t)FB(0x20);
    script[n++] = (fake_in_t)FB(0x08); /* nothing is left to write: 00 */
    script[n++] = (fake_in_t)FB(0x20);
    n = cmd(n, 0x02, 0x20, 9, 1);
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_WRITE_PROTECT | IDC_STATUS_WRITE_BACK,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof want + 128, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    expect_record(sizeof want, 0, 9, 0, 1, 0);
}

static void test_a_write_back_error_on_a_track_change_is_the_reads_result(void) {
    /* A read needing the buffer writes the old sector back first [IDC3 10ba -> 10c5]; that happens after the
     * parameter status is sent [IDC3 0f2e], so the error, with bit 0 [109f], is the result [0b36]. */
    read_only[0] = true;
    size_t n = cmd(0, 0x04, 0x20, 9, 1);
    uint8_t rec[128];
    memset(rec, 0x33, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    n = cmd(n, 0x02, 0x20, 10, 1);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_WRITE_PROTECT | IDC_STATUS_WRITE_BACK};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

/* Writes records first..last of a track (MFM, drive A), each 128 bytes of fill. */
static size_t write_records(size_t n, uint8_t track, uint8_t first, uint8_t last, uint8_t fill) {
    uint8_t rec[128];
    memset(rec, fill, sizeof rec);
    for (unsigned r = first; r <= last; r++) {
        n = cmd(n, 0x04, 0x20, track, (uint8_t)r);
        n = fake_bytes(script, n, rec, 128);
    }
    return n;
}

static void test_a_write_completing_a_sector_writes_it_at_once(void) {
    /* The fourth record of a 512-byte buffer makes the IDC write the sector there and then [IDC3 10a6-10b2 ->
     * 1170]; the MAME traces show the delay of the physical write after the fourth 04 of a SAVE. */
    size_t n = write_records(0, 9, 1, 4, 0x5A); /* logical sector 0 = ID 1 */
    run(n);
    TEST_ASSERT_EQUAL_size_t(4 * 4, f.nout);
    for (unsigned i = 0; i < 16; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, f.out[i]);
    TEST_ASSERT_EQUAL_UINT(1, saves);
    const disk_track_t *t = disc(0, 9, 0);
    TEST_ASSERT_EQUAL_HEX8(0x5A, t->data[t->secs[0].data_off + 511]);
}

static void test_a_write_completing_a_sector_on_a_protected_disc_is_40(void) {
    /* The error of that write is the 04's own result, without the write-back bit [IDC3 1170 -> 0b53 0eb0]. */
    read_only[0] = true;
    size_t n = write_records(0, 9, 1, 4, 0x5A);
    script[n++] = (fake_in_t)FB(0x08); /* nothing is left to write */
    script[n++] = (fake_in_t)FB(0x20);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_WRITE_PROTECT,
                            0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_a_write_to_a_missing_sector_fails_when_the_sector_is_complete(void) {
    /* MAME trace (system disc with cylinder 33 side 0 sector 4 removed, SAVE 2): <04 ... 21 19..1B> each 00 00,
     * then the fourth, 21 1C, answers 00 (block) and 10. */
    disc(0, 9, 0)->secs[0].r = 0x55; /* no sector 1 */
    size_t n = write_records(0, 9, 1, 4, 0x5A);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_NOT_FOUND};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    TEST_ASSERT_EQUAL_UINT(0, saves);
}

static void test_a_partial_write_to_a_missing_sector_fails_on_flush_with_bit_0(void) {
    /* Writing back a part-written buffer reads the sector first [IDC3 112a -> 1137]; that fails, and 08 adds
     * bit 0 [1088 -> 109f]. */
    disc(0, 9, 0)->secs[0].r = 0x55;
    size_t n = write_records(0, 9, 1, 2, 0x5A);
    script[n++] = (fake_in_t)FB(0x08);
    script[n++] = (fake_in_t)FB(0x20);
    run(n);
    const uint8_t want[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_NOT_FOUND | IDC_STATUS_WRITE_BACK};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_drive_map_from_05_swaps_drives(void) {
    size_t n = 0;
    script[n++] = (fake_in_t)FB(0x05);
    script[n++] = (fake_in_t)FB(0x01); /* raw 01 -> 02: logical 0/1 <-> 2/3, i.e. drive A <-> drive B */
    n = cmd(n, 0x02, 0x20, 5, 1);
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    expect_record(2 + 3, 1, 5, 0, 1, 0); /* drive B's data */
}

static void test_duplicate_ids_are_chosen_by_rotation(void) {
    disk_track_t *t = disc(0, 2, 0);      /* 4MATION style: a second sector 8 on cylinder 2 */
    t->secs[3].r = 8;                     /* sector 4 becomes another 8, at 3/9 of a turn */
    size_t n = cmd(0, 0x02, 0x20, 2, 21); /* record 21: logical sector 5 = ID 8, offset 0 */
    script[n++] = (fake_in_t)FB(0x00);
    rot = 0; /* the renumbered sector (3/9 of a turn) comes round before the real 8 (7/9) */
    run(n);
    expect_record(3, 0, 2, 0, 4, 0);
    rot = t->secs[3].pos + 1; /* just past it: the next sector numbered 8 is the original */
    fake480z_init(&f, script, n);
    idc_init(&d, &f.io, NULL, 0);
    idc_set_drives(&d, &drives);
    idc_set_drive_type(&d, 0, type_a);
    idc_run(&d);
    expect_record(3, 0, 2, 0, 8, 0);
}

static void test_crc_error_sector_reads_as_08(void) {
    disc(0, 7, 1)->secs[4].flags = DISK_SEC_CRC_ERROR;
    cmd(0, 0x02, 0x22, 7, 33); /* record 33: logical sector 8 = ID 5 */
    run(4);
    const uint8_t want[] = {0x00, 0x00, IDC_STATUS_CRC};
    TEST_ASSERT_EQUAL_size_t(3, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
}

static void test_read_by_id_sends_the_block_count_then_each_block(void) {
    size_t n = cmd(0, 0x0E, 0x20, 1, 5); /* as the 4MATION loader: cylinder 1, sector 5 */
    for (int i = 0; i < 4; i++)
        script[n++] = (fake_in_t)FB(0x00);
    run(n);
    /* trace: <0E >00 <20 01 05 >00 >00 04 [4 x (128 <00)] */
    TEST_ASSERT_EQUAL_size_t(4 + 4 * 128, f.nout);
    const uint8_t head[] = {0x00, 0x00, 0x00, 0x04};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, f.out, 4);
    for (unsigned b = 0; b < 4; b++)
        expect_record(4 + b * 128, 0, 1, 0, 5, b * 128);
}

static void test_read_by_id_of_a_missing_sector_is_10(void) {
    cmd(0, 0x0E, 0x20, 1, 12); /* no sector 12 on a 9-sector track (12 is within the 36 the IDC allows) */
    run(4);
    const uint8_t want[] = {0x00, 0x00, IDC_STATUS_NOT_FOUND};
    TEST_ASSERT_EQUAL_size_t(3, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, 3);
}

static void test_write_by_id_writes_at_once(void) {
    size_t n = cmd(0, 0x0F, 0x00, 0, 1); /* as FORMAT writes the boot sector: FM track 0, sector 1 */
    uint8_t rec[128];
    memset(rec, 0x3C, sizeof rec);
    n = fake_bytes(script, n, rec, 128);
    run(n);
    /* trace: <0F >00 <01 00 01 >00 01 <[128] ... then the block ack and the result */
    const uint8_t want[] = {0x00, 0x00, 0x01, 0x00, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    TEST_ASSERT_EQUAL_UINT(1, saves);
    TEST_ASSERT_EQUAL_HEX8(0x3C, disc(0, 0, 0)->data[disc(0, 0, 0)->secs[0].data_off + 127]);
}

static void test_write_by_id_to_a_protected_disc_is_40(void) {
    read_only[0] = true;
    size_t n = cmd(0, 0x10, 0x20, 4, 2);
    uint8_t rec[128];
    memset(rec, 0x55, sizeof rec);
    for (int b = 0; b < 4; b++)
        n = fake_bytes(script, n, rec, 128);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_WRITE_PROTECT};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_format_lays_down_a_fresh_track_in_the_given_order(void) {
    const uint8_t ids[9] = {1, 6, 2, 7, 3, 8, 4, 9, 5};
    size_t n = cmd(0, 0x0C, 0x22, 7, 9); /* side 1, MFM, 9 sectors */
    for (int i = 0; i < 9; i++)
        script[n++] = (fake_in_t)FB(ids[i]);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    const disk_track_t *t = disc(0, 7, 1);
    TEST_ASSERT_FALSE(t->fm);
    TEST_ASSERT_EQUAL_UINT8(9, t->count);
    for (unsigned i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_HEX8(ids[i], t->secs[i].r);
        TEST_ASSERT_EQUAL_UINT8(7, t->secs[i].c);
        TEST_ASSERT_EQUAL_UINT8(0, t->secs[i].h); /* RM's IDC records head 0 on side 1 */
        TEST_ASSERT_EQUAL_UINT16(512, t->secs[i].size);
        TEST_ASSERT_EQUAL_HEX8(0xE5, t->data[t->secs[i].data_off + 511]);
    }
}

/*
 * The IDC's own track layout [IDC3 0409, template 08a1]: 80 x 4E, 12 x 00, 3 x C2, index mark, 50 x 4E
 * (146 bytes), then per sector 12 x 00, 3 x A1, ID mark, ID, CRC, 22 x 4E, 12 x 00, 3 x A1, data mark,
 * 512 bytes, CRC, 80 x 4E (654 bytes). Positions are of the ID mark, 15 bytes in; 16 cells per MFM byte.
 */
static void test_format_spaces_mfm_sectors_as_the_idc_writes_them(void) {
    size_t n = cmd(0, 0x0C, 0x20, 5, 9);
    for (int i = 0; i < 9; i++)
        script[n++] = (fake_in_t)FB(i + 1);
    run(n);
    const disk_track_t *t = disc(0, 5, 0);
    TEST_ASSERT_EQUAL_UINT8(9, t->count);
    for (unsigned i = 0; i < 9; i++)
        TEST_ASSERT_EQUAL_UINT32((146u + 15u + 654u * i) * 16u, t->secs[i].pos);
}

/*
 * [IDC3 036d, template 085b]: 40 x FF, 6 x 00, index mark, 26 x FF (73 bytes), then per sector 6 x 00,
 * ID mark, ID, CRC, 11 x FF, 6 x 00, data mark, 128 bytes, CRC, 27 x FF (188 bytes). The ID mark is 6
 * bytes in; 32 cells per FM byte.
 */
static void test_format_spaces_fm_sectors_as_the_idc_writes_them(void) {
    size_t n = cmd(0, 0x0C, 0x00, 0, 16);
    for (int i = 0; i < 16; i++)
        script[n++] = (fake_in_t)FB(i + 1);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    const disk_track_t *t = disc(0, 0, 0);
    TEST_ASSERT_TRUE(t->fm);
    TEST_ASSERT_EQUAL_UINT8(16, t->count);
    for (unsigned i = 0; i < 16; i++)
        TEST_ASSERT_EQUAL_UINT32((73u + 6u + 188u * i) * 32u, t->secs[i].pos);
}

static void test_format_with_the_wrong_sector_count_is_20_and_takes_no_ids(void) {
    size_t n = cmd(0, 0x0C, 0x00, 0, 10); /* FM wants 16 */
    script[n++] = (fake_in_t)FB(0x11);    /* the next command: version */
    run(n);
    const uint8_t want[] = {0x00, 0x20, 0x00, 0x31};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_verify_finds_odd_ids_and_reports_a_missing_one(void) {
    /* 4MATION cylinder 3: arbitrary IDs, two 29s */
    const uint8_t odd[9] = {0xA6, 0x67, 0x32, 0x09, 0x41, 0x29, 0x29, 0x1D, 0x2A};
    disk_track_t *t = disc(0, 3, 0);
    for (unsigned i = 0; i < 9; i++)
        t->secs[i].r = odd[i];
    size_t n = cmd(0, 0x0D, 0x20, 3, 9);
    for (int i = 0; i < 9; i++)
        script[n++] = (fake_in_t)FB(odd[i]);
    n = cmd(n, 0x0D, 0x20, 3, 9);
    for (int i = 0; i < 9; i++)
        script[n++] = (fake_in_t)FB(i == 4 ? 0x01 : odd[i]); /* 01 isn't there */
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_NOT_FOUND};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_probe_reports_density_and_drive_type(void) {
    present[1] = false;
    size_t n = 0;
    script[n++] = (fake_in_t)FB(0x06);
    script[n++] = (fake_in_t)FB(0x00);
    script[n++] = (fake_in_t)FB(0x06);
    script[n++] = (fake_in_t)FB(0x01);
    run(n);
    /* A 40-track MFM disc in an image-shaped (40-track) drive: parameters 00, result 00 (the IDC sends FEA3
     * again once the ID is read [IDC3 0c52]), format 02. Drive B empty: 80, with no result byte. */
    const uint8_t want[] = {0x00, 0x00, 0x00, IDC_PROBE_MFM, 0x00, IDC_STATUS_NOT_READY};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_probe_of_a_40_track_disc_in_an_80_track_drive_is_0a(void) {
    type_a = IDC_DRIVE_80_TRACK; /* MAME's drives are 80-track (525qd): its boot trace answers 00 00 0A */
    size_t n = 0;
    script[n++] = (fake_in_t)FB(0x06);
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    const uint8_t want[] = {0x00, 0x00, 0x00, IDC_PROBE_QD_DRIVE | IDC_PROBE_MFM};
    TEST_ASSERT_EQUAL_size_t(sizeof want, f.nout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
}

static void test_mfm_records_follow_the_2_to_1_skew(void) {
    /* IDC3 table 09c7: logical sectors 0-8 are IDs 1 6 2 7 3 8 4 9 5 */
    const uint8_t ids[9] = {1, 6, 2, 7, 3, 8, 4, 9, 5};
    size_t n = 0;
    for (unsigned k = 0; k < 9; k++) {
        n = cmd(n, 0x02, 0x20, 12, (uint8_t)(k * 4 + 2)); /* the second record of each logical sector */
        script[n++] = (fake_in_t)FB(0x00);
    }
    run(n);
    TEST_ASSERT_EQUAL_size_t(9 * (3 + 128), f.nout);
    for (unsigned k = 0; k < 9; k++)
        expect_record(k * (3 + 128) + 3, 0, 12, 0, ids[k], 128);
}

static void test_record_limit_is_the_formats_not_the_tracks(void) {
    /* 4MATION cylinder 2 as the old images have it: 8 sectors, no ID 9. Records 1-36 are still valid
     * parameters [IDC3 084b, 0f2b]; those in the missing sector (logical 7: records 29-32) are not found. */
    disc(0, 2, 0)->count = 8;
    disc(0, 2, 0)->secs[7].r = 8; /* drop ID 9 (index 7), keep 8 sectors */
    size_t n = cmd(0, 0x02, 0x20, 2, 29);
    n = cmd(n, 0x02, 0x20, 2, 36); /* logical sector 8 = ID 5: there */
    script[n++] = (fake_in_t)FB(0x00);
    run(n);
    TEST_ASSERT_EQUAL_size_t(3 + 3 + 128, f.nout);
    const uint8_t want[] = {0x00, 0x00, IDC_STATUS_NOT_FOUND, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f.out, sizeof want);
    expect_record(6, 0, 2, 0, 5, 384);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_login_ready_and_not_ready);
    RUN_TEST(test_read_mfm_record_maps_to_sector_and_offset);
    RUN_TEST(test_read_side_1_and_fm_track_0);
    RUN_TEST(test_mfm_records_follow_the_2_to_1_skew);
    RUN_TEST(test_record_limit_is_the_formats_not_the_tracks);
    RUN_TEST(test_cpm_drives_c_and_d_are_the_second_sides_of_drives_a_and_b);
    RUN_TEST(test_wrong_density_is_record_not_found);
    RUN_TEST(test_bad_track_record_and_drive);
    RUN_TEST(test_an_80_track_drive_takes_tracks_past_a_40_track_disc);
    RUN_TEST(test_write_goes_to_the_cache_and_reaches_the_disc_on_flush);
    RUN_TEST(test_moving_to_another_track_writes_the_changed_one_back);
    RUN_TEST(test_write_protected_disc_reports_41_on_flush);
    RUN_TEST(test_a_failed_write_back_loses_the_change);
    RUN_TEST(test_a_write_back_error_on_a_track_change_is_the_reads_result);
    RUN_TEST(test_a_write_completing_a_sector_writes_it_at_once);
    RUN_TEST(test_a_write_completing_a_sector_on_a_protected_disc_is_40);
    RUN_TEST(test_a_write_to_a_missing_sector_fails_when_the_sector_is_complete);
    RUN_TEST(test_a_partial_write_to_a_missing_sector_fails_on_flush_with_bit_0);
    RUN_TEST(test_drive_map_from_05_swaps_drives);
    RUN_TEST(test_duplicate_ids_are_chosen_by_rotation);
    RUN_TEST(test_crc_error_sector_reads_as_08);
    RUN_TEST(test_read_by_id_sends_the_block_count_then_each_block);
    RUN_TEST(test_read_by_id_of_a_missing_sector_is_10);
    RUN_TEST(test_write_by_id_writes_at_once);
    RUN_TEST(test_write_by_id_to_a_protected_disc_is_40);
    RUN_TEST(test_format_lays_down_a_fresh_track_in_the_given_order);
    RUN_TEST(test_format_spaces_mfm_sectors_as_the_idc_writes_them);
    RUN_TEST(test_format_spaces_fm_sectors_as_the_idc_writes_them);
    RUN_TEST(test_format_with_the_wrong_sector_count_is_20_and_takes_no_ids);
    RUN_TEST(test_verify_finds_odd_ids_and_reports_a_missing_one);
    RUN_TEST(test_probe_reports_density_and_drive_type);
    RUN_TEST(test_probe_of_a_40_track_disc_in_an_80_track_drive_is_0a);
    return UNITY_END();
}
