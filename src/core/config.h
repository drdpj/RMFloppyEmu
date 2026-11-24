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
 * The SD card's configuration files, after FlashFloppy's (Keir Fraser, https://github.com/keirf/flashfloppy/wiki):
 * our RMFE.CFG is its FF.CFG, and IMAGE_A.CFG and INIT_A.CFG are its files of the same names, with a B pair for our
 * second drive. Option names and values are FlashFloppy's where it has the same option; there is only one system
 * to serve, so there are far fewer. docs/sd-card.md is the user's description.
 *
 * RMFE.CFG lives in the card's root, or in RMFE/ if that folder exists (as FF.CFG and FF/); so do the IMAGE and
 * INIT files. Lines are "key = value"; blank lines and lines starting with # are ignored; keys and keywords are
 * case-insensitive, and a value may be in double quotes. An unknown key or a bad value is reported (by line) and
 * ignored, leaving the default.
 *
 *   image-on-startup = last | static | init   (default last)
 *       last: the image named in IMAGE_A.CFG (IMAGE_B.CFG for B), the last one chosen; static: the one in
 *       INIT_A.CFG (INIT_B.CFG); init: the first image in the root. A missing or empty file, or a missing image,
 *       falls back to init. For drive B, init is an empty drive.
 *   write-protect = yes | no                  (default no)
 *       yes: every image is write-protected; no: an image is write-protected if it has the FAT read-only attribute.
 *   drive-a = auto | 40 | 80, drive-b = ...   (default auto)
 *       The drive's type, which the IDC found by stepping the head (core/idc.h, idc_drive_type_t). auto: 80-track
 *       for an image of 43 cylinders or more.
 *   idc-rom = <path>                          (default IDC.ROM)
 *       The IDC ROM image the boot block (command FF) comes from (docs/hardware.md).
 *   rotary = full | half | quarter [,reverse] (default full)
 *       The knob, as FlashFloppy's option of the same name: one step per full quadrature cycle (an EC11 with as
 *       many detents as pulses), per half cycle, or per quarter cycle (core/rotary.h). ",reverse" turns the other
 *       way round, for an encoder wired or built the other way.
 *   show-commands = yes | no                  (default no)
 *       yes: the display also shows each command as the unit serves it (core/ui.h).
 *   fast-baud = 125000 | 62500                (default 125000)
 *       The rate the link goes fast to (command 00). 125000 is the IDC's; the 480Z takes the rate from the unit's
 *       reply (core/link.h), so 62500 works too, for an RS-232 interface that can't drive the 480Z at 125000.
 *   boot-baud = 125000 | 62500                (default 125000)
 *   display-type = auto | tft | oled-128x64 | oled-128x32, an OLED optionally with -rotate   (default auto)
 *       The same, for the 480Z's ROS loading the boot block (core/link.h, link_set_boot_baud). 125000 is the
 *       IDC's; the block goes paced, two bytes per DTR pulse (link_set_paced). 62500 is a fallback.
 *
 * The images listed (for init, and later the display) are as in FlashFloppy's native mode: .imd and .hfe files,
 * leaving out names starting with "." and hidden files, sorted by name ignoring case.
 */
#ifndef RMFE_CORE_CONFIG_H
#define RMFE_CORE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/idc.h"
#include "core/link.h"

#define CONFIG_PATH_MAX 256u /* a path from the card's root, with its terminating NUL */

typedef enum {
    CONFIG_STARTUP_LAST = 0,
    CONFIG_STARTUP_STATIC,
    CONFIG_STARTUP_INIT,
} config_startup_t;

typedef enum {
    CONFIG_ROTARY_FULL = 0,
    CONFIG_ROTARY_HALF,
    CONFIG_ROTARY_QUARTER,
} config_rotary_t;

/* The display (FlashFloppy's display-type, with our TFT added and its LCDs left out). Auto: an OLED if one answers on
 * I2C, taken as 128 x 64 (FlashFloppy's auto takes 128 x 32), otherwise the TFT. */
typedef enum {
    CONFIG_DISPLAY_AUTO = 0,
    CONFIG_DISPLAY_TFT,
    CONFIG_DISPLAY_OLED_128X64,
    CONFIG_DISPLAY_OLED_128X32,
} config_display_t;

typedef struct {
    config_startup_t image_on_startup;
    bool write_protect;
    idc_drive_type_t drive_type[IDC_PHYS_DRIVES];
    char idc_rom[CONFIG_PATH_MAX];
    config_rotary_t rotary;
    bool rotary_reverse;
    bool show_commands;
    uint32_t fast_baud; /* LINK_BAUD_FAST or LINK_BAUD_HALF */
    uint32_t boot_baud; /* the same, for the ROS's boot block */
    config_display_t display;
    bool display_rotate; /* an OLED turned 180 degrees */
} config_t;

/* Told of each line of RMFE.CFG that is ignored: its number (from 1) and why. */
typedef void (*config_warn_fn)(void *ctx, unsigned line, const char *why);

/* The defaults: as if RMFE.CFG were empty. */
void config_defaults(config_t *c);

/* Reads RMFE.CFG's text (len bytes, not necessarily NUL-terminated) over c, which should hold the defaults.
 * warn may be NULL. */
void config_parse(config_t *c, const char *text, size_t len, config_warn_fn warn, void *ctx);

/* The image path in an IMAGE_x.CFG or INIT_x.CFG: its first line that isn't blank or a comment, trimmed and
 * unquoted, into path (cap bytes). Returns false if there is none, or it doesn't fit. */
bool config_image_path(const char *text, size_t len, char *path, size_t cap);

/* Whether a file in a folder is one of the images listed (see above). */
bool config_is_listed_image(const char *name, bool hidden);

/* The order of the listing: by name, ignoring case; names differing only in case, byte by byte. */
int config_compare_names(const char *a, const char *b);

#endif
