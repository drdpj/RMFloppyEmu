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
 * The menu: see ui.h. Every change of state redraws the whole screen into ui_screen_t, and only rows whose text or
 * attribute changed are marked dirty, so the platform redraws just those. Each row is cut to the width it has on the
 * display, so the screen holds only what is shown.
 *
 * The TFT's layout (rows of 40 characters):
 *   home:     0 title; 2-5 drive A (path over two rows, its kind, its light); 6-9 drive B; 10 the last command
 *             (show-commands); 11 a card or ROM warning; 13 a message; 14 help.
 *   choosing: 0 title; 1 the folder; 2-12 the list (a marker column, a space, 38 columns of name); 13 a
 *             message; 14 help. The marker is "*" for this drive's image and "A" or "B" for the other drive's.
 *   settings: 0 title; 1 RMFE.CFG's key for the option picked; 2-12 the options (two spaces, the label in 12
 *             columns, a space, the value); 13 a message; 14 help.
 */
#include "core/ui.h"

#include "core/rmfe_assert.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ROW_HOME_DRIVE(d) (2u + 4u * (d))
#define ROW_COMMAND       10u
#define ROW_CARD          11u
#define ROW_MESSAGE       13u
#define ROW_HELP          14u
#define PATH_COLS         (UI_COLS - 3u) /* home: after "A: " */
#define ITEM_COLS         (UI_COLS - 2u) /* choosing: after the marker and a space */
#define OLED_STATUS_64    21u            /* 5 x 8 characters */
#define OLED_STATUS_32    32u            /* 4 x 6 */
#define LABEL_COLS        12u            /* settings, on the TFT: the labels' column */

typedef enum {
    ITEM_EJECT,
    ITEM_UP,
    ITEM_ENTRY,
} item_kind_t;

/* ---- the layouts ------------------------------------------------------------------------------------------- */

static bool oled(const ui_t *u) {
    return u->layout != UI_LAYOUT_TFT;
}

/* OLED: the rows in the large font; the status line is the row after them. */
static unsigned oled_rows(const ui_t *u) {
    return u->layout == UI_LAYOUT_OLED_32 ? 2u : 4u;
}

static unsigned status_cols(const ui_t *u) {
    return u->layout == UI_LAYOUT_OLED_32 ? OLED_STATUS_32 : OLED_STATUS_64;
}

/* How many characters of a row are shown. */
static unsigned row_cols(const ui_t *u, unsigned row) {
    if (!oled(u)) return UI_COLS;
    if (row < oled_rows(u)) return UI_OLED_COLS;
    return row == oled_rows(u) ? status_cols(u) : 0u;
}

static unsigned list_first_row(const ui_t *u) {
    switch (u->layout) {
        case UI_LAYOUT_TFT:
            return UI_LIST_FIRST_ROW;
        case UI_LAYOUT_OLED_64:
            return 1u; /* under the title */
        case UI_LAYOUT_OLED_32:
            return 0u;
    }
    return 0u;
}

static unsigned list_rows(const ui_t *u) {
    return oled(u) ? oled_rows(u) - list_first_row(u) : UI_LIST_ROWS;
}

/* The columns of a list entry's name: after the marker and a space. */
static size_t item_cols(const ui_t *u) {
    return oled(u) ? UI_OLED_COLS - 2u : ITEM_COLS;
}

/* ---- the screen -------------------------------------------------------------------------------------------- */

static void set_row(ui_t *u, unsigned row, ui_attr_t attr, const char *text) {
    char line[UI_COLS + 1];
    size_t n = strlen(text);
    if (n > row_cols(u, row)) n = row_cols(u, row);
    memcpy(line, text, n);
    memset(line + n, ' ', UI_COLS - n);
    line[UI_COLS] = '\0';
    ui_screen_t *s = &u->screen;
    if (s->attr[row] != (uint8_t)attr || memcmp(s->text[row], line, sizeof line) != 0) {
        memcpy(s->text[row], line, sizeof line);
        s->attr[row] = (uint8_t)attr;
        s->dirty |= 1u << row;
    }
}

static void set_rowf(ui_t *u, unsigned row, ui_attr_t attr, const char *fmt, ...) {
    char line[UI_COLS * 2 + 1];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap); /* longer than a row: set_row cuts it */
    va_end(ap);
    set_row(u, row, attr, line);
}

/* `width` characters of `name` from `from`, into out; a name that doesn't fit (and isn't scrolled) ends in
 * UI_ELLIPSIS. */
static void fit(char *out, const char *name, size_t width, size_t from) {
    size_t n = strlen(name);
    if (from > n) from = n;
    size_t take = n - from;
    if (take > width) take = width;
    memcpy(out, name + from, take);
    out[take] = '\0';
    if (from == 0 && n > width) out[width - 1] = UI_ELLIPSIS;
}

