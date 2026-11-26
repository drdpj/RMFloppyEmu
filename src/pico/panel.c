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
 * Core 1: see panel.h. The knob's pins interrupt core 1 on every edge, and the handler only notes the pins' levels
 * and the time, so no quadrature state is missed while a row is being drawn (a full screen takes about 40 ms);
 * the loop also samples them every pass, which times the switch's debounce and long press (core/rotary.h).
 * GPIO interrupts rather than PIO: the knob changes a few hundred times a second at most, and core 1 has nothing
 * more urgent to do; core 0's GPIO interrupt (the 480Z's RTS) is its own.
 *
 * The display is the OLED if RMFE.CFG's display-type asks for one (or is auto) and one answers on I2C, otherwise
 * the TFT. On the OLED the menu's rows (core/ui.h, its OLED layouts) are 7 x 13 rows from the top, then the status
 * line at the bottom: under a rule in 5 x 8 on a 128 x 64 panel, in the last 6 rows in 4 x 6 on a 128 x 32.
 */
#include "pico/panel.h"

#include "core/rotary.h"
#include "core/ui.h"
#include "pico/link_uart.h"
#include "pico/mailbox.h"
#include "pico/menu_host.h"
#include "pico/oled.h"
#include "pico/sd_store.h"
#include "pico/stack_meter.h"
#include "pico/tft.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

/* docs/hardware.md, "Pin plan" */
#define PIN_ENC_A  2u
#define PIN_ENC_B  3u
#define PIN_ENC_SW 26u
#define ENC_MASK   ((1u << PIN_ENC_A) | (1u << PIN_ENC_B) | (1u << PIN_ENC_SW))

#define TICK_MS 20u

/* Everything here is core 1's alone, except the ring: written by enc_irq(), read by the loop, both core 1. */
typedef struct {
    uint32_t pins;
    uint32_t ms;
} sample_t;
static volatile sample_t ring[64];
static volatile uint32_t ring_head, ring_tail, ring_dropped;

static const config_t *settings;
static ui_t ui;
static rotary_t knob;
static tft_controller_t controller;
static uint32_t tft_hz;
static bool on_oled;
static unsigned oled_height; /* 64 or 32 */
static uint32_t oled_hz;

_Static_assert(UI_DOT_LIT == OLED_DOT_FILLED && UI_DOT_OUT == OLED_DOT_HOLLOW && UI_ELLIPSIS == OLED_ELLIPSIS,
               "the menu's own characters are the OLED's");
_Static_assert(UI_ELLIPSIS == TFT_ELLIPSIS, "and the TFT's");

static void oled_layout(void);

static const uint16_t fg[] = {
    [UI_ATTR_NORMAL] = TFT_RGB565(230, 230, 230), [UI_ATTR_TITLE] = TFT_RGB565(0, 0, 0),
    [UI_ATTR_CURSOR] = TFT_RGB565(0, 0, 0),       [UI_ATTR_FAINT] = TFT_RGB565(140, 140, 140),
    [UI_ATTR_WARN] = TFT_RGB565(255, 100, 60),    [UI_ATTR_READ] = TFT_RGB565(60, 230, 60),
    [UI_ATTR_WRITE] = TFT_RGB565(255, 150, 0),
};
static const uint16_t bg[] = {
    [UI_ATTR_NORMAL] = 0,
    [UI_ATTR_TITLE] = TFT_RGB565(255, 176, 0),
    [UI_ATTR_CURSOR] = TFT_RGB565(0, 190, 255),
    [UI_ATTR_FAINT] = 0,
    [UI_ATTR_WARN] = 0,
    [UI_ATTR_READ] = 0,
    [UI_ATTR_WRITE] = 0,
};

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void enc_irq(void) {
    for (unsigned pin = 0; pin < 32; pin++) {
        if (!(ENC_MASK & (1u << pin))) continue;
        uint32_t ev = gpio_get_irq_event_mask(pin);
        if (ev) gpio_acknowledge_irq(pin, ev);
    }
    uint32_t h = ring_head;
    if (h - ring_tail >= sizeof ring / sizeof ring[0]) {
        ring_dropped++;
        return;
    }
    ring[h % (sizeof ring / sizeof ring[0])].pins = gpio_get_all();
    ring[h % (sizeof ring / sizeof ring[0])].ms = now_ms();
    __compiler_memory_barrier();
    ring_head = h + 1;
}

static void knob_init(void) {
    for (unsigned pin = 0; pin < 32; pin++) {
        if (!(ENC_MASK & (1u << pin))) continue;
        gpio_init(pin);
        gpio_pull_up(pin);
    }
    rotary_init(&knob, settings->rotary, settings->rotary_reverse, now_ms());
    gpio_add_raw_irq_handler_masked(ENC_MASK, enc_irq);
    for (unsigned pin = 0; pin < 32; pin++) {
        if (ENC_MASK & (1u << pin)) gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    }
    irq_set_enabled(IO_IRQ_BANK0, true);
}

