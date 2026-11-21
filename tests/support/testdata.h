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
 * Test-only: marks a test that reads testdata/ (RM ROMs, real disc images: docs/testing.md section 5).
 * Call RMFE_NEEDS_TESTDATA() before reading the data.
 *
 * On the PC it does nothing, and missing data fails the test (docs/testing.md rule 7). The Pico 2 build
 * of the tests (tests/pico/, RMFE_TEST_ON_TARGET) has no testdata/, so there the test stops and Unity
 * reports it as IGNORE with RMFE_NEEDS_TESTDATA_MSG, by name. tests/pico/run_on_target.py lists every
 * such test and fails on any other IGNORE, so nothing is skipped silently.
 */
#ifndef RMFE_TESTS_TESTDATA_H
#define RMFE_TESTS_TESTDATA_H

#include "unity.h"

#define RMFE_NEEDS_TESTDATA_MSG "needs testdata/: not run on the target"

#ifdef RMFE_TEST_ON_TARGET
#define RMFE_NEEDS_TESTDATA() TEST_IGNORE_MESSAGE(RMFE_NEEDS_TESTDATA_MSG)
#else
#define RMFE_NEEDS_TESTDATA() ((void)0)
#endif

#endif
