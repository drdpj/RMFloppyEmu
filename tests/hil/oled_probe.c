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
 * Bring-up firmware for a small I2C OLED (128 x 64 or 128 x 32, SSD1306 or SH1106) on I2C1, GP10 SDA and GP11 SCL
 * (docs/hardware.md, "An OLED instead of the TFT"). Run by tests/hil/run_oled_probe.py. On the console:
 *   - OLED-SCAN: every 7-bit address that answers, at 100 kHz, and the status byte read from each (both datasheets:
 *     a read in I2C mode returns the status register; D6 is 1 while the display is off).
 *   - OLED-I2C: for each clock, 50 full frames written: how many weren't acknowledged, and the time a frame takes.
 *     The panel's memory can't be read back over I2C, so whether the bytes arrived intact is for the eye: the test
 *     screen is drawn at the fastest clock with no NAK.
 *   - OLED-STATE: the settings the screen was last drawn with. They are also on the screen.
 * Then keys on the console change the settings and redraw: h height 64/32, m controller (SSD1306, columns from 0;
 * SH1106, its 132-column memory shown from column 2), f turned 180 degrees, c contrast, i inverted, k the clock,
 * s scan again, z the next font (on a mock of the menu), l a layout (off, home, choosing: the font's rows over a 5x8
 * status line), t the test screen again.
 *
 * Both controllers are driven in page addressing mode, which both have: a page is 8 rows, a byte a column, the top
 * row in bit 0 (SSD1306 datasheet section 10.1.3; SH1106 datasheet, "Set page address"). The test screen: a
 * one-pixel border round the whole panel (a border missing on one side, or a band of noise, means the wrong height
 * or column offset), and text rows naming the settings.
 */
#include <stdio.h>
#include <string.h>

#include "pico/font_10x20.h"
#include "pico/font_4x6.h"
#include "pico/font_5x8.h"
#include "pico/font_6x12.h"
#include "pico/font_7x13b.h"
#include "pico/font_8x13b.h"
#include "pico/font_9x15b.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

/* docs/hardware.md, "Pin plan": GP10 and GP11 are I2C1's SDA and SCL */
#define PIN_SDA  10u
#define PIN_SCL  11u
#define OLED_I2C i2c1

#define OLED_W         128u
#define OLED_PAGES     8u /* at most: 64 rows */
#define CELL_W         6u /* the 5 x 8 font, a column apart */
#define TEXT_COLS      21u
#define SOAK_FRAMES    50u
#define I2C_TIMEOUT_US 20000u

/* The first byte of each I2C write says what follows (SSD1306 section 8.1.5.2; SH1106 the same): */
#define CTRL_COMMANDS 0x00u /* all commands */
#define CTRL_DATA     0x40u /* all display data */

/* Commands, SSD1306 datasheet section 9 (table 9-1); the SH1106's are the same unless marked. */
#define CMD_DISPLAY_OFF   0xAEu
#define CMD_DISPLAY_ON    0xAFu
#define CMD_CLOCK_DIV     0xD5u /* + divide ratio and oscillator frequency */
#define CMD_MULTIPLEX     0xA8u /* + rows - 1 */
#define CMD_DISPLAY_SHIFT 0xD3u /* + vertical offset */
#define CMD_START_LINE    0x40u /* | line */
#define CMD_CHARGE_PUMP   0x8Du /* SSD1306 only: + 0x14 on */
#define CMD_DCDC          0xADu /* SH1106 only: + 0x8B on */
#define CMD_ADDR_MODE     0x20u /* SSD1306 only: + 0x02 page addressing */
#define CMD_STOP_SCROLL   0x2Eu /* SSD1306 only */
#define CMD_SEG_NORMAL    0xA0u /* column 0 at the left */
#define CMD_SEG_REMAP     0xA1u /* column 0 at the right */
#define CMD_COM_UP        0xC0u /* COM0 first */
#define CMD_COM_DOWN      0xC8u /* COM0 last */
#define CMD_COM_PINS      0xDAu /* + 0x02 sequential (128 x 32 panels), 0x12 alternative (128 x 64) */
#define CMD_CONTRAST      0x81u /* + level */
#define CMD_PRECHARGE     0xD9u /* + phase 2 and phase 1 periods */
#define CMD_VCOMH         0xDBu /* + level */
#define CMD_FOLLOW_RAM    0xA4u
#define CMD_NORMAL        0xA6u
#define CMD_INVERSE       0xA7u
#define CMD_PAGE          0xB0u /* | page */
#define CMD_COL_LOW       0x00u /* | low nibble of the column */
#define CMD_COL_HIGH      0x10u /* | high nibble */

