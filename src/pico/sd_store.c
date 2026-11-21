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
 * The SD card through FatFs (third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico): see sd_store.h.
 *
 * Saving an IMD whose records change length writes a new file beside the image, "<image>.new", and renames it over
 * the image once it is complete (core/disk_image.h). FatFs can't rename over an existing file, so the old image is
 * deleted first: if power is lost between the two, there is only the .new, and open_drive() finishes the rename
 * at the next start. A .new beside an image that still exists is an unfinished one, and is deleted.
 */
#include "pico/sd_store.h"

#include "core/disk.h"
#include "core/disk_image.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/time.h"

#include "ff.h"

#define CELL_BYTES   16384u /* one HFE track side's cells (core/disk_image.h) */
#define TEXT_MAX     2048u  /* the most of a configuration file read */
#define US_PER_CELL  2u     /* core/disk.h: 2 us cells, DISK_CELLS_PER_REV to the revolution */
#define NEW_SUFFIX   ".new"
#define NEW_PATH_MAX (CONFIG_PATH_MAX + sizeof NEW_SUFFIX)

typedef struct {
    bool present;
    char letter;
    char path[CONFIG_PATH_MAX], new_path[NEW_PATH_MAX];
    FIL f, nf; /* the image, and an IMD's replacement while it is written */
    disk_image_io_t io;
    disk_image_t img;
    uint64_t index_us; /* when the disc started turning: its rotational position counts from here */
} sd_drive_t;

static FATFS fs;
static bool mounted;
static char cfg_dir[8] = ""; /* "" or "RMFE/": where RMFE.CFG and the IMAGE and INIT files are */
/* Three slots: the two drives' images, and one being opened for the menu (core/ui.h), a step at a time between
 * commands, which then takes a drive's place. */
static sd_drive_t slots[IDC_PHYS_DRIVES + 1];
static sd_drive_t *drives[IDC_PHYS_DRIVES];
static sd_drive_t *opening; /* the slot being opened, or NULL */
static unsigned opening_for;
static const config_t *cfg;       /* the settings in force (sd_store_insert_startup_images) */
static uint8_t cells[CELL_BYTES]; /* shared: the drives are only ever used one at a time */
static char text[TEXT_MAX];

/* ---- files ---------------------------------------------------------------------------------------- */

static bool exists(const char *path, FILINFO *fi) {
    return f_stat(path, fi) == FR_OK;
}

/* A whole small file into buf; false if it's missing, unreadable or bigger than cap. */
static bool read_file(const char *path, void *buf, size_t cap, size_t *len) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    UINT got = 0;
    bool ok = f_size(&f) <= cap && f_read(&f, buf, (UINT)cap, &got) == FR_OK;
    (void)f_close(&f); /* reading only: nothing to lose */
    *len = got;
    return ok;
}

/* A configuration file's text, or false if it isn't there. One longer than TEXT_MAX is read only that far. */
static bool read_text(const char *name, size_t *len) {
    char path[sizeof cfg_dir + 16];
    snprintf(path, sizeof path, "%s%s", cfg_dir, name);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    UINT got = 0;
    FRESULT fr = f_read(&f, text, sizeof text, &got);
    if (f_size(&f) > sizeof text) printf("SD: %s is over %u bytes: only the start is read\n", path, TEXT_MAX);
    (void)f_close(&f);
    *len = got;
    return fr == FR_OK;
}

/* ---- the drives' file operations (core/disk_image.h) ------------------------------------------------- */

/*
 * Reads go through a read-ahead cache of one block of one drive's image: the image code reads in small pieces
 * (128 bytes at a time when an IMD is rewritten), and each small FatFs read costs about 0.37 ms on its own.
 */
static uint8_t cache[4096];
static const sd_drive_t *cache_drive; /* NULL: the cache holds nothing */
static uint32_t cache_off, cache_len;

static void cache_forget(const sd_drive_t *d) {
    if (cache_drive == d) cache_drive = NULL;
}

