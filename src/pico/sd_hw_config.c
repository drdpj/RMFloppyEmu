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
 * What the FatFs/SD library (third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) asks of the application: the card's
 * wiring (sd_get_num, sd_get_by_num) and, in place of its src/crash.c (not built: cmake/fatfs_sd.cmake), where its
 * failed asserts go. One card, on SPI0 by itself so that its transfers never hold up the display
 * (docs/hardware.md, "Pin plan").
 */
#include "core/rmfe_assert.h"

#include "crash.h"
#include "ff.h"
#include "hw_config.h"

#if FF_USE_LFN != 1
#error "src/pico/fatfs/ffconf.h must be FatFs's configuration: check the include order in cmake/fatfs_sd.cmake"
#endif

#define SD_PIN_MISO 16u
#define SD_PIN_CS   17u
#define SD_PIN_SCK  18u
#define SD_PIN_MOSI 19u

/* SD cards in SPI mode take up to 25 MHz; the SPI clock is 150 MHz over an even number. With a level-shifting
 * micro-SD breakout on jumper wires (2026-11-21): 25 MHz didn't mount, 18.75 MHz read every image correctly at
 * 1.45 MB/s, 15 MHz at 1.22 MB/s. 15 MHz keeps a step in hand; a soldered board may manage more. The library starts
 * the card at 400 kHz, as the SD spec requires. */
#define SD_SPI_HZ 15000000u

static spi_t sd_spi = {
    .hw_inst = spi0,
    .miso_gpio = SD_PIN_MISO,
    .mosi_gpio = SD_PIN_MOSI,
    .sck_gpio = SD_PIN_SCK,
    .baud_rate = SD_SPI_HZ,
};

static sd_spi_if_t sd_spi_if = {
    .spi = &sd_spi,
    .ss_gpio = SD_PIN_CS,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &sd_spi_if,
};

/* hw_config.h declares this as sd_get_num(), with no (void), which in C isn't a prototype. */
size_t sd_get_num(void);

size_t sd_get_num(void) {
    return 1;
}

sd_card_t *sd_get_by_num(size_t num) {
    return num == 0 ? &sd_card : NULL;
}

/* The library's asserts (my_debug.c) print their message, then come here: halt as RMFE_ASSERT does. */
void capture_assert(const char *file, int line, const char *func, const char *pred) {
    (void)func;
    rmfe_assert_fail(file, line, pred);
}
