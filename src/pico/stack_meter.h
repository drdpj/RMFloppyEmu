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
 * The peak stack of each core, measured by painting (docs/plan.md, milestone 6: the firmware's real peak stack).
 * Each core's stack is 4 KB with the hardware limit at its bottom (CMakeLists.txt): core 0's in SCRATCH_Y, core 1's
 * in SCRATCH_X. Unused stack keeps the paint; the lowest word that lost it marks the deepest the stack has been.
 */
#ifndef RMFE_PICO_STACK_METER_H
#define RMFE_PICO_STACK_METER_H

#include <stdint.h>

/* Core 0, first thing in main(): paints its stack below the caller's frame. */
void stack_meter_paint_core0(void);

/* Core 0, before starting core 1: paints core 1's whole stack. */
void stack_meter_paint_core1(void);

/* The bytes of stack core (0 or 1) has used at most so far, and its size. Either core may ask. */
uint32_t stack_meter_peak(unsigned core, uint32_t *size);

#endif
