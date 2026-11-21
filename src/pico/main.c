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
 * The MD1 replacement's firmware. One core, for now: it serves the 480Z's commands over the link
 * (pico/link_uart.c) with the command layer (core/idc.h), from the images and IDC ROM on the SD card
 * (pico/sd_store.c, configured as docs/sd-card.md describes).
 *
 * Not yet here (docs/plan.md, milestone 6): the display and encoder, so images can only be chosen in the card's
 * files; changing the card while running.
 */
#include <stdio.h>

#include "core/config.h"
#include "core/idc.h"
#include "pico/link_uart.h"
#include "pico/sd_store.h"
#include "pico/stdlib.h"

/* Static, never on the 4 KB stack (docs/plan.md): idc_t is 19.5 KB (two track caches). */
static idc_t idc;
static config_t cfg;
static uint8_t rom[SD_STORE_ROM_MAX];

/* Command 0B: the uploaded code isn't run (docs/protocol.md, "Implementation decision"). Saving it to the
 * SD card is still to come. */
static void report_exec(void *ctx, uint16_t address, const uint8_t ram[IDC_RAM_BYTES]) {
    (void)ctx;
    (void)ram;
    printf("0B: the 480Z asked to run uploaded code at %04X (not run; answered 40)\n", address);
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    link_uart_init(); /* first: the 480Z sees the unit isn't listening (CTS down) while the card is read */
    printf("RMFloppyEmu\n");

    const uint8_t *boot = NULL;
    size_t boot_len = 0, rom_len = 0;
    config_defaults(&cfg);
    if (sd_store_mount()) {
        sd_store_read_config(&cfg);
        if (sd_store_read_rom(&cfg, rom, &rom_len)) (void)idc_find_boot_block(rom, rom_len, &boot, &boot_len);
    }
    sd_store_insert_startup_images(&cfg);

    idc_init(&idc, link_uart_io(), boot, boot_len);
    idc.exec_hook = report_exec;
    idc_set_drives(&idc, sd_store_drives());
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
        idc_set_drive_type(&idc, d, cfg.drive_type[d]);
    printf("RMFloppyEmu: serving the 480Z link on UART1\n");
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    idc_run(&idc); /* returns only if the link closes, which the UART link never does */
    printf("RMFloppyEmu: the link closed\n");
    for (;;)
        tight_loop_contents();
}
