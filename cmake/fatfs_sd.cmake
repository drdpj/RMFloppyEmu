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


# The FatFs/SD card library (third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico, vendored unmodified), built from our
# own list rather than its CMakeLists.txt, which also compiles src/crash.c: that defines a HardFault handler of
# its own, which would replace ours (src/pico/fault.c). src/pico/sd_hw_config.c supplies what crash.c provided
# that the rest needs (capture_assert), and the wiring (sd_get_num, sd_get_by_num).
# An INTERFACE library, as the SDK's are: its sources compile in each firmware, without our warnings, and its
# headers are system headers so that ours aren't held to their style either.
set(RMFE_FATFS_SD ${CMAKE_CURRENT_LIST_DIR}/../third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src)
set(RMFE_FATFS_CONF ${CMAKE_CURRENT_LIST_DIR}/../src/pico/fatfs) # our ffconf.h

function(rmfe_add_fatfs_sd_library)
    add_library(rmfe_fatfs_sd INTERFACE)
    pico_generate_pio_header(rmfe_fatfs_sd ${RMFE_FATFS_SD}/sd_driver/SDIO/rp2040_sdio.pio)
    target_compile_definitions(rmfe_fatfs_sd INTERFACE PICO_MAX_SHARED_IRQ_HANDLERS=8u) # as its CMakeLists.txt
    target_sources(rmfe_fatfs_sd INTERFACE
        ${RMFE_FATFS_SD}/ff15/source/ff.c
        ${RMFE_FATFS_SD}/ff15/source/ffsystem.c
        ${RMFE_FATFS_SD}/ff15/source/ffunicode.c
        ${RMFE_FATFS_SD}/sd_driver/dma_interrupts.c
        ${RMFE_FATFS_SD}/sd_driver/sd_card.c
        ${RMFE_FATFS_SD}/sd_driver/sd_timeouts.c
        ${RMFE_FATFS_SD}/sd_driver/SDIO/rp2040_sdio.c
        ${RMFE_FATFS_SD}/sd_driver/SDIO/sd_card_sdio.c
        ${RMFE_FATFS_SD}/sd_driver/SPI/my_spi.c
        ${RMFE_FATFS_SD}/sd_driver/SPI/sd_card_spi.c
        ${RMFE_FATFS_SD}/sd_driver/SPI/sd_spi.c
        ${RMFE_FATFS_SD}/src/crc.c
        ${RMFE_FATFS_SD}/src/f_util.c
        ${RMFE_FATFS_SD}/src/ff_stdio.c
        ${RMFE_FATFS_SD}/src/file_stream.c
        ${RMFE_FATFS_SD}/src/glue.c
        ${RMFE_FATFS_SD}/src/my_debug.c
        ${RMFE_FATFS_SD}/src/my_rtc.c
        ${RMFE_FATFS_SD}/src/util.c
    )
    # src/pico/fatfs first: our ffconf.h (FatFs's configuration) in place of the library's.
    target_include_directories(rmfe_fatfs_sd SYSTEM INTERFACE ${RMFE_FATFS_CONF}
        ${RMFE_FATFS_SD}/ff15/source ${RMFE_FATFS_SD}/sd_driver ${RMFE_FATFS_SD}/include)
    target_link_libraries(rmfe_fatfs_sd INTERFACE
        hardware_dma hardware_pio hardware_spi pico_aon_timer pico_stdlib cmsis_core)
endfunction()
