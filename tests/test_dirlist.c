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
#include "core/dirlist.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

/* Expected orders worked out by hand from docs/sd-card.md: folders first, then images, each by name ignoring
 * case, then byte by byte. */

static dirlist_t l;

void setUp(void) {
    dirlist_clear(&l);
}
void tearDown(void) {}

static void test_only_folders_and_images_are_listed(void) {
    TEST_ASSERT_TRUE(dirlist_add(&l, "games", true, false));
    TEST_ASSERT_TRUE(dirlist_add(&l, "a.imd", false, false));
    TEST_ASSERT_TRUE(dirlist_add(&l, "B.HFE", false, false));
    TEST_ASSERT_FALSE(dirlist_add(&l, "IDC.ROM", false, false));
    TEST_ASSERT_FALSE(dirlist_add(&l, "RMFE.CFG", false, false));
    TEST_ASSERT_FALSE(dirlist_add(&l, "._a.imd", false, false));
    TEST_ASSERT_FALSE(dirlist_add(&l, "hidden.imd", false, true));
    TEST_ASSERT_FALSE(dirlist_add(&l, ".Trashes", true, false));
    TEST_ASSERT_FALSE(dirlist_add(&l, "System Volume Information", true, true));
    TEST_ASSERT_EQUAL_UINT(3, l.count);
    TEST_ASSERT_FALSE(l.truncated);
}

static void test_folders_come_first_then_images_each_by_name_ignoring_case(void) {
    const char *in[] = {"zeta.imd", "Beta", "alpha.HFE", "Alpha.imd", "games", "ALPHA"};
    const bool dir[] = {false, true, false, false, true, true};
    for (unsigned i = 0; i < 6; i++)
        TEST_ASSERT_TRUE(dirlist_add(&l, in[i], dir[i], false));
    dirlist_sort(&l);
    const char *want[] = {"ALPHA", "Beta", "games", "alpha.HFE", "Alpha.imd", "zeta.imd"};
    for (unsigned i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], dirlist_name(&l, i));
        TEST_ASSERT_EQUAL(i < 3, dirlist_is_dir(&l, i));
    }
}

static void test_names_differing_only_in_case_sort_by_bytes(void) {
    TEST_ASSERT_TRUE(dirlist_add(&l, "game.imd", false, false));
    TEST_ASSERT_TRUE(dirlist_add(&l, "GAME.imd", false, false));
    dirlist_sort(&l);
    TEST_ASSERT_EQUAL_STRING("GAME.imd", dirlist_name(&l, 0)); /* 'G' 47 < 'g' 67 */
    TEST_ASSERT_EQUAL_STRING("game.imd", dirlist_name(&l, 1));
}

static void test_an_empty_list_sorts(void) {
    dirlist_sort(&l);
    TEST_ASSERT_EQUAL_UINT(0, l.count);
}

static void test_too_many_entries_are_left_out_and_noted(void) {
    char name[16];
    for (unsigned i = 0; i < DIRLIST_MAX_ENTRIES; i++) {
        snprintf(name, sizeof name, "%04u.imd", i);
        TEST_ASSERT_TRUE(dirlist_add(&l, name, false, false));
    }
    TEST_ASSERT_FALSE(l.truncated);
    TEST_ASSERT_FALSE(dirlist_add(&l, "one-more.imd", false, false));
    TEST_ASSERT_TRUE(l.truncated);
    TEST_ASSERT_EQUAL_UINT(DIRLIST_MAX_ENTRIES, l.count);
    dirlist_sort(&l);
    TEST_ASSERT_EQUAL_STRING("0000.imd", dirlist_name(&l, 0));
    TEST_ASSERT_EQUAL_STRING("0511.imd", dirlist_name(&l, DIRLIST_MAX_ENTRIES - 1));
}

