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

/* Peak stack by painting: see stack_meter.h. As tests/pico/runner.c measures it for the unit tests. */
#include "pico/stack_meter.h"

#include <stdint.h>

#define PAINT     0xC5A3E1D7u
#define PAINT_GAP 64u /* left unpainted below the painter's own frame */

extern uint32_t __StackTop[], __StackBottom[];       /* core 0: SCRATCH_Y */
extern uint32_t __StackOneTop[], __StackOneBottom[]; /* core 1: SCRATCH_X */

static void paint(uint32_t *lo, uint32_t *hi) {
    for (volatile uint32_t *p = lo; p < hi; p++)
        *p = PAINT;
}

void __attribute__((noinline)) stack_meter_paint_core0(void) {
    uint32_t here;
    uintptr_t hi = ((uintptr_t)&here - PAINT_GAP) & ~(uintptr_t)3u;
    paint(__StackBottom, (uint32_t *)hi);
}

void stack_meter_paint_core1(void) {
    paint(__StackOneBottom, __StackOneTop);
}

uint32_t stack_meter_peak(unsigned core, uint32_t *size) {
    const uint32_t *lo = core ? __StackOneBottom : __StackBottom, *top = core ? __StackOneTop : __StackTop;
    const volatile uint32_t *p = lo;
    while (p < top && *p == PAINT)
        p++;
    *size = (uint32_t)((uintptr_t)top - (uintptr_t)lo);
    return (uint32_t)((uintptr_t)top - (uintptr_t)p);
}