static void feed(uint32_t pins, uint32_t ms) {
    unsigned ab = ((pins >> PIN_ENC_A) & 1u) << 1 | ((pins >> PIN_ENC_B) & 1u);
    bool pressed = !((pins >> PIN_ENC_SW) & 1u);
    rotary_event_t ev[ROTARY_MAX_EVENTS];
    unsigned n = rotary_update(&knob, ab, pressed, ms, ev);
    for (unsigned i = 0; i < n; i++)
        ui_event(&ui, ev[i], ms);
}

static void report(void) {
    uint32_t size0, size1;
    uint32_t peak0 = stack_meter_peak(0, &size0), peak1 = stack_meter_peak(1, &size1);
    printf("STATS: core0_stack=%lu/%lu core1_stack=%lu/%lu menu_step_max_us=%lu ", (unsigned long)peak0,
           (unsigned long)size0, (unsigned long)peak1, (unsigned long)size1,
           (unsigned long)menu_host_longest_step_us());
    if (on_oled) {
        printf("oled_khz=%lu oled_errors=%lu ", (unsigned long)(oled_hz / 1000u), (unsigned long)oled_errors());
    } else {
        printf("tft_khz=%lu ", (unsigned long)(tft_hz / 1000u));
    }
    printf("knob_missed=%lu knob_dropped=%lu\n", (unsigned long)knob.missed, (unsigned long)ring_dropped);
}

static void console_key(uint32_t ms) {
    int c = getchar_timeout_us(0);
    switch (c) {
        case '+':
            ui_event(&ui, ROTARY_CW, ms);
            break;
        case '-':
            ui_event(&ui, ROTARY_CCW, ms);
            break;
        case 'p':
            ui_event(&ui, ROTARY_PRESS, ms);
            break;
        case 'l':
            ui_event(&ui, ROTARY_LONG_PRESS, ms);
            break;
        case 'r':
            ui.screen.dirty = (1u << UI_ROWS) - 1u;
            break;
        case 's':
            report();
            break;
        case 'k': /* the link's last exchanges */
            link_uart_print_log();
            break;
        case 'c': /* show-commands, until the next start */
            ui_set_options(&ui, !ui.show_commands);
            break;
        case 'h': /* the OLED's height, until the next start */
            if (on_oled) {
                uint8_t addr;
                oled_height = oled_height == 64u ? 32u : 64u;
                (void)oled_init(oled_height, settings->display_rotate, &addr, &oled_hz);
                oled_layout();
                printf("UI: OLED 128x%u until the next start\n", oled_height);
            }
            break;
        case 'w': { /* test hook: slow saves (sd_store_set_save_delay) */
            static const uint32_t delays[] = {0, 250, 500, 1000, 2000, 4000};
            static unsigned k;
            k = (k + 1u) % (sizeof delays / sizeof delays[0]);
            sd_store_set_save_delay(delays[k]);
            printf("TEST: each save now takes %lu ms longer\n", (unsigned long)delays[k]);
            break;
        }
        case 'd':
            for (unsigned row = 0; row < UI_ROWS; row++)
                printf("SCREEN %2u %u|%s|\n", row, ui.screen.attr[row], ui.screen.text[row]);
            break;
        default:
            break;
    }
}

static const char *const request_names[] = {
    [UI_REQ_LIST] = "list", [UI_REQ_INSERT] = "insert", [UI_REQ_EJECT] = "eject", [UI_REQ_SET] = "set"};

/* The settings core 1 uses, as core 0 posts them: the knob, and show-commands. Each is taken only when it changes,
 * so the console's c (show-commands until the next start) holds until then. */
static void follow_settings(const config_t *c) {
    static bool seen;
    static config_t last;
    if (seen && (c->rotary != last.rotary || c->rotary_reverse != last.rotary_reverse)) {
        rotary_init(&knob, c->rotary, c->rotary_reverse, now_ms());
    }
    if (seen && c->show_commands != last.show_commands) ui_set_options(&ui, c->show_commands);
    last = *c;
    seen = true;
}

