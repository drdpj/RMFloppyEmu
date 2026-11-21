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

/* The knob's decoder: see rotary.h. */
#include "core/rotary.h"

#include "core/rmfe_assert.h"

#include <string.h>

/* +1 for a move with A changing (A leading), -1 with B changing, 0 for none or both, indexed by
 * (old state << 2) | new state. */
static const int8_t move[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void rotary_init(rotary_t *r, config_rotary_t kind, bool reverse, uint32_t now_ms) {
    RMFE_ASSERT(r);
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->reverse = reverse;
    r->ab = ROTARY_AB_REST;
    r->raw_since = now_ms;
}

/* B leading is clockwise (rotary.h): a negative count. */
static rotary_event_t step(const rotary_t *r, int count) {
    bool cw = (count < 0) != r->reverse;
    return cw ? ROTARY_CW : ROTARY_CCW;
}

static unsigned turn(rotary_t *r, unsigned ab, rotary_event_t *out) {
    if (ab == r->ab) return 0;
    unsigned old = r->ab;
    r->ab = ab;
    if ((old ^ ab) == 3u) {
        r->missed++;
        return 0;
    }
    r->count += move[(old << 2) | ab];
    int need = 0;
    switch (r->kind) {
        case CONFIG_ROTARY_FULL:
            if (ab != ROTARY_AB_REST) return 0;
            need = 3;
            break;
        case CONFIG_ROTARY_HALF:
            if (ab != ROTARY_AB_REST && ab != 0u) return 0;
            need = 2;
            break;
        case CONFIG_ROTARY_QUARTER:
            need = 1;
            break;
    }
    int count = r->count;
    r->count = 0; /* at a detent, whatever happened on the way */
    if (count >= need || count <= -need) {
        out[0] = step(r, count);
        return 1;
    }
    return 0;
}

static unsigned button(rotary_t *r, bool pressed, uint32_t now, rotary_event_t *out) {
    if (pressed != r->raw_pressed) {
        r->raw_pressed = pressed;
        r->raw_since = now;
    }
    if (r->raw_pressed != r->pressed && now - r->raw_since >= ROTARY_DEBOUNCE_MS) {
        r->pressed = r->raw_pressed;
        if (r->pressed) {
            r->pressed_at = r->raw_since;
            r->long_given = false;
        } else if (!r->long_given) {
            out[0] = ROTARY_PRESS;
            return 1;
        }
    }
    if (r->pressed && !r->long_given && now - r->pressed_at >= ROTARY_LONG_PRESS_MS) {
        r->long_given = true;
        out[0] = ROTARY_LONG_PRESS;
        return 1;
    }
    return 0;
}

unsigned rotary_update(rotary_t *r, unsigned ab, bool pressed, uint32_t now_ms, rotary_event_t *out) {
    RMFE_ASSERT(r && out && ab <= 3u);
    unsigned n = turn(r, ab, out);
    return n + button(r, pressed, now_ms, out + n);
}
