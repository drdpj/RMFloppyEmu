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
 * Bring-up firmware for the TFT and the rotary encoder (docs/plan.md, milestone 6, step 5): checks the wiring in
 * docs/hardware.md ("Wiring the display and encoder") before the real driver is written. On the console
 * (tests/hil/run_display_probe.py reads it):
 *   - DISP-ID: the controller's answers over MISO: RDDID (04), ILI9341's ID4 (D3, read through its D9 index
 *     register), and the power mode (RDDPM, 0A) after reset and after the display is switched on. ILI9341 and
 *     ST7789 datasheets: RDDPM is 08 after a hardware reset and 9C with the booster on, out of sleep, normal mode
 *     and the display on. ST7789's RDDID is 85 85 52; ILI9341's ID4 is 00 93 41.
 *   - DISP-SPI: for each SPI clock, the time to fill the screen by DMA, and whether a pattern written at that clock
 *     reads back intact from the panel's memory (RAMRD, 2E) at a slow clock. Reads return 18-bit colour, a dummy
 *     byte or bit first; the decoder tries each alignment and colour order rather than assume one.
 *   - DISP-BL: the backlight dimmed and brought back by PWM.
 *   - ENC: every encoder step and press, as the knob is used. A step is counted when the encoder comes back to its
 *     rest state (both contacts open) having moved at least three of the four quadrature states one way.
 * On the screen, in landscape: a title, the controller found, eight labelled colour bars, and a status line that
 * shows the brightness, knob position and presses, redrawn as they change. The knob sets the brightness.
 */
#include <stdio.h>
#include <string.h>

#include "pico/font_8x13.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

/* docs/hardware.md, "Pin plan" */
#define PIN_ENC_A     2
#define PIN_ENC_B     3
#define PIN_TFT_MISO  12
#define PIN_TFT_CS    13
#define PIN_TFT_SCK   14
#define PIN_TFT_MOSI  15
#define PIN_TFT_DC    20
#define PIN_TFT_RESET 21
#define PIN_TFT_LED   22
#define PIN_ENC_SW    26
#define TFT_SPI       spi1

#define SCREEN_W 320u /* landscape */
#define SCREEN_H 240u

/* ILI9341 / ST7789 commands (both datasheets, section 8: the system command set is common to the two) */
#define CMD_SWRESET          0x01
#define CMD_RDDID            0x04
#define CMD_RDDPM            0x0A
#define CMD_SLPOUT           0x11
#define CMD_NORON            0x13
#define CMD_INVON            0x21
#define CMD_DISPON           0x29
#define CMD_CASET            0x2A
#define CMD_PASET            0x2B
#define CMD_RAMWR            0x2C
#define CMD_RAMRD            0x2E
#define CMD_MADCTL           0x36
#define CMD_COLMOD           0x3A
#define CMD_ILI_RDID4        0xD3 /* ILI9341 only */
#define CMD_ILI_SPI_INDEX    0xD9 /* ILI9341, undocumented: selects which byte of the next read answers over SPI */
#define COLMOD_16BIT         0x55 /* RGB565 */
#define MADCTL_ILI_LANDSCAPE 0x28 /* MV | BGR: the ILI9341 panel's colour filter is BGR */
#define MADCTL_ST_LANDSCAPE  0x60 /* MX | MV */
#define RDDPM_ON_BITS        0x14 /* sleep out (bit 4) and display on (bit 2) */

#define READ_HZ     4000000u /* ILI9341: serial read cycle at least 150 ns */
#define BL_PWM_WRAP 999u     /* 0.1 % steps */
#define BL_PWM_HZ   5000u

typedef enum { CTRL_UNKNOWN, CTRL_ILI9341, CTRL_ST7789 } ctrl_t;

static int dma_chan;
static uint16_t row_buf[SCREEN_W];

/* ------------------------------------------------------------------ SPI and commands */

static void tft_select(bool on) {
    gpio_put(PIN_TFT_CS, !on);
}

