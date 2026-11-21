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
 * The TFT: see tft.h. Commands are from the ILI9341 and ST7789 datasheets (section 8; the system commands are
 * common to both). Found with tests/hil/display_probe.c on the module in docs/hardware.md:
 *   - ILI9341: ID4 (D3, read a byte at a time through the undocumented index register D9) is 00 93 41; RDDID (04)
 *     is all zero. ST7789: RDDID is 85 85 52.
 *   - Reads come back with a dummy cycle first, and RAMRD (2E) gives 18-bit colour, 3 bytes a pixel, the colour in
 *     each byte's top bits, after 8 bits of dummy.
 */
#include "pico/tft.h"

#include "pico/font_8x13.h"

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define TFT_SPI spi1

#define CMD_SWRESET     0x01u
#define CMD_RDDID       0x04u
#define CMD_SLPOUT      0x11u
#define CMD_NORON       0x13u
#define CMD_INVON       0x21u
#define CMD_DISPON      0x29u
#define CMD_CASET       0x2Au
#define CMD_PASET       0x2Bu
#define CMD_RAMWR       0x2Cu
#define CMD_RAMRD       0x2Eu
#define CMD_MADCTL      0x36u
#define CMD_COLMOD      0x3Au
#define CMD_ILI_RDID4   0xD3u
#define CMD_ILI_INDEX   0xD9u
#define COLMOD_16BIT    0x55u
#define MADCTL_ILI_LAND 0x28u /* MV | BGR: landscape, and the panel's colour filter is BGR */
#define MADCTL_ST_LAND  0x60u /* MX | MV */
#define ST7789_RDDID    0x858552u

#define READ_HZ      4000000u /* ILI9341: a serial read cycle of at least 150 ns */
#define RAMRD_SHIFT  8u       /* the dummy bits before the first pixel (found by the probe) */
#define BL_PWM_WRAP  999u
#define BL_PWM_HZ    5000u
#define FONT_TOP_PAD 2u

/* Fastest first; clk_peri is 150 MHz, and the SPI divides it by an even number. */
static const uint32_t clocks_hz[] = {37500000u, 25000000u, 18750000u, 12500000u};
#define TFT_SOAK_STRIPS 16u

static int dma_chan = -1;
static uint32_t write_hz;
static uint16_t line[2][TFT_W * TFT_ROW_H]; /* one sent by DMA while the other is drawn */
static unsigned next_line;
static bool sending; /* a DMA transfer, and the window it writes, may still be going */

/* ---- the bus ------------------------------------------------------------------------------------------------- */

static void chip_select(bool on) {
    gpio_put(TFT_PIN_CS, !on);
}

static void bits(unsigned n) {
    spi_set_format(TFT_SPI, n, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

/* After writing without reading, the receive FIFO has overflowed: empty it and clear the overrun. */
static void settle(void) {
    while (spi_is_busy(TFT_SPI))
        tight_loop_contents();
    while (spi_is_readable(TFT_SPI))
        (void)spi_get_hw(TFT_SPI)->dr;
    spi_get_hw(TFT_SPI)->icr = SPI_SSPICR_RORIC_BITS;
}

void tft_wait(void) {
    if (!sending) return;
    dma_channel_wait_for_finish_blocking((uint)dma_chan);
    settle();
    bits(8);
    chip_select(false);
    sending = false;
}

static void command(uint8_t cmd, const uint8_t *data, size_t n) {
    chip_select(true);
    gpio_put(TFT_PIN_DC, 0);
    spi_write_blocking(TFT_SPI, &cmd, 1);
    gpio_put(TFT_PIN_DC, 1);
    if (n) spi_write_blocking(TFT_SPI, data, n);
    chip_select(false);
}

static void command1(uint8_t cmd, uint8_t arg) {
    command(cmd, &arg, 1);
}

static void read_reg(uint8_t cmd, uint8_t *out, size_t n) {
    spi_set_baudrate(TFT_SPI, READ_HZ);
    chip_select(true);
    gpio_put(TFT_PIN_DC, 0);
    spi_write_blocking(TFT_SPI, &cmd, 1);
    gpio_put(TFT_PIN_DC, 1);
    spi_read_blocking(TFT_SPI, 0x00, out, n);
    chip_select(false);
    spi_set_baudrate(TFT_SPI, write_hz);
}

/* Sets the window and starts a memory write or read, leaving the controller selected for the pixels. */
static void window(unsigned x, unsigned y, unsigned w, unsigned h, uint8_t ram) {
    unsigned x1 = x + w - 1u, y1 = y + h - 1u;
    const uint8_t ca[4] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t pa[4] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(y1 >> 8), (uint8_t)y1};
    command(CMD_CASET, ca, sizeof ca);
    command(CMD_PASET, pa, sizeof pa);
    chip_select(true);
    gpio_put(TFT_PIN_DC, 0);
    spi_write_blocking(TFT_SPI, &ram, 1);
    gpio_put(TFT_PIN_DC, 1);
}

/* Sends n pixels by DMA into the window just set; tft_wait() finishes it. */
static void send_pixels(const uint16_t *px, unsigned n, bool repeat) {
    bits(16);
    dma_channel_config c = dma_channel_get_default_config((uint)dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, !repeat);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(TFT_SPI, true));
    dma_channel_configure((uint)dma_chan, &c, &spi_get_hw(TFT_SPI)->dr, px, n, true);
    sending = true;
}

