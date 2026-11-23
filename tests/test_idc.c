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
#include "core/idc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/fake480z.h"
#include "support/testdata.h"
#include "unity.h"

/* A made-up boot block: the real one is RM's and is never in the repository. */
static const uint8_t boot[] = {0xB0, 0x07, 0xB1, 0x0C, 0x4B};

static fake480z_t f;
static idc_t d;

void setUp(void) {}
void tearDown(void) {}

/* Runs the unit on a script until the input runs out. */
static void run(const fake_in_t *script, size_t n) {
    fake480z_init(&f, script, n);
    idc_init(&d, &f.io, boot, sizeof boot);
    idc_run(&d);
}

#define RUN(...)                                                                                                       \
    do {                                                                                                               \
        static const fake_in_t s_[] = {__VA_ARGS__};                                                                   \
        run(s_, sizeof s_ / sizeof s_[0]);                                                                             \
    } while (0)

#define EXPECT_OUT(...)                                                                                                \
    do {                                                                                                               \
        static const uint8_t w_[] = {__VA_ARGS__};                                                                     \
        TEST_ASSERT_EQUAL_size_t(sizeof w_, f.nout);                                                                   \
        TEST_ASSERT_EQUAL_HEX8_ARRAY(w_, f.out, sizeof w_);                                                            \
    } while (0)

static void test_param_byte_swaps_01_and_10_only(void) {
    TEST_ASSERT_EQUAL_HEX8(0x20, idc_param_byte(0x20));
    TEST_ASSERT_EQUAL_HEX8(0x22, idc_param_byte(0x21));
    TEST_ASSERT_EQUAL_HEX8(0x21, idc_param_byte(0x22));
    TEST_ASSERT_EQUAL_HEX8(0x23, idc_param_byte(0x23));
}

static void test_go_fast_from_9600(void) {
    RUN(FB(0x00));
    EXPECT_OUT(0x5A, 0x00, 0x01);
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_FAST, f.baud);
}

/* The ROS's 00 (its SIO at 8N2, so bad parity here), then FF: with the boot rate at 62500 the reply is 5A 00 02
 * and the boot block goes at 62500 (core/link.h, link_set_boot_baud). */
static void test_the_ros_s_go_fast_and_boot_block_use_the_boot_rate(void) {
    static const fake_in_t s[] = {FP(0x00), FP(0xFF)};
    fake480z_init(&f, s, 2);
    idc_init(&d, &f.io, boot, sizeof boot);
    link_set_boot_baud(&d.link, 62500u);
    idc_run(&d);
    EXPECT_OUT(0x5A, 0x00, 0x02, 0xB0, 0x07, 0xB1, 0x0C, 0x4B);
    TEST_ASSERT_EQUAL_UINT32(62500u, f.baud);
}

static void test_boot_block_is_sent_whole_with_no_status_byte(void) {
    RUN(FB(0xFF));
    EXPECT_OUT(0xB0, 0x07, 0xB1, 0x0C, 0x4B);
}

static void test_boot_block_without_a_rom_is_an_unknown_command(void) {
    static const fake_in_t s[] = {FB(0xFF)};
    fake480z_init(&f, s, 1);
    idc_init(&d, &f.io, NULL, 0);
    idc_run(&d);
    EXPECT_OUT(IDC_STATUS_UNKNOWN_CMD);
}

static void test_unknown_commands_get_40_and_no_status_echo(void) {
    RUN(FB(0x50), FB(0x18), FB(0xFE), FB(0xA0));
    EXPECT_OUT(0x40, 0x40, 0x40, 0x40);
}

static void test_configure_00_echoes_status_and_resets_the_drive_map(void) {
    RUN(FB(0x05), FB(0x00));
    EXPECT_OUT(0x00, 0x00); /* trace: <05 >00 <00 >00 */
    TEST_ASSERT_EQUAL_UINT8(1, d.retries);
    for (unsigned i = 0; i < IDC_DRIVES; i++)
        TEST_ASSERT_EQUAL_UINT8(i, d.drive_map[i]);
}

static void test_configure_swaps_drives_0_and_1_with_n_and_n_plus_1(void) {
    RUN(FB(0x05), FB(0x02 ^ 0x03)); /* raw 01 reads as 02 after the parity swap */
    const uint8_t want[IDC_DRIVES] = {2, 3, 0, 1, 4, 5, 6, 7};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, d.drive_map, IDC_DRIVES);
}

static void test_configure_high_nibble_sets_retries_unless_f(void) {
    RUN(FB(0x05), FB(0xA8), FB(0x05), FB(0xF8));
    EXPECT_OUT(0x00, 0x00, 0x00, 0x00);
    TEST_ASSERT_EQUAL_UINT8(0x0B, d.retries); /* A + 1; F leaves it */
    for (unsigned i = 0; i < IDC_DRIVES; i++)
        TEST_ASSERT_EQUAL_UINT8(i, d.drive_map[i]); /* bit 3: kept */
}