static bool d_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    sd_drive_t *d = ctx;
    uint8_t *out = buf;
    while (len) {
        if (cache_drive != d || off < cache_off || off >= cache_off + cache_len) {
            UINT got = 0;
            cache_drive = NULL;
            if (f_lseek(&d->f, off) != FR_OK || f_read(&d->f, cache, sizeof cache, &got) != FR_OK || got == 0) {
                return false;
            }
            cache_drive = d;
            cache_off = off;
            cache_len = got;
        }
        uint32_t n = cache_off + cache_len - off;
        if (n > len) n = len;
        memcpy(out, cache + (off - cache_off), n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

static bool d_write(void *ctx, uint32_t off, const void *buf, uint32_t len) {
    sd_drive_t *d = ctx;
    UINT put = 0;
    cache_forget(d);
    return f_lseek(&d->f, off) == FR_OK && f_write(&d->f, buf, len, &put) == FR_OK && put == len;
}

/* The replacement is written through this buffer: disk_imd_rewrite() hands it over 128 bytes at a time, and the
 * card is much faster at writing many sectors at once than one at a time (a 300 KB image took 4.4 s unbuffered). */
static uint8_t sink_buf[16384];
static uint32_t sink_n;

static bool sink_flush(sd_drive_t *d) {
    UINT put = 0;
    bool ok = sink_n == 0 || (f_write(&d->nf, sink_buf, sink_n, &put) == FR_OK && put == sink_n);
    sink_n = 0;
    return ok;
}

static bool d_replace_begin(void *ctx) {
    sd_drive_t *d = ctx;
    sink_n = 0;
    return f_open(&d->nf, d->new_path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
}

static bool d_replace_sink(void *ctx, const void *buf, uint32_t len) {
    sd_drive_t *d = ctx;
    const uint8_t *p = buf;
    while (len) {
        uint32_t n = len < sizeof sink_buf - sink_n ? len : (uint32_t)sizeof sink_buf - sink_n;
        memcpy(sink_buf + sink_n, p, n);
        sink_n += n;
        p += n;
        len -= n;
        if (sink_n == sizeof sink_buf && !sink_flush(d)) return false;
    }
    return true;
}

static bool reopen(sd_drive_t *d, uint32_t *len) {
    cache_forget(d);
    if (f_open(&d->f, d->path, FA_READ | FA_WRITE) != FR_OK) return false;
    *len = (uint32_t)f_size(&d->f);
    return true;
}

static bool d_replace_commit(void *ctx, uint32_t *new_len) {
    sd_drive_t *d = ctx;
    if (!sink_flush(d) || f_close(&d->nf) != FR_OK) {
        (void)f_close(&d->nf);
        (void)f_unlink(d->new_path);
        return false; /* the image is untouched */
    }
    if (f_close(&d->f) != FR_OK || f_unlink(d->path) != FR_OK) {
        (void)f_unlink(d->new_path);
        uint32_t len;
        if (!reopen(d, &len)) d->present = false; /* still the old image, if it can be opened at all */
        return false;
    }
    if (f_rename(d->new_path, d->path) != FR_OK) {
        printf("SD: %c: couldn't rename %s: the image is in %s\n", d->letter, d->new_path, d->new_path);
        d->present = false;
        return false;
    }
    return reopen(d, new_len);
}

static void d_replace_abort(void *ctx) {
    sd_drive_t *d = ctx;
    sink_n = 0;
    (void)f_close(&d->nf);
    (void)f_unlink(d->new_path); /* best effort: the image itself is untouched */
}

/* ---- idc_drives_t ---------------------------------------------------------------------------------- */

static bool s_present(void *ctx, unsigned drive) {
    (void)ctx;
    return drive < IDC_PHYS_DRIVES && drives[drive]->present;
}

static unsigned s_cylinders(void *ctx, unsigned drive) {
    (void)ctx;
    return disk_image_cylinders(&drives[drive]->img);
}

static disk_status_t s_load(void *ctx, unsigned drive, unsigned cyl, unsigned side, disk_track_t *t) {
    (void)ctx;
    return disk_image_load(&drives[drive]->img, cyl, side, t);
}

static disk_status_t s_save(void *ctx, unsigned drive, const disk_track_t *t) {
    (void)ctx;
    sd_drive_t *d = drives[drive];
    disk_status_t st = disk_image_save(&d->img, t);
    if (st == DISK_OK && f_sync(&d->f) != FR_OK) st = DISK_ERR_IO; /* on the card before the 480Z hears OK */
    if (st != DISK_OK && st != DISK_ERR_READ_ONLY)
        printf("SD: %c: saving cylinder %u side %u failed (%d)\n", d->letter, t->cyl, t->side, st);
    return st;
}

/* Each disc turns at 300 rpm from when its image went in. */
static uint32_t s_position(void *ctx, unsigned drive) {
    (void)ctx;
    uint64_t cells_since = (time_us_64() - drives[drive]->index_us) / US_PER_CELL;
    return (uint32_t)(cells_since % DISK_CELLS_PER_REV);
}

static const idc_drives_t sd_drives = {NULL, s_present, s_cylinders, s_load, s_save, s_position};

const idc_drives_t *sd_store_drives(void) {
    return &sd_drives;
}

/* ---- putting images in ----------------------------------------------------------------------------- */

/* Starts opening the image at path into slot d, for the drive with that letter: the checks, finishing or deleting
 * an interrupted save, opening the file and the first step of reading it (disk_image_open_begin). On failure *why
 * says why in a few words (for the menu), and the console has the details. */
static bool open_begin(sd_drive_t *d, char letter, const char *path, const config_t *c, const char **why) {
    d->letter = letter;
    d->present = false;
    disk_image_kind_t kind;
    if (strlen(path) >= sizeof d->path || !disk_image_kind_from_name(path, &kind)) {
        printf("SD: %c: %s isn't an IMD or HFE image\n", letter, path);
        *why = "not an IMD or HFE image";
        return false;
    }
    snprintf(d->new_path, sizeof d->new_path, "%s%s", path, NEW_SUFFIX);
    FILINFO fi;
    bool have_new = exists(d->new_path, &fi);
    if (!exists(path, &fi)) {
        if (!have_new || f_rename(d->new_path, path) != FR_OK || !exists(path, &fi)) {
            printf("SD: %c: %s isn't on the card\n", letter, path);
            *why = "not on the card";
            return false;
        }
        printf("SD: %c: finished saving %s, interrupted when the power went\n", letter, path);
    } else if (have_new) {
        (void)f_unlink(d->new_path);
        printf("SD: %c: deleted %s, an unfinished save; the image is as it was before it\n", letter, d->new_path);
    }
    cache_forget(d);
    bool read_only = c->write_protect || (fi.fattrib & AM_RDO);
    if (f_open(&d->f, path, FA_READ | (read_only ? 0 : FA_WRITE)) != FR_OK) {
        printf("SD: %c: can't open %s\n", letter, path);
        *why = "can't open it";
        return false;
    }
    strcpy(d->path, path);
    d->io = (disk_image_io_t){d, d_read, d_write, d_replace_begin, d_replace_sink, d_replace_commit, d_replace_abort};
    disk_status_t st =
        disk_image_open_begin(&d->img, kind, &d->io, (uint32_t)f_size(&d->f), read_only, cells, sizeof cells);
    if (st != DISK_OK) {
        (void)f_close(&d->f);
        printf("SD: %c: %s isn't a usable image (%d)\n", letter, path, st);
        *why = "not a usable image";
        return false;
    }
    return true;
}

/* One more step of opening: a track record (disk_image_open_step). */
static bool open_step(sd_drive_t *d, bool *done, const char **why) {
    disk_status_t st = disk_image_open_step(&d->img, done);
    if (st == DISK_OK) return true;
    (void)f_close(&d->f);
    cache_forget(d);
    printf("SD: %c: %s isn't a usable image (%d)\n", d->letter, d->path, st);
    *why = "not a usable image";
    return false;
}

/* The image is open: it is in the drive, and the disc starts turning. */
static void open_finish(sd_drive_t *d) {
    d->present = true;
    d->index_us = time_us_64();
    printf("SD: %c: %s (%s, %u cylinders%s)\n", d->letter, d->path, d->img.kind == DISK_IMAGE_HFE ? "HFE" : "IMD",
           disk_image_cylinders(&d->img), d->img.read_only ? ", write-protected" : "");
}

/* The whole of it at once: at start-up, when nothing waits. */
static bool open_drive(unsigned n, const char *path, const config_t *c) {
    sd_drive_t *d = drives[n];
    const char *why;
    bool done = false;
    if (!open_begin(d, (char)('A' + n), path, c, &why)) return false;
    while (!done) {
        if (!open_step(d, &done, &why)) return false;
    }
    open_finish(d);
    return true;
}

/* The first image in the root, as FlashFloppy's native mode lists them (core/config.h). */
static bool first_listed_image(char path[CONFIG_PATH_MAX]) {
    DIR dir;
    FILINFO fi;
    bool found = false;
    if (f_opendir(&dir, "/") != FR_OK) return false;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & AM_DIR) || !config_is_listed_image(fi.fname, fi.fattrib & AM_HID)) continue;
        if (strlen(fi.fname) >= CONFIG_PATH_MAX) continue;
        if (!found || config_compare_names(fi.fname, path) < 0) {
            strcpy(path, fi.fname);
            found = true;
        }
    }
    (void)f_closedir(&dir);
    return found;
}

