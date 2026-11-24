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
#include "core/config.h"

#include <string.h>

#include "unity.h"

/* Expected values from the option descriptions in core/config.h and docs/sd-card.md. */

static config_t c;
static unsigned nwarn, warn_line[8];
static const char *warn_why[8];

static void on_warn(void *ctx, unsigned line, const char *why) {
    (void)ctx;
    if (nwarn < 8) {
        warn_line[nwarn] = line;
        warn_why[nwarn] = why;
    }
    nwarn++;
}

static void parse(const char *text) {
    config_parse(&c, text, strlen(text), on_warn, NULL);
}

void setUp(void) {
    config_defaults(&c);
    nwarn = 0;
}
void tearDown(void) {}

/* --- RMFE.CFG ------------------------------------------------------------------------------------- */

static void test_defaults_are_last_image_no_write_protect_auto_drives_and_idc_rom(void) {
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_LAST, c.image_on_startup);
    TEST_ASSERT_FALSE(c.write_protect);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[0]);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[1]);
    TEST_ASSERT_EQUAL_STRING("IDC.ROM", c.idc_rom);
}

static void test_an_empty_file_leaves_the_defaults(void) {
    config_parse(&c, NULL, 0, on_warn, NULL);
    parse("");
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_LAST, c.image_on_startup);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
}

static void test_every_option_is_read(void) {
    parse("image-on-startup = static\n"
          "write-protect = yes\n"
          "drive-a = 80\n"
          "drive-b = 40\n"
          "idc-rom = roms/IDC5-1J.ROM\n");
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_STATIC, c.image_on_startup);
    TEST_ASSERT_TRUE(c.write_protect);
    TEST_ASSERT_EQUAL(IDC_DRIVE_80_TRACK, c.drive_type[0]);
    TEST_ASSERT_EQUAL(IDC_DRIVE_40_TRACK, c.drive_type[1]);
    TEST_ASSERT_EQUAL_STRING("roms/IDC5-1J.ROM", c.idc_rom);
}

static void test_each_value_of_each_keyword_option(void) {
    parse("image-on-startup = init\nwrite-protect = no\ndrive-a = auto\n");
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_INIT, c.image_on_startup);
    TEST_ASSERT_FALSE(c.write_protect);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[0]);
    parse("image-on-startup = last\n");
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_LAST, c.image_on_startup);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
}

static void test_comments_blank_lines_spaces_case_quotes_and_line_endings(void) {
    parse("# RMFE.CFG\r\n"
          "\r\n"
          "   \t\n"
          "  # indented comment\n"
          "  WRITE-PROTECT=Yes  \r\n"
          "Drive-A\t=\t\"80\"\r"
          "idc-rom = \"My ROMs/IDC 3.ROM\"");
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
    TEST_ASSERT_TRUE(c.write_protect);
    TEST_ASSERT_EQUAL(IDC_DRIVE_80_TRACK, c.drive_type[0]);
    TEST_ASSERT_EQUAL_STRING("My ROMs/IDC 3.ROM", c.idc_rom); /* a path keeps its case and spaces */
}

static void test_the_last_setting_of_an_option_wins(void) {
    parse("drive-b = 40\ndrive-b = 80\n");
    TEST_ASSERT_EQUAL(IDC_DRIVE_80_TRACK, c.drive_type[1]);
}

static void test_unknown_options_and_bad_values_are_reported_by_line_and_ignored(void) {
    parse("nav-mode = indexed\n"      /* FlashFloppy's, not ours */
          "write-protect = maybe\n"   /* bad value: the default stays */
          "drive-a = 120\n"           /* bad value */
          "just some words\n"         /* no = */
          "image-on-startup = init\n" /* still read after all that */
    );
    TEST_ASSERT_EQUAL_UINT(4, nwarn);
    TEST_ASSERT_EQUAL_UINT(1, warn_line[0]);
    TEST_ASSERT_EQUAL_STRING("unknown option", warn_why[0]);
    TEST_ASSERT_EQUAL_UINT(2, warn_line[1]);
    TEST_ASSERT_EQUAL_UINT(3, warn_line[2]);
    TEST_ASSERT_EQUAL_UINT(4, warn_line[3]);
    TEST_ASSERT_FALSE(c.write_protect);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[0]);
    TEST_ASSERT_EQUAL(CONFIG_STARTUP_INIT, c.image_on_startup);
}

