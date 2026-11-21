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

/* What the two cores pass each other: see mailbox.h. */
#include "pico/mailbox.h"

#include <string.h>

#include "hardware/sync.h"

static dirlist_t list; /* 34 KB: static, never on a stack */

/* The request: written by core 1, then req_full set; read by core 0, then req_full cleared. */
static ui_request_t req;
static volatile bool req_full;

/* The answer: written by core 0, then done_full set; read by core 1, then done_full cleared. */
static bool done_ok;
static char done_message[MAILBOX_MESSAGE_MAX];
static volatile bool done_full;

/* What the unit holds: written by core 0, read by core 1, both under the lock; unit_seq counts the posts. */
static spin_lock_t *lock;
static ui_unit_t unit;
static uint32_t unit_seq;

/* What the unit is doing: written by core 0 (seq odd while it writes), read by core 1. */
static ui_activity_t act;
static volatile uint32_t act_seq;

void mailbox_note_activity(void *ctx, const idc_activity_t *a) {
    (void)ctx;
    act_seq = act_seq + 1u;
    __dmb();
    act.last = *a;
    switch (a->stage) {
        case IDC_ACT_START:
            act.commands++;
            act.busy = true;
            break;
        case IDC_ACT_DRIVE:
            if (a->drive < IDC_PHYS_DRIVES) {
                act.drive_commands[a->drive]++;
                act.drive_writing[a->drive] = idc_command_writes(a->cmd);
                act.drive_has_track[a->drive] = a->has_track;
                act.drive_track[a->drive] = a->track;
            }
            break;
        case IDC_ACT_END:
            act.busy = false;
            break;
    }
    __dmb();
    act_seq = act_seq + 1u;
}

void mailbox_activity(ui_activity_t *out) {
    for (;;) {
        uint32_t seq = act_seq;
        if (seq & 1u) continue; /* core 0 is writing: a few stores, so not for long */
        __dmb();
        *out = act;
        __dmb();
        if (act_seq == seq) return;
    }
}

void mailbox_init(void) {
    lock = spin_lock_init(spin_lock_claim_unused(true));
}

dirlist_t *mailbox_list(void) {
    return &list;
}

bool mailbox_post_request(const ui_request_t *r) {
    if (req_full) return false;
    req = *r;
    __dmb(); /* the request is complete before the flag says so */
    req_full = true;
    return true;
}

bool mailbox_take_request(ui_request_t *out) {
    if (!req_full) return false;
    __dmb();
    *out = req;
    __dmb(); /* copied before core 1 may write the next */
    req_full = false;
    return true;
}

void mailbox_post_done(bool ok, const char *message) {
    while (done_full) /* core 1 takes each answer before it can make the next request: never waits */
        tight_loop_contents();
    done_ok = ok;
    strncpy(done_message, message ? message : "", sizeof done_message - 1);
    done_message[sizeof done_message - 1] = '\0';
    __dmb();
    done_full = true;
}

bool mailbox_take_done(bool *ok, char *message) {
    if (!done_full) return false;
    __dmb();
    *ok = done_ok;
    memcpy(message, done_message, sizeof done_message);
    __dmb();
    done_full = false;
    return true;
}

void mailbox_post_unit(const ui_unit_t *u) {
    uint32_t save = spin_lock_blocking(lock);
    unit = *u;
    unit_seq++;
    spin_unlock(lock, save);
}

bool mailbox_unit_changed(uint32_t *seen, ui_unit_t *out) {
    uint32_t save = spin_lock_blocking(lock);
    bool changed = unit_seq != *seen;
    if (changed) {
        *out = unit;
        *seen = unit_seq;
    }
    spin_unlock(lock, save);
    return changed;
}