/* ---- home -------------------------------------------------------------------------------------------------- */

static bool lit(const ui_t *u, unsigned d) {
    return (int32_t)(u->lit_until[d] - u->now) > 0;
}

static void draw_light(ui_t *u, unsigned d, unsigned row) {
    if (!lit(u, d)) {
        set_row(u, row, UI_ATTR_NORMAL, "");
        return;
    }
    bool w = u->act.drive_writing[d];
    if (u->act.drive_has_track[d]) {
        set_rowf(u, row, w ? UI_ATTR_WRITE : UI_ATTR_READ, "   %s, track %u", w ? "writing" : "reading",
                 u->act.drive_track[d]);
    } else {
        set_row(u, row, w ? UI_ATTR_WRITE : UI_ATTR_READ, w ? "   writing" : "   reading");
    }
}

/* The third byte's meaning, by command (docs/protocol.md "Parameter bytes"): record, sector ID or count. */
static char third_letter(uint8_t cmd) {
    if (cmd == IDC_CMD_READ_ID || cmd == IDC_CMD_WRITE_ID || cmd == IDC_CMD_WRITE_ID_V) return 'S';
    if (cmd == IDC_CMD_FORMAT || cmd == IDC_CMD_VERIFY) return 'N';
    return 'R';
}

static void draw_command(ui_t *u) {
    const idc_activity_t *a = &u->shown;
    if (!u->show_commands || u->shown_number == 0) {
        set_row(u, ROW_COMMAND, UI_ATTR_FAINT, "");
        return;
    }
    char where[24] = "";
    if (a->has_drive && a->has_track) {
        snprintf(where, sizeof where, "%c/%u T%u %c%u", 'A' + a->drive, a->side, a->track, third_letter(a->cmd),
                 a->third);
    } else if (a->has_drive) {
        snprintf(where, sizeof where, "%c/%u", 'A' + a->drive, a->side);
    }
    set_rowf(u, ROW_COMMAND, UI_ATTR_FAINT, " %02X %-14s %-15s#%lu", a->cmd, idc_command_name(a->cmd), where,
             (unsigned long)u->shown_number);
}

static void draw_home(ui_t *u) {
    set_row(u, 0, UI_ATTR_TITLE, " RMFloppyEmu");
    set_row(u, 1, UI_ATTR_NORMAL, "");
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
        const ui_drive_t *dr = &u->unit.drive[d];
        unsigned row = ROW_HOME_DRIVE(d);
        ui_attr_t attr = d == u->home_cursor && u->cursor_shown ? UI_ATTR_CURSOR : UI_ATTR_NORMAL;
        char first[PATH_COLS + 1], second[PATH_COLS + 1];
        if (!dr->present) {
            strcpy(first, "(empty)");
            second[0] = '\0';
        } else {
            size_t n = strlen(dr->path);
            memcpy(first, dr->path, n < PATH_COLS ? n : PATH_COLS);
            first[n < PATH_COLS ? n : PATH_COLS] = '\0';
            fit(second, n > PATH_COLS ? dr->path + PATH_COLS : "", PATH_COLS, 0);
        }
        set_rowf(u, row, attr, "%c: %s", 'A' + (int)d, first);
        set_rowf(u, row + 1, attr, "   %s", second);
        if (dr->present) {
            set_rowf(u, row + 2, UI_ATTR_FAINT, "   %s, %u cylinders%s", dr->hfe ? "HFE" : "IMD", dr->cylinders,
                     dr->read_only ? ", write-protected" : "");
        } else {
            set_row(u, row + 2, UI_ATTR_FAINT, "");
        }
        draw_light(u, d, row + 3);
    }
    draw_command(u);
    if (u->unit.usb) {
        set_row(u, ROW_CARD, UI_ATTR_WARN, " On USB: eject it on the PC to use it");
    } else if (!u->unit.card) {
        set_row(u, ROW_CARD, UI_ATTR_WARN, " No SD card");
    } else if (!u->unit.rom) {
        set_row(u, ROW_CARD, UI_ATTR_WARN, " No IDC ROM: the 480Z can't boot");
    } else {
        set_row(u, ROW_CARD, UI_ATTR_NORMAL, "");
    }
    set_row(u, ROW_CARD + 1, UI_ATTR_NORMAL, "");
    set_row(u, ROW_HELP, UI_ATTR_FAINT,
            u->request_ready || u->request_in_flight ? " Working..." : " Turn, press: choose. Hold: settings");
}

/* ---- choosing ---------------------------------------------------------------------------------------------- */

static unsigned specials(const ui_t *u) {
    return (u->unit.drive[u->drive].present ? 1u : 0u) + (u->folder[0] ? 1u : 0u);
}