static void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t colour) {
    static uint16_t fill_colour; /* read by the DMA after this returns */
    tft_wait();
    fill_colour = colour;
    window(x, y, w, h, CMD_RAMWR);
    send_pixels(&fill_colour, w * h, true);
    tft_wait();
}

/* ---- choosing the clock -------------------------------------------------------------------------------------- */

static uint16_t soak_px[TFT_W];
static uint8_t soak_raw[1 + TFT_W * 3u + 1];

/* A strip of pseudo-random pixels written at the clock being tried and read back slowly: true if every pixel's
 * colour came back. RAMRD gives each colour in the top bits of a byte, RGB, after RAMRD_SHIFT bits of dummy. */
static bool strip_ok(unsigned y, uint32_t seed) {
    uint32_t x = seed;
    for (unsigned i = 0; i < TFT_W; i++) {
        x = x * 1664525u + 1013904223u; /* Numerical Recipes LCG */
        soak_px[i] = (uint16_t)(x >> 16);
    }
    tft_wait();
    window(0, y, TFT_W, 1, CMD_RAMWR);
    bits(16);
    spi_write16_blocking(TFT_SPI, soak_px, TFT_W);
    settle();
    bits(8);
    chip_select(false);
    spi_set_baudrate(TFT_SPI, READ_HZ);
    window(0, y, TFT_W, 1, CMD_RAMRD);
    spi_read_blocking(TFT_SPI, 0x00, soak_raw, sizeof soak_raw);
    chip_select(false);
    spi_set_baudrate(TFT_SPI, write_hz);
    for (unsigned i = 0; i < TFT_W; i++) {
        const uint8_t *p = soak_raw + RAMRD_SHIFT / 8u + 3u * i;
        uint16_t want = soak_px[i];
        if ((p[0] >> 3) != (want >> 11) || (p[1] >> 2) != ((want >> 5) & 0x3Fu) || (p[2] >> 3) != (want & 0x1Fu))
            return false;
    }
    return true;
}

static void choose_clock(bool can_read) {
    size_t n = sizeof clocks_hz / sizeof clocks_hz[0];
    if (!can_read) { /* nothing to check with: the slowest */
        write_hz = spi_set_baudrate(TFT_SPI, clocks_hz[n - 1]);
        return;
    }
    for (size_t k = 0; k < n; k++) {
        write_hz = spi_set_baudrate(TFT_SPI, clocks_hz[k]);
        bool ok = true;
        for (unsigned i = 0; i < TFT_SOAK_STRIPS && ok; i++)
            ok = strip_ok((i * 37u) % TFT_H, 0x2545F491u * (i + 1u));
        if (ok) return;
        printf("TFT: writes at %lu kHz don't read back: slower\n", (unsigned long)(write_hz / 1000u));
    }
}

/* ---- start-up ------------------------------------------------------------------------------------------------ */

static void out_pin(unsigned pin, bool level) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, level);
}