static void talk_to_core0(uint32_t ms) {
    static uint32_t unit_seen;
    static ui_unit_t u;                         /* 900 bytes: not on the 4 KB stack */
    if (mailbox_unit_changed(&unit_seen, &u)) { /* before the answer: the menu then shows it */
        ui_set_unit(&ui, &u);
        follow_settings(&u.config);
    }
    bool ok;
    char message[MAILBOX_MESSAGE_MAX];
    if (mailbox_take_done(&ok, message)) {
        printf("UI: done %s%s%s\n", ok ? "ok" : "failed", message[0] ? ": " : "", message);
        ui_request_done(&ui, ok, message[0] ? message : NULL, ms);
    }
    ui_request_t r;
    if (ui_take_request(&ui, &r)) {
        if (r.kind == UI_REQ_SET) {
            printf("UI: set %s = %s\n", config_item_key(r.item), config_item_value(r.item, r.choice));
        } else {
            printf("UI: %s %c \"%s\"\n", request_names[r.kind], 'A' + (int)r.drive, r.path);
        }
        (void)mailbox_post_request(&r); /* the slot is free: one request at a time, and core 0 took the last */
    }
}

/* The OLED: the menu's rows in 7 x 13 from the top, then its status line at the bottom (core/ui.h). */
static void draw_oled(uint32_t dirty) {
    unsigned main_rows = oled_height == 32u ? 2u : 4u;
    oled_font_t status_font = oled_height == 32u ? OLED_FONT_4X6 : OLED_FONT_5X8;
    for (unsigned row = 0; row <= main_rows; row++) {
        if (!(dirty & (1u << row))) continue;
        bool inverse = ui.screen.attr[row] == UI_ATTR_CURSOR;
        if (row < main_rows) {
            oled_text(row * oled_font_height(OLED_FONT_7X13B), OLED_FONT_7X13B, ui.screen.text[row], inverse);
        } else {
            oled_text(oled_height - oled_font_height(status_font), status_font, ui.screen.text[row], inverse);
        }
    }
    (void)oled_flush(); /* a write not acknowledged is counted (oled_errors, on the s key) */
}

static void draw(void) {
    uint32_t dirty = ui.screen.dirty;
    if (!dirty) return;
    ui_drawn(&ui);
    if (on_oled) {
        draw_oled(dirty);
        return;
    }
    for (unsigned row = 0; row < UI_ROWS; row++) {
        if (!(dirty & (1u << row))) continue;
        unsigned a = ui.screen.attr[row];
        tft_text_row(row * TFT_ROW_H, ui.screen.text[row], fg[a], bg[a]);
    }
    tft_wait();
}

static void oled_layout(void) {
    if (oled_height == 64u) oled_rule(oled_height - oled_font_height(OLED_FONT_5X8) - 2u); /* a blank row between */
    ui_set_layout(&ui, oled_height == 32u ? UI_LAYOUT_OLED_32 : UI_LAYOUT_OLED_64);
}

/* The OLED if display-type allows one and one answers; else the TFT. */
static void display_init(void) {
    config_display_t want = settings->display;
    if (want != CONFIG_DISPLAY_TFT) {
        uint8_t addr;
        oled_height = want == CONFIG_DISPLAY_OLED_128X32 ? 32u : 64u;
        on_oled = oled_init(oled_height, settings->display_rotate, &addr, &oled_hz);
        if (on_oled) {
            printf("UI: display OLED 128x%u at %02X, I2C %lu kHz\n", oled_height, addr,
                   (unsigned long)(oled_hz / 1000u));
            return;
        }
        if (want != CONFIG_DISPLAY_AUTO) printf("UI: no OLED answers at 3C or 3D: using the TFT\n");
    }
    controller = tft_init(&tft_hz);
    printf("UI: display %s, %lu kHz\n",
           controller == TFT_ILI9341  ? "ILI9341"
           : controller == TFT_ST7789 ? "ST7789"
                                      : "not answering (driven as an ILI9341)",
           (unsigned long)(tft_hz / 1000u));
}

static void core1_main(void) {
    display_init();
    uint32_t ms = now_ms();
    ui_init(&ui, mailbox_list(), ms);
    if (on_oled) oled_layout();
    ui_set_options(&ui, settings->show_commands);
    knob_init();
    draw();
    if (!on_oled) tft_backlight(100);
    uint32_t last_tick = ms;
    for (;;) {
        while (ring_tail != ring_head) {
            __compiler_memory_barrier();
            sample_t s = ring[ring_tail % (sizeof ring / sizeof ring[0])];
            ring_tail = ring_tail + 1;
            feed(s.pins, s.ms);
        }
        ms = now_ms();
        feed(gpio_get_all(), ms);
        console_key(ms);
        talk_to_core0(ms);
        if (ms - last_tick >= TICK_MS) {
            last_tick = ms;
            ui_activity_t a;
            mailbox_activity(&a);
            ui_set_activity(&ui, &a, ms);
            ui_tick(&ui, ms);
        }
        draw();
    }
}

void panel_start(const config_t *c) {
    settings = c;
    stack_meter_paint_core1();
    multicore_launch_core1(core1_main);
}
