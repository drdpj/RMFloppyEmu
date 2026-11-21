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
)

add_executable(rmfloppyemu ${RMFE_PICO_SOURCES})

target_include_directories(rmfloppyemu PRIVATE src)
# The SDK's sources are compiled inside this target, so the strict warnings go on our files only.
set_source_files_properties(${RMFE_PICO_SOURCES} PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
target_link_libraries(rmfloppyemu rmfe_core rmfe_fatfs_sd pico_stdlib)

# Core 0's stack: the whole 4 KB of SCRATCH_Y (nothing else uses it), with the Cortex-M33's hardware stack
# limit at its bottom, so an overflow faults at once (src/pico/fault.c) rather than overwriting core 1's
# stack or the heap. The SDK's default is a nominal 2 KB with no limit. docs/plan.md, milestone 6.
target_compile_definitions(rmfloppyemu PRIVATE PICO_STACK_SIZE=0x1000 PICO_USE_STACK_GUARDS=1)

# Debug console: UART0 on GP0/GP1, which the probe bridges to its USB serial port
pico_enable_stdio_uart(rmfloppyemu 1)
pico_enable_stdio_usb(rmfloppyemu 0)

pico_add_extra_outputs(rmfloppyemu)
