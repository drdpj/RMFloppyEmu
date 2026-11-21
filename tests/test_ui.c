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
#include "core/ui.h"

#include <string.h>

#include "unity.h"

/* Expected rows written out by hand from the layout in core/ui.c and docs/sd-card.md, "Choosing an image". */

static dirlist_t list;
static ui_t u;
static ui_unit_t unit;
static uint32_t now;
static ui_request_t req;

static void drive(unsigned d, const char *path, bool hfe, unsigned cyls, bool ro) {
    ui_drive_t *dr = &unit.drive[d];
    memset(dr, 0, sizeof *dr);
    if (!path) return;
    dr->present = true;
    dr->hfe = hfe;
    dr->cylinders = cyls;
    dr->read_only = ro;
    strcpy(dr->path, path);
}

void setUp(void) {
    memset(&unit, 0, sizeof unit);
    unit.card = true;
    unit.rom = true;
    now = 5000;
    dirlist_clear(&list);
    ui_init(&u, &list, now);
    ui_set_unit(&u, &unit);
}
void tearDown(void) {}

static void row_is(unsigned row, ui_attr_t attr, const char *want) {
    char padded[UI_COLS + 1];
    memset(padded, ' ', UI_COLS);
    padded[UI_COLS] = '\0';
    memcpy(padded, want, strlen(want));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(padded, u.screen.text[row], "row text");
    TEST_ASSERT_EQUAL_MESSAGE(attr, u.screen.attr[row], "row attribute");
}

static void event(rotary_event_t e) {
    now += 10;
    ui_event(&u, e, now);
}

static void tick(uint32_t ms) {
    now += ms;
    ui_tick(&u, now);
}

static void take(ui_request_kind_t kind, unsigned d, const char *path) {
    TEST_ASSERT_TRUE(ui_take_request(&u, &req));
    TEST_ASSERT_EQUAL(kind, req.kind);
    TEST_ASSERT_EQUAL_UINT(d, req.drive);
    TEST_ASSERT_EQUAL_STRING(path, req.path);
    TEST_ASSERT_FALSE(ui_take_request(&u, &req)); /* one at a time */
}

/* The platform lists a folder: entries as the card gives them (unsorted), names ending in / are folders. */
static void list_done(const char *const *names, unsigned n) {
    dirlist_clear(&list);
    for (unsigned i = 0; i < n; i++) {
        size_t len = strlen(names[i]);
        char name[64];
        strcpy(name, names[i]);
        bool dir = name[len - 1] == '/';
        if (dir) name[len - 1] = '\0';
        TEST_ASSERT_TRUE(dirlist_add(&list, name, dir, false));
    }
    ui_request_done(&u, true, NULL, now);
}

/* --- home ------------------------------------------------------------------------------------------------ */

static void test_home_shows_each_drive_with_its_kind_and_the_cursor_on_a(void) {
    drive(0, "games/Dragon World.IMD", false, 40, false);
    drive(1, "work.hfe", true, 80, true);
    ui_set_unit(&u, &unit);
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
    row_is(2, UI_ATTR_CURSOR, "A: games/Dragon World.IMD");
    row_is(3, UI_ATTR_CURSOR, "");
    row_is(4, UI_ATTR_FAINT, "   IMD, 40 cylinders");
    row_is(6, UI_ATTR_NORMAL, "B: work.hfe");
    row_is(8, UI_ATTR_FAINT, "   HFE, 80 cylinders, write-protected");
    row_is(11, UI_ATTR_NORMAL, "");
    row_is(13, UI_ATTR_NORMAL, "");
    row_is(14, UI_ATTR_FAINT, " Turn: pick a drive. Press: choose");
}

static void test_an_empty_drive_says_so(void) {
    row_is(2, UI_ATTR_CURSOR, "A: (empty)");
    row_is(4, UI_ATTR_FAINT, "");
    row_is(6, UI_ATTR_NORMAL, "B: (empty)");
}

