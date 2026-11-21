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
 * Forced (-include) into each tests/test_<module>.c in the Pico 2 build (tests/pico/CMakeLists.txt),
 * which compiles it with -Dmain=rmfe_test_main so that runner.c's main() can time and measure the run.
 * With that definition this is the prototype of rmfe_test_main, which -Wmissing-prototypes requires.
 */
#ifndef RMFE_TESTS_PICO_TEST_MAIN_H
#define RMFE_TESTS_PICO_TEST_MAIN_H

int main(void);

#endif
