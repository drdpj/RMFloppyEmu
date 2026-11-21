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
#include <stdlib.h>

#include "unity.h"

/* A failed RMFE_ASSERT in code under test fails the current test with the assert's location.
 * Unity's TEST_FAIL long-jumps back to the test runner; abort() is a backstop if it ever returns. */
_Noreturn void rmfe_assert_fail(const char *file, int line, const char *expr) {
    char msg[256];
    snprintf(msg, sizeof msg, "RMFE_ASSERT(%s) failed at %s:%d", expr, file, line);
    TEST_FAIL_MESSAGE(msg);
    abort();
}
