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

/* Sharing the SD card with a PC over USB: see usb_share.h. */
#include "core/usb_share.h"

#include "core/rmfe_assert.h"

#include <string.h>

void usb_share_init(usb_share_t *s, bool enabled) {
    RMFE_ASSERT(s);
    memset(s, 0, sizeof *s);
    s->enabled = enabled;
}

void usb_share_set_enabled(usb_share_t *s, bool enabled) {
    RMFE_ASSERT(s);
    if (enabled && !s->enabled) s->ejected = false;
    s->enabled = enabled;
}

usb_share_action_t usb_share_step(usb_share_t *s, const usb_share_input_t *in) {
    RMFE_ASSERT(s && in);
    if (in->connected != s->connected) { /* a new connection: an eject before it is forgotten */
        s->connected = in->connected;
        s->ejected = false;
    }
    if (s->lent) {
        if (in->eject) s->ejected = true;
        if (in->eject || !in->host || !s->enabled) {
            s->lent = false;
            return USB_SHARE_TAKE_BACK;
        }
        return USB_SHARE_NOTHING;
    }
    if (s->enabled && in->host && !s->ejected && in->ready) {
        s->lent = true;
        return USB_SHARE_LEND;
    }
    return USB_SHARE_NOTHING;
}
