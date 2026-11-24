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
 * The OLED: see oled.h. Commands are from the SSD1306 datasheet, section 9 (table 9-1), and its software
 * initialisation flow (section 10, as the module makers' examples use it). The panel is driven in page addressing
 * mode: a page is 8 rows, each byte a column of them, the top row in bit 0 (section 10.1.3).
 */
#include "pico/oled.h"

#include "pico/font_4x6.h"
#include "pico/font_5x8.h"
#include "pico/font_7x13b.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define OLED_I2C       i2c1
#define OLED_PAGES     8u /* at most: 64 rows */
#define PROBE_HZ       100000u
#define FAST_HZ        1000000u
#define FALLBACK_HZ    400000u
#define I2C_TIMEOUT_US 20000u

/* The first byte of each write says what follows (section 8.1.5.2). */
#define CTRL_COMMANDS 0x00u
#define CTRL_DATA     0x40u

#define CMD_DISPLAY_OFF   0xAEu
#define CMD_DISPLAY_ON    0xAFu
#define CMD_CLOCK_DIV     0xD5u /* + divide ratio and oscillator frequency */
#define CMD_MULTIPLEX     0xA8u /* + rows - 1 */
#define CMD_DISPLAY_SHIFT 0xD3u /* + vertical offset */
#define CMD_START_LINE    0x40u /* | line */
#define CMD_CHARGE_PUMP   0x8Du /* + 0x14: on */
#define CMD_ADDR_MODE     0x20u /* + 0x02: page addressing */
#define CMD_STOP_SCROLL   0x2Eu
#define CMD_SEG_NORMAL    0xA0u /* column 0 at the left */
#define CMD_SEG_REMAP     0xA1u /* column 0 at the right */
#define CMD_COM_UP        0xC0u
#define CMD_COM_DOWN      0xC8u
#define CMD_COM_PINS      0xDAu /* + 0x02 sequential (128 x 32 panels), 0x12 alternative (128 x 64) */
#define CMD_CONTRAST      0x81u /* + level */
#define CMD_PRECHARGE     0xD9u /* + phase 2 and phase 1 periods */
#define CMD_VCOMH         0xDBu /* + level */
#define CMD_FOLLOW_RAM    0xA4u
#define CMD_NORMAL        0xA6u
#define CMD_PAGE          0xB0u /* | page */
#define CMD_COL_LOW       0x00u /* | low nibble of the column */
#define CMD_COL_HIGH      0x10u /* | high nibble */

#define CONTRAST 0x8Fu /* the reset value is 0x7F; 0x8F is the usual for an internal charge pump */

/* All of it core 1's alone. */
static uint8_t addr_used;
static unsigned rows;
static uint8_t fb[OLED_PAGES][OLED_W];
static uint8_t changed; /* a bit a page */
static uint8_t tx[1u + OLED_W];
static uint32_t errors;

static bool send(const uint8_t *buf, size_t len) {
    if (i2c_write_timeout_us(OLED_I2C, addr_used, buf, len, false, I2C_TIMEOUT_US) == (int)len) return true;
    errors++;
    return false;
}

static bool commands(const uint8_t *cmds, size_t n) {
    tx[0] = CTRL_COMMANDS;
    memcpy(&tx[1], cmds, n);
    return send(tx, n + 1u);
}

static bool answers(uint8_t a) {
    uint8_t status; /* a read in I2C mode returns the status register */
    return i2c_read_timeout_us(OLED_I2C, a, &status, 1, false, I2C_TIMEOUT_US) == 1;
}

static bool set_up(bool rotate) {
    const uint8_t cmds[] = {CMD_DISPLAY_OFF,
                            CMD_CLOCK_DIV,
                            0x80u, /* the reset value: divide by 1, middle frequency */
                            CMD_MULTIPLEX,
                            (uint8_t)(rows - 1u),
                            CMD_DISPLAY_SHIFT,
                            0x00u,
                            CMD_START_LINE | 0u,
                            CMD_CHARGE_PUMP,
                            0x14u,
                            CMD_ADDR_MODE,
                            0x02u,
                            CMD_STOP_SCROLL,
                            rotate ? CMD_SEG_NORMAL : CMD_SEG_REMAP, /* the modules' pins at the left, upright */
                            rotate ? CMD_COM_UP : CMD_COM_DOWN,
                            CMD_COM_PINS,
                            rows == 32u ? 0x02u : 0x12u,
                            CMD_CONTRAST,
                            CONTRAST,
                            CMD_PRECHARGE,
                            0xF1u,
                            CMD_VCOMH,
                            0x40u,
                            CMD_FOLLOW_RAM,
                            CMD_NORMAL};
    return commands(cmds, sizeof cmds);
}

bool oled_init(unsigned height, bool rotate, uint8_t *addr, uint32_t *clock_hz) {
    i2c_init(OLED_I2C, PROBE_HZ);
    gpio_set_function(OLED_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_PIN_SCL, GPIO_FUNC_I2C);
    /* The modules have their own pull-ups; these weak ones only keep the bus up when none is plugged in. */
    gpio_pull_up(OLED_PIN_SDA);
    gpio_pull_up(OLED_PIN_SCL);
    if (answers(0x3Cu)) {
        addr_used = 0x3Cu;
    } else if (answers(0x3Du)) {
        addr_used = 0x3Du;
    } else {
        i2c_deinit(OLED_I2C);
        gpio_set_function(OLED_PIN_SDA, GPIO_FUNC_SIO);
        gpio_set_function(OLED_PIN_SCL, GPIO_FUNC_SIO);
        return false;
    }
    rows = height == 32u ? 32u : 64u;
    uint32_t hz = i2c_set_baudrate(OLED_I2C, FAST_HZ);
    if (!set_up(rotate)) {
        hz = i2c_set_baudrate(OLED_I2C, FALLBACK_HZ);
        (void)set_up(rotate); /* counted in errors if it fails again; the display stays dark */
    }
    memset(fb, 0, sizeof fb);
    changed = 0xFFu; /* clear every page, 64 rows' worth: a 32-row set-up leaves the rest of its memory alone */
    unsigned shown = rows;
    rows = 64u;
    (void)oled_flush();
    rows = shown;
    const uint8_t on[] = {CMD_DISPLAY_ON};
    (void)commands(on, sizeof on);
    *addr = addr_used;
    *clock_hz = hz;
    return true;
}

typedef struct {
    unsigned w, h;
    const uint8_t *rows8; /* (ch - 0x20) * h + row: a byte a row, the leftmost pixel in bit 7 */
} font_t;

static const font_t fonts[] = {
    [OLED_FONT_7X13B] = {FONT_7X13B_WIDTH, FONT_7X13B_HEIGHT, &font_7x13b_glyphs[0][0]},
    [OLED_FONT_5X8] = {FONT_5X8_WIDTH, FONT_5X8_HEIGHT, &font_5x8_glyphs[0][0]},
    [OLED_FONT_4X6] = {FONT_4X6_WIDTH, FONT_4X6_HEIGHT, &font_4x6_glyphs[0][0]},
};

/* The drive lights' dots, 7 x 7, a byte a row, the leftmost pixel in bit 6. */
#define DOT_SIZE 7u
static const uint8_t dot_filled[DOT_SIZE] = {0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C};
static const uint8_t dot_hollow[DOT_SIZE] = {0x1C, 0x22, 0x41, 0x41, 0x41, 0x22, 0x1C};

unsigned oled_font_height(oled_font_t font) {
    return fonts[font].h;
}

static void put(unsigned x, unsigned y, bool on) {
    if (x >= OLED_W || y >= rows) return;
    uint8_t bit = (uint8_t)(1u << (y % 8u));
    uint8_t *b = &fb[y / 8u][x];
    uint8_t was = *b;
    *b = on ? (uint8_t)(was | bit) : (uint8_t)(was & ~bit);
    if (*b != was) changed |= (uint8_t)(1u << (y / 8u));
}

/* Whether a character's pixel (c, r) of its cell is lit. */
static bool ink(const font_t *f, char ch, unsigned c, unsigned r) {
    if (ch == OLED_DOT_FILLED || ch == OLED_DOT_HOLLOW) {
        /* a 7 x 7 dot in the middle of the cell, or as much of it as the cell holds */
        unsigned top = f->h > DOT_SIZE ? (f->h - DOT_SIZE) / 2u : 0u;
        if (r < top || r - top >= DOT_SIZE || c >= DOT_SIZE) return false;
        const uint8_t *dot = ch == OLED_DOT_FILLED ? dot_filled : dot_hollow;
        return (dot[r - top] & (0x40u >> c)) != 0;
    }
    if (ch == OLED_ELLIPSIS) { /* dots a column apart on the baseline (the bold font's two rows thick) */
        unsigned base = f->h >= 13u ? f->h - 3u : f->h - 2u; /* the row "A" stands on */
        return c % 2u == (f->w > 5u ? 1u : 0u) && (r == base || (f->h >= 13u && r == base - 1u));
    }
    unsigned code = (unsigned char)ch;
    if (code < 0x20u || code > 0x7Eu) code = '?';
    return (f->rows8[(size_t)(code - 0x20u) * f->h + r] & (0x80u >> c)) != 0;
}

void oled_text(unsigned y, oled_font_t font, const char *text, bool inverse) {
    const font_t *f = &fonts[font];
    unsigned cols = OLED_W / f->w;
    size_t n = strlen(text);
    for (unsigned r = 0; r < f->h; r++) {
        for (unsigned x = 0; x < OLED_W; x++) {
            unsigned k = x / f->w;
            bool on = k < cols && k < n && ink(f, text[k], x % f->w, r);
            put(x, y + r, on != inverse);
        }
    }
}

void oled_rule(unsigned y) {
    for (unsigned x = 0; x < OLED_W; x++)
        put(x, y, true);
}

bool oled_flush(void) {
    bool ok = true;
    for (unsigned p = 0; p < rows / 8u; p++) {
        if (!(changed & (1u << p))) continue;
        const uint8_t where[] = {(uint8_t)(CMD_PAGE | p), CMD_COL_LOW | 0u, CMD_COL_HIGH | 0u};
        ok = commands(where, sizeof where) && ok;
        tx[0] = CTRL_DATA;
        memcpy(&tx[1], fb[p], OLED_W);
        ok = send(tx, sizeof tx) && ok;
    }
    changed = 0;
    return ok;
}

uint32_t oled_errors(void) {
    return errors;
}
