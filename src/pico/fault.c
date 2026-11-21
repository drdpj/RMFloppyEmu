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
 * The HardFault handler: reports the fault on the debug console, then halts with the LED flashing, as a
 * failed RMFE_ASSERT does (assert_hook.c).
 *
 * Core 0's stack is 4 KB with a hardware limit (PICO_STACK_SIZE, PICO_USE_STACK_GUARDS: CMakeLists.txt): the
 * SDK sets the Cortex-M33's MSPLIM to its bottom, so an overflow faults at once (a UsageFault, STKOF,
 * escalated to HardFault) instead of overwriting what lies below. The handler is then entered with SP at
 * the limit, so before anything else it lifts the limit and moves to a stack of its own. It writes to the
 * UART's registers itself: printf takes locks, which is not safe in a fault.
 */
#include <stdint.h>

#include "hardware/structs/m33.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

#define FAULT_STACK_WORDS 128u

void isr_hardfault(void);
_Noreturn void rmfe_fault_report(uint32_t sp);

/* Not static: the handler's assembly refers to it by name. */
uint32_t rmfe_fault_stack[FAULT_STACK_WORDS];

/* Replaces the SDK's weak isr_hardfault (pico_crt0). Naked: no prologue may push onto the overflowed stack. */
__attribute__((naked)) void isr_hardfault(void) {
    __asm__ volatile("mrs r0, msp\n"
                     "movs r1, #0\n"
                     "msr msplim, r1\n"
                     "ldr r1, =rmfe_fault_stack + 512\n" /* FAULT_STACK_WORDS * 4: its top */
                     "mov sp, r1\n"
                     "b rmfe_fault_report\n");
}

static void put_str(const char *s) {
    while (*s)
        uart_putc_raw(uart_default, *s++);
}

static void put_hex32(uint32_t v) {
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc_raw(uart_default, "0123456789ABCDEF"[(v >> shift) & 0xFu]);
}

/* sp: the stack pointer when the fault was taken (the exception frame, unless the stack had overflowed). */
__attribute__((used)) _Noreturn void rmfe_fault_report(uint32_t sp) {
    uint32_t cfsr = m33_hw->cfsr, hfsr = m33_hw->hfsr;
    bool overflow = (cfsr & M33_CFSR_UFSR_STKOF_BITS) != 0;
    put_str("\r\nFAULT: HardFault CFSR=");
    put_hex32(cfsr);
    put_str(" HFSR=");
    put_hex32(hfsr);
    put_str(" SP=");
    put_hex32(sp);
    if (overflow) {
        put_str("\r\nFAULT: stack overflow: core 0 used its whole 4 KB stack\r\n");
    } else {
        put_str(" PC=");
        put_hex32(((const uint32_t *)sp)[6]); /* the exception frame: r0-r3, r12, lr, pc, xpsr */
        put_str("\r\n");
    }
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (;;) {
        gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
        busy_wait_ms(100);
    }
}
