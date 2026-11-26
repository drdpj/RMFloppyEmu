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
#include "core/config.h"

#include <ctype.h>
#include <string.h>

#include "core/disk_image.h"
#include "core/rmfe_assert.h"

#define LINE_MAX_BYTES 300u /* a key, " = " and a path of CONFIG_PATH_MAX */

typedef struct {
    const char *p;
    size_t n;
} span_t;

static span_t trim(span_t s) {
    while (s.n && isspace((unsigned char)s.p[0])) {
        s.p++;
        s.n--;
    }
    while (s.n && isspace((unsigned char)s.p[s.n - 1]))
        s.n--;
    return s;
}

static span_t unquote(span_t s) {
    if (s.n >= 2 && s.p[0] == '"' && s.p[s.n - 1] == '"') {
        s.p++;
        s.n -= 2;
    }
    return s;
}

static bool is(span_t s, const char *word) {
    size_t n = strlen(word);
    if (s.n != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s.p[i]) != word[i]) return false;
    }
    return true;
}

/* The next line of text from *pos, without its line ending (LF, CR LF or CR). */
static bool next_line(const char *text, size_t len, size_t *pos, span_t *line) {
    if (*pos >= len) return false;
    size_t start = *pos, end = start;
    while (end < len && text[end] != '\n' && text[end] != '\r')
        end++;
    *line = (span_t){text + start, end - start};
    if (end < len && text[end] == '\r') end++;
    if (end < len && text[end] == '\n') end++; /* LF, or the LF of CR LF */
    *pos = end;
    return true;
}

static bool is_comment_or_blank(span_t s) {
    s = trim(s);
    return s.n == 0 || s.p[0] == '#';
}

void config_defaults(config_t *c) {
    RMFE_ASSERT(c);
    memset(c, 0, sizeof *c);
    c->image_on_startup = CONFIG_STARTUP_LAST;
    c->write_protect = false;
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++)
        c->drive_type[d] = IDC_DRIVE_FOLLOW_IMAGE;
    strcpy(c->idc_rom, "IDC.ROM");
    c->rotary = CONFIG_ROTARY_FULL;
    c->rotary_reverse = false;
    c->show_commands = false;
    c->fast_baud = LINK_BAUD_FAST;
    c->boot_baud = LINK_BAUD_FAST; /* the IDC's; the boot block goes paced (core/link.h, link_set_paced) */
    c->display = CONFIG_DISPLAY_AUTO;
    c->display_rotate = false;
    c->usb_storage = false; /* a PC powering the unit would otherwise take the card at every start */
}

static void warn_line(config_warn_fn warn, void *ctx, unsigned line, const char *why) {
    if (warn) warn(ctx, line, why);
}

static bool parse_drive_type(span_t v, idc_drive_type_t *out) {
    if (is(v, "auto")) {
        *out = IDC_DRIVE_FOLLOW_IMAGE;
    } else if (is(v, "40")) {
        *out = IDC_DRIVE_40_TRACK;
    } else if (is(v, "80")) {
        *out = IDC_DRIVE_80_TRACK;
    } else {
        return false;
    }
    return true;
}

/* "full", "half" or "quarter", then optionally ",reverse" (FlashFloppy's rotary option). */
static bool parse_rotary(span_t v, config_rotary_t *kind, bool *reverse) {
    const char *comma = memchr(v.p, ',', v.n);
    span_t k = trim((span_t){v.p, comma ? (size_t)(comma - v.p) : v.n});
    *reverse = false;
    if (comma) {
        if (!is(trim((span_t){comma + 1, v.n - (size_t)(comma - v.p) - 1}), "reverse")) return false;
        *reverse = true;
    }
    if (is(k, "full")) {
        *kind = CONFIG_ROTARY_FULL;
    } else if (is(k, "half")) {
        *kind = CONFIG_ROTARY_HALF;
    } else if (is(k, "quarter")) {
        *kind = CONFIG_ROTARY_QUARTER;
    } else {
        return false;
    }
    return true;
}

/* "auto", "tft", "oled-128x64" or "oled-128x32", an OLED optionally followed by "-rotate" (FlashFloppy's
 * display-type, which has more suffixes, and LCDs). */
