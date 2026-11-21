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
 * mamecmp: the MAME comparison (docs/testing.md, level 2). Replays what the 480Z sent in a MAME trace
 * (tools/mame/mq2trace.lua) into our command layer, built for the PC, and checks that every byte our unit
 * sends, and every baud change, is what the real IDC firmware sent in MAME, in the same order.
 *
 *   mamecmp ROM TRACE WORKDIR [--80-track] [--read-only=A|B ...] [A=IMAGE] [B=IMAGE]
 *
 * --80-track makes both drives 80-track, as MAME's are (rm_mq2.cpp fits 525qd drives); otherwise the drive
 * type follows each image (idc_drive_type_t). --read-only makes that drive's disc write-protected, as MAME
 * does with an image file it can't write.
 * ROM is the IDC ROM (only its boot block is used). Each IMAGE (.imd or .hfe) is copied into WORKDIR as
 * A.<ext> / B.<ext> and served from there, so the inputs are never changed; after the run the copies hold
 * what our unit wrote, for comparison with the images MAME saved. Exit status 0 when everything matched.
 *
 * The trace gives the IDC's side of the link, from its Z80's I/O ports:
 *   RX b    the IDC read b from the link;  TX b  it sent b;
 *   CTC0 v  a write to the baud-rate CTC. The first time constant is the power-up setting (9600); every
 *           later 0D comes from the NMI that the 480Z's RTS raises [IDC3 0066 -> 010b], so it marks a
 *           reset; 01 is the switch to 125000 after command 00 [IDC3 0b1d].
 * Only the order of events is compared, never their timing, with one allowance: when MAME's NMI arrived
 * while the IDC still had bytes to send (the 480Z gave up waiting), our send at that point is answered
 * with the reset too, as the timing would have done. These are counted and reported.
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/disk.h"
#include "core/disk_image.h"
#include "core/idc.h"
#include "core/link.h"
#include "core/rmfe_assert.h"

#define CTC_CONTROL    0x01u /* Z80 CTC: bit 0 set = control word */
#define CTC_TC_FOLLOWS 0x04u /* control word bit 2: the time constant is the next write */
#define CTC_TC_SLOW    0x0Du /* 9600 baud [IDC3 0116] */
#define CTC_TC_FAST    0x01u /* 125000 baud [IDC3 0b1d] */
#define RPM_REVS_PER_S 5.0   /* MAME's 5.25" drives: 300 rpm */
#define SEL_DRIVE_MASK 0x0Fu /* MQ2 port 0E: drive select bits, active low [MAME rm_mq2.cpp] */
#define WD_TYPE2_MASK  0xC0u /* WD1793: 8x/9x read sector, Ax/Bx write sector */
#define WD_TYPE2       0x80u
#define HFE_CELL_BYTES 65536u
#define CONTEXT_BEFORE 12u
#define CONTEXT_AFTER  4u

/* ---- the trace ---------------------------------------------------------------------------------- */

typedef enum { EV_RX, EV_TX, EV_RESET, EV_BAUD } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t value; /* the byte, or the baud rate */
    double t;       /* emulated seconds */
    unsigned line;  /* in the trace file */
} ev_t;

#define MAX_FDC_CMDS 200000u

typedef struct {
    ev_t *ev;
    size_t n, cap;
    /* For the rotation: when MAME started each drive turning, and when the IDC gave the FDC each Type II
     * (read or write sector) command. */
    double motor_on[IDC_PHYS_DRIVES];
    bool motor_seen[IDC_PHYS_DRIVES];
    double *fdc_t;
    size_t nfdc;
} evlist_t;

static void ev_add(evlist_t *l, ev_kind_t kind, uint32_t value, double t, unsigned line) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->ev = realloc(l->ev, l->cap * sizeof *l->ev);
        if (!l->ev) {
            fprintf(stderr, "mamecmp: out of memory\n");
            exit(2);
        }
    }
    l->ev[l->n++] = (ev_t){kind, value, t, line};
}

