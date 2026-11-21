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

/* Internal to the command layer: the disc commands (idc.c dispatches to them). */
#ifndef RMFE_CORE_IDC_DISK_H
#define RMFE_CORE_IDC_DISK_H

#include "core/idc.h"

/* Tells the activity hook which drive (and track and third byte) the command is for (idc.c). */
void idc_note_drive(idc_t *d, unsigned phys, unsigned side, bool has_track, uint8_t track, uint8_t third);

link_status_t idc_cmd_login(idc_t *d);
link_status_t idc_cmd_read(idc_t *d);
link_status_t idc_cmd_write(idc_t *d);
link_status_t idc_cmd_flush(idc_t *d);
link_status_t idc_cmd_probe(idc_t *d);
link_status_t idc_cmd_read_id(idc_t *d);
link_status_t idc_cmd_write_id(idc_t *d);
link_status_t idc_cmd_format(idc_t *d, bool verify_only);

#endif
