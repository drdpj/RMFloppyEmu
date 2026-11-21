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

# The hardware-independent core, shared by the firmware build (CMakeLists.txt) and the host
# test build (tests/CMakeLists.txt). Add every src/core/*.c and src/flux/*.c file here, and nowhere else.
set(RMFE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

set(RMFE_CORE_SOURCES
    ${RMFE_ROOT}/src/core/bytes.c
    ${RMFE_ROOT}/src/core/config.c
    ${RMFE_ROOT}/src/core/disk.c
    ${RMFE_ROOT}/src/core/disk_hfe.c
    ${RMFE_ROOT}/src/core/disk_image.c
    ${RMFE_ROOT}/src/core/disk_imd.c
    ${RMFE_ROOT}/src/core/idc.c
    ${RMFE_ROOT}/src/core/idc_disk.c
    ${RMFE_ROOT}/src/core/link.c
    # src/flux: the self-contained track/image library (see src/flux/README.md)
    ${RMFE_ROOT}/src/flux/crc16.c
    ${RMFE_ROOT}/src/flux/flux_pll.c
    ${RMFE_ROOT}/src/flux/hfe.c
    ${RMFE_ROOT}/src/flux/ibm_encode.c
    ${RMFE_ROOT}/src/flux/ibm_track.c
    ${RMFE_ROOT}/src/flux/scp.c
)

# Warnings for our own code, on both builds. Third-party code (Pico SDK, Unity) doesn't get them.
set(RMFE_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
    -Wpointer-arith -Wcast-align -Wundef -Wdouble-promotion -Wformat=2
)

function(rmfe_add_core_library)
    add_library(rmfe_core STATIC ${RMFE_CORE_SOURCES})
    target_include_directories(rmfe_core PUBLIC ${RMFE_ROOT}/src)
    target_compile_options(rmfe_core PRIVATE ${RMFE_WARNINGS} -Werror)
endfunction()