static void tft_cmd(uint8_t cmd, const uint8_t *data, size_t n) {
    tft_select(true);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(TFT_SPI, &cmd, 1);
    gpio_put(PIN_TFT_DC, 1);
    if (n) spi_write_blocking(TFT_SPI, data, n);
    tft_select(false);
}

static void tft_cmd1(uint8_t cmd, uint8_t arg) {
    tft_cmd(cmd, &arg, 1);
}

/* Sends cmd and reads n bytes at READ_HZ. Restores the write clock. */
static void tft_read(uint8_t cmd, uint8_t *out, size_t n, uint32_t write_hz) {
    spi_set_baudrate(TFT_SPI, READ_HZ);
    tft_select(true);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(TFT_SPI, &cmd, 1);
    gpio_put(PIN_TFT_DC, 1);
    spi_read_blocking(TFT_SPI, 0x00, out, n);
    tft_select(false);
    spi_set_baudrate(TFT_SPI, write_hz);
}

static void spi_16bit(bool on) {
    spi_set_format(TFT_SPI, on ? 16 : 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

/* After a transmit-only DMA the receive FIFO has overflowed: empty it and clear the overrun. */
static void spi_drain_rx(void) {
    while (spi_is_busy(TFT_SPI))
        tight_loop_contents();
    while (spi_is_readable(TFT_SPI))
        (void)spi_get_hw(TFT_SPI)->dr;
    spi_get_hw(TFT_SPI)->icr = SPI_SSPICR_RORIC_BITS;
}

static void tft_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t ram_cmd) {
    uint16_t x1 = (uint16_t)(x + w - 1), y1 = (uint16_t)(y + h - 1);
    const uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t pa[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    tft_cmd(CMD_CASET, ca, sizeof ca);
    tft_cmd(CMD_PASET, pa, sizeof pa);
    tft_select(true);
    gpio_put(PIN_TFT_DC, 0);
    spi_write_blocking(TFT_SPI, &ram_cmd, 1);
    gpio_put(PIN_TFT_DC, 1);
    /* left selected: the pixels follow */
}

static void tft_fill(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour) {
    static uint16_t fill_colour;
    fill_colour = colour;
    tft_window(x, y, w, h, CMD_RAMWR);
    spi_16bit(true);
    dma_channel_config c = dma_channel_get_default_config((uint)dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(TFT_SPI, true));
    dma_channel_configure((uint)dma_chan, &c, &spi_get_hw(TFT_SPI)->dr, &fill_colour, (uint32_t)w * h, true);
    dma_channel_wait_for_finish_blocking((uint)dma_chan);
    spi_drain_rx();
    spi_16bit(false);
    tft_select(false);
}

/* Text, a pixel row at a time: no frame buffer. */
static void tft_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg, unsigned scale) {
    size_t len = strlen(s);
    if (len * FONT_8X13_WIDTH * scale > SCREEN_W - x) len = (SCREEN_W - x) / (FONT_8X13_WIDTH * scale);
    if (!len) return;
    uint16_t w = (uint16_t)(len * FONT_8X13_WIDTH * scale), h = (uint16_t)(FONT_8X13_HEIGHT * scale);
    tft_window(x, y, w, h, CMD_RAMWR);
    spi_16bit(true);
    for (unsigned r = 0; r < h; r++) {
        unsigned gr = r / scale;
        for (size_t i = 0; i < len; i++) {
            char ch = s[i];
            uint8_t bits = (ch >= FONT_8X13_FIRST && ch <= FONT_8X13_LAST) ? font_8x13_glyphs[ch - FONT_8X13_FIRST][gr]
                                                                           : 0xFF; /* a block: not in the font */
            for (unsigned px = 0; px < FONT_8X13_WIDTH * scale; px++)
                row_buf[i * FONT_8X13_WIDTH * scale + px] = (bits & (0x80u >> (px / scale))) ? fg : bg;
        }
        spi_write16_blocking(TFT_SPI, row_buf, w);
    }
    spi_drain_rx();
    spi_16bit(false);
    tft_select(false);
}

/* ------------------------------------------------------------------ the controller */

static void tft_hard_reset(void) {
    gpio_put(PIN_TFT_RESET, 1);
    sleep_ms(5);
    gpio_put(PIN_TFT_RESET, 0);
    sleep_ms(1); /* at least 10 us */
    gpio_put(PIN_TFT_RESET, 1);
    sleep_ms(120); /* both: 120 ms before SLPOUT */
}

static void print_hex(const char *name, const uint8_t *b, size_t n) {
    printf(" %s=", name);
    for (size_t i = 0; i < n; i++)
        printf("%s%02x", i ? ":" : "", b[i]);
}

/* An 8-bit register read: some controllers put a dummy bit first, so both readings are given. */
static uint8_t reg8(const uint8_t two[2], bool dummy_bit) {
    return dummy_bit ? (uint8_t)((two[0] << 1) | (two[1] >> 7)) : two[0];
}

static ctrl_t tft_identify_and_init(uint32_t write_hz) {
    uint8_t rddid[4], id4[3], pm_reset[2], pm_on[2];
    tft_read(CMD_RDDPM, pm_reset, sizeof pm_reset, write_hz);
    tft_read(CMD_RDDID, rddid, sizeof rddid, write_hz);
    for (unsigned i = 0; i < 3; i++) {
        tft_cmd1(CMD_ILI_SPI_INDEX, (uint8_t)(0x10 + i + 1));
        tft_read(CMD_ILI_RDID4, &id4[i], 1, write_hz);
    }
    /* RDDID: one dummy bit, then 24 bits */
    uint32_t w = (uint32_t)rddid[0] << 24 | (uint32_t)rddid[1] << 16 | (uint32_t)rddid[2] << 8 | rddid[3];
    uint32_t id24 = (w >> 7) & 0xFFFFFFu;
    ctrl_t ctrl = CTRL_UNKNOWN;
    if (id24 == 0x858552u) {
        ctrl = CTRL_ST7789;
    } else if (id4[1] == 0x93 && id4[2] == 0x41) {
        ctrl = CTRL_ILI9341;
    }

    tft_cmd(CMD_SWRESET, NULL, 0);
    sleep_ms(150);
    tft_cmd(CMD_SLPOUT, NULL, 0);
    sleep_ms(120);
    tft_cmd1(CMD_COLMOD, COLMOD_16BIT);
    tft_cmd1(CMD_MADCTL, ctrl == CTRL_ST7789 ? MADCTL_ST_LANDSCAPE : MADCTL_ILI_LANDSCAPE);
    if (ctrl == CTRL_ST7789) tft_cmd(CMD_INVON, NULL, 0); /* ST7789 panels are IPS, which want inversion */
    tft_cmd(CMD_NORON, NULL, 0);
    tft_cmd(CMD_DISPON, NULL, 0);
    sleep_ms(20);
    tft_read(CMD_RDDPM, pm_on, sizeof pm_on, write_hz);

    printf("DISP-ID");
    print_hex("rddid", rddid, sizeof rddid);
    printf(" rddid24=%06lx", (unsigned long)id24);
    print_hex("id4", id4, sizeof id4);
    print_hex("rddpm_reset", pm_reset, sizeof pm_reset);
    print_hex("rddpm_on", pm_on, sizeof pm_on);
    /* The power mode answers only if MISO works: it must change from reset to on, in one reading or the other. */
    bool answered = false;
    for (int d = 0; d < 2; d++) {
        uint8_t a = reg8(pm_reset, d), b = reg8(pm_on, d);
        if ((a & RDDPM_ON_BITS) == 0 && (b & RDDPM_ON_BITS) == RDDPM_ON_BITS) answered = true;
    }
    printf(" miso=%s controller=%s\n", answered ? "ok" : "NO-ANSWER",
           ctrl == CTRL_ILI9341  ? "ILI9341"
           : ctrl == CTRL_ST7789 ? "ST7789"
                                 : "unknown");
    return ctrl;
}

/* ------------------------------------------------------------------ write-path check */

#define RB_W 32
#define RB_H 4
#define RB_N (RB_W * RB_H)
static uint16_t rb_pattern[RB_N];
static uint8_t rb_raw[2 + RB_N * 3 + 2];

static uint32_t bits_at(const uint8_t *b, size_t nbytes, size_t bitpos, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t p = bitpos + i;
        unsigned bit = p / 8 < nbytes ? (b[p / 8] >> (7 - p % 8)) & 1u : 0u;
        v = (v << 1) | bit;
    }
    return v;
}