static bool parse_display(span_t v, config_display_t *kind, bool *rotate) {
    static const char suffix[] = "-rotate";
    const size_t sn = sizeof suffix - 1u;
    *rotate = v.n > sn && is((span_t){v.p + v.n - sn, sn}, suffix);
    span_t k = {v.p, *rotate ? v.n - sn : v.n};
    if (is(k, "oled-128x64")) {
        *kind = CONFIG_DISPLAY_OLED_128X64;
    } else if (is(k, "oled-128x32")) {
        *kind = CONFIG_DISPLAY_OLED_128X32;
    } else if (*rotate) {
        return false; /* only an OLED turns round */
    } else if (is(k, "auto")) {
        *kind = CONFIG_DISPLAY_AUTO;
    } else if (is(k, "tft")) {
        *kind = CONFIG_DISPLAY_TFT;
    } else {
        return false;
    }
    return true;
}

/* One "key = value" line; returns why it was ignored, or NULL. */
static const char *parse_option(config_t *c, span_t key, span_t v) {
    if (is(key, "image-on-startup")) {
        if (is(v, "last")) {
            c->image_on_startup = CONFIG_STARTUP_LAST;
        } else if (is(v, "static")) {
            c->image_on_startup = CONFIG_STARTUP_STATIC;
        } else if (is(v, "init")) {
            c->image_on_startup = CONFIG_STARTUP_INIT;
        } else {
            return "image-on-startup must be last, static or init";
        }
    } else if (is(key, "write-protect")) {
        if (is(v, "yes")) {
            c->write_protect = true;
        } else if (is(v, "no")) {
            c->write_protect = false;
        } else {
            return "write-protect must be yes or no";
        }
    } else if (is(key, "drive-a") || is(key, "drive-b")) {
        unsigned d = tolower((unsigned char)key.p[key.n - 1]) == 'a' ? 0u : 1u;
        if (!parse_drive_type(v, &c->drive_type[d])) return "a drive must be auto, 40 or 80";
    } else if (is(key, "idc-rom")) {
        if (v.n == 0 || v.n >= sizeof c->idc_rom) return "idc-rom must be a path of up to 255 characters";
        memcpy(c->idc_rom, v.p, v.n);
        c->idc_rom[v.n] = '\0';
    } else if (is(key, "show-commands")) {
        if (is(v, "yes")) {
            c->show_commands = true;
        } else if (is(v, "no")) {
            c->show_commands = false;
        } else {
            return "show-commands must be yes or no";
        }
    } else if (is(key, "fast-baud")) {
        if (is(v, "125000")) {
            c->fast_baud = LINK_BAUD_FAST;
        } else if (is(v, "62500")) {
            c->fast_baud = LINK_BAUD_HALF;
        } else {
            return "fast-baud must be 125000 or 62500";
        }
    } else if (is(key, "boot-baud")) {
        if (is(v, "125000")) {
            c->boot_baud = LINK_BAUD_FAST;
        } else if (is(v, "62500")) {
            c->boot_baud = LINK_BAUD_HALF;
        } else {
            return "boot-baud must be 62500 or 125000";
        }
    } else if (is(key, "display-type")) {
        config_display_t kind;
        bool rotate;
        if (!parse_display(v, &kind, &rotate)) {
            return "display-type must be auto, tft, oled-128x64 or oled-128x32 (an OLED optionally with -rotate)";
        }
        c->display = kind;
        c->display_rotate = rotate;
    } else if (is(key, "usb-storage")) {
        if (is(v, "yes")) {
            c->usb_storage = true;
        } else if (is(v, "no")) {
            c->usb_storage = false;
        } else {
            return "usb-storage must be yes or no";
        }
    } else if (is(key, "rotary")) {
        config_rotary_t kind;
        bool reverse;
        if (!parse_rotary(v, &kind, &reverse)) return "rotary must be full, half or quarter, optionally with ,reverse";
        c->rotary = kind;
        c->rotary_reverse = reverse;
    } else {
        return "unknown option";
    }
    return NULL;
}

void config_parse(config_t *c, const char *text, size_t len, config_warn_fn warn, void *ctx) {
    RMFE_ASSERT(c && (text || len == 0));
    size_t pos = 0;
    span_t line;
    for (unsigned number = 1; next_line(text, len, &pos, &line); number++) {
        if (is_comment_or_blank(line)) continue;
        if (line.n > LINE_MAX_BYTES) {
            warn_line(warn, ctx, number, "line too long");
            continue;
        }
        const char *eq = memchr(line.p, '=', line.n);
        if (!eq) {
            warn_line(warn, ctx, number, "not key = value");
            continue;
        }
        span_t key = trim((span_t){line.p, (size_t)(eq - line.p)});
        span_t value = unquote(trim((span_t){eq + 1, line.n - (size_t)(eq - line.p) - 1}));
        const char *why = parse_option(c, key, value);
        if (why) warn_line(warn, ctx, number, why);
    }
}