static unsigned item_count(const ui_t *u) {
    return specials(u) + (u->listed ? u->list->count : 0u);
}

static item_kind_t item_kind(const ui_t *u, unsigned i, unsigned *entry) {
    bool eject = u->unit.drive[u->drive].present;
    if (eject && i == 0) return ITEM_EJECT;
    if (u->folder[0] && i == (eject ? 1u : 0u)) return ITEM_UP;
    *entry = i - specials(u);
    return ITEM_ENTRY;
}

/* The path of a list entry, or false if it doesn't fit. */
static bool entry_path(const ui_t *u, unsigned entry, char out[CONFIG_PATH_MAX]) {
    return dirlist_path_join(out, CONFIG_PATH_MAX, u->folder, dirlist_name(u->list, entry));
}

static char marker(const ui_t *u, unsigned entry) {
    if (dirlist_is_dir(u->list, entry)) return ' ';
    char path[CONFIG_PATH_MAX];
    if (!entry_path(u, entry, path)) return ' ';
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
        const ui_drive_t *dr = &u->unit.drive[d];
        if (dr->present && config_compare_names(dr->path, path) == 0) return d == u->drive ? '*' : (char)('A' + d);
    }
    return ' ';
}

/* How far text of len characters, in width columns, has scrolled since `since`: a pause at each end, once, and then
 * back to the start to stay. */
static size_t scroll_offset(const ui_t *u, size_t len, size_t width, uint32_t since) {
    if (len <= width) return 0;
    size_t max = len - width;
    uint32_t cycle = 2u * UI_SCROLL_PAUSE_MS + (uint32_t)max * UI_SCROLL_STEP_MS;
    uint32_t t = u->now - since;
    if (t >= cycle) return 0;
    if (t < UI_SCROLL_PAUSE_MS) return 0;
    t -= UI_SCROLL_PAUSE_MS;
    size_t steps = t / UI_SCROLL_STEP_MS;
    return steps < max ? steps : max;
}

static void draw_list(ui_t *u);

static void draw_choose(ui_t *u) {
    set_rowf(u, 0, UI_ATTR_TITLE, " Drive %c: choose an image", 'A' + (int)u->drive);
    char folder[UI_COLS + 1] = " /";
    size_t fl = strlen(u->folder), room = UI_COLS - 2u;
    const char *tail = u->folder;
    if (fl > room) { /* too long: its end */
        folder[1] = '~';
        tail += fl - room;
    }
    strcpy(folder + 2, tail);
    set_row(u, 1, UI_ATTR_FAINT, folder);

    draw_list(u);
    set_row(u, ROW_HELP, UI_ATTR_FAINT, " Press: choose. Hold: back");
}

/* The list's rows, in either layout: a marker column, a space, and the name. */
static void draw_list(ui_t *u) {
    unsigned n = item_count(u);
    for (unsigned k = 0; k < list_rows(u); k++) {
        unsigned row = list_first_row(u) + k, i = u->top + k;
        if (i >= n) {
            if (i == n && !u->listed) {
                set_row(u, row, u->list_failed ? UI_ATTR_WARN : UI_ATTR_FAINT,
                        u->list_failed ? "  Can't read this folder" : "  Reading the card...");
            } else if (i == n && u->list->count == 0) {
                set_row(u, row, UI_ATTR_FAINT, "  (no images here)");
            } else {
                set_row(u, row, UI_ATTR_NORMAL, "");
            }
            continue;
        }
        unsigned entry = 0;
        char text[CONFIG_PATH_MAX + 1], shown[ITEM_COLS + 1];
        char mark = ' ';
        switch (item_kind(u, i, &entry)) {
            case ITEM_EJECT:
                strcpy(text, "(eject)");
                break;
            case ITEM_UP:
                strcpy(text, "../");
                break;
            case ITEM_ENTRY:
                snprintf(text, sizeof text, "%s%s", dirlist_name(u->list, entry),
                         dirlist_is_dir(u->list, entry) ? "/" : "");
                mark = marker(u, entry);
                break;
        }
        bool at = i == u->cursor;
        fit(shown, text, item_cols(u), at ? scroll_offset(u, strlen(text), item_cols(u), u->scroll_since) : 0);
        set_rowf(u, row, at ? UI_ATTR_CURSOR : UI_ATTR_NORMAL, "%c %s", mark, shown);
    }
}

/* ---- settings ---------------------------------------------------------------------------------------------- */

static bool busy(const ui_t *u);

/* The choice shown for an option: the one being picked, the one being saved, or the one in force. */
static unsigned shown_choice(const ui_t *u, config_item_t item) {
    if (u->editing && (unsigned)item == u->set_cursor) return u->edit_choice;
    if (busy(u) && u->request.kind == UI_REQ_SET && u->request.item == item) return u->request.choice;
    return config_item_get(&u->unit.config, item);
}

