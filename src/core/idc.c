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
#include "core/idc.h"

#include <string.h>

#include "core/bytes.h"
#include "core/idc_disk.h"
#include "core/rmfe_assert.h"

#define IDC_ROM_BYTES 0x2000u /* 8 KB at 0000-1FFF [MAME rm_mq2] */

/* Commands below this (FE, FF, 00 as index 0-2) get no status echo before their handler [IDC3 0a9e-0aa6]. */
#define IDC_FIRST_ECHOED_INDEX 3u
/* Valid command indices (command - FE). The IDC accepts 0-1A, but index 1A (command 18) has no table
 * entry and would run whatever follows the table; we answer it as unknown. */
#define IDC_COMMAND_INDICES 0x1Au

void idc_init(idc_t *d, const link_io_t *io, const uint8_t *boot_block, size_t boot_len) {
    memset(d, 0, sizeof *d);
    link_init(&d->link, io);
    d->boot_block = boot_block;
    d->boot_len = boot_block ? boot_len : 0;
    d->retries = 1; /* the RAM value is not set at power-up; the 480Z's first 05 sets it [trace] */
    for (unsigned i = 0; i < IDC_DRIVES; i++)
        d->drive_map[i] = (uint8_t)i;
}

uint8_t idc_param_byte(uint8_t raw) {
    uint8_t low = raw & 3u;
    return (low == 1u || low == 2u) ? (uint8_t)(raw ^ 3u) : raw;
}

/* 05: reset/configure [IDC3 0b6f]. High nibble (unless F): retry count = nibble + 1. Low nibble with
 * bit 0 cleared: unless bit 3 is set, the drive map is reset and logical drives 0/1 swapped with n/n+1. */
static link_status_t cmd_configure(idc_t *d) {
    uint8_t raw;
    link_status_t st = link_get(&d->link, &raw);
    if (st != LINK_OK) return st;
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK) return st;
    uint8_t p = (uint8_t)(idc_param_byte(raw) & 0xFEu);
    uint8_t hi = (uint8_t)(p >> 4);
    if (hi != 0x0Fu) d->retries = (uint8_t)(hi + 1u);
    uint8_t n = p & 0x0Fu;
    if (!(n & 0x08u)) {
        for (unsigned i = 0; i < IDC_DRIVES; i++)
            d->drive_map[i] = (uint8_t)i;
        d->drive_map[0] = n;
        d->drive_map[1] = (uint8_t)(n + 1u);
        d->drive_map[n] = 0;
        d->drive_map[n + 1u] = 1;
    }
    return LINK_OK;
}

/* 07: retry count = n + 1 for n < 10h, else status 20 [IDC3 0c7c]. */
static link_status_t cmd_set_retries(idc_t *d) {
    uint8_t n;
    link_status_t st = link_get(&d->link, &n);
    if (st != LINK_OK) return st;
    if (n >= 0x10u) return link_put(&d->link, IDC_STATUS_BAD_PARAM);
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK) return st;
    d->retries = (uint8_t)(n + 1u);
    return LINK_OK;
}

/* Reads a 16-bit address or length, low byte first. */
static link_status_t get16(idc_t *d, uint16_t *v) {
    uint8_t lo, hi;
    link_status_t st = link_get(&d->link, &lo);
    if (st == LINK_OK) st = link_get(&d->link, &hi);
    *v = (uint16_t)(lo | (hi << 8));
    return st;
}

/*
 * 0A: upload into unit RAM [IDC3 0cc0, 0ef6, 0ee3]. Address and length (low byte first); status 00, or
 * 20 if the address is below E000 or address + length passes FFFF, after which the IDC sends one more
 * byte from A (20 on overflow, otherwise the length's high byte, still in A) and stops. Then the data,
 * and a 16-bit check: the sum of the data bytes plus the check must be 0 (mod 65536); 00 if so, else 10.
 */
static link_status_t cmd_upload(idc_t *d) {
    uint16_t addr, len;
    link_status_t st = get16(d, &addr);
    if (st == LINK_OK) st = get16(d, &len);
    if (st != LINK_OK) return st;
    bool low = addr < IDC_RAM_BASE;
    bool overflow = (uint32_t)addr + len > 0xFFFFu;
    if (low && d->sticky == IDC_STATUS_OK) d->sticky = IDC_STATUS_BAD_PARAM;
    if (overflow) {
        st = link_put(&d->link, IDC_STATUS_BAD_PARAM);
        return st == LINK_OK ? link_put(&d->link, IDC_STATUS_BAD_PARAM) : st;
    }
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK) return st;
    if (low) return link_put(&d->link, (uint8_t)(len >> 8));

    uint16_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b;
        st = link_get(&d->link, &b);
        if (st != LINK_OK) return st;
        d->ram[(addr + i) % IDC_RAM_BYTES] = b; /* the 2 KB is mirrored through E000-FFFF */
        sum = (uint16_t)(sum + b);
    }
    uint16_t check;
    st = get16(d, &check);
    if (st != LINK_OK) return st;
    return link_put(&d->link, (uint16_t)(sum + check) == 0 ? d->sticky : IDC_STATUS_CHECKSUM);
}