/* The image named in IMAGE_x.CFG or INIT_x.CFG, as image-on-startup says; false for init or no name. */
static bool named_image(unsigned n, const config_t *c, char path[CONFIG_PATH_MAX]) {
    const char *stem = c->image_on_startup == CONFIG_STARTUP_LAST     ? "IMAGE"
                       : c->image_on_startup == CONFIG_STARTUP_STATIC ? "INIT"
                                                                      : NULL;
    if (!stem) return false;
    char name[16];
    snprintf(name, sizeof name, "%s_%c.CFG", stem, drives[n]->letter);
    size_t len;
    if (!read_text(name, &len)) return false;
    if (config_image_path(text, len, path, CONFIG_PATH_MAX)) return true;
    printf("SD: %s%s names no image\n", cfg_dir, name);
    return false;
}

void sd_store_insert_startup_images(const config_t *c) {
    char path[CONFIG_PATH_MAX];
    cfg = c;
    for (unsigned n = 0; n < IDC_PHYS_DRIVES; n++) {
        drives[n] = &slots[n];
        drives[n]->letter = (char)('A' + n);
        drives[n]->present = false;
    }
    if (!mounted) return;
    for (unsigned n = 0; n < IDC_PHYS_DRIVES; n++) {
        bool named = named_image(n, c, path);
        if (named && n == 1 && drives[0]->present && config_compare_names(path, drives[0]->path) == 0) {
            printf("SD: B: %s is already in A\n", path);
            continue;
        }
        if (named && open_drive(n, path, c)) continue;
        /* init, or the named image wouldn't go in: A gets the first image on the card, B none */
        if (n == 0 && first_listed_image(path)) (void)open_drive(n, path, c);
        if (!drives[n]->present) printf("SD: %c: empty\n", drives[n]->letter);
    }
}