/* The value as shown, in angle brackets while it is being changed. */
static void shown_value(const ui_t *u, config_item_t item, char *out, size_t cap) {
    unsigned k = shown_choice(u, item);
    const char *v = k < config_item_choices(item) ? config_item_choice_label(item, k) : "?";
    bool edit = u->editing && (unsigned)item == u->set_cursor;
    snprintf(out, cap, edit ? "<%s>" : "%s", v);
}

static void draw_settings_rows(ui_t *u) {
    for (unsigned k = 0; k < list_rows(u); k++) {
        unsigned row = list_first_row(u) + k, i = u->set_top + k;
        if (i >= CONFIG_ITEM_COUNT) {
            set_row(u, row, UI_ATTR_NORMAL, "");
            continue;
        }
        config_item_t item = (config_item_t)i;
        char value[CONFIG_ITEM_CHOICE_MAX + 3];
        shown_value(u, item, value, sizeof value);
        ui_attr_t attr = i == u->set_cursor ? UI_ATTR_CURSOR : UI_ATTR_NORMAL;
        if (oled(u)) {
            const char *label = config_item_label(item);
            set_rowf(u, row, attr, "%s%*s", label, (int)(UI_OLED_COLS - strlen(label)), value);
        } else {
            set_rowf(u, row, attr, "  %-*s %s", (int)LABEL_COLS, config_item_label(item), value);
        }
    }
}

static void draw_settings(ui_t *u) {
    set_row(u, 0, UI_ATTR_TITLE, " Settings");
    set_rowf(u, 1, UI_ATTR_FAINT, " RMFE.CFG: %s", config_item_key((config_item_t)u->set_cursor));
    draw_settings_rows(u);
    set_row(u, ROW_HELP, UI_ATTR_FAINT,
            u->editing ? " Turn: change. Press: save. Hold: cancel" : " Press: change. Hold: back");
}

/* ---- the OLED's layouts (ui.h) ------------------------------------------------------------------------------ */

/* A row that scrolls when it is too long: `width` characters of text, from where it has got to since `since`. */
static void set_row_scrolled(ui_t *u, unsigned row, ui_attr_t attr, const char *text, size_t width, uint32_t since) {
    size_t len = strlen(text), from = scroll_offset(u, len, width, since);
    set_row(u, row, attr, text + (from < len ? from : len));
}

/* Each drive's light: "A read T12  B idle", made shorter until it fits. */
static void oled_lights(const ui_t *u, char *out, size_t cap) {
    char drive[IDC_PHYS_DRIVES][16];
    for (unsigned brief = 0; brief < 2u; brief++) {
        for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
            bool w = u->act.drive_writing[d];
            const char *what = !lit(u, d) ? "idle" : brief ? (w ? "wr" : "rd") : (w ? "write" : "read");
            if (lit(u, d) && u->act.drive_has_track[d]) {
                snprintf(drive[d], sizeof drive[d], "%c %s T%u", 'A' + (int)d, what, u->act.drive_track[d]);
            } else {
                snprintf(drive[d], sizeof drive[d], "%c %s", 'A' + (int)d, what);
            }
        }
        snprintf(out, cap, "%s  %s", drive[0], drive[1]);
        if (strlen(out) <= status_cols(u)) return;
        snprintf(out, cap, "%s %s", drive[0], drive[1]);
        if (strlen(out) <= status_cols(u)) return;
    }
}

/* The last command (show-commands): "02 read A/1 T12 R5", without its name if that doesn't fit. */
static void oled_command(const ui_t *u, char *out, size_t cap) {
    const idc_activity_t *a = &u->shown;
    char where[24] = "";
    if (a->has_drive && a->has_track) {
        snprintf(where, sizeof where, " %c/%u T%u %c%u", 'A' + a->drive, a->side, a->track, third_letter(a->cmd),
                 a->third);
    } else if (a->has_drive) {
        snprintf(where, sizeof where, " %c/%u", 'A' + a->drive, a->side);
    }
    snprintf(out, cap, "%02X %s%s", a->cmd, idc_command_name(a->cmd), where);
    if (strlen(out) > status_cols(u)) snprintf(out, cap, "%02X%s", a->cmd, where);
}