#define SH1106_COLUMN_OFFSET 2u /* its 132 columns of memory, the 128 in the middle shown */

typedef enum { CTRL_SSD1306 = 0, CTRL_SH1106 } controller_t;

static const uint32_t clocks_hz[] = {100000u, 400000u, 1000000u};
static const uint8_t contrasts[] = {0x01u, 0x40u, 0x8Fu, 0xFFu};

static struct {
    uint8_t addr;
    unsigned height; /* 64 or 32 */
    controller_t ctrl;
    bool flip, inverse;
    unsigned contrast; /* index into contrasts */
    unsigned clock;    /* index into clocks_hz */
    uint32_t clock_hz; /* as set */
} st = {0x3Cu, 64u, CTRL_SSD1306, false, false, 2u, 0u, 0u};

/* The fonts on trial (tools/font/pcf2c.py): up to 8 pixels wide a glyph row is a byte, wider a uint16_t. */
typedef struct {
    const char *name;
    unsigned w, h, scale;
    const uint8_t *rows8;   /* (ch - 0x20) * h + row, or NULL */
    const uint16_t *rows16; /* the same, for fonts wider than 8 */
} font_t;

static const font_t fonts[] = {
    {"5x8", FONT_5X8_WIDTH, FONT_5X8_HEIGHT, 1, &font_5x8_glyphs[0][0], NULL},
    {"6x12", FONT_6X12_WIDTH, FONT_6X12_HEIGHT, 1, &font_6x12_glyphs[0][0], NULL},
    {"7x13B", FONT_7X13B_WIDTH, FONT_7X13B_HEIGHT, 1, &font_7x13b_glyphs[0][0], NULL},
    {"8x13B", FONT_8X13B_WIDTH, FONT_8X13B_HEIGHT, 1, &font_8x13b_glyphs[0][0], NULL},
    {"9x15B", FONT_9X15B_WIDTH, FONT_9X15B_HEIGHT, 1, NULL, &font_9x15b_glyphs[0][0]},
    {"10x20", FONT_10X20_WIDTH, FONT_10X20_HEIGHT, 1, NULL, &font_10x20_glyphs[0][0]},
    {"5x8 x2", FONT_5X8_WIDTH, FONT_5X8_HEIGHT, 2, &font_5x8_glyphs[0][0], NULL},
};
/* The 128 x 32 panel's status line: the 6 rows left under two rows of 7x13 (not in the 'z' cycle). */
static const font_t font_status_32 = {"4x6", FONT_4X6_WIDTH, FONT_4X6_HEIGHT, 1, &font_4x6_glyphs[0][0], NULL};
#define FONT_COUNT (sizeof fonts / sizeof fonts[0])

/* What the font screen shows: the menu's kind of content (core/ui.h), cut to the columns that fit. */
static const char *const sample[] = {
    "",                    /* the font's name and size go here */
    "A: CPM22F-BASIC.IMD", /* home: each drive's image */
    "B: GAMES/ELITE.IMD",
    "A reading T12",            /* a drive's light */
    "> FLOWERS-OF-CRYSTAL.HFE", /* choosing: the cursor */
    "  DRAGON-WORLD.IMD",
    "  (eject)",
    "  ..",
};

static int font_shown = -1; /* -1: the test screen */

static uint8_t fb[OLED_PAGES][OLED_W];
static uint8_t tx[1u + OLED_W];

static bool send(const uint8_t *buf, size_t len) {
    return i2c_write_timeout_us(OLED_I2C, st.addr, buf, len, false, I2C_TIMEOUT_US) == (int)len;
}

static bool commands(const uint8_t *cmds, size_t n) {
    tx[0] = CTRL_COMMANDS;
    memcpy(&tx[1], cmds, n);
    return send(tx, n + 1u);
}

