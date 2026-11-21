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
 * Bring-up firmware for the SD card (docs/plan.md, milestone 6, step 3): mounts it, prints its size and
 * filesystem, lists the root, finds the boot block in IDC.ROM, and reads every IMD and HFE end to end,
 * printing each one's size, CRC-32 and read speed, so they can be compared with the files on the PC.
 */
#include <stdio.h>
#include <string.h>

#include "core/idc.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "hw_config.h"

static FATFS fs;
static uint8_t buf[4096];
static uint8_t rom[8192];

/* CRC-32 (IEEE 802.3, reflected, as zlib.crc32), bit by bit: speed doesn't matter here. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool is_image(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    const char *ext = name + n - 4;
    return !strcasecmp(ext, ".imd") || !strcasecmp(ext, ".hfe");
}

static void read_whole(const char *name) {
    FIL f;
    FRESULT fr = f_open(&f, name, FA_READ);
    if (fr != FR_OK) {
        printf("SD-FILE %s open failed: %d\n", name, fr);
        return;
    }
    uint32_t crc = 0, total = 0;
    uint64_t read_us = 0;
    for (;;) {
        UINT got = 0;
        uint64_t t0 = time_us_64();
        fr = f_read(&f, buf, sizeof buf, &got);
        read_us += time_us_64() - t0;
        if (fr != FR_OK || got == 0) break;
        crc = crc32_update(crc, buf, got);
        total += got;
    }
    (void)f_close(&f);
    printf("SD-FILE %s size=%lu crc32=%08lx read_ms=%lu kB_per_s=%lu%s\n", name, (unsigned long)total,
           (unsigned long)crc, (unsigned long)(read_us / 1000u),
           (unsigned long)(read_us ? (uint64_t)total * 1000u / read_us : 0), fr == FR_OK ? "" : " READ-ERROR");
}

int main(void) {
    stdio_init_all();
    sleep_ms(200);
    printf("\nSD-PROBE begin\n");

    FRESULT fr = f_mount(&fs, "0:", 1);
    if (fr != FR_OK) {
        printf("SD-PROBE mount failed: FRESULT %d\nSD-PROBE end\n", fr);
        for (;;)
            tight_loop_contents();
    }
    sd_card_t *sd = sd_get_by_num(0);
    const char *type = fs.fs_type == FS_EXFAT ? "exFAT" : fs.fs_type == FS_FAT32 ? "FAT32" : "FAT12/16";
    printf("SD-PROBE mounted: %s, %lu sectors (%lu MB), cluster %u bytes\n", type,
           (unsigned long)sd->get_num_sectors(sd), (unsigned long)(sd->get_num_sectors(sd) / 2048u),
           (unsigned)(fs.csize * 512u));

    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, "/") == FR_OK) {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            printf("SD-DIR %s %s %lu\n", (fi.fattrib & AM_DIR) ? "d" : "-", fi.fname, (unsigned long)fi.fsize);
        }
        (void)f_closedir(&dir);
    }

    FIL f;
    UINT got = 0;
    if (f_open(&f, "IDC.ROM", FA_READ) == FR_OK && f_read(&f, rom, sizeof rom, &got) == FR_OK) {
        const uint8_t *blk;
        size_t len;
        bool found = idc_find_boot_block(rom, got, &blk, &len);
        printf("SD-ROM IDC.ROM %u bytes: boot block %s, %u bytes at %04x\n", (unsigned)got,
               found ? "found" : "NOT found", (unsigned)(found ? len : 0), (unsigned)(found ? blk - rom : 0));
        (void)f_close(&f);
    } else {
        printf("SD-ROM IDC.ROM not readable\n");
    }

    if (f_opendir(&dir, "/") == FR_OK) {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            if (!(fi.fattrib & AM_DIR) && is_image(fi.fname)) read_whole(fi.fname);
        }
        (void)f_closedir(&dir);
    }
    (void)f_unmount("0:");
    printf("SD-PROBE end\n");
    for (;;)
        tight_loop_contents();
}
