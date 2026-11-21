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

/* Core 0's half of the menu: see menu_host.h. */
#include "pico/menu_host.h"

#include "pico/mailbox.h"
#include "pico/sd_store.h"

#include <stdio.h>

#include "pico/time.h"

typedef enum {
    HOST_WAITING, /* for a request */
    HOST_LISTING,
    HOST_OPENING,
    HOST_SAVING,      /* IMAGE_x.CFG, after a change */
    HOST_REINSERTING, /* a card has come back: putting the start-up images back, drive by drive */
} host_state_t;

#define CARD_CHECK_US 2000000u

/* All of it core 0's alone. */
static idc_t *unit;
static bool rom_found;
static volatile bool between_commands; /* written by idc's idle hook, read by menu_host_poll: both core 0 */
static host_state_t state;
static ui_request_t cur;
static volatile uint32_t longest_us; /* written by core 0; core 1 reads it for its report */
static const config_t *settings;
static bool (*card_back_fn)(void);
static uint64_t next_check_us;
static bool reinserting; /* the insert under way puts a start-up image back: no request to answer */
static unsigned reinsert_next;

static void post_unit(void) {
    ui_unit_t u;
    sd_store_unit(&u);
    u.rom = rom_found;
    mailbox_post_unit(&u);
}

void menu_host_init(idc_t *idc, bool rom, const config_t *c, bool (*card_back)(void)) {
    unit = idc;
    rom_found = rom;
    settings = c;
    card_back_fn = card_back;
    next_check_us = time_us_64() + CARD_CHECK_US;
    state = HOST_WAITING;
    post_unit();
}

void menu_host_idle(void *ctx, bool idle) {
    (void)ctx;
    between_commands = idle;
}

/* The drive's disc is changing: what is cached for the old one goes to it first (idc_change_disc). */
static void change_disc(unsigned drive) {
    uint8_t st = idc_change_disc(unit, drive);
    if (st != IDC_STATUS_OK) {
        printf("SD: %c: writing back to the old image failed (%02X): that change is lost, as 08 would report\n",
               'A' + (int)drive, st);
    }
}

static void finish(bool ok, const char *message) {
    state = HOST_WAITING;
    mailbox_post_done(ok, message);
}

/* Putting the start-up images back after a card came back: the next drive's, or done. */
static void reinsert_step(void) {
    const char *why = NULL;
    while (reinsert_next < IDC_PHYS_DRIVES) {
        unsigned d = reinsert_next++;
        if (sd_store_startup_path(d, settings, cur.path) && sd_store_insert_begin(d, cur.path, &why)) {
            cur.drive = d;
            reinserting = true;
            state = HOST_OPENING;
            return;
        }
    }
    reinserting = false;
    state = HOST_WAITING;
    post_unit();
}

/* Is the card still there, or back? Only between requests. */
static void check_card(void) {
    if (sd_store_mounted()) {
        if (sd_store_card_responds()) return;
        sd_store_card_gone(); /* first: so writing back the caches fails at once, not after the card's timeouts */
        for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
            change_disc(d);
        post_unit();
    } else if (sd_store_card_responds()) {
        rom_found = card_back_fn && card_back_fn();
        if (!sd_store_mounted()) return;
        reinsert_next = 0;
        state = HOST_REINSERTING;
        post_unit();
    }
}

static void step(void) {
    const char *why = NULL;
    switch (state) {
        case HOST_WAITING:
            if (time_us_64() >= next_check_us) {
                next_check_us = time_us_64() + CARD_CHECK_US;
                check_card();
                return;
            }
            if (!mailbox_take_request(&cur)) return;
            switch (cur.kind) {
                case UI_REQ_LIST:
                    dirlist_clear(mailbox_list());
                    if (sd_store_list_begin(cur.path)) {
                        state = HOST_LISTING;
                    } else {
                        finish(false, "can't read the card");
                    }
                    return;
                case UI_REQ_INSERT:
                    if (sd_store_insert_begin(cur.drive, cur.path, &why)) {
                        state = HOST_OPENING;
                    } else {
                        finish(false, why);
                    }
                    return;
                case UI_REQ_EJECT:
                    change_disc(cur.drive);
                    sd_store_eject(cur.drive);
                    post_unit();
                    state = HOST_SAVING;
                    return;
            }
            return;
        case HOST_LISTING:
            switch (sd_store_list_step(mailbox_list())) {
                case SD_STEP_MORE:
                    return;
                case SD_STEP_DONE:
                    finish(true, NULL);
                    return;
                case SD_STEP_FAILED:
                    finish(false, "can't read the folder");
                    return;
            }
            return;
        case HOST_OPENING:
            switch (sd_store_insert_step(&why)) {
                case SD_STEP_MORE:
                    return;
                case SD_STEP_FAILED:
                    if (reinserting) {
                        state = HOST_REINSERTING;
                    } else {
                        finish(false, why);
                    }
                    return;
                case SD_STEP_DONE:
                    change_disc(cur.drive);
                    sd_store_insert_commit();
                    post_unit();
                    state = reinserting ? HOST_REINSERTING : HOST_SAVING;
                    return;
            }
            return;
        case HOST_REINSERTING:
            reinsert_step();
            return;
        case HOST_SAVING:
            /* the change is made whether or not it can be remembered */
            if (!sd_store_save_choice(cur.drive)) printf("SD: the choice won't be remembered at the next start\n");
            finish(true, NULL);
            return;
    }
}

void menu_host_poll(void) {
    if (!between_commands) return;
    uint64_t t0 = time_us_64();
    step();
    uint32_t took = (uint32_t)(time_us_64() - t0);
    if (took > longest_us) longest_us = took;
}

uint32_t menu_host_longest_step_us(void) {
    return longest_us;
}