static bool set_up(void) {
    const uint8_t on_off[] = {CMD_DISPLAY_OFF,
                              CMD_CLOCK_DIV,
                              0x80u, /* the reset value: divide by 1, middle frequency */
                              CMD_MULTIPLEX,
                              (uint8_t)(st.height - 1u),
                              CMD_DISPLAY_SHIFT,
                              0x00u,
                              CMD_START_LINE | 0u,
                              st.flip ? CMD_SEG_NORMAL : CMD_SEG_REMAP,
                              st.flip ? CMD_COM_UP : CMD_COM_DOWN,
                              CMD_COM_PINS,
                              st.height == 32u ? 0x02u : 0x12u,
                              CMD_CONTRAST,
                              contrasts[st.contrast],
                              CMD_PRECHARGE,
                              0xF1u, /* the usual for an internal charge pump */
                              CMD_VCOMH,
                              0x40u,
                              CMD_FOLLOW_RAM,
                              st.inverse ? CMD_INVERSE : CMD_NORMAL};
    const uint8_t ssd1306[] = {CMD_CHARGE_PUMP, 0x14u, CMD_ADDR_MODE, 0x02u, CMD_STOP_SCROLL};
    const uint8_t sh1106[] = {CMD_DCDC, 0x8Bu};
    const uint8_t on[] = {CMD_DISPLAY_ON};
    bool ok = commands(on_off, sizeof on_off);
    ok = (st.ctrl == CTRL_SSD1306 ? commands(ssd1306, sizeof ssd1306) : commands(sh1106, sizeof sh1106)) && ok;
    return commands(on, sizeof on) && ok;
}

/* Writes the frame buffer's top height/8 pages. */
static bool show(void) {
    unsigned col = st.ctrl == CTRL_SH1106 ? SH1106_COLUMN_OFFSET : 0u;
    bool ok = true;
    for (unsigned p = 0; p < st.height / 8u; p++) {
        const uint8_t where[] = {(uint8_t)(CMD_PAGE | p), (uint8_t)(CMD_COL_LOW | (col & 0x0Fu)),
                                 (uint8_t)(CMD_COL_HIGH | (col >> 4))};
        ok = commands(where, sizeof where) && ok;
        tx[0] = CTRL_DATA;
        memcpy(&tx[1], fb[p], OLED_W);
        ok = send(tx, sizeof tx) && ok;
    }
    return ok;
}

/* Text in page `page`, from column `x` (pixels), OR'd into the frame buffer. */
static void text(unsigned page, unsigned x, const char *s) {
    for (; *s && x + FONT_5X8_WIDTH <= OLED_W; s++, x += CELL_W) {
        unsigned ch = (unsigned char)*s;
        if (ch < FONT_5X8_FIRST || ch > FONT_5X8_LAST) ch = '?';
        const uint8_t *g = font_5x8_glyphs[ch - FONT_5X8_FIRST];
        for (unsigned c = 0; c < FONT_5X8_WIDTH; c++) {
            uint8_t column = 0;
            for (unsigned r = 0; r < FONT_5X8_HEIGHT; r++) {
                if (g[r] & (0x80u >> c)) column |= (uint8_t)(1u << r);
            }
            fb[page][x + c] |= column;
        }
    }
}

static void draw_test_screen(void) {
    unsigned pages = st.height / 8u;
    char line[2u * TEXT_COLS]; /* text() stops at the panel's edge */
    memset(fb, 0, sizeof fb);
    snprintf(line, sizeof line, "RMFE OLED %ux%u", OLED_W, st.height);
    text(0, 1, line);
    snprintf(line, sizeof line, "%s %02X %luk%s", st.ctrl == CTRL_SH1106 ? "SH1106" : "SSD1306", st.addr,
             (unsigned long)(st.clock_hz / 1000u), st.flip ? " flip" : "");
    text(1, 1, line);
    if (pages == 8u) {
        text(2, 1, "ABCDEFGHIJKLMNOPQRSTU");
        text(3, 1, "abcdefghijklmnopqrstu");
        text(4, 1, "0123456789 !?/()[]<>");
        text(5, 1, "A: GAMES/ELITE.IMD");
        text(6, 1, "<- left    right ->");
        text(7, 1, "bottom row 8");
    } else {
        text(2, 1, "AZaz09 <- left right>");
        text(3, 1, "bottom row 4");
    }
    for (unsigned x = 0; x < OLED_W; x++) {
        fb[0][x] |= 0x01u;
        fb[pages - 1u][x] |= 0x80u;
    }
    for (unsigned p = 0; p < pages; p++) {
        fb[p][0] = 0xFFu;
        fb[p][OLED_W - 1u] = 0xFFu;
    }
}