static void oled_home(ui_t *u) {
    bool tall = u->layout == UI_LAYOUT_OLED_64;
    size_t width = tall ? UI_OLED_COLS - 1u : UI_OLED_COLS - 3u; /* after " ", or after "A* " */
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
        const ui_drive_t *dr = &u->unit.drive[d];
        bool at = d == u->home_cursor;
        ui_attr_t attr = at && u->cursor_shown ? UI_ATTR_CURSOR : UI_ATTR_NORMAL;
        const char *path = dr->present ? dr->path : "(empty)";
        char shown[UI_OLED_COLS + 1];
        fit(shown, path, width, at && u->cursor_shown ? scroll_offset(u, strlen(path), width, u->scroll_since) : 0);
        char dot = lit(u, d) ? UI_DOT_LIT : UI_DOT_OUT;
        if (tall) {
            set_rowf(u, 2u * d, attr, "%c: %c", 'A' + (int)d, dot);
            set_rowf(u, 2u * d + 1u, attr, " %s", shown);
        } else {
            set_rowf(u, d, attr, "%c%c %s", 'A' + (int)d, dot, shown);
        }
    }
}

static void oled_choose(ui_t *u) {
    if (u->layout == UI_LAYOUT_OLED_64) set_rowf(u, 0, UI_ATTR_TITLE, "Image for %c:", 'A' + (int)u->drive);
    draw_list(u);
}

/* While choosing, with nothing else to say: the folder, cut from the left if it must be, and where the cursor is. */
static void oled_where(const ui_t *u, char *out, size_t cap) {
    char pos[24] = "", start[8];
    if (u->listed && item_count(u)) snprintf(pos, sizeof pos, "%u of %u", u->cursor + 1u, item_count(u));
    snprintf(start, sizeof start, u->layout == UI_LAYOUT_OLED_32 ? "%c: /" : "/", 'A' + (int)u->drive);
    size_t cols = status_cols(u), used = strlen(pos) + (pos[0] ? 1u : 0u) + strlen(start);
    /* at most "514 of 514" (DIRLIST_MAX_ENTRIES and the two specials) after "A: /": room for 6 at least */
    size_t room = used + 2u < cols ? cols - used : 2u;
    const char *folder = u->folder;
    size_t fl = strlen(folder);
    char shown[CONFIG_PATH_MAX + 1];
    if (fl > room) { /* its end, after a "~" */
        snprintf(shown, sizeof shown, "~%s", folder + fl - (room - 1u));
    } else {
        snprintf(shown, sizeof shown, "%s", folder);
    }
    snprintf(out, cap, "%s%s%*s", start, shown, (int)(cols - strlen(start) - strlen(shown)), pos);
}

static void oled_status(ui_t *u) {
    unsigned row = oled_rows(u);
    char text[CONFIG_PATH_MAX + 32];
    if (u->message[0]) {
        set_row_scrolled(u, row, u->message_warn ? UI_ATTR_WARN : UI_ATTR_FAINT, u->message, status_cols(u),
                         u->message_since);
    } else if (u->mode == UI_CHOOSE) {
        if (u->listed && u->list->truncated) {
            set_row(u, row, UI_ATTR_WARN, "Too many to list");
        } else {
            oled_where(u, text, sizeof text);
            set_row(u, row, UI_ATTR_FAINT, text);
        }
    } else if (u->mode == UI_SETTINGS) {
        set_row(u, row, UI_ATTR_FAINT, config_item_key((config_item_t)u->set_cursor));
    } else if (u->unit.usb) {
        set_row(u, row, UI_ATTR_WARN, "On USB: eject on PC");
    } else if (!u->unit.card) {
        set_row(u, row, UI_ATTR_WARN, "No SD card");
    } else if (!u->unit.rom) {
        set_row(u, row, UI_ATTR_WARN, "No IDC ROM");
    } else if (u->show_commands && u->shown_number) {
        oled_command(u, text, sizeof text);
        set_row(u, row, UI_ATTR_FAINT, text);
    } else {
        oled_lights(u, text, sizeof text);
        set_row(u, row, UI_ATTR_FAINT, text);
    }
}

static void draw_oled(ui_t *u) {
    for (unsigned row = 0; row < oled_rows(u); row++)
        set_row(u, row, UI_ATTR_NORMAL, ""); /* what the mode leaves alone */
    switch (u->mode) {
        case UI_HOME:
            oled_home(u);
            break;
        case UI_CHOOSE:
            oled_choose(u);
            break;
        case UI_SETTINGS:
            if (u->layout == UI_LAYOUT_OLED_64) set_row(u, 0, UI_ATTR_TITLE, "Settings");
            draw_settings_rows(u);
            break;
    }
    oled_status(u);
    for (unsigned row = oled_rows(u) + 1u; row < UI_ROWS; row++)
        set_row(u, row, UI_ATTR_NORMAL, "");
}

/* ---- both -------------------------------------------------------------------------------------------------- */

