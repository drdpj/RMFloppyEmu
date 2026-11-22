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
 */
#include "pico/panel.h"

#include "core/rotary.h"
#include "core/ui.h"
#include "pico/link_uart.h"
#include "pico/mailbox.h"
#include "pico/menu_host.h"
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
    printf("STATS: core0_stack=%lu/%lu core1_stack=%lu/%lu menu_step_max_us=%lu tft_khz=%lu knob_missed=%lu "
           "knob_dropped=%lu\n",
           (unsigned long)peak0, (unsigned long)size0, (unsigned long)peak1, (unsigned long)size1,
           (unsigned long)menu_host_longest_step_us(), (unsigned long)(tft_hz / 1000u), (unsigned long)knob.missed,
           (unsigned long)ring_dropped);
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
    [UI_REQ_LIST] = "list", [UI_REQ_INSERT] = "insert", [UI_REQ_EJECT] = "eject"};

static void talk_to_core0(uint32_t ms) {
    static uint32_t unit_seen;
    ui_unit_t u;
    if (mailbox_unit_changed(&unit_seen, &u)) ui_set_unit(&ui, &u); /* before the answer: the menu then shows it */
    bool ok;
    char message[MAILBOX_MESSAGE_MAX];
    if (mailbox_take_done(&ok, message)) {
        printf("UI: done %s%s%s\n", ok ? "ok" : "failed", message[0] ? ": " : "", message);
        ui_request_done(&ui, ok, message[0] ? message : NULL, ms);
    }
    ui_request_t r;
    if (ui_take_request(&ui, &r)) {
        printf("UI: %s %c \"%s\"\n", request_names[r.kind], 'A' + (int)r.drive, r.path);
        (void)mailbox_post_request(&r); /* the slot is free: one request at a time, and core 0 took the last */
    }
}

static void draw(void) {
    uint32_t dirty = ui.screen.dirty;
    if (!dirty) return;
    ui_drawn(&ui);
    for (unsigned row = 0; row < UI_ROWS; row++) {
        if (!(dirty & (1u << row))) continue;
        unsigned a = ui.screen.attr[row];
        tft_text_row(row * TFT_ROW_H, ui.screen.text[row], fg[a], bg[a]);
    }
    tft_wait();
}

static void core1_main(void) {
    controller = tft_init(&tft_hz);
    printf("UI: display %s, %lu kHz\n",
           controller == TFT_ILI9341  ? "ILI9341"
           : controller == TFT_ST7789 ? "ST7789"
                                      : "not answering (driven as an ILI9341)",
           (unsigned long)(tft_hz / 1000u));
    uint32_t ms = now_ms();
    ui_init(&ui, mailbox_list(), ms);
    ui_set_options(&ui, settings->show_commands);
    knob_init();
    draw();
    tft_backlight(100);
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