typedef enum { FMT_RGB666, FMT_BGR666, FMT_RGB565, FMT_COUNT } rb_fmt_t;
static const char *const fmt_names[FMT_COUNT] = {"rgb666", "bgr666", "rgb565"};

/* How many of the n pixels in `pattern` the read-back data `raw` gets wrong, starting at bit `shift`, in format
   `fmt`; stops counting at `stop`. */
static unsigned rb_mismatches(const uint16_t *pattern, unsigned n, const uint8_t *raw, size_t raw_len, size_t shift,
                              rb_fmt_t fmt, unsigned stop) {
    unsigned bpp = fmt == FMT_RGB565 ? 16 : 24, bad = 0;
    for (unsigned i = 0; i < n && bad < stop; i++) {
        uint16_t want = pattern[i];
        unsigned r5 = want >> 11, g6 = (want >> 5) & 0x3F, b5 = want & 0x1F;
        size_t at = shift + (size_t)i * bpp;
        if (fmt == FMT_RGB565) {
            if (bits_at(raw, raw_len, at, 16) != want) bad++;
            continue;
        }
        unsigned c0 = bits_at(raw, raw_len, at, 8), c1 = bits_at(raw, raw_len, at + 8, 8),
                 c2 = bits_at(raw, raw_len, at + 16, 8);
        unsigned r = fmt == FMT_RGB666 ? c0 : c2, b = fmt == FMT_RGB666 ? c2 : c0;
        if ((r >> 3) != r5 || (c1 >> 2) != g6 || (b >> 3) != b5) bad++;
    }
    return bad;
}