/* ---- the card, its configuration and the ROM --------------------------------------------------------- */

static void warn(void *ctx, unsigned line, const char *why) {
    (void)ctx;
    printf("SD: %sRMFE.CFG line %u ignored: %s\n", cfg_dir, line, why);
}

bool sd_store_mount(void) {
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD: no card, or it can't be read (FatFs %d)\n", fr);
        return false;
    }
    mounted = true;
    FILINFO fi;
    if (exists("RMFE", &fi) && (fi.fattrib & AM_DIR)) strcpy(cfg_dir, "RMFE/");
    printf("SD: card mounted (%s)\n", fs.fs_type == FS_EXFAT ? "exFAT" : fs.fs_type == FS_FAT32 ? "FAT32" : "FAT");
    return true;
}

void sd_store_read_config(config_t *c) {
    config_defaults(c);
    size_t len;
    if (!mounted || !read_text("RMFE.CFG", &len)) return;
    printf("SD: reading %sRMFE.CFG\n", cfg_dir);
    config_parse(c, text, len, warn, NULL);
}

static bool rom_ok(const char *path, uint8_t *rom, size_t *len) {
    const uint8_t *blk;
    size_t blen;
    return read_file(path, rom, SD_STORE_ROM_MAX, len) && idc_find_boot_block(rom, *len, &blk, &blen);
}

static bool is_rom_name(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && name[0] != '.' && strcasecmp(dot, ".rom") == 0;
}

bool sd_store_read_rom(const config_t *c, uint8_t rom[SD_STORE_ROM_MAX], size_t *len) {
    if (!mounted) return false;
    if (rom_ok(c->idc_rom, rom, len)) {
        printf("SD: IDC ROM %s\n", c->idc_rom);
        return true;
    }
    printf("SD: %s isn't an IDC ROM with a boot block; looking for another\n", c->idc_rom);
    char best[CONFIG_PATH_MAX] = "";
    DIR dir;
    FILINFO fi;
    if (f_opendir(&dir, "/") == FR_OK) {
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            if ((fi.fattrib & AM_DIR) || !is_rom_name(fi.fname) || strlen(fi.fname) >= sizeof best) continue;
            if ((!best[0] || config_compare_names(fi.fname, best) < 0) && rom_ok(fi.fname, rom, len)) {
                strcpy(best, fi.fname);
            }
        }
        (void)f_closedir(&dir);
    }
    if (best[0] && rom_ok(best, rom, len)) {
        printf("SD: IDC ROM %s\n", best);
        return true;
    }
    printf("SD: no IDC ROM: command FF (the boot block) will be answered as unknown\n");
    return false;
}

/* ---- the menu's work, a step at a time (core/ui.h requests; sd_store.h) ----------------------------------------- */

static DIR list_dir;
static bool listing;

