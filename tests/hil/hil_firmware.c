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
 * Firmware for the hardware-in-the-loop tests (docs/testing.md, level 3; tests/hil/run_hil.py): the real link
 * driver (pico/link_uart.c) and command layer (core/idc.h), with a made-up disc in drive A, so the PC can run
 * record exchanges (02, 04, 08) over the real UART and pins without an SD card.
 *
 * The disc is shaped like the RM 480Z system disc, as in tests/test_idc_disk.c: 40 cylinders, two sides, track 0
 * FM 16 x 128, the rest MFM 9 x 512, IDs from 1. Every data byte says where it is (hil_pattern below; run_hil.py
 * has its own copy). Tracks are made from the pattern on demand; written tracks are kept, up to HIL_KEPT.
 * Drive B is empty.
 */
#include <stdio.h>
#include <string.h>

#include "core/disk.h"
#include "core/idc.h"
#include "pico/link_uart.h"
#include "pico/stdlib.h"

#define HIL_CYLS 40u
#define HIL_KEPT 8u

static idc_t idc;
static disk_track_t kept[HIL_KEPT];
static unsigned kept_cs[HIL_KEPT][2]; /* cylinder, side */
static unsigned nkept;

static uint8_t hil_pattern(unsigned c, unsigned s, unsigned r, unsigned k) {
    return (uint8_t)(c * 7u + s * 50u + r * 13u + k);
}

static void make_track(disk_track_t *t, unsigned c, unsigned s) {
    disk_track_clear(t, (uint8_t)c, (uint8_t)s);
    bool fm = c == 0;
    t->fm = fm;
    t->rate_kbps = fm ? 125 : 250;
    unsigned n = fm ? 16 : 9, size = fm ? 128 : 512;
    for (unsigned i = 0; i < n; i++) {
        uint8_t buf[512];
        for (unsigned k = 0; k < size; k++)
            buf[k] = hil_pattern(c, s, i + 1, k);
        disk_sector_t sec = {.c = (uint8_t)c,
                             .h = 0,
                             .r = (uint8_t)(i + 1),
                             .n = fm ? 0 : 2,
                             .size = (uint16_t)size,
                             .pos = i * DISK_CELLS_PER_REV / n};
        (void)disk_track_add(t, &sec, buf); /* fits by construction: 4608 bytes, 16 sectors at most */
    }
}

static disk_track_t *find_kept(unsigned c, unsigned s) {
    for (unsigned i = 0; i < nkept; i++) {
        if (kept_cs[i][0] == c && kept_cs[i][1] == s) return &kept[i];
    }
    return NULL;
}

static bool d_present(void *ctx, unsigned drive) {
    (void)ctx;
    return drive == 0;
}

static unsigned d_cylinders(void *ctx, unsigned drive) {
    (void)ctx;
    (void)drive;
    return HIL_CYLS;
}

static disk_status_t d_load(void *ctx, unsigned drive, unsigned c, unsigned s, disk_track_t *t) {
    (void)ctx;
    (void)drive;
    if (c >= HIL_CYLS) {
        disk_track_clear(t, (uint8_t)c, (uint8_t)s);
        return DISK_OK;
    }
    const disk_track_t *k = find_kept(c, s);
    if (k) {
        memcpy(t, k, sizeof *t);
    } else {
        make_track(t, c, s);
    }
    return DISK_OK;
}

static disk_status_t d_save(void *ctx, unsigned drive, const disk_track_t *t) {
    (void)ctx;
    (void)drive;
    disk_track_t *k = find_kept(t->cyl, t->side);
    if (!k) {
        if (nkept >= HIL_KEPT || t->cyl >= HIL_CYLS) return DISK_ERR_IO; /* the test writes too many tracks */
        kept_cs[nkept][0] = t->cyl;
        kept_cs[nkept][1] = t->side;
        k = &kept[nkept++];
    }
    memcpy(k, t, sizeof *t);
    return DISK_OK;
}

static uint32_t d_position(void *ctx, unsigned drive) {
    (void)ctx;
    (void)drive;
    return 0; /* no duplicate IDs on this disc, so rotation doesn't matter */
}

static const idc_drives_t drives = {NULL, d_present, d_cylinders, d_load, d_save, d_position};

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    link_uart_init();
    idc_init(&idc, link_uart_io(), NULL, 0);
    idc_set_drives(&idc, &drives);
    printf("RMFE-HIL ready: made-up disc in drive A\n");
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    idc_run(&idc);
    for (;;)
        tight_loop_contents();
}