static void test_set_retries_accepts_0_to_f(void) {
    RUN(FB(0x07), FB(0x05), FB(0x07), FB(0x10));
    EXPECT_OUT(0x00, 0x00, 0x00, 0x20);
    TEST_ASSERT_EQUAL_UINT8(6, d.retries);
}

static void test_version_is_31(void) {
    RUN(FB(0x11));
    EXPECT_OUT(0x00, 0x31);
}

static void test_nop_only_echoes_status(void) {
    RUN(FB(0x16));
    EXPECT_OUT(0x00);
}

static void test_reset_mid_command_abandons_it_and_the_next_command_is_served(void) {
    RUN(FB(0x00), FB(0x05), FRESET, FB(0x00), FB(0x11));
    EXPECT_OUT(0x5A, 0x00, 0x01, 0x00, 0x5A, 0x00, 0x01, 0x00, 0x31);
    /* fast, back to slow at the reset, fast again */
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_FAST, f.baud);
    TEST_ASSERT_EQUAL_size_t(4, f.nbauds);
    TEST_ASSERT_EQUAL_UINT32(LINK_BAUD_SLOW, f.bauds[2].baud);
}

static void test_cp_m_boot_opening_matches_the_mame_trace(void) {
    /* docs/protocol.md "Observed boot": <00 >5A 00 01 [fast] <FF >block [reset] <00 >5A 00 01 <05 >00 <00 >00 */
    RUN(FB(0x00), FB(0xFF), FRESET, FB(0x00), FB(0x05), FB(0x00));
    EXPECT_OUT(0x5A, 0x00, 0x01, 0xB0, 0x07, 0xB1, 0x0C, 0x4B, 0x5A, 0x00, 0x01, 0x00, 0x00);
}

static void test_upload_stores_the_data_and_checks_the_sum(void) {
    /* E100, 3 bytes 01 02 03; check = -(1+2+3) = FFFA */
    RUN(FB(0x0A), FB(0x00), FB(0xE1), FB(0x03), FB(0x00), FB(0x01), FB(0x02), FB(0x03), FB(0xFA), FB(0xFF));
    EXPECT_OUT(0x00, 0x00, 0x00); /* accept, parameters OK, checksum OK */
    const uint8_t want[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d.ram + 0x100, 3);
}

static void test_upload_with_a_wrong_check_answers_10(void) {
    RUN(FB(0x0A), FB(0x00), FB(0xE1), FB(0x01), FB(0x00), FB(0x05), FB(0x00), FB(0x00));
    EXPECT_OUT(0x00, 0x00, IDC_STATUS_CHECKSUM);
}

static void test_upload_ram_is_mirrored_through_e000_to_ffff(void) {
    RUN(FB(0x0A), FB(0x01), FB(0xF8), FB(0x01), FB(0x00), FB(0x7E), FB(0x82), FB(0xFF));
    EXPECT_OUT(0x00, 0x00, 0x00);
    TEST_ASSERT_EQUAL_HEX8(0x7E, d.ram[1]); /* F801 is E001 */
}

static void test_upload_below_e000_answers_20_then_the_length_high_byte(void) {
    /* the IDC sends 20, then whatever is left in A (the length's high byte), and takes no data */
    RUN(FB(0x0A), FB(0x00), FB(0x10), FB(0x02), FB(0x01), FB(0x11));
    EXPECT_OUT(0x00, 0x20, 0x01, 0x00, 0x31); /* ... then the next command (11) is served */
}

static void test_upload_past_ffff_answers_20_twice(void) {
    RUN(FB(0x0A), FB(0x00), FB(0xFF), FB(0x00), FB(0x02), FB(0x11));
    EXPECT_OUT(0x00, 0x20, 0x20, 0x00, 0x31);
}

static int hook_calls;
static uint16_t hook_addr;
static uint8_t hook_first;
static void hook(void *ctx, uint16_t addr, const uint8_t ram[IDC_RAM_BYTES]) {
    (void)ctx;
    hook_calls++;
    hook_addr = addr;
    hook_first = ram[addr - IDC_RAM_BASE];
}

static void test_execute_is_reported_and_answered_as_a_failure(void) {
    static const fake_in_t s[] = {FB(0x0A), FB(0x00), FB(0xE2), FB(0x01), FB(0x00), FB(0xC9),
                                  FB(0x37), FB(0xFF), FB(0x0B), FB(0x00), FB(0xE2)};
    fake480z_init(&f, s, sizeof s / sizeof s[0]);
    idc_init(&d, &f.io, boot, sizeof boot);
    d.exec_hook = hook;
    hook_calls = 0;
    idc_run(&d);
    EXPECT_OUT(0x00, 0x00, 0x00, 0x00, 0x00, IDC_STATUS_UNKNOWN_CMD);
    TEST_ASSERT_EQUAL_INT(1, hook_calls);
    TEST_ASSERT_EQUAL_HEX16(0xE200, hook_addr);
    TEST_ASSERT_EQUAL_HEX8(0xC9, hook_first); /* the uploaded code (a RET) is what the platform sees */
}