bool sd_store_list_begin(const char *folder) {
    if (listing) (void)f_closedir(&list_dir);
    listing = false;
    if (!mounted) return false;
    char path[CONFIG_PATH_MAX + 1];
    snprintf(path, sizeof path, "/%s", folder);
    FRESULT fr = f_opendir(&list_dir, path);
    if (fr != FR_OK) {
        printf("SD: can't list %s (FatFs %d)\n", path, fr);
        return false;
    }
    listing = true;
    return true;
}

sd_step_t sd_store_list_step(dirlist_t *l) {
    if (!listing) return SD_STEP_FAILED;
    FILINFO fi;
    FRESULT fr = f_readdir(&list_dir, &fi);
    if (fr != FR_OK || !fi.fname[0]) {
        (void)f_closedir(&list_dir);
        listing = false;
        if (fr != FR_OK) printf("SD: listing a folder failed (FatFs %d)\n", fr);
        return fr == FR_OK ? SD_STEP_DONE : SD_STEP_FAILED;
    }
    (void)dirlist_add(l, fi.fname, (fi.fattrib & AM_DIR) != 0, (fi.fattrib & AM_HID) != 0);
    return SD_STEP_MORE;
}

bool sd_store_insert_begin(unsigned drive, const char *path, const char **why) {
    if (!mounted || !cfg) {
        *why = "no SD card";
        return false;
    }
    for (unsigned n = 0; n < IDC_PHYS_DRIVES; n++) {
        if (n != drive && drives[n]->present && config_compare_names(drives[n]->path, path) == 0) {
            *why = "it is in the other drive"; /* docs/sd-card.md: the same image can't be in both */
            return false;
        }
    }
    /* the free slot: the one neither drive uses */
    opening = NULL;
    for (unsigned k = 0; k < IDC_PHYS_DRIVES + 1u && !opening; k++) {
        if (&slots[k] != drives[0] && &slots[k] != drives[1]) opening = &slots[k];
    }
    opening_for = drive;
    if (!open_begin(opening, (char)('A' + drive), path, cfg, why)) {
        opening = NULL;
        return false;
    }
    return true;
}

sd_step_t sd_store_insert_step(const char **why) {
    if (!opening) return SD_STEP_FAILED;
    bool done = false;
    if (!open_step(opening, &done, why)) {
        opening = NULL;
        return SD_STEP_FAILED;
    }
    return done ? SD_STEP_DONE : SD_STEP_MORE;
}

/* Takes a drive's image out: its file closed, the drive empty. */
static void take_out(unsigned drive) {
    sd_drive_t *d = drives[drive];
    if (!d->present) return;
    cache_forget(d);
    if (f_close(&d->f) != FR_OK) printf("SD: %c: closing %s failed\n", d->letter, d->path);
    d->present = false;
    printf("SD: %c: %s taken out\n", d->letter, d->path);
}

void sd_store_insert_commit(void) {
    if (!opening) return;
    unsigned drive = opening_for;
    take_out(drive);
    drives[drive] = opening;
    opening = NULL;
    open_finish(drives[drive]);
}

void sd_store_eject(unsigned drive) {
    take_out(drive);
    printf("SD: %c: empty\n", drives[drive]->letter);
}

bool sd_store_save_choice(unsigned drive) {
    if (!mounted) return false;
    char name[sizeof cfg_dir + 16];
    snprintf(name, sizeof name, "%sIMAGE_%c.CFG", cfg_dir, (char)('A' + drive));
    FIL f;
    if (f_open(&f, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        printf("SD: can't write %s\n", name);
        return false;
    }
    const sd_drive_t *d = drives[drive];
    UINT put = 0;
    bool ok = true;
    if (d->present) {
        size_t n = strlen(d->path);
        ok = f_write(&f, d->path, (UINT)n, &put) == FR_OK && put == n && f_write(&f, "\n", 1, &put) == FR_OK;
    }
    ok = f_close(&f) == FR_OK && ok;
    if (!ok) printf("SD: writing %s failed\n", name);
    return ok;
}

void sd_store_unit(ui_unit_t *u) {
    memset(u, 0, sizeof *u);
    u->card = mounted;
    for (unsigned n = 0; n < IDC_PHYS_DRIVES; n++) {
        const sd_drive_t *d = drives[n];
        ui_drive_t *out = &u->drive[n];
        if (!d || !d->present) continue;
        out->present = true;
        out->read_only = d->img.read_only;
        out->hfe = d->img.kind == DISK_IMAGE_HFE;
        out->cylinders = disk_image_cylinders(&d->img);
        strcpy(out->path, d->path);
    }
}