static void draw(ui_t *u) {
    if (oled(u)) {
        draw_oled(u);
        return;
    }
    switch (u->mode) {
        case UI_HOME:
            draw_home(u);
            break;
        case UI_CHOOSE:
            draw_choose(u);
            break;
        case UI_SETTINGS:
            draw_settings(u);
            break;
    }
    if (u->message[0]) {
        set_rowf(u, ROW_MESSAGE, u->message_warn ? UI_ATTR_WARN : UI_ATTR_NORMAL, " %s", u->message);
    } else if (u->mode == UI_CHOOSE && u->listed && u->list->truncated) {
        set_row(u, ROW_MESSAGE, UI_ATTR_WARN, " Too many to list: some are left out");
    } else {
        set_row(u, ROW_MESSAGE, UI_ATTR_NORMAL, "");
    }
}

static void say(ui_t *u, bool warn, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(u->message, sizeof u->message - 1, fmt, ap); /* the row has a space before it */
    va_end(ap);
    u->message_warn = warn;
    u->message_since = u->now;
}

static bool busy(const ui_t *u) {
    return u->request_ready || u->request_in_flight;
}

static void request(ui_t *u, ui_request_kind_t kind, unsigned drive, const char *path) {
    memset(&u->request, 0, sizeof u->request);
    u->request.kind = kind;
    u->request.drive = drive;
    strcpy(u->request.path, path);
    u->request_ready = true;
}

/* Lists `folder` (which may be u->folder itself), with the cursor to go on the entry named `select` (may be NULL)
 * once the list comes. */
static void open_folder(ui_t *u, const char *folder, const char *select) {
    char sel[CONFIG_PATH_MAX];
    snprintf(sel, sizeof sel, "%s", select ? select : "");
    memmove(u->folder, folder, strlen(folder) + 1);
    strcpy(u->select, sel);
    u->listed = false;
    u->list_failed = false;
    u->cursor = 0;
    u->top = 0;
    u->scroll_since = u->now;
    request(u, UI_REQ_LIST, u->drive, u->folder);
}

static void go_home(ui_t *u) {
    u->mode = UI_HOME;
    u->editing = false;
    u->scroll_since = u->now; /* the drive just chosen for shows its cursor, and on the OLED its path scrolls once */
    u->cursor_shown = true;
}

/* Home hides its cursor until the knob is used: the first turn or press only shows it. True if this event was
 * that one. */
static bool wakes_cursor(ui_t *u) {
    if (u->cursor_shown) return false;
    u->cursor_shown = true;
    u->scroll_since = u->now;
    return true;
}

static void start_choosing(ui_t *u, unsigned drive) {
    if (u->unit.usb) {
        say(u, true, "The card is on USB");
        return;
    }
    if (busy(u)) {
        say(u, true, "Busy: try again in a moment");
        return;
    }
    u->mode = UI_CHOOSE;
    u->drive = drive;
    const ui_drive_t *dr = &u->unit.drive[drive];
    char folder[CONFIG_PATH_MAX] = "";
    if (dr->present) (void)dirlist_path_folder(folder, sizeof folder, dr->path);
    open_folder(u, folder, dr->present ? dirlist_path_name(dr->path) : NULL);
}

static void choose(ui_t *u) {
    if (busy(u)) return; /* the list is still coming */
    unsigned entry = 0;
    if (u->cursor >= item_count(u)) return;
    switch (item_kind(u, u->cursor, &entry)) {
        case ITEM_EJECT:
            request(u, UI_REQ_EJECT, u->drive, "");
            say(u, false, "%c: emptying...", 'A' + (int)u->drive);
            go_home(u);
            return;
        case ITEM_UP: {
            char from[CONFIG_PATH_MAX], up[CONFIG_PATH_MAX];
            strcpy(from, dirlist_path_name(u->folder));
            (void)dirlist_path_folder(up, sizeof up, u->folder);
            open_folder(u, up, from);
            return;
        }
        case ITEM_ENTRY:
            break;
    }
    char path[CONFIG_PATH_MAX];
    if (!entry_path(u, entry, path)) {
        say(u, true, "Path too long");
        return;
    }
    if (dirlist_is_dir(u->list, entry)) {
        open_folder(u, path, NULL);
        return;
    }
    const ui_drive_t *mine = &u->unit.drive[u->drive];
    if (mine->present && config_compare_names(mine->path, path) == 0) {
        go_home(u);
        return;
    }
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
        if (d != u->drive && u->unit.drive[d].present && config_compare_names(u->unit.drive[d].path, path) == 0) {
            say(u, true, "That image is in drive %c", 'A' + (int)d);
            return;
        }
    }
    request(u, UI_REQ_INSERT, u->drive, path);
    say(u, false, "%c: inserting...", 'A' + (int)u->drive);
    go_home(u);
}

