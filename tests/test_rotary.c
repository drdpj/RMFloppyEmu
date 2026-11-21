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
#include "core/rotary.h"

#include <string.h>

#include "unity.h"

/*
 * Expected values worked out by hand from the Gray sequence (core/rotary.h): states are (A << 1) | B, 1 = open.
 * Clockwise is B changing first: from rest 3 -> 2 -> 0 -> 1 -> 3. Anticlockwise, A first: 3 -> 1 -> 0 -> 2 -> 3.
 */
static const unsigned cw_cycle[] = {2, 0, 1, 3};
static const unsigned ccw_cycle[] = {1, 0, 2, 3};

static rotary_t r;
static uint32_t now;
static rotary_event_t got[64];
static unsigned ngot;

void setUp(void) {
    now = 1000;
    ngot = 0;
    rotary_init(&r, CONFIG_ROTARY_FULL, false, now);
}
void tearDown(void) {}

/* One sample of the pins, 1 ms after the last. */
static void at(unsigned ab, bool pressed) {
    now++;
    rotary_event_t ev[ROTARY_MAX_EVENTS];
    unsigned n = rotary_update(&r, ab, pressed, now, ev);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(ROTARY_MAX_EVENTS, n);
    for (unsigned i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(ngot < sizeof got / sizeof got[0]);
        got[ngot++] = ev[i];
    }
}

static void turn(const unsigned *states, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        at(states[i], false);
}

/* `samples` samples, 1 ms apart. A level first seen at sample t has held n ms at sample t + n, the (n + 1)th. */
static void hold(bool pressed, unsigned samples) {
    for (unsigned i = 0; i < samples; i++)
        at(r.ab, pressed);
}

static void expect(const rotary_event_t *want, unsigned n) {
    TEST_ASSERT_EQUAL_UINT(n, ngot);
    for (unsigned i = 0; i < n; i++)
        TEST_ASSERT_EQUAL(want[i], got[i]);
}

/* --- turning --------------------------------------------------------------------------------------- */

static void test_a_full_cycle_b_first_is_one_step_clockwise(void) {
    turn(cw_cycle, 4);
    const rotary_event_t want[] = {ROTARY_CW};
    expect(want, 1);
}

static void test_a_full_cycle_a_first_is_one_step_anticlockwise(void) {
    turn(ccw_cycle, 4);
    const rotary_event_t want[] = {ROTARY_CCW};
    expect(want, 1);
}

static void test_no_step_until_the_detent_is_reached(void) {
    turn(cw_cycle, 3);
    expect(NULL, 0);
    at(3, false);
    TEST_ASSERT_EQUAL_UINT(1, ngot);
}

static void test_bounce_on_a_contact_cancels_out(void) {
    const unsigned bouncy[] = {2, 3, 2, 3, 2, 0, 2, 0, 1, 0, 1, 3, 1, 3};
    turn(bouncy, sizeof bouncy / sizeof bouncy[0]);
    const rotary_event_t want[] = {ROTARY_CW};
    expect(want, 1);
}

static void test_turning_half_way_and_back_is_no_step(void) {
    const unsigned back[] = {2, 0, 2, 3};
    turn(back, 4);
    expect(NULL, 0);
}

static void test_reverse_swaps_the_directions(void) {
    rotary_init(&r, CONFIG_ROTARY_FULL, true, now);
    turn(cw_cycle, 4);
    turn(ccw_cycle, 4);
    const rotary_event_t want[] = {ROTARY_CCW, ROTARY_CW};
    expect(want, 2);
}

static void test_half_steps_at_both_rests(void) {
    rotary_init(&r, CONFIG_ROTARY_HALF, false, now);
    turn(cw_cycle, 4);
    turn(ccw_cycle, 2);
    const rotary_event_t want[] = {ROTARY_CW, ROTARY_CW, ROTARY_CCW};
    expect(want, 3);
}

static void test_quarter_steps_at_every_move(void) {
    rotary_init(&r, CONFIG_ROTARY_QUARTER, false, now);
    turn(cw_cycle, 4);
    at(1, false); /* one back */
    const rotary_event_t want[] = {ROTARY_CW, ROTARY_CW, ROTARY_CW, ROTARY_CW, ROTARY_CCW};
    expect(want, 5);
}

