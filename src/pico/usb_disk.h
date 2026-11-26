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
 * The SD card as a USB drive for a PC (docs/sd-card.md, "USB storage"): a TinyUSB mass-storage device whose
 * sectors are the card's, read and written through the SD driver underneath FatFs. Only while the card is lent
 * (core/usb_share.h decides; pico/menu_host.c does it) is the medium there; otherwise the PC sees a card reader with
 * no card in it. Only core 0 calls this, and TinyUSB's callbacks run inside usb_disk_poll(), on core 0 too, so the
 * card is never used by two at once.
 */
#ifndef RMFE_PICO_USB_DISK_H
#define RMFE_PICO_USB_DISK_H

#include <stdbool.h>
#include <stdint.h>

/* Shows the device to a PC (on: starting TinyUSB the first time), or takes it off the bus. Only while the card
 * isn't lent. */
void usb_disk_set_connected(bool on);

/* TinyUSB's work: call often, between the 480Z's commands. */
void usb_disk_poll(void);

/* The card is the PC's (true: the PC sees a medium change and can read it), or not. */
void usb_disk_lend(bool lend);

/* A PC has configured the device, and the cable is still in (VBUS: the Pico's USB controller is told VBUS is
 * always there, so an unplugging looks like a suspend to TinyUSB). */
bool usb_disk_host(void);

/* How many times a PC has configured the device. */
uint32_t usb_disk_connections(void);

/* True once for each time the PC has ejected the medium (SCSI START STOP UNIT). */
bool usb_disk_take_eject(void);

#endif
