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


# The host unit tests (level 1) built for the Pico 2 and run there (docs/testing.md section 2, "On the
# Pico 2"). A project of its own, so the firmware build (../../CMakeLists.txt) is untouched:
#   cmake -S tests/pico -B build-pico-tests -G Ninja && ninja -C build-pico-tests
#   timeout 900 tests/pico/run_on_target.py build-pico-tests
# Each tests/test_<module>.c becomes its own firmware, pico_test_<module>.elf, because together their
# fixtures would not fit in the RP2350's 520 KB of SRAM.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2 CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/../../pico_sdk_import.cmake)

project(rmfloppyemu_pico_tests C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/core.cmake)
rmfe_add_core_library()

set(RMFE_TESTS ${RMFE_ROOT}/tests)

add_library(unity STATIC ${RMFE_ROOT}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${RMFE_ROOT}/third_party/unity)
# unity_config.h: Unity writes through runner.c, which times its console output.
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_CONFIG_H)

add_library(rmfe_test_support STATIC
    ${RMFE_TESTS}/support/ibm_encode.c ${RMFE_TESTS}/support/flux_synth.c ${RMFE_TESTS}/support/fake480z.c)
target_link_libraries(rmfe_test_support PUBLIC rmfe_core unity)
target_include_directories(rmfe_test_support PUBLIC ${RMFE_TESTS})
target_compile_options(rmfe_test_support PRIVATE ${RMFE_WARNINGS} -Werror)

# Every module, so a new tests/test_<module>.c runs here without being listed. tests/pico/test_<module>.c are
# tests of src/pico that only run on the target, built with the src/pico files they test.
file(GLOB RMFE_TEST_MODULES CONFIGURE_DEPENDS ${RMFE_TESTS}/test_*.c)
file(GLOB RMFE_PICO_TEST_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/test_*.c)
set(RMFE_PICO_UNDER_TEST ${RMFE_ROOT}/src/pico/link_uart.c)

# The SDK's sources are compiled inside each executable, so the strict warnings go on our files only.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/runner.c ${RMFE_TESTS}/support/assert_hook.c
                            ${RMFE_PICO_UNDER_TEST} PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
# The module's main() becomes rmfe_test_main(), which runner.c calls; test_main.h declares it.
set_source_files_properties(${RMFE_TEST_MODULES} ${RMFE_PICO_TEST_MODULES} PROPERTIES
    COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror;-include;${CMAKE_CURRENT_LIST_DIR}/test_main.h"
    COMPILE_DEFINITIONS "main=rmfe_test_main")

function(rmfe_add_pico_test src)
    get_filename_component(name ${src} NAME_WE)
    set(exe pico_${name})
    add_executable(${exe} ${CMAKE_CURRENT_LIST_DIR}/runner.c ${src} ${RMFE_TESTS}/support/assert_hook.c ${ARGN})
    target_include_directories(${exe} PRIVATE ${RMFE_ROOT}/src ${RMFE_TESTS})
    # RMFE_TEST_ON_TARGET: no testdata/ here (support/testdata.h), and smaller fixtures where the PC's are
    # sized for real discs.
    target_compile_definitions(${exe} PRIVATE RMFE_TEST_ON_TARGET RMFE_TEST_MODULE="${name}")
    target_link_libraries(${exe} rmfe_test_support rmfe_core unity pico_stdlib)
    # Results go to the debug console: UART0 on GP0/GP1, which the probe bridges to its USB serial port
    pico_enable_stdio_uart(${exe} 1)
    pico_enable_stdio_usb(${exe} 0)
    pico_add_extra_outputs(${exe})
endfunction()

foreach(src ${RMFE_TEST_MODULES})
    rmfe_add_pico_test(${src})
endforeach()
foreach(src ${RMFE_PICO_TEST_MODULES})
    rmfe_add_pico_test(${src} ${RMFE_PICO_UNDER_TEST})
endforeach()