static void test_line_numbers_count_crlf_and_blank_lines_once_each(void) {
    parse("\r\n\r\n\nbad\r\n");
    TEST_ASSERT_EQUAL_UINT(1, nwarn);
    TEST_ASSERT_EQUAL_UINT(4, warn_line[0]);
}

static void test_a_rom_path_too_long_or_empty_is_refused(void) {
    char text[400] = "idc-rom = ";
    memset(text + strlen(text), 'x', CONFIG_PATH_MAX); /* 256 characters: one too many */
    parse(text);
    parse("idc-rom =\n");
    TEST_ASSERT_EQUAL_UINT(2, nwarn);
    TEST_ASSERT_EQUAL_STRING("IDC.ROM", c.idc_rom);
}

static void test_a_rom_path_of_255_characters_fits(void) {
    char text[400] = "idc-rom = ";
    memset(text + strlen(text), 'y', CONFIG_PATH_MAX - 1);
    parse(text);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
    TEST_ASSERT_EQUAL_size_t(CONFIG_PATH_MAX - 1, strlen(c.idc_rom));
}

static void test_a_line_too_long_is_reported_not_read(void) {
    char text[600] = "drive-a = 80";
    memset(text + strlen(text), ' ', 400); /* trailing spaces would be trimmed, but the line is too long */
    parse(text);
    TEST_ASSERT_EQUAL_UINT(1, nwarn);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[0]);
}

static void test_without_a_warning_callback_bad_lines_are_still_ignored(void) {
    const char *text = "bogus\ndrive-a = 80\n";
    config_parse(&c, text, strlen(text), NULL, NULL);
    TEST_ASSERT_EQUAL(IDC_DRIVE_80_TRACK, c.drive_type[0]);
}

static void test_text_is_read_only_up_to_its_length(void) {
    const char *text = "drive-a = 80\ndrive-b = 80\n";
    config_parse(&c, text, 13, on_warn, NULL); /* just the first line */
    TEST_ASSERT_EQUAL(IDC_DRIVE_80_TRACK, c.drive_type[0]);
    TEST_ASSERT_EQUAL(IDC_DRIVE_FOLLOW_IMAGE, c.drive_type[1]);
}

static void test_show_commands_is_off_by_default_and_takes_yes_or_no(void) {
    TEST_ASSERT_FALSE(c.show_commands);
    parse("show-commands = yes\n");
    TEST_ASSERT_TRUE(c.show_commands);
    parse("show-commands = maybe\n");
    TEST_ASSERT_TRUE(c.show_commands);
    TEST_ASSERT_EQUAL_UINT(1, nwarn);
    TEST_ASSERT_EQUAL_STRING("show-commands must be yes or no", warn_why[0]);
    parse("Show-Commands = NO\n");
    TEST_ASSERT_FALSE(c.show_commands);
}

static void test_fast_baud_is_125000_by_default_and_takes_62500(void) {
    TEST_ASSERT_EQUAL_UINT32(125000u, c.fast_baud);
    parse("fast-baud = 62500\n");
    TEST_ASSERT_EQUAL_UINT32(62500u, c.fast_baud);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
    parse("Fast-Baud = \"125000\"\n");
    TEST_ASSERT_EQUAL_UINT32(125000u, c.fast_baud);
}

static void test_fast_baud_rejects_other_rates_and_keeps_the_last(void) {
    parse("fast-baud = 62500\nfast-baud = 9600\nfast-baud = 115200\nfast-baud =\n");
    TEST_ASSERT_EQUAL_UINT32(62500u, c.fast_baud);
    TEST_ASSERT_EQUAL_UINT(3, nwarn);
    TEST_ASSERT_EQUAL_STRING("fast-baud must be 125000 or 62500", warn_why[0]);
    TEST_ASSERT_EQUAL_UINT(2, warn_line[0]);
    TEST_ASSERT_EQUAL_UINT(4, warn_line[2]);
}

/* 125000, the IDC's, since the boot block goes paced (core/link.h, link_set_paced). */
static void test_boot_baud_is_125000_by_default_and_takes_62500(void) {
    TEST_ASSERT_EQUAL_UINT32(125000u, c.boot_baud);
    parse("boot-baud = 62500\n");
    TEST_ASSERT_EQUAL_UINT32(62500u, c.boot_baud);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
    parse("BOOT-BAUD = 125000\n");
    TEST_ASSERT_EQUAL_UINT32(125000u, c.boot_baud);
}