bool config_image_path(const char *text, size_t len, char *path, size_t cap) {
    RMFE_ASSERT(path && cap > 0 && (text || len == 0));
    size_t pos = 0;
    span_t line;
    while (next_line(text, len, &pos, &line)) {
        if (is_comment_or_blank(line)) continue;
        span_t v = unquote(trim(line));
        if (v.n == 0 || v.n >= cap) return false;
        memcpy(path, v.p, v.n);
        path[v.n] = '\0';
        return true;
    }
    return false;
}

bool config_is_listed_image(const char *name, bool hidden) {
    disk_image_kind_t kind;
    return !hidden && name[0] != '.' && disk_image_kind_from_name(name, &kind);
}

int config_compare_names(const char *a, const char *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; *x || *y; x++, y++) {
        int d = tolower(*x) - tolower(*y);
        if (d) return d;
    }
    return strcmp(a, b);
}

/* ---- the settings menu's options ------------------------------------------------------------------------- */

typedef struct {
    const char *value; /* as RMFE.CFG has it */
    const char *label; /* as the menu shows it */
} choice_t;

typedef struct {
    const char *key;
    const char *label;
    unsigned count;
    choice_t choices[6];
} item_t;

#define YES_NO                                                                                                         \
    2u, {                                                                                                              \
        {"yes", "yes"}, {"no", "no"}                                                                                   \
    }
#define BAUDS                                                                                                          \
    2u, {                                                                                                              \
        {"125000", "125000"}, {"62500", "62500"}                                                                       \
    }
#define DRIVE                                                                                                          \
    3u, {                                                                                                              \
        {"auto", "auto"}, {"40", "40"}, {"80", "80"}                                                                   \
    }

static const item_t items[CONFIG_ITEM_COUNT] = {
    [CONFIG_ITEM_USB_STORAGE] = {"usb-storage", "USB storage", YES_NO},
    [CONFIG_ITEM_WRITE_PROTECT] = {"write-protect", "Write-prot", YES_NO},
    [CONFIG_ITEM_DRIVE_A] = {"drive-a", "Drive A", DRIVE},
    [CONFIG_ITEM_DRIVE_B] = {"drive-b", "Drive B", DRIVE},
    [CONFIG_ITEM_IMAGE_ON_STARTUP] = {"image-on-startup",
                                      "Startup",
                                      3u,
                                      {{"last", "last"}, {"static", "static"}, {"init", "init"}}},
    [CONFIG_ITEM_SHOW_COMMANDS] = {"show-commands", "Show cmds", YES_NO},
    [CONFIG_ITEM_FAST_BAUD] = {"fast-baud", "Fast baud", BAUDS},
    [CONFIG_ITEM_BOOT_BAUD] = {"boot-baud", "Boot baud", BAUDS},
    [CONFIG_ITEM_ROTARY] = {"rotary",
                            "Knob",
                            6u,
                            {{"full", "full"},
                             {"half", "half"},
                             {"quarter", "quarter"},
                             {"full,reverse", "full rev"},
                             {"half,reverse", "half rev"},
                             {"quarter,reverse", "quarter rev"}}},
};

static const item_t *item_of(config_item_t item) {
    RMFE_ASSERT((unsigned)item < CONFIG_ITEM_COUNT);
    return &items[item];
}

const char *config_item_key(config_item_t item) {
    return item_of(item)->key;
}

const char *config_item_label(config_item_t item) {
    return item_of(item)->label;
}

unsigned config_item_choices(config_item_t item) {
    return item_of(item)->count;
}

const char *config_item_value(config_item_t item, unsigned choice) {
    RMFE_ASSERT(choice < item_of(item)->count);
    return item_of(item)->choices[choice].value;
}

const char *config_item_choice_label(config_item_t item, unsigned choice) {
    RMFE_ASSERT(choice < item_of(item)->count);
    return item_of(item)->choices[choice].label;
}

static span_t span_of(const char *s) {
    return (span_t){s, strlen(s)};
}