# The firmware for the hardware-in-the-loop tests (tests/hil/run_hil.py): the real link driver and command layer
# with a made-up disc. Same stack set-up as the device's firmware (../../CMakeLists.txt).
set(RMFE_HIL_SOURCES ${RMFE_ROOT}/tests/hil/hil_firmware.c ${RMFE_ROOT}/src/pico/link_uart.c
                     ${RMFE_ROOT}/src/pico/fault.c ${RMFE_ROOT}/src/pico/assert_hook.c)
add_executable(hil_firmware ${RMFE_HIL_SOURCES})
set_source_files_properties(${RMFE_ROOT}/tests/hil/hil_firmware.c ${RMFE_ROOT}/src/pico/fault.c
                            ${RMFE_ROOT}/src/pico/assert_hook.c PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
target_include_directories(hil_firmware PRIVATE ${RMFE_ROOT}/src)
target_compile_definitions(hil_firmware PRIVATE PICO_STACK_SIZE=0x1000 PICO_USE_STACK_GUARDS=1)
target_link_libraries(hil_firmware rmfe_core pico_stdlib)
pico_enable_stdio_uart(hil_firmware 1)
pico_enable_stdio_usb(hil_firmware 0)
pico_add_extra_outputs(hil_firmware)

include(${RMFE_ROOT}/cmake/fatfs_sd.cmake)
rmfe_add_fatfs_sd_library()

# SD card bring-up (tests/hil/sd_probe.c): mounts the card, lists it, reads every image and prints CRC-32s.
set(RMFE_SD_PROBE_SOURCES ${RMFE_ROOT}/tests/hil/sd_probe.c ${RMFE_ROOT}/src/pico/sd_hw_config.c
                          ${RMFE_ROOT}/src/pico/fault.c ${RMFE_ROOT}/src/pico/assert_hook.c)
add_executable(sd_probe ${RMFE_SD_PROBE_SOURCES})
set_source_files_properties(${RMFE_ROOT}/tests/hil/sd_probe.c ${RMFE_ROOT}/src/pico/sd_hw_config.c
                            PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
target_include_directories(sd_probe PRIVATE ${RMFE_ROOT}/src)
target_compile_definitions(sd_probe PRIVATE PICO_STACK_SIZE=0x1000 PICO_USE_STACK_GUARDS=1)
target_link_libraries(sd_probe rmfe_core rmfe_fatfs_sd pico_stdlib)
pico_enable_stdio_uart(sd_probe 1)
pico_enable_stdio_usb(sd_probe 0)
pico_add_extra_outputs(sd_probe)

# Display and encoder bring-up (tests/hil/display_probe.c, run by tests/hil/run_display_probe.py): the TFT's ID,
# the SPI clocks it takes, colour bars and text, the backlight's PWM, and every encoder step and press.
set(RMFE_DISPLAY_PROBE_SOURCES ${RMFE_ROOT}/tests/hil/display_probe.c ${RMFE_ROOT}/src/pico/font_8x13.c
                               ${RMFE_ROOT}/src/pico/fault.c ${RMFE_ROOT}/src/pico/assert_hook.c)
add_executable(display_probe ${RMFE_DISPLAY_PROBE_SOURCES})
set_source_files_properties(${RMFE_ROOT}/tests/hil/display_probe.c ${RMFE_ROOT}/src/pico/font_8x13.c
                            PROPERTIES COMPILE_OPTIONS "${RMFE_WARNINGS};-Werror")
target_include_directories(display_probe PRIVATE ${RMFE_ROOT}/src)
target_compile_definitions(display_probe PRIVATE PICO_STACK_SIZE=0x1000 PICO_USE_STACK_GUARDS=1)
target_link_libraries(display_probe rmfe_core pico_stdlib hardware_spi hardware_dma hardware_pwm)
pico_enable_stdio_uart(display_probe 1)
pico_enable_stdio_usb(display_probe 0)
pico_add_extra_outputs(display_probe)