static void test_names_that_overflow_the_pool_are_left_out_and_noted(void) {
    /* 255-character names (the longest FAT allows) with their NUL: 256 bytes each, 128 fill the pool */
    char name[256];
    memset(name, 'x', 251);
    strcpy(name + 251, ".imd");
    for (unsigned i = 0; i < DIRLIST_POOL_BYTES / 256u; i++) {
        name[0] = (char)('A' + i % 26u);
        TEST_ASSERT_TRUE(dirlist_add(&l, name, false, false));
    }
    TEST_ASSERT_EQUAL_size_t(DIRLIST_POOL_BYTES, l.pool_used);
    TEST_ASSERT_FALSE(dirlist_add(&l, "a.imd", false, false));
    TEST_ASSERT_TRUE(l.truncated);
}

static void test_clear_empties_the_list_and_the_note(void) {
    TEST_ASSERT_TRUE(dirlist_add(&l, "a.imd", false, false));
    l.truncated = true;
    dirlist_clear(&l);
    TEST_ASSERT_EQUAL_UINT(0, l.count);
    TEST_ASSERT_EQUAL_size_t(0, l.pool_used);
    TEST_ASSERT_FALSE(l.truncated);
}

static void test_paths_join_in_the_root_and_in_folders(void) {
    char out[32];
    TEST_ASSERT_TRUE(dirlist_path_join(out, sizeof out, "", "a.imd"));
    TEST_ASSERT_EQUAL_STRING("a.imd", out);
    TEST_ASSERT_TRUE(dirlist_path_join(out, sizeof out, "games/arcade", "b.hfe"));
    TEST_ASSERT_EQUAL_STRING("games/arcade/b.hfe", out);
    strcpy(out, "games");
    TEST_ASSERT_TRUE(dirlist_path_join(out, sizeof out, out, "sub")); /* in place */
    TEST_ASSERT_EQUAL_STRING("games/sub", out);
}

static void test_a_join_that_does_not_fit_is_refused(void) {
    char out[10];
    TEST_ASSERT_TRUE(dirlist_path_join(out, sizeof out, "abcd", "efgh")); /* 9 + NUL */
    TEST_ASSERT_FALSE(dirlist_path_join(out, sizeof out, "abcd", "efghi"));
    TEST_ASSERT_FALSE(dirlist_path_join(out, sizeof out, "", "0123456789"));
}

static void test_a_paths_folder_and_name(void) {
    char out[32];
    TEST_ASSERT_TRUE(dirlist_path_folder(out, sizeof out, "games/arcade/b.hfe"));
    TEST_ASSERT_EQUAL_STRING("games/arcade", out);
    TEST_ASSERT_TRUE(dirlist_path_folder(out, sizeof out, "a.imd"));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(dirlist_path_folder(out, sizeof out, "games"));
    TEST_ASSERT_EQUAL_STRING("", out); /* up from a folder in the root */
    strcpy(out, "games/arcade");
    TEST_ASSERT_TRUE(dirlist_path_folder(out, sizeof out, out)); /* in place: up one */
    TEST_ASSERT_EQUAL_STRING("games", out);
    TEST_ASSERT_FALSE(dirlist_path_folder(out, 3, "abc/d"));
    TEST_ASSERT_EQUAL_STRING("b.hfe", dirlist_path_name("games/arcade/b.hfe"));
    TEST_ASSERT_EQUAL_STRING("a.imd", dirlist_path_name("a.imd"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_only_folders_and_images_are_listed);
    RUN_TEST(test_folders_come_first_then_images_each_by_name_ignoring_case);
    RUN_TEST(test_names_differing_only_in_case_sort_by_bytes);
    RUN_TEST(test_an_empty_list_sorts);
    RUN_TEST(test_too_many_entries_are_left_out_and_noted);
    RUN_TEST(test_names_that_overflow_the_pool_are_left_out_and_noted);
    RUN_TEST(test_clear_empties_the_list_and_the_note);
    RUN_TEST(test_paths_join_in_the_root_and_in_folders);
    RUN_TEST(test_a_join_that_does_not_fit_is_refused);
    RUN_TEST(test_a_paths_folder_and_name);
    return UNITY_END();
}
