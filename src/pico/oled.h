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
 * A small OLED on I2C1 (docs/hardware.md, "An OLED instead of the TFT"): an SSD1306, 128 x 64 or 128 x 32, 4-pin
 * modules at 0x3C (or 0x3D). Text only, drawn into a frame buffer in bands a font high; oled_flush() sends the
 * 8-row pages that changed. Only core 1 calls this.
 *
 * Both modules tried (tests/hil/oled_probe.c) wrote 50 full frames at 100, 400 and 1000 kHz without a NAK: a frame
 * takes 12 ms at 1 MHz, a page 1.5 ms. 1 MHz is Fast-mode Plus, above the SSD1306's rated 400 kHz, so the set-up
 * falls back to 400 kHz if it isn't acknowledged at 1 MHz.
 */
#ifndef RMFE_PICO_OLED_H
#define RMFE_PICO_OLED_H

#include <stdbool.h>
#include <stdint.h>

/* docs/hardware.md, "Pin plan": I2C1's SDA and SCL */
#define OLED_PIN_SDA 10u
#define OLED_PIN_SCL 11u

#define OLED_W 128u

/* Characters of their own (core/ui.h's): a dot a cell wide in the middle of the row, filled and hollow (the drive
 * lights), and an ellipsis (a name cut short). */
#define OLED_DOT_FILLED '\x01'
#define OLED_DOT_HOLLOW '\x02'
#define OLED_ELLIPSIS   '\x03' /* three dots on the baseline, in one cell */

typedef enum {
    OLED_FONT_7X13B, /* the rows: 18 columns */
    OLED_FONT_5X8,   /* a 128 x 64 panel's status line: 21 */
    OLED_FONT_4X6,   /* a 128 x 32 panel's: 32 */
} oled_font_t;

/* Sets up I2C1 and looks for an OLED at 0x3C, then 0x3D. If one answers: sets it up for `height` rows (64 or 32),
 * turned 180 degrees if rotate, clears it and returns true, with its address and the I2C clock in *addr and
 * *clock_hz. If none does, returns false and leaves GP10/GP11 as inputs with pull-ups. */
bool oled_init(unsigned height, bool rotate, uint8_t *addr, uint32_t *clock_hz);

/* The height of font's cell, in pixels. */
unsigned oled_font_height(oled_font_t font);

/* A row of text, its top at pixel row y: the band the font's height across the whole width is cleared, then up to
 * as many characters as fit are drawn. inverse: light text on a lit band. Shown at the next oled_flush(). */
void oled_text(unsigned y, oled_font_t font, const char *text, bool inverse);

/* A one-pixel line across the width at pixel row y. */
void oled_rule(unsigned y);

/* Sends the pages changed since the last flush. Returns false if any write wasn't acknowledged. */
bool oled_flush(void);

/* Writes not acknowledged since the start: an OLED unplugged, or a bad wire. */
uint32_t oled_errors(void);

#endif
