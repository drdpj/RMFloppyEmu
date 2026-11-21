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

/* Test-only: turns bitcells into flux intervals, as a flux capture would record them, with
 * deterministic jitter and speed error, so the PLL and SCP reader can be tested from known tracks. */
#ifndef RMFE_TESTS_FLUX_SYNTH_H
#define RMFE_TESTS_FLUX_SYNTH_H

#include <stdint.h>

/* Writes the intervals between 1 cells of cells[0..ncells) (taken repeat times round) into flux[],
 * in ticks of a clock with cell_ticks ticks per nominal cell, scaled by speed_permille (1000 =
 * nominal, 1020 = 2% slow) and jittered by up to +-jitter ticks. Returns the count, or 0 if cap
 * is too small. *total gets the summed ticks of one revolution (the index-to-index time). */
uint32_t synth_flux(const uint8_t *cells, uint32_t ncells, unsigned repeat, uint32_t cell_ticks,
                    uint32_t speed_permille, uint32_t jitter, uint32_t *flux, uint32_t cap, uint32_t *total);

#endif