static void lcg_fill(uint16_t *out, unsigned n, uint32_t seed) {
    uint32_t x = seed;
    for (unsigned i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u; /* Numerical Recipes LCG */
        out[i] = (uint16_t)(x >> 16);
    }
}

/* Writes n pixels into the window at the current clock; then reads raw_len bytes of it back at READ_HZ. */
static void rb_write_read(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pattern, uint8_t *raw,
                          size_t raw_len, uint32_t write_hz) {
    tft_window(x, y, w, h, CMD_RAMWR);
    spi_16bit(true);
    spi_write16_blocking(TFT_SPI, pattern, (size_t)w * h);
    spi_drain_rx();
    spi_16bit(false);
    tft_select(false);

    spi_set_baudrate(TFT_SPI, READ_HZ);
    tft_window(x, y, w, h, CMD_RAMRD);
    spi_read_blocking(TFT_SPI, 0x00, raw, raw_len);
    tft_select(false);
    spi_set_baudrate(TFT_SPI, write_hz);
}

/* Writes a pattern at the current clock, reads it back slowly. Returns true and the alignment if it matches. */
static bool rb_check(uint32_t write_hz, uint32_t seed, size_t *shift_out, rb_fmt_t *fmt_out) {
    lcg_fill(rb_pattern, RB_N, seed);
    rb_write_read(0, 0, RB_W, RB_H, rb_pattern, rb_raw, sizeof rb_raw, write_hz);
    for (size_t shift = 0; shift <= 16; shift++) {
        for (int f = 0; f < FMT_COUNT; f++) {
            if (!rb_mismatches(rb_pattern, RB_N, rb_raw, sizeof rb_raw, shift, (rb_fmt_t)f, 1)) {
                *shift_out = shift;
                *fmt_out = (rb_fmt_t)f;
                return true;
            }
        }
    }
    return false;
}

/* The soak: full-width strips, a text line high, at random rows, as the status line is redrawn. Each strip sends
   its own window commands, so a garbled command shows up as well as garbled pixels. */
#define SOAK_STRIPS 100u
#define SOAK_H      FONT_8X13_HEIGHT
#define SOAK_N      (SCREEN_W * SOAK_H)
static uint16_t soak_pattern[SOAK_N];
static uint8_t soak_raw[2 + SOAK_N * 3 + 2];