static bool load_trace(const char *path, evlist_t *l) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "mamecmp: can't open trace %s: %s\n", path, strerror(errno));
        return false;
    }
    char buf[256];
    unsigned line = 0;
    bool tc_next = false, powered = false;
    uint32_t baud = 0;
    while (fgets(buf, sizeof buf, f)) {
        line++;
        double t;
        char name[16];
        unsigned v;
        if (sscanf(buf, "%lf %15s %x", &t, name, &v) != 3) continue; /* comments, KEY lines, MAME's chatter */
        if (strcmp(name, "RX") == 0) {
            ev_add(l, EV_RX, v, t, line);
        } else if (strcmp(name, "TX") == 0) {
            ev_add(l, EV_TX, v, t, line);
        } else if (strcmp(name, "SEL") == 0) {
            /* rm_mq2.cpp port0_w: the drive is ~data & 0F (1 = drive 0, 2 = drive 1); selecting it calls
             * mon_w(0), and the first such call starts its revolution clock (floppy.cpp mon_w) */
            unsigned drive = (~v & SEL_DRIVE_MASK) - 1u;
            if (drive < IDC_PHYS_DRIVES && !l->motor_seen[drive]) {
                l->motor_seen[drive] = true;
                l->motor_on[drive] = t;
            }
        } else if (strcmp(name, "FDC") == 0) {
            if ((v & WD_TYPE2_MASK) == WD_TYPE2 && l->nfdc < MAX_FDC_CMDS) l->fdc_t[l->nfdc++] = t;
        } else if (strcmp(name, "CTC0") == 0) {
            if (!tc_next) {
                tc_next = (v & CTC_CONTROL) && (v & CTC_TC_FOLLOWS);
                continue;
            }
            tc_next = false;
            uint32_t rate;
            if (v == CTC_TC_SLOW) {
                rate = LINK_BAUD_SLOW;
            } else if (v == CTC_TC_FAST) {
                rate = LINK_BAUD_FAST;
            } else {
                /* Before power-up is complete the firmware loads other constants (it is not yet the link). */
                if (!powered) continue;
                fprintf(stderr, "mamecmp: %s:%u: unexpected CTC0 time constant %02X\n", path, line, v);
                fclose(f);
                return false;
            }
            if (powered && rate == LINK_BAUD_SLOW) ev_add(l, EV_RESET, 0, t, line);
            if (!powered || rate != baud) ev_add(l, EV_BAUD, rate, t, line);
            powered = true;
            baud = rate;
        }
    }
    fclose(f);
    if (!powered) {
        fprintf(stderr, "mamecmp: %s: no power-up baud setting: not an mq2trace log?\n", path);
        return false;
    }
    return true;
}

static void ev_describe(const ev_t *e, char *out, size_t cap) {
    switch (e->kind) {
        case EV_RX:
            snprintf(out, cap, "480Z sends %02X", e->value);
            return;
        case EV_TX:
            snprintf(out, cap, "unit sends %02X", e->value);
            return;
        case EV_RESET:
            snprintf(out, cap, "RTS reset (NMI)");
            return;
        case EV_BAUD:
            snprintf(out, cap, "baud -> %u", e->value);
            return;
    }
    snprintf(out, cap, "?");
}

/* ---- the replayed 480Z: a link_io_t that checks the unit against the trace ----------------------- */

typedef struct {
    const evlist_t *trace;
    size_t pos;
    double now; /* time of the last event consumed */
    bool failed;
    size_t resets_in_send;
    size_t rx, tx;
} replay_t;

static void replay_fail(replay_t *r, const char *fmt, ...) {
    if (r->failed) return;
    r->failed = true;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "MISMATCH at trace event %zu: ", r->pos);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    size_t from = r->pos > CONTEXT_BEFORE ? r->pos - CONTEXT_BEFORE : 0;
    for (size_t i = from; i < r->trace->n && i < r->pos + CONTEXT_AFTER; i++) {
        char d[64];
        const ev_t *e = &r->trace->ev[i];
        ev_describe(e, d, sizeof d);
        fprintf(stderr, "  %s line %6u  %11.6f  %s\n", i == r->pos ? "->" : "  ", e->line, e->t, d);
    }
}

