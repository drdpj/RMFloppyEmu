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
 * Runs one unit-test module (tests/test_<module>.c, its main() renamed rmfe_test_main) on the Pico 2 and
 * reports on the debug console: Unity's own output, then one RMFE-TARGET-RESULT line with the time taken,
 * the stack and heap used and the static RAM, then RMFE-TARGET-DONE. The time is given in total and spent
 * writing Unity's output (115200 baud, about 87 us a character), so the tests' own time is the difference.
 * tests/pico/run_on_target.py reads it.
 *
 * Stack use is measured by painting: the 64 KB below the top of core 0's stack are filled with a pattern
 * before the tests and scanned afterwards. The SDK gives core 0 a nominal 2 KB (PICO_STACK_SIZE) at the
 * top of SCRATCH_Y; below that lie SCRATCH_X (core 1's stack) and then the top of main RAM, where the heap
 * ends, and nothing stops the stack running on into them. Hence the wide painted area, and the check that
 * the heap never reached it.
 */
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "core/disk.h"
#include "core/idc.h"
#include "hardware/clocks.h"
#include "hardware/structs/timer.h"
#include "pico/stdlib.h"
#include "unity.h"

int rmfe_test_main(void);

/* Linker symbols (pico_standard_link, section_end.incl) */
extern uint8_t __end__[];       /* end of static RAM (.data, .bss): the heap starts here */
extern uint8_t __StackTop[];    /* top of core 0's stack (end of SCRATCH_Y) */
extern uint8_t __StackBottom[]; /* its nominal bottom: __StackTop - PICO_STACK_SIZE */

#define STACK_PAINT       0xC5A3E1D7u
#define STACK_PAINT_BYTES (64u * 1024u)
#define STACK_PAINT_GAP   256u /* left unpainted below paint_stack()'s own frame */

static uint32_t *paint_lo, *paint_hi;
static uint64_t output_us; /* time spent in rmfe_unity_putc */

/* Unity's UNITY_OUTPUT_CHAR (tests/pico/CMakeLists.txt); declared by unity_internals.h */
void rmfe_unity_putc(int c) {
    uint64_t t = time_us_64();
    (void)putchar(c); /* a console error can't be reported anywhere else */
    output_us += time_us_64() - t;
}

static uintptr_t heap_top(void) {
    struct mallinfo mi = mallinfo();
    return (uintptr_t)__end__ + (uintptr_t)mi.arena; /* arena: bytes the heap has taken from sbrk */
}

/* Not inlined, so that its frame lies below main()'s and the painting starts below both. */
static void __attribute__((noinline)) paint_stack(void) {
    uint8_t here;
    uintptr_t hi = ((uintptr_t)&here - STACK_PAINT_GAP) & ~(uintptr_t)3u;
    uintptr_t lo = (uintptr_t)__StackTop - STACK_PAINT_BYTES;
    if (lo < heap_top()) lo = (heap_top() + 3u) & ~(uintptr_t)3u;
    paint_lo = (uint32_t *)lo;
    paint_hi = (uint32_t *)hi;
    for (volatile uint32_t *p = paint_lo; p < paint_hi; p++)
        *p = STACK_PAINT;
}

/* Bytes of stack used, from __StackTop down to the lowest word that lost its paint. */
static uint32_t stack_peak(bool *overflowed) {
    const volatile uint32_t *p = paint_lo;
    while (p < paint_hi && *p == STACK_PAINT)
        p++;
    *overflowed = p == paint_lo; /* used the whole painted area: the true figure is at least this */
    return (uint32_t)((uintptr_t)__StackTop - (uintptr_t)p);
}

int main(void) {
    /* The timer stops while the debugger has either core halted (DBGPAUSE), and OpenOCD keeps one halted
     * for a while after flashing and resetting: long enough, without this, to miss a whole module's run. */
    timer_hw->dbgpause = 0;
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    printf("\nRMFE-TARGET-BEGIN %s\n", RMFE_TEST_MODULE);
    paint_stack();
    uint64_t t0 = time_us_64();
    int failures = rmfe_test_main();
    uint64_t us = time_us_64() - t0;
    bool overflowed;
    uint32_t stack = stack_peak(&overflowed);
    uintptr_t heap_end = heap_top();
    bool heap_met_stack = heap_end > (uintptr_t)paint_lo;

    printf("RMFE-TARGET-RESULT module=%s failures=%d time_us=%lu output_us=%lu clk_sys_hz=%lu stack_peak=%lu%s "
           "stack_nominal=%lu static_ram=%lu heap_peak=%lu heap_met_stack=%d sizeof_idc_t=%lu "
           "sizeof_disk_track_t=%lu\n",
           RMFE_TEST_MODULE, failures, (unsigned long)us, (unsigned long)output_us,
           (unsigned long)clock_get_hz(clk_sys), (unsigned long)stack, overflowed ? "+" : "",
           (unsigned long)((uintptr_t)__StackTop - (uintptr_t)__StackBottom),
           (unsigned long)((uintptr_t)__end__ - SRAM_BASE), (unsigned long)(heap_end - (uintptr_t)__end__),
           heap_met_stack ? 1 : 0, (unsigned long)sizeof(idc_t), (unsigned long)sizeof(disk_track_t));
    printf("RMFE-TARGET-DONE\n");

    /* LED steady: passed. Flashing: failed. */
    for (unsigned n = 0;; n++) {
        gpio_put(PICO_DEFAULT_LED_PIN, failures == 0 || (n & 1));
        sleep_ms(250);
    }
}
