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
 * The MD1 replacement's firmware: it serves the 480Z's commands over the link (pico/link_uart.c) with the command
 * layer (core/idc.h), from the images and IDC ROM on the SD card (pico/sd_store.c, configured as docs/sd-card.md
 * describes), and shows them on the display, where the knob chooses them (core/ui.h).
 *
 * The two cores:
 *   - Core 0 owns the 480Z's link (UART1 and its pins, the RTS interrupt), the command layer, the SD card
 *     (SPI0, FatFs, which is not re-entrant here) and USB (TinyUSB, its interrupt and callbacks: pico/usb_disk.c).
 * Between commands, while it waits for the next one, it does the menu's work on the card a short step at a time
 * (pico/menu_host.c), so a command waits at most one step for it, and an image changes only between commands.
 *   - Core 1 owns the display (SPI1 and a DMA channel), the knob (its pins and their interrupt) and the menu
 *     (pico/panel.c). It never touches the link or the card, so drawing can't hold up the 480Z.
 *   - They meet only in pico/mailbox.c. Each has its own 4 KB stack with the hardware limit at its bottom.
 *   - Both print on the console; the SDK serialises that, so a line from core 1 can delay one from core 0, which
 *     prints only at start-up, on errors and when an image changes.
 * The card may be taken out and put back while it runs, and lent to a PC over USB (pico/menu_host.c).
 */
#include <stdio.h>

#include "core/config.h"
#include "core/idc.h"
#include "pico/link_uart.h"
#include "pico/mailbox.h"
#include "pico/menu_host.h"
#include "pico/panel.h"
#include "pico/sd_store.h"
#include "pico/stack_meter.h"
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

/* The settings the command layer and the link hold: at start-up, when a card comes back, and when the settings
 * menu changes one. Core 0, between commands. A new rate takes effect at the 480Z's next 00. */
static void apply_settings(void) {
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
        idc_set_drive_type(&idc, d, cfg.drive_type[d]);
    link_set_fast_baud(&idc.link, cfg.fast_baud);
    link_set_boot_baud(&idc.link, cfg.boot_baud);
}

/* A card has come back, or a PC has given it back (pico/menu_host.c): mount it, and read the settings and the ROM
 * again, as at start-up. Core 0, between commands. Core 1 follows the settings it uses (the knob, show-commands)
 * from what core 0 posts (pico/mailbox.h). */
static bool card_back(void) {
    if (!sd_store_mount()) return false;
    sd_store_read_config(&cfg);
    apply_settings();
    size_t rom_len = 0, boot_len = 0;
    const uint8_t *boot = NULL;
    if (sd_store_read_rom(&cfg, rom, &rom_len)) (void)idc_find_boot_block(rom, rom_len, &boot, &boot_len);
    idc.boot_block = boot; /* the ROM is read into the same buffer, so FF sends it from there as before */
    idc.boot_len = boot_len;
    return boot != NULL;
}

int main(void) {
    stack_meter_paint_core0();
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
    apply_settings();

    mailbox_init();
    menu_host_init(&idc, boot != NULL, &cfg, card_back, apply_settings); /* starts USB if usb-storage is on */
    idc.idle_hook = menu_host_idle;
    idc.activity_hook = mailbox_note_activity;
    link_uart_set_idle(menu_host_poll);
    panel_start(&cfg);
    printf("RMFloppyEmu: serving the 480Z link on UART1, going fast at %lu baud, the ROS's boot block at %lu baud\n",
           (unsigned long)cfg.fast_baud, (unsigned long)cfg.boot_baud);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    idc_run(&idc); /* returns only if the link closes, which the UART link never does */
    printf("RMFloppyEmu: the link closed\n");
    for (;;)
        tight_loop_contents();
}