static void pixel(unsigned x, unsigned y) {
    if (x < OLED_W && y < OLED_PAGES * 8u) fb[y / 8u][x] |= (uint8_t)(1u << (y % 8u));
}

/* A string in font f, its top left at (x, y) pixels; characters that don't fit whole are left out. */
static void text_font(const font_t *f, unsigned x, unsigned y, const char *s) {
    unsigned cw = f->w * f->scale, ch_h = f->h * f->scale;
    for (; *s && x + cw <= OLED_W && y + ch_h <= st.height; s++, x += cw) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20u || ch > 0x7Eu) ch = '?';
        for (unsigned r = 0; r < f->h; r++) {
            size_t at = (size_t)(ch - 0x20u) * f->h + r;
            uint16_t row = f->rows8 ? (uint16_t)(f->rows8[at] << 8) : f->rows16[at];
            for (unsigned c = 0; c < f->w; c++) {
                if (!(row & (0x8000u >> c))) continue;
                for (unsigned dy = 0; dy < f->scale; dy++) {
                    for (unsigned dx = 0; dx < f->scale; dx++)
                        pixel(x + c * f->scale + dx, y + r * f->scale + dy);
                }
            }
        }
    }
}

/* The layouts on trial ('l'): rows in the main font over a rule and a status line in 5x8, the menu's home and
 * choosing screens (core/ui.h) with what would be on each. */
typedef enum { LAYOUT_OFF = 0, LAYOUT_HOME, LAYOUT_CHOOSING, LAYOUT_COUNT } layout_t;

static const char *const layout_rows[LAYOUT_COUNT][4] = {
    [LAYOUT_HOME] = {"A:", " CPM22F-BASIC.IMD", "B:", " GAMES/ELITE.IMD"},
    [LAYOUT_CHOOSING] = {"Image for A:", "> FLOWERS-OF-CRYSTAL.HFE", "  DRAGON-WORLD.IMD", "  (eject)"},
};
static const char *const layout_status[LAYOUT_COUNT] = {
    [LAYOUT_HOME] = "A read T12   B idle",
    [LAYOUT_CHOOSING] = "GAMES/       3 of 12",
};
static layout_t layout;

#define STATUS_H 10u /* the rule, a blank row, and 5x8 */

/* A drive's activity dot, 7 x 7: filled while it is in use, hollow while idle. One row a byte, leftmost in bit 6. */
#define DOT_SIZE 7u
static const uint8_t dot_filled[DOT_SIZE] = {0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C};
static const uint8_t dot_hollow[DOT_SIZE] = {0x1C, 0x22, 0x41, 0x41, 0x41, 0x22, 0x1C};

static void dot(unsigned x, unsigned y, bool active) {
    const uint8_t *rows = active ? dot_filled : dot_hollow;
    for (unsigned r = 0; r < DOT_SIZE; r++) {
        for (unsigned c = 0; c < DOT_SIZE; c++) {
            if (rows[r] & (0x40u >> c)) pixel(x + c, y + r);
        }
    }
}

/* On a 128 x 32 panel two rows of the main font fit, and the status line in 4x6 in the 6 rows left, with no rule: each
 * drive on a row, its dot after the letter; while choosing, the cursor's row and the next. */
static const char *const layout_rows_32[LAYOUT_COUNT][2] = {
    [LAYOUT_HOME] = {"A  CPM22F-BASIC.IMD", "B  GAMES/ELITE.IMD"},
    [LAYOUT_CHOOSING] = {"> FLOWERS-OF-CRYSTAL.HFE", "  DRAGON-WORLD.IMD"},
};

static void draw_layout_screen_32(const font_t *f) {
    unsigned line_h = f->h * f->scale, cw = f->w * f->scale;
    memset(fb, 0, sizeof fb);
    for (unsigned i = 0; i < 2u; i++) {
        text_font(f, 0, i * line_h, layout_rows_32[layout][i]);
    }
    if (2u * line_h + font_status_32.h <= st.height) {
        text_font(&font_status_32, 0, st.height - font_status_32.h, layout_status[layout]);
    }
    if (layout == LAYOUT_HOME) { /* in the letter's next cell, in the middle of the row: A in use, B idle */
        unsigned y = (line_h - DOT_SIZE) / 2u;
        dot(cw, y, true);
        dot(cw, line_h + y, false);
    }
}