void config_item_set(config_t *c, config_item_t item, unsigned choice) {
    RMFE_ASSERT(c);
    const char *why = parse_option(c, span_of(config_item_key(item)), span_of(config_item_value(item, choice)));
    RMFE_ASSERT(why == NULL); /* the table's values are all ones the parser takes */
}

static unsigned yes_no(bool yes) {
    return yes ? 0u : 1u;
}

static unsigned baud(uint32_t b) {
    return b == LINK_BAUD_FAST ? 0u : b == LINK_BAUD_HALF ? 1u : 2u;
}

static unsigned drive_type(idc_drive_type_t t) {
    switch (t) {
        case IDC_DRIVE_FOLLOW_IMAGE:
            return 0u;
        case IDC_DRIVE_40_TRACK:
            return 1u;
        case IDC_DRIVE_80_TRACK:
            return 2u;
    }
    return 3u;
}

/* In the table's order: tests/test_config.c checks each choice set reads back as itself. */
unsigned config_item_get(const config_t *c, config_item_t item) {
    RMFE_ASSERT(c);
    switch (item) {
        case CONFIG_ITEM_USB_STORAGE:
            return yes_no(c->usb_storage);
        case CONFIG_ITEM_WRITE_PROTECT:
            return yes_no(c->write_protect);
        case CONFIG_ITEM_DRIVE_A:
            return drive_type(c->drive_type[0]);
        case CONFIG_ITEM_DRIVE_B:
            return drive_type(c->drive_type[1]);
        case CONFIG_ITEM_IMAGE_ON_STARTUP:
            return c->image_on_startup <= CONFIG_STARTUP_INIT ? (unsigned)c->image_on_startup : 3u;
        case CONFIG_ITEM_SHOW_COMMANDS:
            return yes_no(c->show_commands);
        case CONFIG_ITEM_FAST_BAUD:
            return baud(c->fast_baud);
        case CONFIG_ITEM_BOOT_BAUD:
            return baud(c->boot_baud);
        case CONFIG_ITEM_ROTARY:
            if (c->rotary > CONFIG_ROTARY_QUARTER) return 6u;
            return (unsigned)c->rotary + (c->rotary_reverse ? 3u : 0u);
        case CONFIG_ITEM_COUNT:
            break;
    }
    RMFE_ASSERT(false);
    return 0u;
}

/* Appends n bytes to out, if they fit. */
static bool put(char *out, size_t cap, size_t *at, const char *p, size_t n) {
    if (n > cap - *at) return false;
    memcpy(out + *at, p, n);
    *at += n;
    return true;
}

static bool put_setting(char *out, size_t cap, size_t *at, const char *key, const char *value, span_t ending) {
    return put(out, cap, at, key, strlen(key)) && put(out, cap, at, " = ", 3u) &&
           put(out, cap, at, value, strlen(value)) && put(out, cap, at, ending.p, ending.n);
}

/* The line's key, if it is a "key = value" line rather than a comment or blank. */
static bool line_key(span_t line, span_t *key) {
    if (is_comment_or_blank(line)) return false;
    const char *eq = memchr(line.p, '=', line.n);
    if (!eq) return false;
    *key = trim((span_t){line.p, (size_t)(eq - line.p)});
    return true;
}

bool config_set_line(const char *text, size_t len, const char *key, const char *value, char *out, size_t cap,
                     size_t *out_len) {
    RMFE_ASSERT((text || len == 0) && key && value && out && out_len);
    size_t at = 0, pos = 0;
    span_t line, ending = {"\n", 1u}, k;
    bool found = false, have_ending = false;
    while (next_line(text, len, &pos, &line)) {
        size_t end = (size_t)(line.p - text) + line.n;
        span_t this_ending = {text + end, pos - end};
        if (!have_ending && this_ending.n) {
            ending = this_ending;
            have_ending = true;
        }
        if (line_key(line, &k) && is(k, key)) {
            found = true;
            if (!put_setting(out, cap, &at, key, value, this_ending)) return false;
        } else if (!put(out, cap, &at, line.p, line.n + this_ending.n)) {
            return false;
        }
    }
    if (!found) {
        bool unended = len > 0 && text[len - 1] != '\n' && text[len - 1] != '\r';
        if (unended && !put(out, cap, &at, ending.p, ending.n)) return false;
        if (!put_setting(out, cap, &at, key, value, ending)) return false;
    }
    *out_len = at;
    return true;
}