/*
 * 0B: run code in unit RAM [IDC3 0cf0]. Address (low byte first); below E000 the IDC returns to its
 * idle loop with no reply. Otherwise it sends status 00, CALLs the address, and sends whatever the code
 * left in A. We can't run it: the platform hook reports it, and we send 40 as the result, so the 480Z
 * sees a failure instead of waiting for ever. (Deviation from the real unit, documented in
 * docs/protocol.md.)
 */
static link_status_t cmd_execute(idc_t *d) {
    uint16_t addr;
    link_status_t st = get16(d, &addr);
    if (st != LINK_OK) return st;
    if (addr < IDC_RAM_BASE) return LINK_OK;
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK) return st;
    if (d->exec_hook) d->exec_hook(d->exec_ctx, addr, d->ram);
    return link_put(&d->link, IDC_STATUS_UNKNOWN_CMD);
}

static link_status_t cmd_boot_block(idc_t *d) {
    for (size_t i = 0; i < d->boot_len; i++) {
        link_status_t st = link_put(&d->link, d->boot_block[i]);
        if (st != LINK_OK) return st;
    }
    return LINK_OK;
}

static void tell(idc_t *d, idc_act_stage_t stage) {
    d->activity.stage = stage;
    if (d->activity_hook) d->activity_hook(d->activity_ctx, &d->activity);
}

static link_status_t serve(idc_t *d, uint8_t c);

link_status_t idc_serve_one(idc_t *d) {
    uint8_t c;
    if (d->idle_hook) d->idle_hook(d->idle_ctx, true);
    link_status_t st = link_get(&d->link, &c);
    if (d->idle_hook) d->idle_hook(d->idle_ctx, false);
    if (st != LINK_OK) return st;
    memset(&d->activity, 0, sizeof d->activity);
    d->activity.cmd = c;
    tell(d, IDC_ACT_START);
    st = serve(d, c);
    tell(d, IDC_ACT_END);
    return st;
}

void idc_note_drive(idc_t *d, unsigned phys, unsigned side, bool has_track, uint8_t track, uint8_t third) {
    d->activity.has_drive = true;
    d->activity.drive = (uint8_t)phys;
    d->activity.side = (uint8_t)side;
    d->activity.has_track = has_track;
    d->activity.track = track;
    d->activity.third = third;
    tell(d, IDC_ACT_DRIVE);
}

static link_status_t serve(idc_t *d, uint8_t c) {
    link_status_t st;
    d->sticky = IDC_STATUS_OK;

    uint8_t index = (uint8_t)(c - IDC_CMD_MONITOR);
    bool known = index < IDC_COMMAND_INDICES &&
                 (c == IDC_CMD_GO_FAST || c == IDC_CMD_CONFIGURE || c == IDC_CMD_SET_RETRIES || c == IDC_CMD_UPLOAD ||
                  c == IDC_CMD_EXECUTE || c == IDC_CMD_VERSION || c == IDC_CMD_NOP || c == IDC_CMD_LOGIN ||
                  c == IDC_CMD_READ || c == IDC_CMD_WRITE || c == IDC_CMD_WRITE_VER || c == IDC_CMD_FLUSH ||
                  c == IDC_CMD_PROBE || c == IDC_CMD_FORMAT || c == IDC_CMD_VERIFY || c == IDC_CMD_READ_ID ||
                  c == IDC_CMD_WRITE_ID || c == IDC_CMD_WRITE_ID_V || (c == IDC_CMD_BOOT_BLOCK && d->boot_block));
    if (!known) return link_put(&d->link, IDC_STATUS_UNKNOWN_CMD);
    if (index >= IDC_FIRST_ECHOED_INDEX) {
        st = link_put(&d->link, d->sticky);
        if (st != LINK_OK) return st;
    }
    switch (c) {
        case IDC_CMD_GO_FAST:
            return link_go_fast(&d->link);
        case IDC_CMD_BOOT_BLOCK:
            return cmd_boot_block(d);
        case IDC_CMD_CONFIGURE:
            return cmd_configure(d);
        case IDC_CMD_SET_RETRIES:
            return cmd_set_retries(d);
        case IDC_CMD_LOGIN:
            return idc_cmd_login(d);
        case IDC_CMD_READ:
            return idc_cmd_read(d);
        case IDC_CMD_WRITE:
        case IDC_CMD_WRITE_VER: /* our writes are exact, so the read-back verify can't fail */
            return idc_cmd_write(d);
        case IDC_CMD_FLUSH:
            return idc_cmd_flush(d);
        case IDC_CMD_PROBE:
            return idc_cmd_probe(d);
        case IDC_CMD_READ_ID:
            return idc_cmd_read_id(d);
        case IDC_CMD_WRITE_ID:
        case IDC_CMD_WRITE_ID_V:
            return idc_cmd_write_id(d);
        case IDC_CMD_FORMAT:
            return idc_cmd_format(d, false);
        case IDC_CMD_VERIFY:
            return idc_cmd_format(d, true);
        case IDC_CMD_UPLOAD:
            return cmd_upload(d);
        case IDC_CMD_EXECUTE:
            return cmd_execute(d);
        case IDC_CMD_VERSION:
            return link_put(&d->link, IDC_VERSION_BYTE);
        case IDC_CMD_NOP:
            return LINK_OK;
        default:
            RMFE_ASSERT(!"command marked known without a handler");
            return LINK_OK;
    }
}