static void draw_layout_screen(const font_t *f) {
    unsigned line_h = f->h * f->scale, rows = (st.height - STATUS_H) / line_h;
    if (st.height == 32u) {
        draw_layout_screen_32(f);
        return;
    }
    memset(fb, 0, sizeof fb);
    for (unsigned i = 0; i < rows && i < 4u; i++)
        text_font(f, 0, i * line_h, layout_rows[layout][i]);
    for (unsigned x = 0; x < OLED_W; x++)
        pixel(x, st.height - STATUS_H);
    text_font(&fonts[0], 0, st.height - FONT_5X8_HEIGHT, layout_status[layout]);
    if (layout == LAYOUT_HOME) { /* after "A:" and "B:", in the middle of the row: A in use, B idle */
        unsigned x = 3u * f->w * f->scale, y = (line_h - DOT_SIZE) / 2u;
        dot(x, y, true);
        dot(x, 2u * line_h + y, false);
    }
}

static void draw_font_screen(const font_t *f) {
    unsigned line_h = f->h * f->scale, rows = st.height / line_h, cols = OLED_W / (f->w * f->scale);
    char line[48];
    if (layout != LAYOUT_OFF) {
        draw_layout_screen(f);
        return;
    }
    memset(fb, 0, sizeof fb);
    snprintf(line, sizeof line, "%s %ux%u", f->name, cols, rows);
    for (unsigned i = 0; i < rows && i < sizeof sample / sizeof sample[0]; i++) {
        text_font(f, 0, i * line_h, i ? sample[i] : line);
    }
}

static void use_clock(unsigned i) {
    st.clock = i;
    st.clock_hz = i2c_set_baudrate(OLED_I2C, clocks_hz[i]);
}

/* A scan by bit-banging, either way round, for when the I2C block finds nothing: it tells SDA and SCL swapped from
 * nothing there. Open drain: a line is driven low, or let go to its pull-up. About 50 kHz. */
static void bb_line(unsigned pin, bool high) {
    gpio_set_dir(pin, high ? GPIO_IN : GPIO_OUT);
    busy_wait_us_32(10);
}

static bool bb_address_acked(unsigned sda, unsigned scl, uint8_t addr) {
    bb_line(sda, true);
    bb_line(scl, true);
    bb_line(sda, false); /* start */
    bb_line(scl, false);
    uint8_t byte = (uint8_t)(addr << 1); /* write */
    for (unsigned i = 0; i < 8u; i++, byte = (uint8_t)(byte << 1)) {
        bb_line(sda, (byte & 0x80u) != 0);
        bb_line(scl, true);
        bb_line(scl, false);
    }
    bb_line(sda, true);
    bb_line(scl, true);
    bool acked = !gpio_get(sda);
    bb_line(scl, false);
    bb_line(sda, false); /* stop */
    bb_line(scl, true);
    bb_line(sda, true);
    return acked;
}

static void scan_bitbang(void) {
    const unsigned ways[2][2] = {{PIN_SDA, PIN_SCL}, {PIN_SCL, PIN_SDA}};
    for (unsigned w = 0; w < 2u; w++) {
        unsigned sda = ways[w][0], scl = ways[w][1];
        printf("OLED-SCAN bitbang sda=GP%u scl=GP%u:", sda, scl);
        gpio_set_function(sda, GPIO_FUNC_SIO);
        gpio_set_function(scl, GPIO_FUNC_SIO);
        gpio_put(sda, false);
        gpio_put(scl, false);
        bb_line(sda, true);
        bb_line(scl, true);
        unsigned found = 0;
        for (uint8_t a = 0x08u; a < 0x78u; a++) {
            if (bb_address_acked(sda, scl, a)) {
                printf(" %02x", a);
                found++;
            }
        }
        printf(found ? "%s\n" : " nothing%s\n", w ? " (SDA and SCL swapped)" : "");
    }
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
}

static void scan(void) {
    use_clock(0);
    unsigned found = 0;
    uint8_t first = 0;
    printf("OLED-SCAN");
    for (uint8_t a = 0x08u; a < 0x78u; a++) { /* 0x00-0x07 and 0x78-0x7F are reserved (I2C specification) */
        uint8_t status;
        if (i2c_read_timeout_us(OLED_I2C, a, &status, 1, false, I2C_TIMEOUT_US) == 1) {
            printf(" %02x:status=%02x", a, status);
            if (!found || a == 0x3Cu) first = a;
            found++;
        }
    }
    printf(found ? "\n" : " nothing\n");
    if (found) {
        st.addr = first;
    } else {
        scan_bitbang();
    }
}

