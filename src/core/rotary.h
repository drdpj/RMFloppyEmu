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

/*
 * The knob: a quadrature rotary encoder with a push switch (an EC11; docs/hardware.md). The platform passes in
 * the pins' levels whenever one may have changed, and at least every few milliseconds, with the time; this turns
 * them into steps and presses.
 *
 * Turning: the two contacts A and B go through the Gray sequence 11 10 00 01 (one way) or 11 01 00 10 (the other),
 * 1 being open (pulled up). Each move to a neighbouring state counts +1 or -1; contact bounce only moves back and
 * forth between two neighbours, so it cancels out. A step is given when the count reaches the detent:
 *   - full: back at rest (both open, 11) having moved at least three of the four states one way: one step per
 *     detent of an EC11 with as many detents as pulses (FlashFloppy's "full", the default);
 *   - half: at 11 or 00, having moved two states one way;
 *   - quarter: every move.
 * Both contacts changing at once means a state was missed: it is counted (rotary_t.missed) and ignored.
 * Clockwise is B changing before A on the encoder as wired in docs/hardware.md (measured with
 * tests/hil/run_display_probe.py); "reverse" (RMFE.CFG rotary = ...,reverse) swaps the two.
 *
 * The switch: a change of level counts once it has held for ROTARY_DEBOUNCE_MS. Letting go before
 * ROTARY_LONG_PRESS_MS is a press; holding it that long is a long press, given as soon as it is reached, and the
 * release after it gives nothing.
 *
 * Deterministic: time is passed in, in milliseconds (wrapping is fine).
 */
#ifndef RMFE_CORE_ROTARY_H
#define RMFE_CORE_ROTARY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/config.h"

#define ROTARY_DEBOUNCE_MS   10u
#define ROTARY_LONG_PRESS_MS 800u
#define ROTARY_AB_REST       3u /* both contacts open */
#define ROTARY_MAX_EVENTS    2u /* rotary_update() gives at most a step and a switch event */

typedef enum {
    ROTARY_CW = 1,
    ROTARY_CCW,
    ROTARY_PRESS,
    ROTARY_LONG_PRESS,
} rotary_event_t;

typedef struct {
    config_rotary_t kind;
    bool reverse;
    unsigned ab;      /* the contacts' last state: bit 1 A, bit 0 B, 1 = open */
    int count;        /* moves since the last step or rest */
    uint32_t missed;  /* moves where both contacts changed at once */
    bool pressed;     /* the switch, debounced */
    bool raw_pressed; /* the level being debounced */
    uint32_t raw_since;
    uint32_t pressed_at;
    bool long_given;
} rotary_t;

/* Starts with the contacts at rest and the switch up. */
void rotary_init(rotary_t *r, config_rotary_t kind, bool reverse, uint32_t now_ms);

/*
 * The pins now: ab (bit 1 A, bit 0 B, 1 = open), pressed (the switch closed), at now_ms. Writes the events this
 * gives into out (at least ROTARY_MAX_EVENTS) and returns how many.
 */
unsigned rotary_update(rotary_t *r, unsigned ab, bool pressed, uint32_t now_ms, rotary_event_t *out);

#endif