static void move_cursor(ui_t *u, int by) {
    unsigned n = item_count(u);
    if (n == 0) return;
    if (by < 0 && u->cursor > 0) u->cursor--;
    if (by > 0 && u->cursor + 1 < n) u->cursor++;
    if (u->cursor < u->top) u->top = u->cursor;
    if (u->cursor >= u->top + list_rows(u)) u->top = u->cursor - list_rows(u) + 1;
    u->scroll_since = u->now;
}

static void open_settings(ui_t *u) {
    u->mode = UI_SETTINGS;
    u->set_cursor = 0;
    u->set_top = 0;
    u->editing = false;
}

static void move_setting(ui_t *u, int by) {
    if (u->editing) {
        unsigned n = config_item_choices((config_item_t)u->set_cursor);
        if (by < 0 && u->edit_choice > 0) u->edit_choice--;
        if (by > 0 && u->edit_choice + 1 < n) u->edit_choice++;
        return;
    }
    if (by < 0 && u->set_cursor > 0) u->set_cursor--;
    if (by > 0 && u->set_cursor + 1 < CONFIG_ITEM_COUNT) u->set_cursor++;
    if (u->set_cursor < u->set_top) u->set_top = u->set_cursor;
    if (u->set_cursor >= u->set_top + list_rows(u)) u->set_top = u->set_cursor - list_rows(u) + 1;
}

static void press_setting(ui_t *u) {
    config_item_t item = (config_item_t)u->set_cursor;
    unsigned now_choice = config_item_get(&u->unit.config, item);
    if (!u->editing) {
        if (u->unit.usb && item != CONFIG_ITEM_USB_STORAGE) {
            say(u, true, "The card is on USB");
            return;
        }
        u->editing = true;
        u->edit_choice = now_choice < config_item_choices(item) ? now_choice : 0u;
        return;
    }
    if (u->edit_choice == now_choice) {
        u->editing = false;
        return;
    }
    if (busy(u)) {
        say(u, true, "Busy: try again in a moment");
        return;
    }
    request(u, UI_REQ_SET, 0, "");
    u->request.item = item;
    u->request.choice = u->edit_choice;
    u->editing = false;
    say(u, false, "Saving...");
}

/* ---- the interface ------------------------------------------------------------------------------------------ */

void ui_init(ui_t *u, dirlist_t *list, uint32_t now_ms) {
    RMFE_ASSERT(u && list);
    memset(u, 0, sizeof *u);
    u->list = list;
    u->now = now_ms;
    u->last_input = now_ms;
    u->scroll_since = now_ms;
    config_defaults(&u->unit.config);
    for (unsigned row = 0; row < UI_ROWS; row++)
        u->screen.attr[row] = 0xFF; /* nothing drawn yet: every row is dirty */
    draw(u);
}

void ui_set_layout(ui_t *u, ui_layout_t layout) {
    RMFE_ASSERT(u);
    u->layout = layout;
    for (unsigned row = 0; row < UI_ROWS; row++) {
        u->screen.text[row][0] = '\0'; /* not a padded row: drawn again whatever it holds */
        u->screen.attr[row] = 0xFF;
    }
    if (u->cursor >= u->top + list_rows(u)) u->top = u->cursor - list_rows(u) + 1;
    if (u->set_cursor >= u->set_top + list_rows(u)) u->set_top = u->set_cursor - list_rows(u) + 1;
    draw(u);
}

void ui_set_options(ui_t *u, bool show_commands) {
    RMFE_ASSERT(u);
    u->show_commands = show_commands;
    draw(u);
}

void ui_set_activity(ui_t *u, const ui_activity_t *a, uint32_t now_ms) {
    RMFE_ASSERT(u && a);
    u->now = now_ms;
    for (unsigned d = 0; d < IDC_PHYS_DRIVES; d++) {
        bool serving = a->busy && a->last.stage == IDC_ACT_DRIVE && a->last.drive == d;
        if (a->drive_commands[d] != u->act.drive_commands[d] || serving) u->lit_until[d] = now_ms + UI_LIGHT_HOLD_MS;
    }
    u->act = *a;
    /* a command just started has no drive yet: keep the last one whole, rather than flicker */
    if (a->commands && a->last.stage != IDC_ACT_START) {
        u->shown = a->last;
        u->shown_number = a->commands;
    }
    draw(u);
}

void ui_set_unit(ui_t *u, const ui_unit_t *unit) {
    RMFE_ASSERT(u && unit);
    u->unit = *unit;
    if (u->mode == UI_CHOOSE && u->cursor >= item_count(u)) u->cursor = item_count(u) ? item_count(u) - 1u : 0u;
    draw(u);
}