static void test_a_missed_state_is_counted_and_gives_no_step(void) {
    const unsigned jump[] = {0, 1, 3}; /* 3 -> 0: both contacts changed */
    turn(jump, 3);
    expect(NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(1, r.missed);
    turn(cw_cycle, 4); /* and the next detent is counted normally */
    const rotary_event_t want[] = {ROTARY_CW};
    expect(want, 1);
}

static void test_many_detents_each_way(void) {
    for (unsigned i = 0; i < 20; i++)
        turn(cw_cycle, 4);
    for (unsigned i = 0; i < 20; i++)
        turn(ccw_cycle, 4);
    TEST_ASSERT_EQUAL_UINT(40, ngot);
    for (unsigned i = 0; i < 40; i++)
        TEST_ASSERT_EQUAL(i < 20 ? ROTARY_CW : ROTARY_CCW, got[i]);
}

/* --- the switch -------------------------------------------------------------------------------------- */

static void test_a_short_press_is_given_on_release(void) {
    hold(true, 100);
    expect(NULL, 0);
    hold(false, ROTARY_DEBOUNCE_MS); /* released for 9 ms: not yet */
    expect(NULL, 0);
    hold(false, 1);
    const rotary_event_t want[] = {ROTARY_PRESS};
    expect(want, 1);
}

static void test_bounce_shorter_than_the_debounce_is_ignored(void) {
    for (unsigned i = 0; i < 5; i++) {
        hold(true, ROTARY_DEBOUNCE_MS - 1);
        hold(false, ROTARY_DEBOUNCE_MS - 1);
    }
    hold(false, 50);
    expect(NULL, 0);
}

static void test_a_bouncy_press_is_one_press(void) {
    hold(true, 3);
    hold(false, 2);
    hold(true, 200);
    hold(false, 3);
    hold(true, 1);
    hold(false, 50);
    const rotary_event_t want[] = {ROTARY_PRESS};
    expect(want, 1);
}

static void test_holding_gives_a_long_press_once_and_nothing_on_release(void) {
    hold(true, ROTARY_LONG_PRESS_MS + ROTARY_DEBOUNCE_MS);
    const rotary_event_t want[] = {ROTARY_LONG_PRESS};
    expect(want, 1);
    hold(true, 2000);
    hold(false, 50);
    expect(want, 1);
}

static void test_the_long_press_is_timed_from_when_the_press_began(void) {
    /* first seen pressed at sample `began`; debounced 10 ms later; long once 800 ms have passed since `began`,
       at the 801st sample */
    uint32_t began = now + 1;
    hold(true, ROTARY_LONG_PRESS_MS);
    expect(NULL, 0);
    hold(true, 1);
    TEST_ASSERT_EQUAL_UINT32(began + ROTARY_LONG_PRESS_MS, now);
    TEST_ASSERT_EQUAL_UINT(1, ngot);
}

static void test_time_may_wrap(void) {
    now = 0xFFFFFFF0u;
    rotary_init(&r, CONFIG_ROTARY_FULL, false, now);
    hold(true, 100); /* crosses 0 */
    hold(false, ROTARY_DEBOUNCE_MS + 1);
    const rotary_event_t want[] = {ROTARY_PRESS};
    expect(want, 1);
}

static void test_a_step_and_a_press_in_one_sample(void) {
    turn(cw_cycle, 3);
    hold(true, ROTARY_DEBOUNCE_MS + 5);
    now++;
    rotary_event_t ev[ROTARY_MAX_EVENTS];
    /* the last move to rest, and the release, debounced at this very sample */
    r.raw_pressed = false;
    r.raw_since = now - ROTARY_DEBOUNCE_MS;
    unsigned n = rotary_update(&r, 3, false, now, ev);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL(ROTARY_CW, ev[0]);
    TEST_ASSERT_EQUAL(ROTARY_PRESS, ev[1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_full_cycle_b_first_is_one_step_clockwise);
    RUN_TEST(test_a_full_cycle_a_first_is_one_step_anticlockwise);
    RUN_TEST(test_no_step_until_the_detent_is_reached);
    RUN_TEST(test_bounce_on_a_contact_cancels_out);
    RUN_TEST(test_turning_half_way_and_back_is_no_step);
    RUN_TEST(test_reverse_swaps_the_directions);
    RUN_TEST(test_half_steps_at_both_rests);
    RUN_TEST(test_quarter_steps_at_every_move);
    RUN_TEST(test_a_missed_state_is_counted_and_gives_no_step);
    RUN_TEST(test_many_detents_each_way);
    RUN_TEST(test_a_short_press_is_given_on_release);
    RUN_TEST(test_bounce_shorter_than_the_debounce_is_ignored);
    RUN_TEST(test_a_bouncy_press_is_one_press);
    RUN_TEST(test_holding_gives_a_long_press_once_and_nothing_on_release);
    RUN_TEST(test_the_long_press_is_timed_from_when_the_press_began);
    RUN_TEST(test_time_may_wrap);
    RUN_TEST(test_a_step_and_a_press_in_one_sample);
    return UNITY_END();
}