static void test_a_long_path_runs_onto_the_next_row_and_ends_in_a_tilde_if_still_too_long(void) {
    /* 37 columns a row after "A: ": 80 characters show 37 + 36, then "~" */
    char path[81];
    for (unsigned i = 0; i < 80; i++)
        path[i] = (char)('a' + i % 26u);
    path[80] = '\0';
    drive(0, path, false, 40, false);
    ui_set_unit(&u, &unit);
    char want2[41] = "A: ", want3[41] = "   ";
    memcpy(want2 + 3, path, 37);
    want2[40] = '\0';
    memcpy(want3 + 3, path + 37, 36);
    want3[39] = '~';
    want3[40] = '\0';
    row_is(2, UI_ATTR_CURSOR, want2);
    row_is(3, UI_ATTR_CURSOR, want3);
}

static void test_no_card_or_no_rom_is_a_warning(void) {
    unit.card = false;
    ui_set_unit(&u, &unit);
    row_is(11, UI_ATTR_WARN, " No SD card");
    unit.card = true;
    unit.rom = false;
    ui_set_unit(&u, &unit);
    row_is(11, UI_ATTR_WARN, " No IDC ROM: the 480Z can't boot");
}

static void test_turning_at_home_picks_a_drive_and_stops_at_the_ends(void) {
    event(ROTARY_CW);
    row_is(2, UI_ATTR_NORMAL, "A: (empty)");
    row_is(6, UI_ATTR_CURSOR, "B: (empty)");
    event(ROTARY_CW);
    row_is(6, UI_ATTR_CURSOR, "B: (empty)");
    event(ROTARY_CCW);
    event(ROTARY_CCW);
    row_is(2, UI_ATTR_CURSOR, "A: (empty)");
}

static void test_only_rows_that_change_are_dirty(void) {
    TEST_ASSERT_EQUAL_HEX32((1u << UI_ROWS) - 1u, u.screen.dirty); /* at first, everything */
    ui_drawn(&u);
    tick(100);
    TEST_ASSERT_EQUAL_HEX32(0, u.screen.dirty);
    event(ROTARY_CW);
    TEST_ASSERT_EQUAL_HEX32((1u << 2) | (1u << 3) | (1u << 6) | (1u << 7), u.screen.dirty);
}

/* --- choosing ---------------------------------------------------------------------------------------------- */