void ui_event(ui_t *u, rotary_event_t e, uint32_t now_ms) {
    RMFE_ASSERT(u);
    u->now = now_ms;
    u->last_input = now_ms;
    switch (u->mode) {
        case UI_HOME:
            if (e == ROTARY_LONG_PRESS) { /* deliberate, so it needn't wake the cursor first */
                open_settings(u);
                break;
            }
            if (wakes_cursor(u)) break;
            switch (e) {
                case ROTARY_CW:
                    if (u->home_cursor + 1 < IDC_PHYS_DRIVES) u->home_cursor++;
                    u->scroll_since = now_ms; /* the OLED scrolls the picked drive's path once */
                    break;
                case ROTARY_CCW:
                    if (u->home_cursor > 0) u->home_cursor--;
                    u->scroll_since = now_ms;
                    break;
                case ROTARY_PRESS:
                    start_choosing(u, u->home_cursor);
                    break;
                case ROTARY_LONG_PRESS:
                    break;
            }
            break;
        case UI_SETTINGS:
            switch (e) {
                case ROTARY_CW:
                    move_setting(u, 1);
                    break;
                case ROTARY_CCW:
                    move_setting(u, -1);
                    break;
                case ROTARY_PRESS:
                    press_setting(u);
                    break;
                case ROTARY_LONG_PRESS:
                    if (u->editing) {
                        u->editing = false;
                    } else {
                        go_home(u);
                    }
                    break;
            }
            break;
        case UI_CHOOSE:
            switch (e) {
                case ROTARY_CW:
                    move_cursor(u, 1);
                    break;
                case ROTARY_CCW:
                    move_cursor(u, -1);
                    break;
                case ROTARY_PRESS:
                    choose(u);
                    break;
                case ROTARY_LONG_PRESS:
                    go_home(u);
                    break;
            }
            break;
    }
    draw(u);
}

void ui_tick(ui_t *u, uint32_t now_ms) {
    RMFE_ASSERT(u);
    u->now = now_ms;
    if (u->mode != UI_HOME && now_ms - u->last_input >= UI_IDLE_MS) {
        go_home(u);
        u->cursor_shown = false; /* not chosen: left */
    }
    if (u->mode == UI_HOME && now_ms - u->last_input >= UI_CURSOR_MS) u->cursor_shown = false;
    if (u->message[0] && !busy(u) && now_ms - u->message_since >= UI_MESSAGE_MS) u->message[0] = '\0';
    draw(u);
}

bool ui_take_request(ui_t *u, ui_request_t *out) {
    RMFE_ASSERT(u && out);
    if (!u->request_ready) return false;
    *out = u->request;
    u->request_ready = false;
    u->request_in_flight = true;
    draw(u);
    return true;
}

/* The list has come: sort it, and put the cursor on the entry asked for, if it is there. */
static void listed(ui_t *u) {
    dirlist_sort(u->list);
    u->listed = true;
    for (unsigned k = 0; u->select[0] && k < u->list->count; k++) {
        if (config_compare_names(dirlist_name(u->list, k), u->select) == 0) {
            u->cursor = specials(u) + k;
            break;
        }
    }
    if (u->cursor >= u->top + list_rows(u)) u->top = u->cursor - list_rows(u) + 1;
}

void ui_request_done(ui_t *u, bool ok, const char *message, uint32_t now_ms) {
    RMFE_ASSERT(u);
    if (!u->request_in_flight) return;
    u->request_in_flight = false;
    u->now = now_ms;
    unsigned d = u->request.drive;
    switch (u->request.kind) {
        case UI_REQ_LIST:
            /* only if it is still the folder being shown */
            if (u->mode == UI_CHOOSE && strcmp(u->request.path, u->folder) == 0) {
                if (ok) {
                    listed(u);
                } else {
                    u->list_failed = true;
                }
            }
            break;
        case UI_REQ_INSERT:
            if (ok) {
                say(u, false, "%c: %s", 'A' + (int)d, dirlist_path_name(u->request.path));
            } else {
                say(u, true, "%c: %s", 'A' + (int)d, message ? message : "couldn't insert it");
            }
            break;
        case UI_REQ_EJECT:
            if (ok) {
                say(u, false, "%c: empty", 'A' + (int)d);
            } else {
                say(u, true, "%c: %s", 'A' + (int)d, message ? message : "couldn't empty it");
            }
            break;
        case UI_REQ_SET: {
            config_item_t item = u->request.item;
            if (ok) {
                say(u, false, "%s: %s", config_item_label(item), config_item_choice_label(item, u->request.choice));
            } else {
                say(u, true, "%s", message ? message : "couldn't change it");
            }
            break;
        }
    }
    draw(u);
}

void ui_drawn(ui_t *u) {
    RMFE_ASSERT(u);
    u->screen.dirty = 0;
}
