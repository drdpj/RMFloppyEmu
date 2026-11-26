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
 * What the two cores pass each other (src/pico/main.c): core 1 runs the menu (core/ui.h), core 0 the 480Z's link
 * and the SD card. One request at a time goes from the menu to core 0, which answers it when done; core 0 also
 * posts what the drives hold whenever that changes; and the folder list (core/dirlist.h) is filled by core 0 for a
 * UI_REQ_LIST request and read by core 1 only once that is done.
 *
 * Each slot has one writer and one reader, handed over by a flag with a memory barrier; what the drives hold is
 * copied under a spin lock (held for a copy of about 900 bytes, never in an interrupt). What the unit is doing
 * (for the drives' lights) is a sequence-counted record: core 0 never waits to write it, and core 1 reads it
 * again if core 0 was writing meanwhile.
 */
#ifndef RMFE_PICO_MAILBOX_H
#define RMFE_PICO_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "core/dirlist.h"
#include "core/ui.h"

#define MAILBOX_MESSAGE_MAX 40u

/* Claims the spin lock. Before core 1 starts. */
void mailbox_init(void);

/* The folder list. Written by core 0 between taking a UI_REQ_LIST and posting it done; read by core 1 after. */
dirlist_t *mailbox_list(void);

/* Core 1: posts a request. False if the last one hasn't been taken yet (the menu makes one at a time, so it has). */
bool mailbox_post_request(const ui_request_t *r);
/* Core 0: takes the request, if there is one. */
bool mailbox_take_request(ui_request_t *out);

/* Core 0: the request taken is done; message (may be NULL) says why it failed. */
void mailbox_post_done(bool ok, const char *message);
/* Core 1: takes the answer, if there is one. message is MAILBOX_MESSAGE_MAX bytes, "" if none. */
bool mailbox_take_done(bool *ok, char *message);

/* Core 0, from the command layer's activity hook (idc_t.activity_hook): a few stores, never waits. */
void mailbox_note_activity(void *ctx, const idc_activity_t *a);
/* Core 1: a consistent copy of what the unit has been doing. */
void mailbox_activity(ui_activity_t *out);

/* Core 0: what the unit holds now. */
void mailbox_post_unit(const ui_unit_t *u);
/* Core 1: true and a copy if it has changed since *seen (which it updates). */
bool mailbox_unit_changed(uint32_t *seen, ui_unit_t *out);

#endif