/* Strips (of SOAK_STRIPS) with any pixel wrong, read with the alignment rb_check found. */
static unsigned soak(uint32_t write_hz, size_t shift, rb_fmt_t fmt, unsigned *bad_pixels) {
    unsigned bad_strips = 0;
    *bad_pixels = 0;
    for (uint32_t i = 0; i < SOAK_STRIPS; i++) {
        lcg_fill(soak_pattern, SOAK_N, 0x2545F491u * (i + 1));
        uint16_t y = (uint16_t)((soak_pattern[0] % (SCREEN_H - SOAK_H)));
        rb_write_read(0, y, SCREEN_W, SOAK_H, soak_pattern, soak_raw, sizeof soak_raw, write_hz);
        unsigned bad = rb_mismatches(soak_pattern, SOAK_N, soak_raw, sizeof soak_raw, shift, fmt, SOAK_N);
        if (bad) bad_strips++;
        *bad_pixels += bad;
    }
    return bad_strips;
}

/* ------------------------------------------------------------------ encoder (polled every millisecond) */

typedef enum { EV_PLUS = 1, EV_MINUS, EV_PRESS, EV_RELEASE } enc_event_t;

/* Written by the timer interrupt (head, ring) and read by the main loop (tail): single producer, single consumer. */
static volatile uint8_t ev_ring[64];
static volatile uint32_t ev_head, ev_tail;
/* Written by the timer interrupt only; the main loop reads them for reports. */
static volatile uint32_t enc_transitions, enc_invalid;

static void ev_push(enc_event_t e) {
    uint32_t h = ev_head;
    if (h - ev_tail >= sizeof ev_ring) return; /* full: drop (the main loop is far faster than a hand) */
    ev_ring[h % sizeof ev_ring] = (uint8_t)e;
    __compiler_memory_barrier();
    ev_head = h + 1;
}

static bool enc_poll(repeating_timer_t *t) {
    (void)t;
    /* Quadrature: +1 or -1 for each move to a neighbouring state, 0 for none; both contacts changing at once is a
       missed state, counted as invalid. States are (A << 1) | B, 1 = open (pulled up). */
    static const int8_t delta[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    static unsigned prev = 3;
    static int acc;
    static unsigned sw_stable = 1, sw_count;
    unsigned ab = (gpio_get(PIN_ENC_A) ? 2u : 0u) | (gpio_get(PIN_ENC_B) ? 1u : 0u);
    if (ab != prev) {
        if ((ab ^ prev) == 3) {
            enc_invalid++;
        } else {
            enc_transitions++;
            acc += delta[(prev << 2) | ab];
        }
        prev = ab;
        if (ab == 3) { /* rest: both open */
            /* Clockwise is B changing first on the EC11 as wired (docs/hardware.md), which this table counts as
               negative: measured with run_display_probe.py. */
            if (acc <= -3) ev_push(EV_PLUS);
            if (acc >= 3) ev_push(EV_MINUS);
            acc = 0;
        }
    }
    /* Switch: a change counts once it has held for 10 samples (10 ms). */
    unsigned sw = gpio_get(PIN_ENC_SW) ? 1u : 0u;
    if (sw == sw_stable) {
        sw_count = 0;
    } else if (++sw_count >= 10) {
        sw_stable = sw;
        sw_count = 0;
        ev_push(sw ? EV_RELEASE : EV_PRESS);
    }
    return true;
}

/* ------------------------------------------------------------------ backlight */

static void bl_set(unsigned percent) {
    pwm_set_gpio_level(PIN_TFT_LED, (uint16_t)(percent * (BL_PWM_WRAP + 1) / 100u));
}

/* ------------------------------------------------------------------ the screen */

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))
#define C_WHITE         RGB565(255, 255, 255)
#define C_BLACK         RGB565(0, 0, 0)
#define C_GREY          RGB565(64, 64, 64)