static void test_press_lists_the_folder_of_the_drives_image(void) {
    drive(0, "games/x.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "games");
    row_is(0, UI_ATTR_TITLE, " Drive A: choose an image");
    row_is(1, UI_ATTR_FAINT, " /games");
    row_is(2, UI_ATTR_CURSOR, "  (eject)");
    row_is(3, UI_ATTR_NORMAL, "  ../");
    row_is(4, UI_ATTR_FAINT, "  Reading the card...");
    row_is(14, UI_ATTR_FAINT, " Press: choose. Hold: back");
}

static void test_the_list_comes_sorted_with_the_cursor_on_the_drives_image(void) {
    drive(0, "games/x.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "games");
    const char *names[] = {"zz.imd", "x.imd", "sub/"};
    list_done(names, 3);
    row_is(2, UI_ATTR_NORMAL, "  (eject)");
    row_is(3, UI_ATTR_NORMAL, "  ../");
    row_is(4, UI_ATTR_NORMAL, "  sub/");
    row_is(5, UI_ATTR_CURSOR, "* x.imd");
    row_is(6, UI_ATTR_NORMAL, "  zz.imd");
    row_is(7, UI_ATTR_NORMAL, "");
}

static void test_an_empty_drive_lists_the_root_without_eject_or_up(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    row_is(1, UI_ATTR_FAINT, " /");
    const char *names[] = {"b.hfe", "a.imd"};
    list_done(names, 2);
    row_is(2, UI_ATTR_CURSOR, "  a.imd");
    row_is(3, UI_ATTR_NORMAL, "  b.hfe");
}

static void test_a_folder_with_no_images_says_so(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    list_done(NULL, 0);
    row_is(2, UI_ATTR_FAINT, "  (no images here)");
}

static void test_choosing_an_image_asks_for_it_and_goes_home_to_wait(void) {
    event(ROTARY_CW);
    event(ROTARY_PRESS); /* drive B */
    take(UI_REQ_LIST, 1, "");
    const char *names[] = {"a.imd"};
    list_done(names, 1);
    event(ROTARY_PRESS);
    take(UI_REQ_INSERT, 1, "a.imd");
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
    row_is(13, UI_ATTR_NORMAL, " B: inserting...");
    row_is(14, UI_ATTR_FAINT, " Working...");
    tick(UI_MESSAGE_MS * 2); /* the message stays while the request is out */
    row_is(13, UI_ATTR_NORMAL, " B: inserting...");
    drive(1, "a.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    ui_request_done(&u, true, NULL, now);
    row_is(6, UI_ATTR_CURSOR, "B: a.imd");
    row_is(13, UI_ATTR_NORMAL, " B: a.imd");
    row_is(14, UI_ATTR_FAINT, " Turn: pick a drive. Press: choose");
    tick(UI_MESSAGE_MS - 1);
    row_is(13, UI_ATTR_NORMAL, " B: a.imd");
    tick(1);
    row_is(13, UI_ATTR_NORMAL, "");
}

static void test_a_failed_insert_says_why(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *names[] = {"bad.imd"};
    list_done(names, 1);
    event(ROTARY_PRESS);
    take(UI_REQ_INSERT, 0, "bad.imd");
    ui_request_done(&u, false, "not an IMD image", now);
    row_is(13, UI_ATTR_WARN, " A: not an IMD image");
}

static void test_the_image_in_the_other_drive_is_marked_and_refused(void) {
    drive(0, "a.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    event(ROTARY_CW);
    event(ROTARY_PRESS); /* drive B */
    take(UI_REQ_LIST, 1, "");
    const char *names[] = {"a.imd", "b.imd"};
    list_done(names, 2);
    row_is(2, UI_ATTR_CURSOR, "A a.imd");
    event(ROTARY_PRESS);
    TEST_ASSERT_FALSE(ui_take_request(&u, &req));
    row_is(0, UI_ATTR_TITLE, " Drive B: choose an image");
    row_is(13, UI_ATTR_WARN, " That image is in drive A");
}

static void test_choosing_the_image_already_in_the_drive_just_goes_home(void) {
    drive(0, "a.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *names[] = {"a.imd"};
    list_done(names, 1);
    row_is(3, UI_ATTR_CURSOR, "* a.imd");
    event(ROTARY_PRESS);
    TEST_ASSERT_FALSE(ui_take_request(&u, &req));
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
}

static void test_into_a_folder_and_back_up_lands_on_that_folder(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *root[] = {"a.imd", "games/", "demos/"};
    list_done(root, 3);
    event(ROTARY_CW); /* demos/, games/, a.imd: to games/ */
    row_is(3, UI_ATTR_CURSOR, "  games/");
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "games");
    const char *games[] = {"x.imd"};
    list_done(games, 1);
    row_is(1, UI_ATTR_FAINT, " /games");
    row_is(2, UI_ATTR_CURSOR, "  ../");
    row_is(3, UI_ATTR_NORMAL, "  x.imd");
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    list_done(root, 3);
    row_is(3, UI_ATTR_CURSOR, "  games/");
}

static void test_an_image_in_a_folder_is_asked_for_by_its_whole_path(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *root[] = {"games/"};
    list_done(root, 1);
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "games");
    const char *games[] = {"x.imd"};
    list_done(games, 1);
    event(ROTARY_CW);
    event(ROTARY_PRESS);
    take(UI_REQ_INSERT, 0, "games/x.imd");
}

static void test_eject_asks_to_empty_the_drive(void) {
    drive(0, "a.imd", false, 40, false);
    ui_set_unit(&u, &unit);
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *names[] = {"a.imd"};
    list_done(names, 1);
    event(ROTARY_CCW); /* from a.imd up to (eject) */
    row_is(2, UI_ATTR_CURSOR, "  (eject)");
    event(ROTARY_PRESS);
    take(UI_REQ_EJECT, 0, "");
    row_is(13, UI_ATTR_NORMAL, " A: emptying...");
    drive(0, NULL, false, 0, false);
    ui_set_unit(&u, &unit);
    ui_request_done(&u, true, NULL, now);
    row_is(2, UI_ATTR_CURSOR, "A: (empty)");
    row_is(13, UI_ATTR_NORMAL, " A: empty");
}

static void test_a_folder_that_cannot_be_read_says_so(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    ui_request_done(&u, false, "FatFs 1", now);
    row_is(2, UI_ATTR_WARN, "  Can't read this folder");
}

static void test_a_long_press_goes_home(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    event(ROTARY_LONG_PRESS);
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
}

static void test_leaving_the_knob_alone_goes_home(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    list_done(NULL, 0);
    tick(UI_IDLE_MS - 1);
    row_is(0, UI_ATTR_TITLE, " Drive A: choose an image");
    tick(1);
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
}

static void test_a_list_that_comes_after_leaving_is_ignored(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    event(ROTARY_LONG_PRESS);
    const char *names[] = {"a.imd"};
    list_done(names, 1);
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
    event(ROTARY_PRESS); /* and choosing again asks again */
    take(UI_REQ_LIST, 0, "");
    row_is(2, UI_ATTR_FAINT, "  Reading the card...");
}

static void test_choosing_again_before_the_last_request_is_taken_is_refused(void) {
    event(ROTARY_PRESS); /* a LIST, not yet taken */
    event(ROTARY_LONG_PRESS);
    event(ROTARY_PRESS);
    row_is(0, UI_ATTR_TITLE, " RMFloppyEmu");
    row_is(13, UI_ATTR_WARN, " Busy: try again in a moment");
}

static void test_the_list_scrolls_to_keep_the_cursor_on_screen(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *names[] = {"00.imd", "01.imd", "02.imd", "03.imd", "04.imd", "05.imd", "06.imd",
                           "07.imd", "08.imd", "09.imd", "10.imd", "11.imd", "12.imd"};
    list_done(names, 13);
    row_is(12, UI_ATTR_NORMAL, "  10.imd"); /* 11 rows: 00-10 */
    for (unsigned i = 0; i < 11; i++)
        event(ROTARY_CW);
    row_is(2, UI_ATTR_NORMAL, "  01.imd");
    row_is(12, UI_ATTR_CURSOR, "  11.imd");
    event(ROTARY_CW);
    event(ROTARY_CW); /* stops at the last */
    row_is(12, UI_ATTR_CURSOR, "  12.imd");
    for (unsigned i = 0; i < 12; i++)
        event(ROTARY_CCW);
    row_is(2, UI_ATTR_CURSOR, "  00.imd");
}

static void test_a_long_name_scrolls_under_the_cursor_and_ends_in_a_tilde_elsewhere(void) {
    /* 45 characters, 38 columns: 7 steps of 250 ms, after a 1 s pause; then 1 s at the end, and round again */
    const char *name = "abcdefghijklmnopqrstuvwxyz0123456789ABCD.imd";
    TEST_ASSERT_EQUAL_size_t(44, strlen(name));
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    const char *names[] = {"a.imd", name};
    list_done(names, 2);
    row_is(3, UI_ATTR_NORMAL, "  abcdefghijklmnopqrstuvwxyz0123456789A~");
    event(ROTARY_CW);
    row_is(3, UI_ATTR_CURSOR, "  abcdefghijklmnopqrstuvwxyz0123456789A~");
    tick(UI_SCROLL_PAUSE_MS - 1);
    row_is(3, UI_ATTR_CURSOR, "  abcdefghijklmnopqrstuvwxyz0123456789A~");
    tick(1 + 3 * UI_SCROLL_STEP_MS);
    row_is(3, UI_ATTR_CURSOR, "  defghijklmnopqrstuvwxyz0123456789ABCD.");
    tick(3 * UI_SCROLL_STEP_MS); /* 2500 ms: 6 steps, the end */
    row_is(3, UI_ATTR_CURSOR, "  ghijklmnopqrstuvwxyz0123456789ABCD.imd");
    tick(UI_SCROLL_PAUSE_MS - 1); /* it stays for the pause */
    row_is(3, UI_ATTR_CURSOR, "  ghijklmnopqrstuvwxyz0123456789ABCD.imd");
    tick(1); /* 3500 ms = 2 x 1000 + 6 x 250: back to the start */
    row_is(3, UI_ATTR_CURSOR, "  abcdefghijklmnopqrstuvwxyz0123456789A~");
}

static void test_too_many_to_list_is_noted(void) {
    event(ROTARY_PRESS);
    take(UI_REQ_LIST, 0, "");
    dirlist_clear(&list);
    TEST_ASSERT_TRUE(dirlist_add(&list, "a.imd", false, false));
    list.truncated = true;
    ui_request_done(&u, true, NULL, now);
    row_is(13, UI_ATTR_WARN, " Too many to list: some are left out");
}

/* --- drive lights and the command row ----------------------------------------------------------------- */

static ui_activity_t act;

/* A command for drive d has been served: its counts go up. */
static void served(unsigned d, uint8_t cmd, bool writes, int track) {
    act.commands++;
    act.busy = false;
    memset(&act.last, 0, sizeof act.last);
    act.last.stage = IDC_ACT_END;
    act.last.cmd = cmd;
    act.last.has_drive = true;
    act.last.drive = (uint8_t)d;
    act.last.has_track = track >= 0;
    act.last.track = (uint8_t)(track >= 0 ? track : 0);
    act.drive_commands[d]++;
    act.drive_writing[d] = writes;
    act.drive_has_track[d] = track >= 0;
    act.drive_track[d] = act.last.track;
    ui_set_activity(&u, &act, now);
}

static void test_a_read_lights_the_drive_with_its_track_then_goes_out(void) {
    memset(&act, 0, sizeof act);
    ui_set_activity(&u, &act, now);
    row_is(5, UI_ATTR_NORMAL, "");
    served(0, 0x02, false, 12);
    row_is(5, UI_ATTR_READ, "   reading, track 12");
    row_is(9, UI_ATTR_NORMAL, "");
    tick(UI_LIGHT_HOLD_MS - 1);
    row_is(5, UI_ATTR_READ, "   reading, track 12");
    tick(1);
    row_is(5, UI_ATTR_NORMAL, "");
}

static void test_a_write_lights_drive_b_and_a_command_without_a_track_says_only_what(void) {
    memset(&act, 0, sizeof act);
    served(1, 0x04, true, 3);
    row_is(9, UI_ATTR_WRITE, "   writing, track 3");
    served(1, 0x01, false, -1);
    row_is(9, UI_ATTR_READ, "   reading");
}

static void test_the_light_stays_on_while_a_command_is_being_served(void) {
    memset(&act, 0, sizeof act);
    act.commands = 1;
    act.busy = true;
    act.last.stage = IDC_ACT_DRIVE;
    act.last.cmd = 0x0C;
    act.last.has_drive = true;
    act.last.drive = 1;
    act.drive_commands[1] = 1;
    act.drive_writing[1] = true;
    act.drive_has_track[1] = true;
    act.drive_track[1] = 7;
    for (unsigned i = 0; i < 50; i++) { /* a format taking a second */
        now += 20;
        ui_set_activity(&u, &act, now);
        row_is(9, UI_ATTR_WRITE, "   writing, track 7");
    }
    act.busy = false;
    act.last.stage = IDC_ACT_END;
    now += UI_LIGHT_HOLD_MS;
    ui_set_activity(&u, &act, now);
    row_is(9, UI_ATTR_NORMAL, "");
}

static void test_commands_are_shown_only_with_show_commands(void) {
    memset(&act, 0, sizeof act);
    served(0, 0x02, false, 12);
    row_is(10, UI_ATTR_FAINT, "");
    ui_set_options(&u, true);
    act.last.side = 1;
    act.last.third = 5;
    ui_set_activity(&u, &act, now);
    /* " %02X %-14s %-15s#%lu" (core/ui.c, draw_command) */
    row_is(10, UI_ATTR_FAINT, " 02 read           A/1 T12 R5     #1");
}

static void test_the_command_row_names_sectors_counts_and_drive_less_commands(void) {
    ui_set_options(&u, true);
    memset(&act, 0, sizeof act);
    served(1, 0x0E, false, 4);
    act.last.third = 9;
    ui_set_activity(&u, &act, now);
    row_is(10, UI_ATTR_FAINT, " 0E read sector    B/0 T4 S9      #1");
    served(1, 0x0C, true, 7);
    act.last.third = 16;
    ui_set_activity(&u, &act, now);
    row_is(10, UI_ATTR_FAINT, " 0C format         B/0 T7 N16     #2");
    act.commands++;
    memset(&act.last, 0, sizeof act.last);
    act.last.cmd = 0x00;
    act.last.stage = IDC_ACT_END;
    ui_set_activity(&u, &act, now);
    row_is(10, UI_ATTR_FAINT, " 00 go fast                       #3");
}

static void test_a_command_just_started_leaves_the_last_one_shown(void) {
    ui_set_options(&u, true);
    memset(&act, 0, sizeof act);
    served(0, 0x02, false, 12);
    act.last.third = 5;
    ui_set_activity(&u, &act, now);
    act.commands++; /* the next read: its drive byte hasn't come */
    act.busy = true;
    memset(&act.last, 0, sizeof act.last);
    act.last.stage = IDC_ACT_START;
    act.last.cmd = 0x02;
    ui_set_activity(&u, &act, now);
    row_is(10, UI_ATTR_FAINT, " 02 read           A/0 T12 R5     #1");
    act.last.stage = IDC_ACT_DRIVE; /* now it has */
    act.last.has_drive = true;
    act.last.has_track = true;
    act.last.track = 13;
    act.last.third = 1;
    ui_set_activity(&u, &act, now);
    row_is(10, UI_ATTR_FAINT, " 02 read           A/0 T13 R1     #2");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_home_shows_each_drive_with_its_kind_and_the_cursor_on_a);
    RUN_TEST(test_an_empty_drive_says_so);
    RUN_TEST(test_a_long_path_runs_onto_the_next_row_and_ends_in_a_tilde_if_still_too_long);
    RUN_TEST(test_no_card_or_no_rom_is_a_warning);
    RUN_TEST(test_turning_at_home_picks_a_drive_and_stops_at_the_ends);
    RUN_TEST(test_only_rows_that_change_are_dirty);
    RUN_TEST(test_press_lists_the_folder_of_the_drives_image);
    RUN_TEST(test_the_list_comes_sorted_with_the_cursor_on_the_drives_image);
    RUN_TEST(test_an_empty_drive_lists_the_root_without_eject_or_up);
    RUN_TEST(test_a_folder_with_no_images_says_so);
    RUN_TEST(test_choosing_an_image_asks_for_it_and_goes_home_to_wait);
    RUN_TEST(test_a_failed_insert_says_why);
    RUN_TEST(test_the_image_in_the_other_drive_is_marked_and_refused);
    RUN_TEST(test_choosing_the_image_already_in_the_drive_just_goes_home);
    RUN_TEST(test_into_a_folder_and_back_up_lands_on_that_folder);
    RUN_TEST(test_an_image_in_a_folder_is_asked_for_by_its_whole_path);
    RUN_TEST(test_eject_asks_to_empty_the_drive);
    RUN_TEST(test_a_folder_that_cannot_be_read_says_so);
    RUN_TEST(test_a_long_press_goes_home);
    RUN_TEST(test_leaving_the_knob_alone_goes_home);
    RUN_TEST(test_a_list_that_comes_after_leaving_is_ignored);
    RUN_TEST(test_choosing_again_before_the_last_request_is_taken_is_refused);
    RUN_TEST(test_the_list_scrolls_to_keep_the_cursor_on_screen);
    RUN_TEST(test_a_long_name_scrolls_under_the_cursor_and_ends_in_a_tilde_elsewhere);
    RUN_TEST(test_too_many_to_list_is_noted);
    RUN_TEST(test_a_read_lights_the_drive_with_its_track_then_goes_out);
    RUN_TEST(test_a_write_lights_drive_b_and_a_command_without_a_track_says_only_what);
    RUN_TEST(test_the_light_stays_on_while_a_command_is_being_served);
    RUN_TEST(test_commands_are_shown_only_with_show_commands);
    RUN_TEST(test_the_command_row_names_sectors_counts_and_drive_less_commands);
    RUN_TEST(test_a_command_just_started_leaves_the_last_one_shown);
    return UNITY_END();
}