static const ev_t *next_ev(replay_t *r) {
    return r->pos < r->trace->n ? &r->trace->ev[r->pos] : NULL;
}

static link_status_t r_recv(void *ctx, uint8_t *byte, bool *parity_error) {
    replay_t *r = ctx;
    if (r->failed) return LINK_CLOSED;
    const ev_t *e = next_ev(r);
    if (!e) return LINK_CLOSED; /* the end of the trace */
    char d[64];
    ev_describe(e, d, sizeof d);
    switch (e->kind) {
        case EV_RX:
            *byte = (uint8_t)e->value;
            *parity_error = false; /* the trace can't show parity errors; the 480Z's software never makes them */
            r->pos++;
            r->rx++;
            r->now = e->t;
            return LINK_OK;
        case EV_RESET:
            r->pos++;
            r->now = e->t;
            return LINK_RESET;
        case EV_TX:
        case EV_BAUD:
            break;
    }
    replay_fail(r, "our unit waits for a byte from the 480Z; the IDC: %s", d);
    return LINK_CLOSED;
}

static link_status_t r_send(void *ctx, uint8_t byte) {
    replay_t *r = ctx;
    if (r->failed) return LINK_CLOSED;
    const ev_t *e = next_ev(r);
    if (!e) {
        replay_fail(r, "our unit sends %02X after the end of the trace", byte);
        return LINK_CLOSED;
    }
    if (e->kind == EV_TX && e->value == byte) {
        r->pos++;
        r->tx++;
        r->now = e->t;
        return LINK_OK;
    }
    if (e->kind == EV_RESET) { /* the 480Z reset the IDC before it sent this byte */
        r->pos++;
        r->now = e->t;
        r->resets_in_send++;
        return LINK_RESET;
    }
    char d[64];
    ev_describe(e, d, sizeof d);
    replay_fail(r, "our unit sends %02X; the IDC: %s", byte, d);
    return LINK_CLOSED;
}

static link_status_t r_drain(void *ctx) {
    replay_t *r = ctx;
    return r->failed ? LINK_CLOSED : LINK_OK;
}

static void r_set_baud(void *ctx, uint32_t baud) {
    replay_t *r = ctx;
    if (r->failed) return;
    const ev_t *e = next_ev(r);
    if (e && e->kind == EV_BAUD && e->value == baud) {
        r->pos++;
        r->now = e->t;
        return;
    }
    char d[64] = "end of trace";
    if (e) ev_describe(e, d, sizeof d);
    replay_fail(r, "our unit sets %u baud; the IDC: %s", baud, d);
}

/* ---- the discs: image files on the PC ------------------------------------------------------------ */

typedef struct {
    bool present, read_only;
    char path[1024], new_path[1040];
    FILE *f, *nf; /* the image, and an IMD's replacement while it is written */
    disk_image_io_t io;
    disk_image_t img;
    uint8_t *cells;
} host_drive_t;

typedef struct {
    host_drive_t drive[IDC_PHYS_DRIVES];
    replay_t *replay;
} host_drives_t;