static void test_execute_below_e000_gets_no_reply(void) {
    RUN(FB(0x0B), FB(0x00), FB(0x10), FB(0x11));
    EXPECT_OUT(0x00, 0x00, 0x31); /* only the accept, then the next command */
}

static void test_boot_block_found_in_a_rom_by_its_handler(void) {
    static uint8_t rom[0x2000];
    memset(rom, 0xFF, sizeof rom);
    const uint8_t handler[] = {0xED, 0x4B, 0x28, 0x1B, 0x21, 0x2A, 0x1B, 0x7E, 0xCD, 0x56, 0x0A};
    memcpy(rom + 0x0AF6, handler, sizeof handler);
    rom[0x1B28] = 0x05;
    rom[0x1B29] = 0x00;
    memcpy(rom + 0x1B2A, boot, sizeof boot);
    const uint8_t *blk;
    size_t n;
    TEST_ASSERT_TRUE(idc_find_boot_block(rom, sizeof rom, &blk, &n));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_PTR(rom + 0x1B2A, blk);
    rom[0x1B29] = 0x20; /* length runs past the ROM */
    TEST_ASSERT_FALSE(idc_find_boot_block(rom, sizeof rom, &blk, &n));
    TEST_ASSERT_FALSE(idc_find_boot_block(rom, 4096, &blk, &n));
}

/* Real ROMs from testdata/roms (never committed). Missing data fails the test. */
static void check_real_rom(const char *name, size_t want_len) {
    RMFE_NEEDS_TESTDATA();
    const char *dir = getenv("RMFE_TESTDATA");
    TEST_ASSERT_NOT_NULL_MESSAGE(dir, "RMFE_TESTDATA not set");
    char path[1024];
    snprintf(path, sizeof path, "%s/roms/%s", dir, name);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    static uint8_t rom[0x2000];
    size_t got = fread(rom, 1, sizeof rom, fp);
    fclose(fp);
    TEST_ASSERT_EQUAL_size_t(sizeof rom, got);
    const uint8_t *blk;
    size_t n;
    TEST_ASSERT_TRUE(idc_find_boot_block(rom, sizeof rom, &blk, &n));
    TEST_ASSERT_EQUAL_size_t(want_len, n);
    TEST_ASSERT_EQUAL_PTR(rom + 0x1B2A, blk);
}

static void test_real_idc3_boot_block_is_1049_bytes_at_1b2a(void) {
    check_real_rom("IDC3-1I.ROM", 1049); /* docs/protocol.md; MAME trace: FF sends 1049 bytes */
}

static void test_real_idc5_boot_block_is_1181_bytes_at_1b2a(void) {
    check_real_rom("IDC5-1J.ROM", 1181);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_param_byte_swaps_01_and_10_only);
    RUN_TEST(test_go_fast_from_9600);
    RUN_TEST(test_the_ros_s_go_fast_and_boot_block_use_the_boot_rate);
    RUN_TEST(test_boot_block_is_sent_whole_with_no_status_byte);
    RUN_TEST(test_boot_block_without_a_rom_is_an_unknown_command);
    RUN_TEST(test_unknown_commands_get_40_and_no_status_echo);
    RUN_TEST(test_configure_00_echoes_status_and_resets_the_drive_map);
    RUN_TEST(test_configure_swaps_drives_0_and_1_with_n_and_n_plus_1);
    RUN_TEST(test_configure_high_nibble_sets_retries_unless_f);
    RUN_TEST(test_set_retries_accepts_0_to_f);
    RUN_TEST(test_version_is_31);
    RUN_TEST(test_nop_only_echoes_status);
    RUN_TEST(test_reset_mid_command_abandons_it_and_the_next_command_is_served);
    RUN_TEST(test_cp_m_boot_opening_matches_the_mame_trace);
    RUN_TEST(test_upload_stores_the_data_and_checks_the_sum);
    RUN_TEST(test_upload_with_a_wrong_check_answers_10);
    RUN_TEST(test_upload_ram_is_mirrored_through_e000_to_ffff);
    RUN_TEST(test_upload_below_e000_answers_20_then_the_length_high_byte);
    RUN_TEST(test_upload_past_ffff_answers_20_twice);
    RUN_TEST(test_execute_is_reported_and_answered_as_a_failure);
    RUN_TEST(test_execute_below_e000_gets_no_reply);
    RUN_TEST(test_boot_block_found_in_a_rom_by_its_handler);
    RUN_TEST(test_real_idc3_boot_block_is_1049_bytes_at_1b2a);
    RUN_TEST(test_real_idc5_boot_block_is_1181_bytes_at_1b2a);
    return UNITY_END();
}
