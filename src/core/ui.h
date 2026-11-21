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
 * The menu on the display (docs/sd-card.md, "Choosing an image"): which image is in each drive, and choosing
 * another, after FlashFloppy's native mode. Driven by the knob's events (core/rotary.h) and a tick count; it draws
 * into a screen of text rows, which the platform puts on the display, and asks the platform for what only it can
 * do (list a folder, put an image in a drive, empty one), one request at a time.
 *
 *   Home: both drives, each with its image's path and kind, and a light: "reading" or "writing" and the track,
 *   while a command for that drive is served and for UI_LIGHT_HOLD_MS after, like a drive's light. With
 *   show-commands (RMFE.CFG), also the last command the unit served. Turn to pick a drive, press to choose its
 *   image.
 *   Choosing: the folder of the drive's image (the root if it is empty), listed by core/dirlist.h, after
 *   "(eject)" when the drive has an image, and ".." when not in the root. Turn to move, press to open a folder or
 *   choose an image; a long press, or UI_IDLE_MS without touching the knob, goes back home. Choosing the image in
 *   the other drive is refused (docs/sd-card.md: the same image can't be in both). The row under the cursor
 *   scrolls a name too long to show.
 *
 * Deterministic: time comes in as milliseconds (wrapping is fine). Nothing is allocated.
 */
#ifndef RMFE_CORE_UI_H
#define RMFE_CORE_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "core/config.h"
#include "core/dirlist.h"
#include "core/idc.h"
#include "core/rotary.h"

#define UI_ROWS            15u /* 16-pixel rows of 8 x 13 characters on the 320 x 240 display */
#define UI_COLS            40u
#define UI_LIST_FIRST_ROW  2u
#define UI_LIST_ROWS       11u    /* rows 2-12 */
#define UI_IDLE_MS         30000u /* choosing: back home after this long without the knob */
#define UI_MESSAGE_MS      4000u
#define UI_SCROLL_PAUSE_MS 1000u /* a long name under the cursor waits this long at each end */
#define UI_SCROLL_STEP_MS  250u  /* and moves a character this often */
#define UI_LIGHT_HOLD_MS   300u  /* a drive's light stays on this long after its last command */

typedef enum {
    UI_ATTR_NORMAL = 0,
    UI_ATTR_TITLE,
    UI_ATTR_CURSOR,
    UI_ATTR_FAINT,
    UI_ATTR_WARN,
    UI_ATTR_READ,  /* a drive's light: reading */
    UI_ATTR_WRITE, /* writing */
} ui_attr_t;

typedef struct {
    char text[UI_ROWS][UI_COLS + 1]; /* NUL-terminated, padded with spaces to UI_COLS */
    uint8_t attr[UI_ROWS];           /* ui_attr_t */
    uint32_t dirty;                  /* a bit per row changed since the platform last drew it (ui_drawn) */
} ui_screen_t;

typedef enum {
    UI_REQ_LIST = 1, /* fill the dirlist with the folder `path` */
    UI_REQ_INSERT,   /* put the image `path` in `drive`, and name it in IMAGE_x.CFG */
    UI_REQ_EJECT,    /* empty `drive` */
} ui_request_kind_t;

typedef struct {
    ui_request_kind_t kind;
    unsigned drive;
    char path[CONFIG_PATH_MAX];
} ui_request_t;

typedef struct {
    bool present;
    bool read_only;
    bool hfe; /* else IMD */
    unsigned cylinders;
    char path[CONFIG_PATH_MAX];
} ui_drive_t;

/* What the platform tells the menu about the unit. */
typedef struct {
    bool card; /* the SD card is mounted */
    bool rom;  /* the IDC ROM's boot block was found */
    ui_drive_t drive[IDC_PHYS_DRIVES];
} ui_unit_t;

/* What the unit has been doing: the platform keeps it from the command layer's activity hook (core/idc.h). */
typedef struct {
    uint32_t commands;                        /* commands started so far */
    bool busy;                                /* `last` is still being served */
    idc_activity_t last;                      /* the last command started (stage: how far it has got) */
    uint32_t drive_commands[IDC_PHYS_DRIVES]; /* commands so far for each drive */
    bool drive_writing[IDC_PHYS_DRIVES];      /* the drive's last command writes (idc_command_writes) */
    bool drive_has_track[IDC_PHYS_DRIVES];
    uint8_t drive_track[IDC_PHYS_DRIVES];
} ui_activity_t;

typedef enum {
    UI_HOME,
    UI_CHOOSE,
} ui_mode_t;

typedef struct {
    ui_screen_t screen;
    ui_unit_t unit;
    dirlist_t *list; /* the platform fills it for UI_REQ_LIST; the menu reads it only once that is done */
    ui_mode_t mode;
    unsigned home_cursor; /* the drive picked at home */
    unsigned drive;       /* the drive being chosen for */
    char folder[CONFIG_PATH_MAX];
    char select[CONFIG_PATH_MAX]; /* the name the cursor goes to when the list comes */
    bool listed;                  /* the list holds `folder` */
    bool list_failed;             /* the folder couldn't be read */
    unsigned cursor, top;
    ui_request_t request;
    bool request_ready;     /* made, not yet taken by the platform */
    bool request_in_flight; /* taken, not yet done */
    char message[UI_COLS + 1];
    bool message_warn;
    uint32_t message_since;
    uint32_t last_input;
    uint32_t scroll_since; /* when the cursor last moved */
    uint32_t now;
    bool show_commands;
    ui_activity_t act;
    uint32_t lit_until[IDC_PHYS_DRIVES];
    idc_activity_t shown;  /* the command on the command row: the last with its drive known, or ended */
    uint32_t shown_number; /* its number (ui_activity_t.commands), 0 for none */
} ui_t;

/* Starts at home, with no card and empty drives until ui_set_unit(). list is the platform's, filled for
 * UI_REQ_LIST requests. */
void ui_init(ui_t *u, dirlist_t *list, uint32_t now_ms);

/* show_commands: RMFE.CFG's show-commands. */
void ui_set_options(ui_t *u, bool show_commands);

/* What the unit has been doing, at now_ms. Call often (every tick): a drive's light comes on for each command it
 * has had since the last call, even one that has already ended. */
void ui_set_activity(ui_t *u, const ui_activity_t *a, uint32_t now_ms);

/* What the unit holds now. Call whenever it changes (and after each request is done). */
void ui_set_unit(ui_t *u, const ui_unit_t *unit);

/* A knob event, at now_ms. */
void ui_event(ui_t *u, rotary_event_t e, uint32_t now_ms);

/* Time passing: timeouts, messages and scrolling. Call every few tens of milliseconds. */
void ui_tick(ui_t *u, uint32_t now_ms);

/* The next request for the platform, if there is one: true and a copy in *out. The menu makes no other until
 * ui_request_done(). */
bool ui_take_request(ui_t *u, ui_request_t *out);

/* The request taken is done. ok false: it failed, and message (may be NULL) says why, in a few words. For
 * UI_REQ_LIST, the dirlist now holds the folder (unsorted; the menu sorts it). */
void ui_request_done(ui_t *u, bool ok, const char *message, uint32_t now_ms);

/* The platform has drawn the rows marked dirty. */
void ui_drawn(ui_t *u);

#endif