tft_controller_t tft_init(uint32_t *clock_hz) {
    gpio_set_function(TFT_PIN_LED, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(TFT_PIN_LED);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, BL_PWM_WRAP);
    pwm_config_set_clkdiv(&pc, (float)clock_get_hz(clk_sys) / (float)(BL_PWM_HZ * (BL_PWM_WRAP + 1u)));
    pwm_init(slice, &pc, true);
    tft_backlight(0);

    out_pin(TFT_PIN_CS, true);
    out_pin(TFT_PIN_DC, true);
    out_pin(TFT_PIN_RESET, true);
    write_hz = spi_init(TFT_SPI, clocks_hz[sizeof clocks_hz / sizeof clocks_hz[0] - 1]);
    bits(8);
    gpio_set_function(TFT_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(TFT_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(TFT_PIN_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(TFT_PIN_MISO); /* unconnected, it reads FF: no controller */
    if (dma_chan < 0) dma_chan = dma_claim_unused_channel(true);

    sleep_ms(5);
    gpio_put(TFT_PIN_RESET, 0);
    sleep_ms(1); /* at least 10 us */
    gpio_put(TFT_PIN_RESET, 1);
    sleep_ms(120); /* both: 120 ms before SLPOUT */

    uint8_t rddid[4], id4[3];
    read_reg(CMD_RDDID, rddid, sizeof rddid);
    for (unsigned i = 0; i < 3; i++) {
        command1(CMD_ILI_INDEX, (uint8_t)(0x10u + i + 1u));
        read_reg(CMD_ILI_RDID4, &id4[i], 1);
    }
    uint32_t w = (uint32_t)rddid[0] << 24 | (uint32_t)rddid[1] << 16 | (uint32_t)rddid[2] << 8 | rddid[3];
    tft_controller_t ctrl = TFT_UNKNOWN;
    if (((w >> 7) & 0xFFFFFFu) == ST7789_RDDID) {
        ctrl = TFT_ST7789;
    } else if (id4[1] == 0x93u && id4[2] == 0x41u) {
        ctrl = TFT_ILI9341;
    }

    command(CMD_SWRESET, NULL, 0);
    sleep_ms(150);
    command(CMD_SLPOUT, NULL, 0);
    sleep_ms(120);
    command1(CMD_COLMOD, COLMOD_16BIT);
    command1(CMD_MADCTL, ctrl == TFT_ST7789 ? MADCTL_ST_LAND : MADCTL_ILI_LAND);
    if (ctrl == TFT_ST7789) command(CMD_INVON, NULL, 0); /* ST7789 panels are IPS, which want inversion */
    command(CMD_NORON, NULL, 0);
    command(CMD_DISPON, NULL, 0);
    sleep_ms(20);

    choose_clock(ctrl == TFT_ILI9341); /* the read-back format is the ILI9341's */
    fill(0, 0, TFT_W, TFT_H, 0);
    *clock_hz = write_hz;
    return ctrl;
}

void tft_backlight(unsigned percent) {
    if (percent > 100u) percent = 100u;
    pwm_set_gpio_level(TFT_PIN_LED, (uint16_t)(percent * (BL_PWM_WRAP + 1u) / 100u));
}

/* ---- text ---------------------------------------------------------------------------------------------------- */

void tft_text_row(unsigned y, const char *text, uint16_t fg, uint16_t bg) {
    uint16_t *px = line[next_line];
    next_line ^= 1u;
    size_t len = strlen(text);
    const unsigned cols = TFT_W / FONT_8X13_WIDTH;
    for (unsigned r = 0; r < TFT_ROW_H; r++) {
        uint16_t *out = px + r * TFT_W;
        unsigned gr = r - FONT_TOP_PAD; /* wraps for the padding rows: tested below */
        for (unsigned c = 0; c < cols; c++) {
            char ch = c < len ? text[c] : ' ';
            uint8_t g = 0;
            if (gr < FONT_8X13_HEIGHT && ch >= FONT_8X13_FIRST && ch <= FONT_8X13_LAST)
                g = font_8x13_glyphs[ch - FONT_8X13_FIRST][gr];
            for (unsigned b = 0; b < FONT_8X13_WIDTH; b++)
                *out++ = (g & (0x80u >> b)) ? fg : bg;
        }
    }
    tft_wait(); /* the previous row: its buffer is the other one */
    window(0, y, TFT_W, TFT_ROW_H, CMD_RAMWR);
    send_pixels(px, TFT_W * TFT_ROW_H, false);
}
