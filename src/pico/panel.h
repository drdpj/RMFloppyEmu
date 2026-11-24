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
 * Core 1: the display, the knob and the menu (core/ui.h), talking to core 0 through src/pico/mailbox.h. Nothing on
 * core 1 touches the 480Z's link or the SD card, so drawing never holds up the link (src/pico/main.c).
 *
 * The console can work the menu too, for tests and for using it without a knob: + and - turn, p presses, l is a
 * long press. c turns show-commands on or off until the next start, and h switches an OLED between 128 x 64 and
 * 128 x 32 until then. r redraws the whole screen; d prints it ("SCREEN
 * <row> <attribute>|<text>|"); s reports the peak stacks, the longest menu step on core 0 and the display's clock. k
 * prints the link's last exchanges (src/pico/link_uart.h). w is a test hook: each press adds more time to every save on
 * the card (0, 250, 500, 1000, 2000, 4000 ms, then 0 again), as a slow card would.
 */
#ifndef RMFE_PICO_PANEL_H
#define RMFE_PICO_PANEL_H

#include "core/config.h"

/* Starts core 1, with the knob set as RMFE.CFG says. c must stay valid. */
void panel_start(const config_t *c);

#endif
