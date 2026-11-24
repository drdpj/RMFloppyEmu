# RMFloppyEmu - a Raspberry Pi Pico 2 replacement for the RML 480Z's disk unit
# Copyright (C) 2026 Daniel Jameson
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(rmfloppyemu C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(cmake/core.cmake)
rmfe_add_core_library()
include(cmake/fatfs_sd.cmake)
rmfe_add_fatfs_sd_library()

set(RMFE_PICO_SOURCES
    src/pico/main.c
    src/pico/assert_hook.c
    src/pico/link_uart.c
    src/pico/fault.c
    src/pico/sd_hw_config.c
    src/pico/sd_store.c
    src/pico/font_8x13.c
    src/pico/font_7x13b.c
    src/pico/font_5x8.c
    src/pico/font_4x6.c
    src/pico/oled.c
    src/pico/mailbox.c
    src/pico/menu_host.c
    src/pico/panel.c
    src/pico/stack_meter.c
    src/pico/tft.c
)

add_executable(rmfloppyemu ${RMFE_PICO_SOURCES})

target_include_directories(rmfloppyemu PRIVATE src)
# The SDK's sources are compiled inside this target, so the strict warnings go on our files only.
set_source_files_properties(${RMFE_PICO_SOURCES} PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
target_link_libraries(rmfloppyemu rmfe_core rmfe_fatfs_sd pico_stdlib pico_multicore hardware_spi hardware_dma
                      hardware_pwm hardware_i2c)

# Core 0's stack: the whole 4 KB of SCRATCH_Y (nothing else uses it), with the Cortex-M33's hardware stack
# limit at its bottom, so an overflow faults at once (src/pico/fault.c) rather than overwriting core 1's
# stack or the heap. The SDK's default is a nominal 2 KB with no limit. docs/plan.md, milestone 6.
# Core 1's (the display and menu, src/pico/main.c): the same, in SCRATCH_X. The SDK puts its limit at its bottom too.
target_compile_definitions(rmfloppyemu PRIVATE PICO_STACK_SIZE=0x1000 PICO_CORE1_STACK_SIZE=0x1000
                                               PICO_USE_STACK_GUARDS=1)

# Debug console: UART0 on GP0/GP1, which the probe bridges to its USB serial port
pico_enable_stdio_uart(rmfloppyemu 1)
pico_enable_stdio_usb(rmfloppyemu 0)

pico_add_extra_outputs(rmfloppyemu)
