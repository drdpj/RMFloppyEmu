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
 * RMFE_ASSERT: for programmer errors only (broken invariants), never for bad data from the 480Z,
 * a disk image or a ROM (docs/coding.md section 8). The platform supplies rmfe_assert_fail():
 * src/pico/assert_hook.c logs and halts; tests/support/assert_hook.c fails the current test.
 */
#ifndef RMFE_CORE_RMFE_ASSERT_H
#define RMFE_CORE_RMFE_ASSERT_H

_Noreturn void rmfe_assert_fail(const char *file, int line, const char *expr);

#define RMFE_ASSERT(cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) rmfe_assert_fail(__FILE__, __LINE__, #cond);                                                      \
    } while (0)

#endif
