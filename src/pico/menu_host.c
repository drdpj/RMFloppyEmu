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

#include "core/usb_share.h"
#include "pico/mailbox.h"
#include "pico/sd_store.h"
#include "pico/usb_disk.h"

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
static config_t *settings;
static bool (*card_back_fn)(void);
static void (*apply_fn)(void);
static usb_share_t share;
static uint64_t next_check_us;
static bool reinserting; /* the insert under way puts a start-up image back: no request to answer */
static unsigned reinsert_next;

static void post_unit(void) {
    static ui_unit_t u; /* 900 bytes: not on the 4 KB stack */
    sd_store_unit(&u);
    u.rom = rom_found;
    u.usb = share.lent;
    u.config = *settings;
    mailbox_post_unit(&u);
}

/* usb-storage is as the settings say: on the bus or not. */
static void follow_usb_setting(void) {
    usb_share_set_enabled(&share, settings->usb_storage);
    if (!share.lent) usb_disk_set_connected(settings->usb_storage);
}

void menu_host_init(idc_t *idc, bool rom, config_t *c, bool (*card_back)(void), void (*apply)(void)) {
    unit = idc;
    rom_found = rom;
    settings = c;
    card_back_fn = card_back;
    apply_fn = apply;
    next_check_us = time_us_64() + CARD_CHECK_US;
    state = HOST_WAITING;
    usb_share_init(&share, c->usb_storage);
    follow_usb_setting();
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

/* A card is mounted again (it came back, or the PC gave it back): the settings and ROM were read afresh by
 * card_back; put the start-up images back, a step at a time. */
static void card_is_back(void) {
    follow_usb_setting();
    if (!sd_store_mounted()) return;
    reinsert_next = 0;
    state = HOST_REINSERTING;
}

/* ---- the card on USB (core/usb_share.h) ------------------------------------------------------------------------ */

static void lend(void) {
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
        change_disc(d); /* what the 480Z wrote goes to the card first */
    sd_store_release();
    usb_disk_lend(true);
    printf("USB: the card is the PC's until it ejects it\n");
    post_unit();
}

static void take_back(void) {
    usb_disk_lend(false);
    printf("USB: the card is the unit's again\n");
    rom_found = card_back_fn && card_back_fn();
    card_is_back();
    post_unit();
}

/* Lends or takes back the card if it is time. Only between requests. */
static void share_step(void) {
    usb_share_input_t in = {
        .host = usb_disk_host(),
        .connected = usb_disk_connections(),
        .eject = usb_disk_take_eject(),
        .ready = sd_store_mounted(),
    };
    switch (usb_share_step(&share, &in)) {
        case USB_SHARE_NOTHING:
            return;
        case USB_SHARE_LEND:
            lend();
            return;
        case USB_SHARE_TAKE_BACK:
            take_back();
            return;
    }
}

/* ---- the settings menu (UI_REQ_SET) ------------------------------------------------------------------------------ */

/* Sets the option, applies it at once, and writes it into RMFE.CFG. While the PC has the card only usb-storage can
 * change: turning it off takes the card back first, which reads RMFE.CFG again, so the option is set after that. */
static void set_option(void) {
    static char message[MAILBOX_MESSAGE_MAX];
    config_item_t item = cur.item;
    if ((unsigned)item >= CONFIG_ITEM_COUNT || cur.choice >= config_item_choices(item)) {
        finish(false, "no such setting");
        return;
    }
    if (share.lent && item != CONFIG_ITEM_USB_STORAGE) {
        finish(false, "the card is on USB");
        return;
    }
    config_item_set(settings, item, cur.choice);
    if (item == CONFIG_ITEM_USB_STORAGE) {
        usb_share_set_enabled(&share, settings->usb_storage);
        if (share.lent && !settings->usb_storage) {
            share_step(); /* takes it back */
            config_item_set(settings, item, cur.choice);
        }
        follow_usb_setting();
    }
    if (item == CONFIG_ITEM_WRITE_PROTECT) {
        for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
            change_disc(d); /* what was written under the old setting goes first */
        sd_store_apply_write_protect();
    }
    if (item == CONFIG_ITEM_DRIVE_A || item == CONFIG_ITEM_DRIVE_B) {
        change_disc(item == CONFIG_ITEM_DRIVE_A ? 0u : 1u); /* the cached track was found with the old type */
    }
    if (apply_fn) apply_fn();
    const char *why = NULL;
    bool saved = sd_store_save_option(config_item_key(item), config_item_value(item, cur.choice), &why);
    post_unit();
    bool reinserting_after = state == HOST_REINSERTING;
    snprintf(message, sizeof message, "Not saved: %s", why ? why : "?");
    finish(saved, saved ? NULL : message); /* set all the same, until the next start */
    if (reinserting_after) state = HOST_REINSERTING;
}

/* ---- the card coming and going ----------------------------------------------------------------------------------- */

/* Is the card still there, or back? Only between requests, and not while the PC has it. */
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
        card_is_back();
        post_unit();
    }
}

static void step(void) {
    const char *why = NULL;
    switch (state) {
        case HOST_WAITING:
            share_step();
            if (state != HOST_WAITING) return; /* the PC gave the card back: putting the images back */
            if (!share.lent && time_us_64() >= next_check_us) {
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
                case UI_REQ_SET:
                    set_option();
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
    usb_disk_poll();
    step();
    uint32_t took = (uint32_t)(time_us_64() - t0);
    if (took > longest_us) longest_us = took;
}

uint32_t menu_host_longest_step_us(void) {
    return longest_us;
}
