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
 * The SD card as a USB drive: see usb_disk.h. Descriptors and SCSI after TinyUSB's msc_dual_lun example, with one
 * logical unit, removable, whose sectors are the card's 512-byte blocks.
 *
 * A medium change is told the way a card reader tells it: TEST UNIT READY fails with NOT READY / MEDIUM NOT
 * PRESENT (02/3A/00) while the card isn't lent, and once with UNIT ATTENTION / NOT READY TO READY CHANGE
 * (06/28/00) when it becomes so, so the PC reads it afresh (SCSI Primary Commands, sense codes).
 */
#include "pico/usb_disk.h"

#include <stdio.h>
#include <string.h>

#include "hardware/gpio.h"
#include "pico/unique_id.h"

#include "hw_config.h"
#include "sd_card.h"
#include "tusb.h"

/* pid.codes' VID, with its test PID (pid.codes, "1209:0001 Test PID"): for development and private use, until the
 * device has a PID of its own. */
#define USB_VID        0x1209u
#define USB_PID        0x0001u
#define USB_BCD_DEVICE 0x0100u
#define USB_MAX_MA     100u
#define BLOCK_BYTES    512u
#define EP_MSC_OUT     0x01u
#define EP_MSC_IN      0x81u
#define EP_MSC_SIZE    64u /* full speed bulk */

#define SCSI_ASC_WRITE_ERROR         0x0Cu
#define SCSI_ASC_UNRECOVERED_READ    0x11u
#define SCSI_ASC_INVALID_COMMAND     0x20u
#define SCSI_ASC_NOT_READY_TO_READY  0x28u
#define SCSI_ASC_MEDIUM_NOT_PRESENT  0x3Au
#define SCSI_CMD_SYNCHRONIZE_CACHE10 0x35u

enum { ITF_MSC = 0, ITF_COUNT };
enum { STR_LANGID = 0, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_COUNT };

/* All of it core 0's: written by the calls above and by TinyUSB's callbacks, which run in tud_task(). */
static bool started;
static bool connected;  /* on the bus (usb_disk_set_connected) */
static bool configured; /* a PC has configured it since it was last reset, unplugged or taken off */
static uint32_t connections;
static bool lent;
static bool attention; /* the next TEST UNIT READY says the medium has changed */
static bool ejected;

/* ---- descriptors (USB 2.0, chapter 9) -------------------------------------------------------------------------- */

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, /* each interface says its class */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, USB_MAX_MA),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0, EP_MSC_OUT, EP_MSC_IN, EP_MSC_SIZE),
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t out[1 + 32];
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *s;
    switch (index) {
        case STR_LANGID:
            out[1] = 0x0409; /* English (United States) */
            out[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4u);
            return out;
        case STR_MANUFACTURER:
            s = "RMFloppyEmu";
            break;
        case STR_PRODUCT:
            s = "RMFloppyEmu SD card";
            break;
        case STR_SERIAL:
            pico_get_unique_board_id_string(serial, sizeof serial);
            s = serial;
            break;
        default:
            return NULL;
    }
    size_t n = strlen(s);
    if (n > 32u) n = 32u;
    for (size_t i = 0; i < n; i++)
        out[1 + i] = (uint8_t)s[i];
    out[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
    return out;
}

void tud_mount_cb(void) {
    configured = true;
    connections++;
    printf("USB: a PC has connected\n");
}

void tud_umount_cb(void) {
    configured = false;
}

/* ---- SCSI (TinyUSB's msc_device.h) --------------------------------------------------------------------------- */

static sd_card_t *card(void) {
    return sd_get_by_num(0);
}

static void no_medium(void) {
    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT, 0x00);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "RMFE    ", 8);
    memcpy(product_id, "SD card         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (!lent) {
        no_medium();
        return false;
    }
    if (attention) {
        attention = false;
        tud_msc_set_sense(0, SCSI_SENSE_UNIT_ATTENTION, SCSI_ASC_NOT_READY_TO_READY, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    sd_card_t *sd = card();
    *block_count = lent && sd ? sd->get_num_sectors(sd) : 0u;
    *block_size = BLOCK_BYTES;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject && !start && lent) {
        ejected = true;
        printf("USB: the PC has ejected the card\n");
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}

/* TinyUSB asks for whole buffers of CFG_TUD_MSC_EP_BUFSIZE, a multiple of the block, so offset is always 0 and
 * bufsize whole blocks; anything else is refused rather than half done. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    sd_card_t *sd = card();
    if (!lent || !sd) {
        no_medium();
        return -1;
    }
    if (offset % BLOCK_BYTES || bufsize % BLOCK_BYTES) return -1;
    if (sd->read_blocks(sd, buffer, lba + offset / BLOCK_BYTES, bufsize / BLOCK_BYTES) != SD_BLOCK_DEVICE_ERROR_NONE) {
        tud_msc_set_sense(0, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ, 0x00);
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    sd_card_t *sd = card();
    if (!lent || !sd) {
        no_medium();
        return -1;
    }
    if (offset % BLOCK_BYTES || bufsize % BLOCK_BYTES) return -1;
    if (sd->write_blocks(sd, buffer, lba + offset / BLOCK_BYTES, bufsize / BLOCK_BYTES) != SD_BLOCK_DEVICE_ERROR_NONE) {
        tud_msc_set_sense(0, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_WRITE_ERROR, 0x00);
        return -1;
    }
    return (int32_t)bufsize;
}

/* The commands TinyUSB leaves to us. */
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)buffer;
    (void)bufsize;
    sd_card_t *sd = card();
    switch (scsi_cmd[0]) {
        case SCSI_CMD_SYNCHRONIZE_CACHE10:
            if (!lent || !sd) {
                no_medium();
                return -1;
            }
            return sd->sync(sd) == SD_BLOCK_DEVICE_ERROR_NONE ? 0 : -1;
        default:
            tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_COMMAND, 0x00);
            return -1;
    }
}

/* ---- the interface (usb_disk.h) ------------------------------------------------------------------------------ */

void usb_disk_set_connected(bool on) {
    if (on == connected) return;
    connected = on;
    if (on && !started) {
        gpio_init(PICO_VBUS_PIN);
        gpio_set_dir(PICO_VBUS_PIN, GPIO_IN);
        started = tusb_init();
        if (!started) printf("USB: TinyUSB didn't start\n");
        return;
    }
    if (!started) return;
    if (on) {
        (void)tud_connect(); /* the PC then resets and configures it afresh */
    } else {
        (void)tud_disconnect();
        configured = false; /* no reset comes to tell TinyUSB */
    }
}

void usb_disk_poll(void) {
    if (!started) return;
    tud_task();
    if (configured && !gpio_get(PICO_VBUS_PIN)) {
        configured = false;
        printf("USB: unplugged\n");
    }
}

void usb_disk_lend(bool lend) {
    lent = lend;
    attention = lend;
    ejected = false;
}

bool usb_disk_host(void) {
    return started && configured && gpio_get(PICO_VBUS_PIN);
}

uint32_t usb_disk_connections(void) {
    return connections;
}

bool usb_disk_take_eject(void) {
    bool e = ejected;
    ejected = false;
    return e;
}