static bool f_read(void *ctx, uint32_t offset, void *buf, uint32_t len) {
    FILE *f = ((host_drive_t *)ctx)->f;
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

static bool f_write(void *ctx, uint32_t offset, const void *buf, uint32_t len) {
    FILE *f = ((host_drive_t *)ctx)->f;
    return fseek(f, (long)offset, SEEK_SET) == 0 && fwrite(buf, 1, len, f) == len && fflush(f) == 0;
}

/* An IMD rewrite (core/disk_image.h): a new file beside the image, renamed over it once complete. */
static bool f_replace_begin(void *ctx) {
    host_drive_t *hd = ctx;
    snprintf(hd->new_path, sizeof hd->new_path, "%s.new", hd->path);
    hd->nf = fopen(hd->new_path, "wb");
    return hd->nf != NULL;
}

static bool f_replace_sink(void *ctx, const void *buf, uint32_t len) {
    return fwrite(buf, 1, len, ((host_drive_t *)ctx)->nf) == len;
}

static bool open_rw(host_drive_t *hd, uint32_t *len) {
    hd->f = fopen(hd->path, "r+b");
    if (!hd->f || fseek(hd->f, 0, SEEK_END) != 0) return false;
    long n = ftell(hd->f);
    *len = (uint32_t)n;
    return n > 0;
}

static bool f_replace_commit(void *ctx, uint32_t *new_len) {
    host_drive_t *hd = ctx;
    bool ok = fclose(hd->nf) == 0;
    hd->nf = NULL;
    if (!ok) return false;
    fclose(hd->f);
    hd->f = NULL;
    return rename(hd->new_path, hd->path) == 0 && open_rw(hd, new_len);
}

static void f_replace_abort(void *ctx) {
    host_drive_t *hd = ctx;
    fclose(hd->nf);
    hd->nf = NULL;
    (void)remove(hd->new_path); /* best effort: the image itself is untouched */
}

static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return false;
    FILE *b = fopen(to, "wb");
    bool ok = b != NULL;
    char buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, a)) > 0)
        ok = fwrite(buf, 1, n, b) == n;
    ok = ok && !ferror(a);
    fclose(a);
    if (b && fclose(b) != 0) ok = false;
    return ok;
}

static bool drive_open(host_drive_t *hd, const char *image, const char *workdir, char letter) {
    disk_image_kind_t kind;
    if (!disk_image_kind_from_name(image, &kind)) kind = DISK_IMAGE_IMD;
    snprintf(hd->path, sizeof hd->path, "%s/%c.%s", workdir, letter, kind == DISK_IMAGE_HFE ? "hfe" : "imd");
    if (!copy_file(image, hd->path)) {
        fprintf(stderr, "mamecmp: can't copy %s to %s\n", image, hd->path);
        return false;
    }
    hd->io = (disk_image_io_t){hd, f_read, f_write, f_replace_begin, f_replace_sink, f_replace_commit, f_replace_abort};
    hd->cells = malloc(HFE_CELL_BYTES);
    uint32_t len;
    if (!hd->cells || !open_rw(hd, &len) ||
        disk_image_open(&hd->img, kind, &hd->io, len, false, hd->cells, HFE_CELL_BYTES) != DISK_OK) {
        fprintf(stderr, "mamecmp: %s is not a usable image\n", image);
        return false;
    }
    hd->present = true;
    return true;
}

static bool d_present(void *ctx, unsigned drive) {
    const host_drives_t *h = ctx;
    return drive < IDC_PHYS_DRIVES && h->drive[drive].present;
}

static unsigned d_cylinders(void *ctx, unsigned drive) {
    return disk_image_cylinders(&((const host_drives_t *)ctx)->drive[drive].img);
}

static disk_status_t d_load(void *ctx, unsigned drive, unsigned cyl, unsigned side, disk_track_t *t) {
    return disk_image_load(&((host_drives_t *)ctx)->drive[drive].img, cyl, side, t);
}

/* As the firmware does (core/disk_image.h): in place where it can be, else a new file renamed over the old one. */
static disk_status_t d_save(void *ctx, unsigned drive, const disk_track_t *t) {
    host_drive_t *hd = &((host_drives_t *)ctx)->drive[drive];
    if (hd->read_only) return DISK_ERR_READ_ONLY;
    return disk_image_save(&hd->img, t);
}

/*
 * Where a drive's disc is when the IDC gives the FDC its next read or write command, in cells from the index.
 * MAME's index passes at the time the drive was first selected plus whole revolutions, and its IMD tracks are
 * laid out as disk_imd lays them out, so this finds the same one of two sectors with one ID as MAME does (the
 * 4MATION loaders). Checked against the trace: every read of slot k on a 9 x 512 track ends at the same phase.
 */