static void test_boot_baud_rejects_other_rates_and_keeps_the_last(void) {
    parse("boot-baud = 62500\nboot-baud = 9600\nboot-baud = fast\n");
    TEST_ASSERT_EQUAL_UINT32(62500u, c.boot_baud);
    TEST_ASSERT_EQUAL_UINT(2, nwarn);
    TEST_ASSERT_EQUAL_STRING("boot-baud must be 62500 or 125000", warn_why[0]);
    TEST_ASSERT_EQUAL_UINT(2, warn_line[0]);
    TEST_ASSERT_EQUAL_UINT(3, warn_line[1]);
}

static void test_rotary_is_full_and_forward_by_default(void) {
    TEST_ASSERT_EQUAL(CONFIG_ROTARY_FULL, c.rotary);
    TEST_ASSERT_FALSE(c.rotary_reverse);
}

static void test_rotary_takes_each_kind_with_or_without_reverse(void) {
    parse("rotary = half\n");
    TEST_ASSERT_EQUAL(CONFIG_ROTARY_HALF, c.rotary);
    TEST_ASSERT_FALSE(c.rotary_reverse);
    parse("rotary = quarter,reverse\n");
    TEST_ASSERT_EQUAL(CONFIG_ROTARY_QUARTER, c.rotary);
    TEST_ASSERT_TRUE(c.rotary_reverse);
    parse("ROTARY = Full , Reverse\n");
    TEST_ASSERT_EQUAL(CONFIG_ROTARY_FULL, c.rotary);
    TEST_ASSERT_TRUE(c.rotary_reverse);
    parse("rotary = full\n"); /* a later line without reverse turns it off again */
    TEST_ASSERT_FALSE(c.rotary_reverse);
    TEST_ASSERT_EQUAL_UINT(0, nwarn);
}

