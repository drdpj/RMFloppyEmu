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
#include "core/dirlist.h"
#include "core/idc.h"
#include "core/ui.h"

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

/*
 * The menu's work (core/ui.h requests), a step at a time: the firmware does it between the 480Z's commands, and a
 * step must be short, because a command that arrives waits for it (docs/plan.md: the 480Z gives up after about
 * 145 ms). Each step is one FatFs call or one track record of an IMD.
 */
typedef enum {
    SD_STEP_MORE,
    SD_STEP_DONE,
    SD_STEP_FAILED,
} sd_step_t;

/* Lists a folder ("" the root, else its path without a leading "/"): begin, then step until done, each step
 * offering one entry to the list (dirlist_add). */
bool sd_store_list_begin(const char *folder);
sd_step_t sd_store_list_step(dirlist_t *l);

/* Opens an image for a drive, without touching what the drive holds: begin, then step until done. On failure *why
 * says why in a few words. Refused if the other drive has that image. */
bool sd_store_insert_begin(unsigned drive, const char *path, const char **why);
sd_step_t sd_store_insert_step(const char **why);

/* Puts the image just opened into its drive, taking the old one out. Only between commands, after
 * idc_change_disc() for the drive. */
void sd_store_insert_commit(void);

/* Takes a drive's image out, leaving it empty. Only between commands, after idc_change_disc(). */
void sd_store_eject(unsigned drive);

/* write-protect (the settings given to sd_store_insert_startup_images) has changed: the images in the drives
 * follow it now. Only between commands, after idc_change_disc() for each drive, so nothing written under the old
 * setting is still in the command layer's cache. */
void sd_store_apply_write_protect(void);

/* Sets key = value in RMFE.CFG (core/config.h, config_set_line), keeping the rest of it; makes the file if there
 * is none. On failure *why says why in a few words. One step: a read and a write of at most 2 KB. */
bool sd_store_save_option(const char *key, const char *value, const char **why);

/* Names the drive's image in IMAGE_x.CFG (empty if the drive is), as FlashFloppy remembers the last one chosen. */
bool sd_store_save_choice(unsigned drive);

/*
 * The card coming and going while the unit runs (pico/menu_host.c checks every few seconds, between commands).
 * card_responds: true if a card answers, a cheap check either way (CMD13 to a card in use, or a short CMD0).
 * card_gone: the card in use no longer answers: both drives are emptied and FatFs forgets it; mount it again with
 * sd_store_mount(). startup_path: the image image-on-startup puts in drive n (as at start-up), for putting the
 * images back in a step at a time (sd_store_insert_begin); false for none.
 */
bool sd_store_mounted(void);
bool sd_store_card_responds(void);
void sd_store_card_gone(void);

/* Lets the card go, for a PC to have over USB (core/usb_share.h): both drives' images closed (after
 * idc_change_disc() for each), FatFs unmounted. Mount it again with sd_store_mount(). Only between requests. */
void sd_store_release(void);
bool sd_store_startup_path(unsigned n, const config_t *c, char path[CONFIG_PATH_MAX]);

/* Test hook: every save takes ms longer (0: none), as a slow card would. Either core may call it. */
void sd_store_set_save_delay(uint32_t ms);

/* What the drives hold, for the menu (ui_unit_t; rom is the caller's to fill). */
void sd_store_unit(ui_unit_t *u);

#endif
