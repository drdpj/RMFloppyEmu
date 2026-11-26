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
 * Sharing the SD card with a PC over USB (docs/sd-card.md, "USB storage"): who has it, and when it changes hands.
 * A USB mass-storage device hands the PC raw sectors, and the PC's file system caches them, so the PC and the unit
 * can't both use the card: while the PC has it ("lent"), the unit has unmounted it and both drives are empty (the
 * 480Z sees no disc, 80).
 *
 * The card is lent when usb-storage is on, a PC has configured the device (USB enumeration, not just power: a
 * charger never does), and the unit is ready to let it go (the card mounted, between the 480Z's commands, nothing
 * of the menu's under way). It comes back when the PC ejects it (SCSI START STOP UNIT), the PC goes (unplugged,
 * or the device unconfigured), or usb-storage is turned off. After an eject the PC doesn't get it again until it
 * connects anew, or usb-storage is turned off and on again.
 *
 * The platform does the work (pico/menu_host.c, pico/usb_disk.c); this only decides. Deterministic: no clock.
 */
#ifndef RMFE_CORE_USB_SHARE_H
#define RMFE_CORE_USB_SHARE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled;       /* usb-storage */
    bool lent;          /* the PC has the card */
    bool ejected;       /* the PC ejected it since it last connected: don't lend it again */
    uint32_t connected; /* the PC's connection count when last seen */
} usb_share_t;

/* What the platform sees, each time it asks. */
typedef struct {
    bool host;          /* a PC has configured the device */
    uint32_t connected; /* how many times a PC has configured it: a new number is a new connection */
    bool eject;         /* the PC has ejected the card since the last call */
    bool ready;         /* the unit can let the card go now */
} usb_share_input_t;

typedef enum {
    USB_SHARE_NOTHING = 0,
    USB_SHARE_LEND,      /* unmount the card and give it to the PC */
    USB_SHARE_TAKE_BACK, /* take it from the PC and mount it again */
} usb_share_action_t;

/* Starts with the card the unit's. */
void usb_share_init(usb_share_t *s, bool enabled);

/* usb-storage changed. Turning it on lets a PC that ejected the card have it again. */
void usb_share_set_enabled(usb_share_t *s, bool enabled);

/* What to do now; the platform must do it before the next call. */
usb_share_action_t usb_share_step(usb_share_t *s, const usb_share_input_t *in);

#endif