static void test_rotary_refuses_other_kinds_and_suffixes_and_keeps_its_setting(void) {
    parse("rotary = half,reverse\n"
          "rotary = gray\n"
          "rotary = full,backwards\n"
          "rotary = ,reverse\n"
          "rotary =\n");
    TEST_ASSERT_EQUAL_UINT(4, nwarn);
    for (unsigned i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_UINT(i + 2, warn_line[i]);
    TEST_ASSERT_EQUAL_STRING("rotary must be full, half or quarter, optionally with ,reverse", warn_why[0]);
    TEST_ASSERT_EQUAL(CONFIG_ROTARY_HALF, c.rotary);
    TEST_ASSERT_TRUE(c.rotary_reverse);
}

/* --- IMAGE_x.CFG / INIT_x.CFG -------------------------------------------------------------------------- */

static void test_the_image_path_is_the_first_real_line_trimmed(void) {
    char path[CONFIG_PATH_MAX];
    const char *text = "# chosen on the display\r\n\r\n  games/Dragon World.IMD  \r\nignored.imd\r\n";
    TEST_ASSERT_TRUE(config_image_path(text, strlen(text), path, sizeof path));
    TEST_ASSERT_EQUAL_STRING("games/Dragon World.IMD", path);
}

static void test_an_image_path_may_be_quoted_and_needs_no_line_ending(void) {
    char path[CONFIG_PATH_MAX];
    const char *text = "\"SYSTEM.IMD\"";
    TEST_ASSERT_TRUE(config_image_path(text, strlen(text), path, sizeof path));
    TEST_ASSERT_EQUAL_STRING("SYSTEM.IMD", path);
}

static void test_no_image_path_in_an_empty_or_comment_only_file(void) {
    char path[CONFIG_PATH_MAX];
    TEST_ASSERT_FALSE(config_image_path("", 0, path, sizeof path));
    TEST_ASSERT_FALSE(config_image_path(NULL, 0, path, sizeof path));
    TEST_ASSERT_FALSE(config_image_path("# none\n\n", 8, path, sizeof path));
    TEST_ASSERT_FALSE(config_image_path("\"\"\n", 3, path, sizeof path));
}

static void test_an_image_path_that_does_not_fit_is_refused(void) {
    char path[8];
    TEST_ASSERT_FALSE(config_image_path("ABCDEFGH", 8, path, sizeof path)); /* 8 + NUL > 8 */
    TEST_ASSERT_TRUE(config_image_path("ABCDEFG", 7, path, sizeof path));
    TEST_ASSERT_EQUAL_STRING("ABCDEFG", path);
}

/* --- the listing -------------------------------------------------------------------------------------- */

static void test_listed_images_are_imd_and_hfe_not_dot_names_or_hidden(void) {
    TEST_ASSERT_TRUE(config_is_listed_image("SYSTEM.IMD", false));
    TEST_ASSERT_TRUE(config_is_listed_image("dragon.hfe", false));
    TEST_ASSERT_FALSE(config_is_listed_image("SYSTEM.IMD", true));    /* hidden */
    TEST_ASSERT_FALSE(config_is_listed_image("._SYSTEM.IMD", false)); /* macOS's resource forks */
    TEST_ASSERT_FALSE(config_is_listed_image(".hidden.hfe", false));
    TEST_ASSERT_FALSE(config_is_listed_image("IDC.ROM", false));
    TEST_ASSERT_FALSE(config_is_listed_image("RMFE.CFG", false));
}

static void test_names_sort_ignoring_case_then_by_bytes(void) {
    TEST_ASSERT_TRUE(config_compare_names("apple.imd", "Banana.imd") < 0);
    TEST_ASSERT_TRUE(config_compare_names("Banana.imd", "apple.imd") > 0);
    TEST_ASSERT_TRUE(config_compare_names("a.imd", "ab.imd") < 0); /* '.' sorts before 'b' */
    TEST_ASSERT_TRUE(config_compare_names("ab", "a") > 0);
    TEST_ASSERT_TRUE(config_compare_names("A.IMD", "a.imd") < 0); /* only case differs: 'A' < 'a' */
    TEST_ASSERT_EQUAL_INT(0, config_compare_names("same.imd", "same.imd"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_last_image_no_write_protect_auto_drives_and_idc_rom);
    RUN_TEST(test_an_empty_file_leaves_the_defaults);
    RUN_TEST(test_every_option_is_read);
    RUN_TEST(test_each_value_of_each_keyword_option);
    RUN_TEST(test_comments_blank_lines_spaces_case_quotes_and_line_endings);
    RUN_TEST(test_the_last_setting_of_an_option_wins);
    RUN_TEST(test_unknown_options_and_bad_values_are_reported_by_line_and_ignored);
    RUN_TEST(test_line_numbers_count_crlf_and_blank_lines_once_each);
    RUN_TEST(test_a_rom_path_too_long_or_empty_is_refused);
    RUN_TEST(test_a_rom_path_of_255_characters_fits);
    RUN_TEST(test_a_line_too_long_is_reported_not_read);
    RUN_TEST(test_without_a_warning_callback_bad_lines_are_still_ignored);
    RUN_TEST(test_text_is_read_only_up_to_its_length);
    RUN_TEST(test_show_commands_is_off_by_default_and_takes_yes_or_no);
    RUN_TEST(test_fast_baud_is_125000_by_default_and_takes_62500);
    RUN_TEST(test_fast_baud_rejects_other_rates_and_keeps_the_last);
    RUN_TEST(test_boot_baud_is_125000_by_default_and_takes_62500);
    RUN_TEST(test_boot_baud_rejects_other_rates_and_keeps_the_last);
    RUN_TEST(test_rotary_is_full_and_forward_by_default);
    RUN_TEST(test_rotary_takes_each_kind_with_or_without_reverse);
    RUN_TEST(test_rotary_refuses_other_kinds_and_suffixes_and_keeps_its_setting);
    RUN_TEST(test_the_image_path_is_the_first_real_line_trimmed);
    RUN_TEST(test_an_image_path_may_be_quoted_and_needs_no_line_ending);
    RUN_TEST(test_no_image_path_in_an_empty_or_comment_only_file);
    RUN_TEST(test_an_image_path_that_does_not_fit_is_refused);
    RUN_TEST(test_listed_images_are_imd_and_hfe_not_dot_names_or_hidden);
    RUN_TEST(test_names_sort_ignoring_case_then_by_bytes);
    return UNITY_END();
}