static void report(bool ok) {
    printf(
        "OLED-STATE addr=%02x height=%u controller=%s flip=%u contrast=%02x inverse=%u clock_khz=%lu screen=%s%s %s\n",
        st.addr, st.height, st.ctrl == CTRL_SH1106 ? "sh1106" : "ssd1306", st.flip, contrasts[st.contrast], st.inverse,
        (unsigned long)(st.clock_hz / 1000u), font_shown < 0 ? "test" : fonts[font_shown].name,
        layout == LAYOUT_HOME       ? "+home"
        : layout == LAYOUT_CHOOSING ? "+choosing"
                                    : "",
        ok ? "acked" : "NAK");
}

static void redraw(void) {
    bool ok = set_up();
    if (font_shown < 0) {
        draw_test_screen();
    } else {
        draw_font_screen(&fonts[font_shown]);
    }
    ok = show() && ok;
    report(ok);
}

/* SOAK_FRAMES full frames at each clock: NAKs and the time per frame. Leaves the fastest clock with none in use. */
static void soak(void) {
    int best = -1;
    for (unsigned i = 0; i < sizeof clocks_hz / sizeof clocks_hz[0]; i++) {
        use_clock(i);
        unsigned bad = 0;
        uint32_t t0 = time_us_32();
        for (unsigned f = 0; f < SOAK_FRAMES; f++) {
            for (unsigned p = 0; p < OLED_PAGES; p++) {
                for (unsigned x = 0; x < OLED_W; x++)
                    fb[p][x] = (uint8_t)(((x + f) & 8u) ? 0xAAu : 0x55u);
            }
            if (!show()) bad++;
        }
        uint32_t frame_us = (time_us_32() - t0) / SOAK_FRAMES;
        printf("OLED-I2C khz=%lu frames=%u nak=%u frame_us=%lu\n", (unsigned long)(st.clock_hz / 1000u), SOAK_FRAMES,
               bad, (unsigned long)frame_us);
        if (!bad) best = (int)i;
    }
    use_clock(best < 0 ? 0u : (unsigned)best);
    printf("OLED-I2C use_khz=%lu%s\n", (unsigned long)(st.clock_hz / 1000u), best < 0 ? " (none clean)" : "");
}

int main(void) {
    stdio_init_all();
    sleep_ms(200);
    printf("\nOLED-PROBE begin\n");

    i2c_init(OLED_I2C, clocks_hz[0]);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    /* The modules have their own pull-ups; these weak ones only keep the bus up when none is plugged in. */
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);

    scan();
    redraw();
    soak();
    redraw();
    printf("OLED-READY\n");

    for (;;) {
        int c = getchar_timeout_us(100000);
        switch (c) {
            case 'h':
                st.height = st.height == 64u ? 32u : 64u;
                break;
            case 'm':
                st.ctrl = st.ctrl == CTRL_SSD1306 ? CTRL_SH1106 : CTRL_SSD1306;
                break;
            case 'f':
                st.flip = !st.flip;
                break;
            case 'c':
                st.contrast = (st.contrast + 1u) % (sizeof contrasts);
                break;
            case 'i':
                st.inverse = !st.inverse;
                break;
            case 'k':
                use_clock((st.clock + 1u) % (sizeof clocks_hz / sizeof clocks_hz[0]));
                break;
            case 's':
                scan();
                break;
            case 'z':
                font_shown = (font_shown + 1) % (int)FONT_COUNT;
                break;
            case 't':
                font_shown = -1;
                break;
            case 'l':
                layout = (layout_t)((layout + 1) % LAYOUT_COUNT);
                if (font_shown < 0) font_shown = 0;
                break;
            default:
                continue;
        }
        /* A 64-row frame on a 32-row panel would leave stale pages, and the other way round: clear all first. */
        memset(fb, 0, sizeof fb);
        unsigned h = st.height;
        st.height = 64u;
        (void)show(); /* only clearing: report() says whether the redraw was acknowledged */
        st.height = h;
        redraw();
    }
}
