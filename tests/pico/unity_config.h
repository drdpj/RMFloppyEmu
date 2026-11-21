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
 * Unity's configuration for the Pico 2 build of the tests (UNITY_INCLUDE_CONFIG_H, tests/pico/CMakeLists.txt).
 * Unity writes through runner.c, which times its console output so that it can be left out of the tests' time.
 */
#ifndef RMFE_TESTS_PICO_UNITY_CONFIG_H
#define RMFE_TESTS_PICO_UNITY_CONFIG_H

#define UNITY_OUTPUT_CHAR(a)                 rmfe_unity_putc(a)
#define UNITY_OUTPUT_CHAR_HEADER_DECLARATION rmfe_unity_putc(int)

#endif