const char *idc_command_name(uint8_t cmd) {
    switch (cmd) {
        case IDC_CMD_MONITOR:
            return "monitor";
        case IDC_CMD_BOOT_BLOCK:
            return "boot block";
        case IDC_CMD_GO_FAST:
            return "go fast";
        case IDC_CMD_LOGIN:
            return "log in";
        case IDC_CMD_READ:
            return "read";
        case IDC_CMD_WRITE:
            return "write";
        case IDC_CMD_WRITE_VER:
            return "write+verify";
        case IDC_CMD_CONFIGURE:
            return "configure";
        case IDC_CMD_PROBE:
            return "probe";
        case IDC_CMD_SET_RETRIES:
            return "retries";
        case IDC_CMD_FLUSH:
            return "flush";
        case IDC_CMD_UPLOAD:
            return "upload";
        case IDC_CMD_EXECUTE:
            return "execute";
        case IDC_CMD_FORMAT:
            return "format";
        case IDC_CMD_VERIFY:
            return "verify";
        case IDC_CMD_READ_ID:
            return "read sector";
        case IDC_CMD_WRITE_ID:
            return "write sector";
        case IDC_CMD_WRITE_ID_V:
            return "write sector+v";
        case IDC_CMD_VERSION:
            return "version";
        case IDC_CMD_NOP:
            return "nop";
        default:
            return "?";
    }
}

bool idc_command_writes(uint8_t cmd) {
    return cmd == IDC_CMD_WRITE || cmd == IDC_CMD_WRITE_VER || cmd == IDC_CMD_FLUSH || cmd == IDC_CMD_FORMAT ||
           cmd == IDC_CMD_WRITE_ID || cmd == IDC_CMD_WRITE_ID_V;
}

void idc_set_drives(idc_t *d, const idc_drives_t *drives) {
    d->drives = drives;
    for (unsigned i = 0; i < IDC_PHYS_DRIVES; i++)
        d->cache[i].valid = false;
}

void idc_set_drive_type(idc_t *d, unsigned drive, idc_drive_type_t type) {
    RMFE_ASSERT(drive < IDC_PHYS_DRIVES);
    d->drive_type[drive] = type;
}

void idc_run(idc_t *d) {
    while (idc_serve_one(d) != LINK_CLOSED) {}
}

bool idc_find_boot_block(const uint8_t *rom, size_t rom_len, const uint8_t **block, size_t *len) {
    if (!rom || rom_len != IDC_ROM_BYTES || !block || !len) return false;
    for (size_t i = 0; i + 9 <= rom_len; i++) {
        /* ED 4B ll ll : ld bc,(len)   21 bb bb : ld hl,block   7E : ld a,(hl)   CD : call */
        if (rom[i] != 0xEDu || rom[i + 1] != 0x4Bu || rom[i + 4] != 0x21u || rom[i + 7] != 0x7Eu ||
            rom[i + 8] != 0xCDu) {
            continue;
        }
        uint16_t len_at = bytes_get_le16(rom + i + 2);
        uint16_t at = bytes_get_le16(rom + i + 5);
        if ((size_t)len_at + 2 > rom_len) continue;
        uint16_t n = bytes_get_le16(rom + len_at);
        if (n == 0 || (size_t)at + n > rom_len) continue;
        *block = rom + at;
        *len = n;
        return true;
    }
    return false;
}
