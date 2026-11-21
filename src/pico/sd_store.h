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
 * The SD card: its configuration files (core/config.h, after FlashFloppy's), the IDC ROM, and the images in drives
 * A and B (core/disk_image.h), presented to the command layer as idc_drives_t. What happens, and anything that
 * goes wrong, is reported on the debug console with "SD:" in front. Only core 0 calls this (FatFs isn't
 * re-entrant here: src/pico/fatfs/ffconf.h). docs/sd-card.md is the user's description.
 */
#ifndef RMFE_PICO_SD_STORE_H
#define RMFE_PICO_SD_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/config.h"
#include "core/idc.h"

#define SD_STORE_ROM_MAX 8192u /* an IDC ROM (2764) */

/* Mounts the card. False if there is none or it can't be read (the console says which). */
bool sd_store_mount(void);

/* The defaults, then RMFE.CFG if the card has one (each ignored line reported). */
void sd_store_read_config(config_t *c);

/* The IDC ROM: c->idc_rom, or else the first *.ROM in the root, by name, in which the boot block is found
 * (docs/hardware.md). Returns false if there is none; rom then holds nothing useful. */
bool sd_store_read_rom(const config_t *c, uint8_t rom[SD_STORE_ROM_MAX], size_t *len);

/* Puts the images in the drives as image-on-startup says (core/config.h). */
void sd_store_insert_startup_images(const config_t *c);

/* The drives, for idc_set_drives(). A drive with no image isn't ready. */
const idc_drives_t *sd_store_drives(void);

#endif
