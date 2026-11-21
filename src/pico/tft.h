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
 * The 2.8" TFT on SPI1 (docs/hardware.md): an ILI9341, with an ST7789 also recognised (by its ID) and driven, in
 * landscape, 320 x 240, RGB565. Text only, a row at a time from a line buffer, sent by DMA while the next row is
 * drawn: there is no frame buffer. Only core 1 calls this.
 *
 * At start-up the controller is identified over MISO, and the write clock is chosen by writing patterns and reading
 * them back (tests/hil/run_display_probe.py found 75 MHz garbled on jumper wires and 37.5 MHz clean): the fastest of
 * TFT_CLOCKS_HZ that writes TFT_SOAK_STRIPS full-width strips, window commands and all, without an error.
 */
#ifndef RMFE_PICO_TFT_H
#define RMFE_PICO_TFT_H

#include <stdbool.h>
#include <stdint.h>

/* docs/hardware.md, "Pin plan" */
#define TFT_PIN_MISO  12u
#define TFT_PIN_CS    13u
#define TFT_PIN_SCK   14u
#define TFT_PIN_MOSI  15u
#define TFT_PIN_DC    20u
#define TFT_PIN_RESET 21u
#define TFT_PIN_LED   22u

#define TFT_W     320u
#define TFT_H     240u
#define TFT_ROW_H 16u /* a text row: the 8 x 13 font with 2 pixels above and 1 below */

#define TFT_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

typedef enum {
    TFT_UNKNOWN = 0, /* nothing answered over MISO: driven as an ILI9341 at the slowest clock */
    TFT_ILI9341,
    TFT_ST7789,
} tft_controller_t;

/* Sets up SPI1, the pins, DMA and the backlight's PWM (off); resets, identifies and switches on the controller; and
 * chooses the clock. Clears the screen. *clock_hz is the clock chosen. */
tft_controller_t tft_init(uint32_t *clock_hz);

/* Backlight brightness, 0-100. */
void tft_backlight(unsigned percent);

/* Draws one row of text at pixel row y: up to 40 characters (the rest of the row in bg), fg on bg. Returns as soon
 * as the row is on its way: the next call (or tft_wait) waits for it. */
void tft_text_row(unsigned y, const char *text, uint16_t fg, uint16_t bg);

/* Waits until everything drawn has reached the display. */
void tft_wait(void);

#endif
