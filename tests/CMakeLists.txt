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

# Host unit tests: src/core built for the PC, tested with Unity, run by CTest.
#   cmake -S tests -B build-tests -G Ninja && ninja -C build-tests
#   timeout 300 ctest --test-dir build-tests --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(rmfloppyemu_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Sanitizers on everything in the host build, including Unity, so they link consistently.
option(RMFE_SANITIZE "Build host tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
if(RMFE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/core.cmake)
rmfe_add_core_library()

add_library(unity STATIC ${RMFE_ROOT}/third_party/unity/unity.c)
target_include_directories(unity PUBLIC ${RMFE_ROOT}/third_party/unity)

# Test support linked into every test. The RMFE_ASSERT hook (fails the current test) is compiled into each
# test program itself: the core library calls it, so it can't sit in a library linked before the core.
add_library(rmfe_test_support STATIC support/ibm_encode.c support/flux_synth.c support/fake480z.c)
target_link_libraries(rmfe_test_support PUBLIC rmfe_core unity)
target_include_directories(rmfe_test_support PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(rmfe_test_support PRIVATE ${RMFE_WARNINGS} -Werror)

# Test data that can't be in the repo (RM ROMs, disk images, MAME traces): docs/testing.md section 5.
# Tests that need it must fail, not skip, when it is missing.
set(RMFE_TESTDATA "${RMFE_ROOT}/testdata" CACHE PATH "Directory holding roms/, images/ and traces/")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

enable_testing()

# rmfe_add_test(<name>): builds tests/<name>.c against the core and Unity and registers it.
function(rmfe_add_test name)
    add_executable(${name} ${name}.c support/assert_hook.c)
    target_link_libraries(${name} PRIVATE rmfe_test_support rmfe_core unity)
    target_compile_options(${name} PRIVATE ${RMFE_WARNINGS} -Werror)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60 ENVIRONMENT "RMFE_TESTDATA=${RMFE_TESTDATA}")
endfunction()

rmfe_add_test(test_bytes)
rmfe_add_test(test_crc16)
rmfe_add_test(test_config)
rmfe_add_test(test_ibm_track)
rmfe_add_test(test_hfe)
rmfe_add_test(test_flux_pll)
rmfe_add_test(test_scp)
rmfe_add_test(test_link)
rmfe_add_test(test_idc)
rmfe_add_test(test_idc_disk)
rmfe_add_test(test_disk)
rmfe_add_test(test_disk_write)
rmfe_add_test(test_disk_image)
rmfe_add_test(test_ibm_encode)
rmfe_add_test(test_rotary)
rmfe_add_test(test_dirlist)
rmfe_add_test(test_ui)
rmfe_add_test(test_usb_share)

# hfedump: prints what src/flux decodes from an HFE image (tools/hfedump).
add_executable(hfedump ${RMFE_ROOT}/tools/hfedump/hfedump.c)
target_link_libraries(hfedump PRIVATE rmfe_core)
target_compile_options(hfedump PRIVATE ${RMFE_WARNINGS} -Werror)

# scpdump: prints what src/flux decodes from every revolution of an SCP flux image (tools/scpdump).
add_executable(scpdump ${RMFE_ROOT}/tools/scpdump/scpdump.c)
target_link_libraries(scpdump PRIVATE rmfe_core)
target_compile_options(scpdump PRIVATE ${RMFE_WARNINGS} -Werror)

# src/flux stays reusable: its headers compile and link as C++ (Disk Wrangler is C++17), and it
# includes nothing from the rest of the project.
add_executable(flux_cxx_check flux_cxx_check.cpp)
target_link_libraries(flux_cxx_check PRIVATE rmfe_core)
target_compile_options(flux_cxx_check PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME flux_cxx_check COMMAND flux_cxx_check)
add_test(NAME flux_self_contained
         COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/check_flux_self_contained.py ${RMFE_ROOT}/src/flux)

# Real-data checks: src/flux's decode of each HFE must match the IMD of the same disc, read by an
# independent Python parser. They need testdata/ (docs/testing.md section 5) and fail without it.
foreach(disc 480Z_4MATION_Dragon_World_1985 480Z_4MATION_Flowers_of_Crystal_1985)
    add_test(NAME hfe_vs_imd_${disc}
             COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/check_hfe_vs_imd.py $<TARGET_FILE:hfedump>
                     ${RMFE_TESTDATA}/images/${disc}.hfe ${RMFE_TESTDATA}/images/${disc}.IMD)
    set_tests_properties(hfe_vs_imd_${disc} PROPERTIES TIMEOUT 120 LABELS realdata)
endforeach()

# Real flux (read at 360 rpm) through scp + flux_pll + ibm_track: the protected layout must come out.
add_test(NAME scp_4mation_dragon_world
         COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/check_4mation_scp.py $<TARGET_FILE:scpdump>
                 ${RMFE_TESTDATA}/images/480Z_4MATION_Dragon_World_1985.scp --all-good)
add_test(NAME scp_4mation_flowers_of_crystal
         COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/check_4mation_scp.py $<TARGET_FILE:scpdump>
                 ${RMFE_TESTDATA}/images/480Z_4MATION_Flowers_of_Crystal_1985.scp
                 # The radial scratch destroys this ID on every revolution (docs/disk-survey.md).
                 --damaged 0:1:02)
set_tests_properties(scp_4mation_dragon_world scp_4mation_flowers_of_crystal PROPERTIES TIMEOUT 300 LABELS realdata)

# tools/scpdump/rebuild.py's SCP writer: no zero flux words, no stretched gaps, every sector good.
add_test(NAME rebuild_scp_flowers_of_crystal
         COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/check_rebuild_scp.py $<TARGET_FILE:scpdump>
                 ${RMFE_TESTDATA}/images/480Z_4MATION_Flowers_of_Crystal_1985.scp
                 ${RMFE_TESTDATA}/images/480Z_4MATION_Flowers_of_Crystal_1985.IMD ${CMAKE_CURRENT_BINARY_DIR}/rebuild_scp)
set_tests_properties(rebuild_scp_flowers_of_crystal PROPERTIES TIMEOUT 600 LABELS realdata)

# mamecmp: replays a MAME trace of the real IDC firmware into our command layer and compares every byte
# (docs/testing.md section 3, level 2).
add_executable(mamecmp mame/mamecmp.c)
target_link_libraries(mamecmp PRIVATE rmfe_core m)
target_compile_options(mamecmp PRIVATE ${RMFE_WARNINGS} -Werror)

# The MAME comparison tests: one per scenario in tests/mame/scenarios/. The reference traces are made with
# tests/mame/make_traces.py into testdata/traces/; a missing trace fails the test.
file(GLOB RMFE_MAME_SCENARIOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/mame/scenarios/*.scn)
foreach(scn ${RMFE_MAME_SCENARIOS})
    get_filename_component(name ${scn} NAME_WE)
    add_test(NAME mame_${name}
             COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/mame/compare.py $<TARGET_FILE:mamecmp> ${scn}
                     ${RMFE_TESTDATA} ${CMAKE_CURRENT_BINARY_DIR}/mame)
    set_tests_properties(mame_${name} PROPERTIES TIMEOUT 300 LABELS "mame;realdata")
endforeach()
add_test(NAME mame_selfcheck_detects_mismatches
         COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_LIST_DIR}/mame/check_detects_mismatch.py $<TARGET_FILE:mamecmp>
                 ${RMFE_TESTDATA} ${CMAKE_CURRENT_BINARY_DIR}/mame)
set_tests_properties(mame_selfcheck_detects_mismatches PROPERTIES TIMEOUT 300 LABELS "mame;realdata")
