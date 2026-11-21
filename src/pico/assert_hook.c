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
#include "core/rmfe_assert.h"

#include <stdio.h>

#include "pico/stdlib.h"

/* Log where it happened, then halt with the LED flashing fast, so a failed assert is visible
 * on the bench even without the console attached. */
_Noreturn void rmfe_assert_fail(const char *file, int line, const char *expr) {
    printf("\nASSERT FAILED %s:%d: %s\n", file, line, expr);
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (;;) {
        gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        busy_wait_ms(100);
    }
}