static void draw_status(unsigned bl, int pos, unsigned presses) {
    char line[48];
    snprintf(line, sizeof line, "BL %3u%%  knob %+4d  presses %u   ", bl, pos, presses);
    tft_text(8, SCREEN_H - 20, line, C_WHITE, C_BLACK, 1);
}

static void draw_screen(ctrl_t ctrl, uint32_t write_hz) {
    static const struct {
        const char *name;
        uint16_t colour;
    } bars[8] = {
        {"WHITE", RGB565(255, 255, 255)}, {"YELLW", RGB565(255, 255, 0)}, {"CYAN", RGB565(0, 255, 255)},
        {"GREEN", RGB565(0, 255, 0)},     {"MAGNT", RGB565(255, 0, 255)}, {"RED", RGB565(255, 0, 0)},
        {"BLUE", RGB565(0, 0, 255)},      {"BLACK", RGB565(0, 0, 0)},
    };
    tft_fill(0, 0, SCREEN_W, SCREEN_H, C_BLACK);
    tft_text(8, 4, "RMFE display probe", C_WHITE, C_BLACK, 2);
    char line[48];
    snprintf(line, sizeof line, "%s, SPI %lu.%02lu MHz",
             ctrl == CTRL_ILI9341  ? "ILI9341"
             : ctrl == CTRL_ST7789 ? "ST7789"
                                   : "controller unknown",
             (unsigned long)(write_hz / 1000000u), (unsigned long)(write_hz % 1000000u / 10000u));
    tft_text(8, 36, line, C_WHITE, C_BLACK, 1);
    for (unsigned i = 0; i < 8; i++) {
        uint16_t x = (uint16_t)(i * 40);
        tft_fill(x, 56, 40, 140, bars[i].colour);
        tft_text((uint16_t)(x + (40 - strlen(bars[i].name) * 8) / 2), 200, bars[i].name, C_WHITE, C_GREY, 1);
    }
    /* A marker in the top-right corner: shows which way round the panel is, and that x reaches 319. */
    tft_fill(SCREEN_W - 12, 0, 12, 12, RGB565(255, 0, 0));
}

/* ------------------------------------------------------------------ main */

