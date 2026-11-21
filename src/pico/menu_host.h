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
 * Core 0's half of the menu: it does what the menu on core 1 asks (src/pico/mailbox.h) on the SD card, a short step
 * at a time, only while the unit waits for a 480Z command (idc_t.idle_hook), so a command is never held up for
 * longer than one step, and a disc changes only between commands (docs/protocol.md has no disc-change signal; an
 * image change is instant, owner 2026-11-21).
 */
#ifndef RMFE_PICO_MENU_HOST_H
#define RMFE_PICO_MENU_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "core/config.h"
#include "core/idc.h"

/* Before serving: the command layer (for idc_change_disc), whether the IDC ROM was found, the settings, and what to
 * do when a card comes back (mount it, re-read the settings and the ROM: returns whether the ROM was found). Posts
 * what the drives hold. Every few seconds, between commands, it also checks the card is still there; when it goes,
 * both drives empty, and when one comes back, the images are put back as at start-up, a step at a time. */
void menu_host_init(idc_t *idc, bool rom, const config_t *c, bool (*card_back)(void));

/* For idc_t.idle_hook: the unit is (or stops being) between commands. */
void menu_host_idle(void *ctx, bool idle);

/* For link_uart_set_idle(): one step of the menu's work, if the unit is between commands. */
void menu_host_poll(void);

/* The longest step so far, in microseconds: how long a command could have waited for one. */
uint32_t menu_host_longest_step_us(void);

#endif
