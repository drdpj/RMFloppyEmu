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
#include "core/usb_share.h"

#include "unity.h"

/* Expected actions from the rules in core/usb_share.h and docs/sd-card.md, "USB storage". */

static usb_share_t s;
static usb_share_input_t in;

void setUp(void) {
    usb_share_init(&s, true);
    in = (usb_share_input_t){.host = false, .connected = 0, .eject = false, .ready = true};
}
void tearDown(void) {}

static usb_share_action_t step(void) {
    usb_share_action_t a = usb_share_step(&s, &in);
    in.eject = false; /* an eject is told once */
    return a;
}

static void connect(void) {
    in.host = true;
    in.connected++;
}

static void test_nothing_happens_without_a_pc(void) {
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    TEST_ASSERT_FALSE(s.lent);
}

static void test_a_pc_gets_the_card_once_the_unit_is_ready_and_keeps_it(void) {
    connect();
    in.ready = false;
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    in.ready = true;
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    TEST_ASSERT_TRUE(s.lent);
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    in.ready = false; /* the card is unmounted while lent: no longer matters */
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
}

static void test_with_usb_storage_off_a_pc_never_gets_it(void) {
    usb_share_init(&s, false);
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
}

static void test_an_eject_takes_it_back_until_the_pc_connects_again(void) {
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    in.eject = true;
    TEST_ASSERT_EQUAL(USB_SHARE_TAKE_BACK, step());
    TEST_ASSERT_FALSE(s.lent);
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step()); /* still connected, but ejected */
    in.host = false;
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
}

static void test_a_pc_that_goes_gives_it_back_and_gets_it_again_when_it_returns(void) {
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    in.host = false;
    TEST_ASSERT_EQUAL(USB_SHARE_TAKE_BACK, step());
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
}

/* Unplugged and plugged in again between two looks: the count shows a new connection. */
static void test_a_reconnection_between_two_looks_still_forgets_the_eject(void) {
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    in.eject = true;
    TEST_ASSERT_EQUAL(USB_SHARE_TAKE_BACK, step());
    in.connected++;
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
}

static void test_turning_usb_storage_off_takes_it_back_and_on_again_lends_it_even_after_an_eject(void) {
    connect();
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    usb_share_set_enabled(&s, false);
    TEST_ASSERT_EQUAL(USB_SHARE_TAKE_BACK, step());
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    usb_share_set_enabled(&s, true);
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
    in.eject = true;
    TEST_ASSERT_EQUAL(USB_SHARE_TAKE_BACK, step());
    usb_share_set_enabled(&s, true); /* already on: no change, so the eject stands */
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    usb_share_set_enabled(&s, false);
    usb_share_set_enabled(&s, true);
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
}

static void test_an_eject_while_the_unit_has_the_card_changes_nothing(void) {
    connect();
    in.ready = false;
    in.eject = true;
    TEST_ASSERT_EQUAL(USB_SHARE_NOTHING, step());
    in.ready = true;
    TEST_ASSERT_EQUAL(USB_SHARE_LEND, step());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_happens_without_a_pc);
    RUN_TEST(test_a_pc_gets_the_card_once_the_unit_is_ready_and_keeps_it);
    RUN_TEST(test_with_usb_storage_off_a_pc_never_gets_it);
    RUN_TEST(test_an_eject_takes_it_back_until_the_pc_connects_again);
    RUN_TEST(test_a_pc_that_goes_gives_it_back_and_gets_it_again_when_it_returns);
    RUN_TEST(test_a_reconnection_between_two_looks_still_forgets_the_eject);
    RUN_TEST(test_turning_usb_storage_off_takes_it_back_and_on_again_lends_it_even_after_an_eject);
    RUN_TEST(test_an_eject_while_the_unit_has_the_card_changes_nothing);
    return UNITY_END();
}