int main(void) {
    stdio_init_all();
    sleep_ms(200);
    printf("\nDISP-PROBE begin\n");

    /* Backlight off until the screen is drawn: it also shows the switch works. */
    gpio_set_function(PIN_TFT_LED, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_TFT_LED);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, BL_PWM_WRAP);
    pwm_config_set_clkdiv(&pc, (float)clock_get_hz(clk_sys) / (float)(BL_PWM_HZ * (BL_PWM_WRAP + 1)));
    pwm_init(slice, &pc, true);
    bl_set(0);

    gpio_init(PIN_TFT_CS);
    gpio_set_dir(PIN_TFT_CS, GPIO_OUT);
    gpio_put(PIN_TFT_CS, 1);
    gpio_init(PIN_TFT_DC);
    gpio_set_dir(PIN_TFT_DC, GPIO_OUT);
    gpio_put(PIN_TFT_DC, 1);
    gpio_init(PIN_TFT_RESET);
    gpio_set_dir(PIN_TFT_RESET, GPIO_OUT);
    gpio_put(PIN_TFT_RESET, 1);
    gpio_set_function(PIN_TFT_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TFT_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_TFT_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_TFT_MISO); /* unconnected or tri-stated MISO reads FF, not noise */
    dma_chan = dma_claim_unused_channel(true);

    for (unsigned p = PIN_ENC_A; p <= PIN_ENC_B; p++) {
        gpio_init(p);
        gpio_pull_up(p);
    }
    gpio_init(PIN_ENC_SW);
    gpio_pull_up(PIN_ENC_SW);

    /* Clocks to try: clk_peri over even divisors (150 MHz: 75, 37.5, 25, 18.75, 12.5 MHz). */
    static const uint32_t try_hz[] = {75000000u, 37500000u, 25000000u, 18750000u, 12500000u};
    uint32_t start_hz = spi_init(TFT_SPI, 12500000u);
    spi_16bit(false);
    tft_hard_reset();
    ctrl_t ctrl = tft_identify_and_init(start_hz);

    uint32_t best_hz = 0;
    for (size_t i = 0; i < sizeof try_hz / sizeof try_hz[0]; i++) {
        uint32_t hz = spi_set_baudrate(TFT_SPI, try_hz[i]);
        uint64_t t0 = time_us_64();
        tft_fill(0, 0, SCREEN_W, SCREEN_H, (uint16_t)(0x1111u * (i + 1)));
        uint32_t fill_us = (uint32_t)(time_us_64() - t0);
        size_t shift = 0;
        rb_fmt_t fmt = FMT_RGB666;
        bool ok = true;
        for (uint32_t seed = 1; seed <= 4 && ok; seed++)
            ok = rb_check(hz, seed * 0x9E3779B9u, &shift, &fmt);
        unsigned bad_pixels = 0, bad_strips = 0;
        if (ok) bad_strips = soak(hz, shift, fmt, &bad_pixels); /* before printing: a line isn't split by the wait */
        printf("DISP-SPI write_khz=%lu fill_us=%lu readback=%s", (unsigned long)(hz / 1000u), (unsigned long)fill_us,
               ok ? "ok" : "BAD");
        if (ok) {
            printf(" fmt=%s shift=%u soak_bad_strips=%u/%u soak_bad_pixels=%u", fmt_names[fmt], (unsigned)shift,
                   bad_strips, SOAK_STRIPS, bad_pixels);
            if (!best_hz && !bad_strips) best_hz = hz;
        } else {
            print_hex("raw", rb_raw, 12);
        }
        printf("\n");
    }
    uint32_t use_hz = spi_set_baudrate(TFT_SPI, best_hz ? best_hz : 12500000u);
    printf("DISP-SPI use_khz=%lu%s\n", (unsigned long)(use_hz / 1000u), best_hz ? "" : " (no clock read back: guess)");

    uint64_t t0 = time_us_64();
    draw_screen(ctrl, use_hz);
    uint32_t screen_us = (uint32_t)(time_us_64() - t0);
    t0 = time_us_64();
    tft_text(0, 220, "0123456789012345678901234567890123456789", C_WHITE, C_BLACK, 1);
    uint32_t line_us = (uint32_t)(time_us_64() - t0);
    tft_fill(0, 220, SCREEN_W, FONT_8X13_HEIGHT, C_BLACK);
    printf("DISP-TEXT screen_us=%lu line40_us=%lu\n", (unsigned long)screen_us, (unsigned long)line_us);

    unsigned bl = 100;
    for (int pct = 100; pct >= 0; pct -= 10) {
        bl_set((unsigned)pct);
        printf("DISP-BL %d%%\n", pct);
        sleep_ms(150);
    }
    for (int pct = 10; pct <= 100; pct += 10) {
        bl_set((unsigned)pct);
        printf("DISP-BL %d%%\n", pct);
        sleep_ms(150);
    }
    int pos = 0;
    unsigned presses = 0;
    draw_status(bl, pos, presses);

    static repeating_timer_t timer;
    add_repeating_timer_ms(-1, enc_poll, NULL, &timer);
    printf("DISP-READY\n");

    for (;;) {
        if (ev_tail == ev_head) {
            tight_loop_contents();
            continue;
        }
        __compiler_memory_barrier();
        enc_event_t e = (enc_event_t)ev_ring[ev_tail % sizeof ev_ring];
        ev_tail = ev_tail + 1;
        switch (e) {
            case EV_PLUS:
            case EV_MINUS:
                pos += e == EV_PLUS ? 1 : -1;
                if (e == EV_PLUS && bl < 100) bl += 5;
                if (e == EV_MINUS && bl > 0) bl -= 5;
                bl_set(bl);
                printf("ENC step %s pos=%d transitions=%lu invalid=%lu bl=%u\n", e == EV_PLUS ? "+1" : "-1", pos,
                       (unsigned long)enc_transitions, (unsigned long)enc_invalid, bl);
                break;
            case EV_PRESS:
                presses++;
                printf("ENC press\n");
                break;
            case EV_RELEASE:
                printf("ENC release\n");
                break;
        }
        draw_status(bl, pos, presses);
    }
}