static uint32_t d_position(void *ctx, unsigned drive) {
    const host_drives_t *h = ctx;
    const evlist_t *l = h->replay->trace;
    double t = h->replay->now;
    size_t lo = 0, hi = l->nfdc; /* the first FDC command at or after now */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (l->fdc_t[mid] < t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < l->nfdc) t = l->fdc_t[lo];
    double t0 = drive < IDC_PHYS_DRIVES && l->motor_seen[drive] ? l->motor_on[drive] : 0.0;
    double revs = (t - t0) * RPM_REVS_PER_S;
    return (uint32_t)((revs - floor(revs)) * DISK_CELLS_PER_REV) % DISK_CELLS_PER_REV;
}

/* ---- main ----------------------------------------------------------------------------------------- */

_Noreturn void rmfe_assert_fail(const char *file, int line, const char *expr) {
    fprintf(stderr, "mamecmp: RMFE_ASSERT(%s) failed at %s:%d\n", expr, file, line);
    abort();
}

static void on_exec(void *ctx, uint16_t address, const uint8_t ram[IDC_RAM_BYTES]) {
    (void)ctx;
    (void)ram;
    fprintf(stderr, "mamecmp: the 480Z asked the unit to run code at %04X (command 0B)\n", address);
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)n);
            if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
                free(buf);
                buf = NULL;
            }
            *len = (size_t)n;
        }
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: mamecmp ROM TRACE WORKDIR [--80-track] [--read-only=A|B ...] [A=IMAGE] [B=IMAGE]\n");
        return 2;
    }
    size_t rom_len = 0;
    uint8_t *rom = read_all(argv[1], &rom_len);
    const uint8_t *block;
    size_t block_len;
    if (!rom || !idc_find_boot_block(rom, rom_len, &block, &block_len)) {
        fprintf(stderr, "mamecmp: %s is missing or not an IDC ROM\n", argv[1]);
        return 2;
    }

    static evlist_t trace;
    trace.fdc_t = malloc(MAX_FDC_CMDS * sizeof *trace.fdc_t);
    if (!trace.fdc_t || !load_trace(argv[2], &trace)) return 2;

    static replay_t replay;
    replay.trace = &trace;
    static host_drives_t drives;
    drives.replay = &replay;
    idc_drive_type_t type = IDC_DRIVE_FOLLOW_IMAGE;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--80-track") == 0) {
            type = IDC_DRIVE_80_TRACK;
            continue;
        }
        if (strcmp(argv[i], "--read-only=A") == 0 || strcmp(argv[i], "--read-only=B") == 0) {
            drives.drive[argv[i][12] - 'A'].read_only = true;
            continue;
        }
        char letter = argv[i][0];
        if ((letter != 'A' && letter != 'B') || argv[i][1] != '=') {
            fprintf(stderr, "mamecmp: drive argument must be A=IMAGE or B=IMAGE: %s\n", argv[i]);
            return 2;
        }
        if (!drive_open(&drives.drive[letter - 'A'], argv[i] + 2, argv[3], letter)) return 2;
    }
    const idc_drives_t ops = {&drives, d_present, d_cylinders, d_load, d_save, d_position};
    const link_io_t io = {&replay, r_recv, r_send, r_drain, r_set_baud};

    static idc_t unit; /* about 17 KB of track caches */
    idc_init(&unit, &io, block, block_len);
    idc_set_drives(&unit, &ops);
    for (unsigned i = 0; i < IDC_PHYS_DRIVES; i++)
        idc_set_drive_type(&unit, i, type);
    unit.exec_hook = on_exec;
    idc_run(&unit);

    for (unsigned i = 0; i < IDC_PHYS_DRIVES; i++) {
        if (drives.drive[i].f) fclose(drives.drive[i].f);
        free(drives.drive[i].cells);
    }
    free(rom);
    if (!replay.failed && replay.pos != trace.n) replay_fail(&replay, "our unit stopped before the end of the trace");
    printf("mamecmp: %zu of %zu trace events replayed: %zu bytes from the 480Z, %zu from the unit; "
           "%zu resets arrived while the IDC was still sending\n",
           replay.pos, trace.n, replay.rx, replay.tx, replay.resets_in_send);
    free(trace.ev);
    free(trace.fdc_t);
    if (replay.failed) {
        printf("mamecmp: FAIL\n");
        return 1;
    }
    printf("mamecmp: match\n");
    return 0;
}
