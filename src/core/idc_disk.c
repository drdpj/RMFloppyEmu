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
 * The disc commands (docs/protocol.md "Command table", "Parameter bytes"). Each follows the IDC's order
 * of replies: parameters, then a status byte (00, or 20/80: stop), then the operation and its result.
 */
#include "core/idc_disk.h"

#include "core/rmfe_assert.h"

#include <string.h>

typedef struct {
    unsigned phys, side; /* after the drive map */
    bool mfm;
} target_t;

/* Reads the drive byte and decodes it through the drive map [IDC3 0e83, 0f77, 0527]. */
static link_status_t get_drive(idc_t *d, target_t *tg, uint8_t *param) {
    uint8_t raw;
    link_status_t st = link_get(&d->link, &raw);
    if (st != LINK_OK) return st;
    uint8_t p = idc_param_byte(raw);
    uint8_t logical = d->drive_map[p & 7u];
    tg->phys = logical >> 1;
    tg->side = logical & IDC_PARAM_SIDE;
    tg->mfm = (p & IDC_PARAM_MFM) != 0;
    if (param) *param = p;
    return LINK_OK;
}

static bool ready(const idc_t *d, unsigned phys) {
    return d->drives && phys < IDC_PHYS_DRIVES && d->drives->present(d->drives->ctx, phys);
}

/* Whether a drive is 80-track: its type setting (the IDC's stepping test), or else the image's cylinder count. */
static bool qd_drive(idc_t *d, unsigned phys) {
    switch (d->drive_type[phys]) {
        case IDC_DRIVE_40_TRACK:
            return false;
        case IDC_DRIVE_80_TRACK:
            return true;
        case IDC_DRIVE_FOLLOW_IMAGE:
            break;
    }
    return d->drives->cylinders(d->drives->ctx, phys) >= IDC_QD_CYLINDERS;
}

/* The tracks the IDC lets the 480Z ask for: 50h once it has found an 80-track drive [IDC3 028b, checked at
 * 0f14, 0d07, 0d3b], else 40. Past the end of a shorter disc a track reads as blank, as on a real drive. */
#define IDC_QD_TRACKS 80u
#define IDC_DD_TRACKS 40u
static unsigned drive_tracks(idc_t *d, unsigned phys) {
    return qd_drive(d, phys) ? IDC_QD_TRACKS : IDC_DD_TRACKS;
}

static void set_sticky(idc_t *d, uint8_t status) {
    if (d->sticky == IDC_STATUS_OK) d->sticky = status;
}

/* Forgets what was written to a drive's buffers: they have reached the disc, or failed to. */
static void clear_written(idc_cache_t *c) {
    memset(c->written, 0, sizeof c->written);
    c->missing = 0;
}

/* Saves a drive's cached track. When the write fails the IDC still marks its buffer clean [IDC3 1198], so
 * the change is lost: we drop the track, and later reads see what the disc holds (MAME trace write_protect).
 * Returns the plain IDC status. */
static uint8_t save_track(idc_t *d, unsigned phys) {
    idc_cache_t *c = &d->cache[phys];
    disk_status_t st = d->drives->save(d->drives->ctx, phys, &c->t);
    c->dirty = false;
    if (st == DISK_OK) return IDC_STATUS_OK;
    c->valid = false;
    clear_written(c);
    return st == DISK_ERR_READ_ONLY ? IDC_STATUS_WRITE_PROTECT : IDC_STATUS_NOT_READY;
}

/*
 * Writes a drive's cached track back if it changed. A buffer written to a sector the track doesn't have fails
 * (the IDC reads the sector first to fill the rest of the buffer [IDC3 112a -> 1137]); the others are written
 * anyway, as the IDC goes on through its buffers [1072-1086]. Returns the plain IDC status (the first error);
 * the cache's callers add IDC_STATUS_WRITE_BACK.
 */
static uint8_t flush_drive(idc_t *d, unsigned phys) {
    idc_cache_t *c = &d->cache[phys];
    if (!c->valid) return IDC_STATUS_OK;
    uint8_t res = c->missing ? IDC_STATUS_NOT_FOUND : IDC_STATUS_OK;
    clear_written(c);
    if (c->dirty) {
        uint8_t st = save_track(d, phys);
        if (res == IDC_STATUS_OK) res = st;
    }
    return res;
}

/* The track at cyl/side of a drive, from the cache or loaded, writing the old one back first (an error doing
 * that has IDC_STATUS_WRITE_BACK added, as when the IDC frees a buffer [IDC3 10c5, 109a -> 109f]). */
static uint8_t get_track(idc_t *d, unsigned phys, unsigned cyl, unsigned side, disk_track_t **out) {
    idc_cache_t *c = &d->cache[phys];
    if (!(c->valid && c->t.cyl == cyl && c->t.side == side)) {
        uint8_t st = flush_drive(d, phys);
        if (st != IDC_STATUS_OK) return (uint8_t)(st | IDC_STATUS_WRITE_BACK);
        c->valid = false;
        if (d->drives->load(d->drives->ctx, phys, cyl, side, &c->t) != DISK_OK) return IDC_STATUS_NOT_READY;
        c->valid = true;
        c->dirty = false;
        clear_written(c);
    }
    *out = &c->t;
    return IDC_STATUS_OK;
}

/*
 * Records per track by format, whatever the track holds [IDC3 084b, a table of the first record past the end;
 * checked at 0f2b]: 5.25" FM is 16 x 128, MFM 9 x 512 = 36 records.
 */
#define IDC_FM_RECORDS     16u
#define IDC_MFM_RECORDS    36u
#define IDC_MFM_SECTORS    9u
#define IDC_MFM_PER_SECTOR 4u

/* 128-byte blocks per sector: from the track's first sector, else (no track, or none on it) from the format
 * (FM 1, MFM 4). */
static unsigned blocks_per_sector(const disk_track_t *t, bool mfm) {
    if (!t || t->count == 0) return mfm ? IDC_MFM_PER_SECTOR : 1u;
    unsigned per = t->secs[0].size / LINK_RECORD;
    return per ? per : 1u;
}

/*
 * The sector ID holding a record, and the record's offset in it. FM: the ID is the record (on track 0 the
 * IDC reads IDs in sequence; elsewhere its two FM tables at 0957 and 099b compose to the identity). MFM:
 * logical sector k = (record - 1) / 4 is laid down with a 2:1 skew, IDs 1 6 2 7 3 8 4 9 5 [IDC3 09c7; MAME
 * boot trace: track 01 record 05 is ID 6], i.e. ID = 5k mod 9 + 1. MFM on track 0 has not been traced, nor
 * worked out from the ROM (its cache fill reads IDs in sequence there [IDC3 0746]): it is left unskewed.
 */
static uint8_t record_sector(uint8_t record, uint8_t track, bool mfm, uint32_t *offset) {
    if (!mfm) {
        *offset = 0;
        return record;
    }
    unsigned k = (record - 1u) / IDC_MFM_PER_SECTOR;
    *offset = ((record - 1u) % IDC_MFM_PER_SECTOR) * LINK_RECORD;
    return (uint8_t)(track == 0 ? k + 1u : (k * 5u) % IDC_MFM_SECTORS + 1u);
}

typedef struct {
    target_t tg;
    bool by_id; /* 0E/0F/10: the third byte is a sector ID, not a record */
    uint8_t track, record;
    disk_track_t *t;
    uint8_t track_status; /* from getting the track (an error writing the old one back, or loading it) */
    int idx;              /* sector index, -1 if not found */
    uint32_t offset;      /* of the record within the sector */
} rw_t;

/*
 * Reads drive, track and record, then sends the status byte [IDC3 0f08]: 80 not ready, 20 bad track;
 * a record out of range is answered 20 at once [IDC3 0f2b]. Then, as the IDC goes to its cache only after
 * the status [IDC3 0f2e, then 10ba/108f], gets the track: an error there is the command's result, not its
 * status (rw->track_status). Returns LINK_OK with *go set when the command may proceed.
 */
static link_status_t rw_params(idc_t *d, rw_t *rw, bool *go) {
    *go = false;
    link_status_t st = get_drive(d, &rw->tg, NULL);
    if (st == LINK_OK) st = link_get(&d->link, &rw->track);
    if (st == LINK_OK) st = link_get(&d->link, &rw->record);
    if (st != LINK_OK) return st;
    idc_note_drive(d, rw->tg.phys, rw->tg.side, true, rw->track, rw->record);

    bool ok = ready(d, rw->tg.phys);
    if (!ok) set_sticky(d, IDC_STATUS_NOT_READY);
    if (ok && rw->track >= drive_tracks(d, rw->tg.phys)) {
        set_sticky(d, IDC_STATUS_BAD_PARAM);
        ok = false;
    }
    /* The IDC range-checks the third byte the same way for records and sector IDs, by the format
     * [IDC3 0f2b]. It doesn't reject 0; what it then does with it is not known, so we answer 20. */
    unsigned records = rw->tg.mfm ? IDC_MFM_RECORDS : IDC_FM_RECORDS;
    if (ok && (rw->record == 0 || rw->record > records)) return link_put(&d->link, IDC_STATUS_BAD_PARAM);
    st = link_put(&d->link, d->sticky);
    *go = st == LINK_OK && d->sticky == IDC_STATUS_OK;
    if (!*go) return st;

    rw->t = NULL;
    rw->idx = -1;
    rw->track_status = get_track(d, rw->tg.phys, rw->track, rw->tg.side, &rw->t);
    if (rw->track_status != IDC_STATUS_OK) return LINK_OK;
    uint8_t id = rw->record;
    rw->offset = 0;
    if (!rw->by_id) id = record_sector(rw->record, rw->track, rw->tg.mfm, &rw->offset);
    rw->idx = disk_track_find(rw->t, id, d->drives->position(d->drives->ctx, rw->tg.phys));
    return LINK_OK;
}

/* Result of reaching the sector: 00, or the WD1793 status a real read/write would give. */
static uint8_t sector_status(const rw_t *rw, bool for_write) {
    if (rw->track_status != IDC_STATUS_OK) return rw->track_status;
    if (rw->idx < 0 || rw->t->fm == rw->tg.mfm) return IDC_STATUS_NOT_FOUND; /* missing, or wrong density */
    const disk_sector_t *s = &rw->t->secs[rw->idx];
    if (s->flags & DISK_SEC_NO_DATA) return IDC_STATUS_NOT_FOUND;
    if (!for_write && (s->flags & DISK_SEC_CRC_ERROR)) return IDC_STATUS_CRC;
    return IDC_STATUS_OK;
}

link_status_t idc_cmd_login(idc_t *d) {
    target_t tg;
    link_status_t st = get_drive(d, &tg, NULL);
    if (st != LINK_OK) return st;
    idc_note_drive(d, tg.phys, tg.side, false, 0, 0);
    if (!ready(d, tg.phys)) set_sticky(d, IDC_STATUS_NOT_READY);
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK || d->sticky != IDC_STATUS_OK) return st;
    d->cache[tg.phys].valid = d->cache[tg.phys].valid && !d->cache[tg.phys].dirty; /* recalibrated; keep clean data */
    return link_put(&d->link, IDC_STATUS_OK);                                      /* restore done [IDC3 07e2, 0eb0] */
}

link_status_t idc_cmd_read(idc_t *d) {
    rw_t rw = {.by_id = false};
    bool go;
    link_status_t st = rw_params(d, &rw, &go);
    if (st != LINK_OK || !go) return st;
    uint8_t res = sector_status(&rw, false);
    st = link_put(&d->link, res);
    if (st != LINK_OK || res != IDC_STATUS_OK) return st;
    const disk_sector_t *s = &rw.t->secs[rw.idx];
    return link_send_record(&d->link, rw.t->data + s->data_off + rw.offset);
}

/*
 * Which of the IDC's 4-record buffers a record falls in, and its bit there. The IDC groups records by their
 * index after its FM remapping [IDC3 0717]: on FM tracks other than 0 that is index i -> 11i mod 16 (its table
 * at 0957); otherwise the index itself, so an MFM buffer is one sector.
 */
static void record_buffer(uint8_t record, uint8_t track, bool mfm, unsigned *buffer, uint8_t *bit) {
    unsigned i = record - 1u;
    if (!mfm && track != 0) i = (i * 11u) % IDC_FM_RECORDS;
    *buffer = i / IDC_MFM_PER_SECTOR;
    *bit = (uint8_t)(1u << (i % IDC_MFM_PER_SECTOR));
}

#define IDC_BUFFER_FULL 0x0Fu /* all four records written */

/*
 * 03/04: a record into the cache [IDC3 0b3d, 0b56]. The IDC takes it into a buffer even when the sector isn't
 * on the disc: that shows only when the buffer goes to the disc. A record completing its buffer sends it there
 * at once, and the result of that write is this command's result [10a6-10b2 -> 1170; MAME trace: the fourth
 * record of a SAVE to a missing sector answers 10].
 */
link_status_t idc_cmd_write(idc_t *d) {
    rw_t rw = {.by_id = false};
    bool go;
    link_status_t st = rw_params(d, &rw, &go);
    if (st != LINK_OK || !go) return st;
    uint8_t rec[LINK_RECORD];
    st = link_recv_record(&d->link, rec);
    if (st != LINK_OK) return st;
    if (rw.track_status != IDC_STATUS_OK) return link_put(&d->link, rw.track_status);

    idc_cache_t *c = &d->cache[rw.tg.phys];
    unsigned buffer;
    uint8_t bit;
    record_buffer(rw.record, rw.track, rw.tg.mfm, &buffer, &bit);
    if (sector_status(&rw, true) == IDC_STATUS_OK) {
        disk_track_write_part(rw.t, (unsigned)rw.idx, rw.offset, rec, LINK_RECORD);
        c->dirty = true;
    } else {
        c->missing |= (uint16_t)(1u << buffer);
    }
    c->written[buffer] |= bit;
    uint8_t res = IDC_STATUS_OK;
    if (c->written[buffer] == IDC_BUFFER_FULL) {
        c->written[buffer] = 0;
        if (c->missing & (1u << buffer)) {
            c->missing &= (uint16_t)~(1u << buffer);
            res = IDC_STATUS_NOT_FOUND;
        } else {
            res = save_track(d, rw.tg.phys);
        }
    }
    return link_put(&d->link, res);
}

link_status_t idc_cmd_flush(idc_t *d) {
    target_t tg;
    uint8_t p;
    link_status_t st = get_drive(d, &tg, &p);
    if (st != LINK_OK) return st;
    idc_note_drive(d, tg.phys, tg.side, false, 0, 0);
    if (!ready(d, tg.phys)) set_sticky(d, IDC_STATUS_NOT_READY);
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK || d->sticky != IDC_STATUS_OK) return st;
    uint8_t res = (p & IDC_PARAM_ALL) ? idc_flush_all(d) : flush_drive(d, tg.phys);
    if (res != IDC_STATUS_OK) res |= IDC_STATUS_WRITE_BACK; /* [IDC3 1088 -> 109f; MAME trace write_protect: 41] */
    return link_put(&d->link, res);
}

uint8_t idc_flush_all(idc_t *d) {
    uint8_t first = IDC_STATUS_OK;
    if (!d->drives) return first;
    for (unsigned i = 0; i < IDC_PHYS_DRIVES; i++) {
        uint8_t st = flush_drive(d, i);
        if (first == IDC_STATUS_OK) first = st;
    }
    return first;
}

uint8_t idc_change_disc(idc_t *d, unsigned drive) {
    RMFE_ASSERT(d && drive < IDC_PHYS_DRIVES);
    uint8_t res = d->drives ? flush_drive(d, drive) : IDC_STATUS_OK;
    d->cache[drive].valid = false;
    d->cache[drive].dirty = false;
    clear_written(&d->cache[drive]);
    return res;
}

/* 0E: read a whole sector by its ID [IDC3 0d6b]: status, result, the count of 128-byte blocks, then each
 * block with its acknowledgement. */
link_status_t idc_cmd_read_id(idc_t *d) {
    rw_t rw = {.by_id = true};
    bool go;
    link_status_t st = rw_params(d, &rw, &go);
    if (st != LINK_OK || !go) return st;
    uint8_t res = sector_status(&rw, false);
    st = link_put(&d->link, res);
    if (st != LINK_OK || res != IDC_STATUS_OK) return st;
    const disk_sector_t *s = &rw.t->secs[rw.idx];
    unsigned blocks = s->size / LINK_RECORD;
    st = link_put(&d->link, (uint8_t)blocks);
    for (unsigned b = 0; st == LINK_OK && b < blocks; b++) {
        st = link_send_record(&d->link, rw.t->data + s->data_off + b * LINK_RECORD);
    }
    return st;
}

/* 0F/10: write a whole sector by its ID [IDC3 0d9d, 0d99]: status, the count of blocks, the blocks
 * (each acknowledged, with parity retry), then the result of the write, which the IDC makes at once
 * rather than into its cache, so a write-protected disc answers 40 here. 10 also verifies; our writes are
 * exact. */
link_status_t idc_cmd_write_id(idc_t *d) {
    rw_t rw = {.by_id = true};
    bool go;
    link_status_t st = rw_params(d, &rw, &go);
    if (st != LINK_OK || !go) return st;
    /* Block count: from the sector when it exists, else from the format (FM 1, MFM 4). */
    unsigned blocks = rw.idx >= 0 ? rw.t->secs[rw.idx].size / LINK_RECORD : blocks_per_sector(rw.t, rw.tg.mfm);
    st = link_put(&d->link, (uint8_t)blocks);
    if (st != LINK_OK) return st;
    uint8_t res = sector_status(&rw, true);
    for (unsigned b = 0; b < blocks; b++) {
        uint8_t rec[LINK_RECORD];
        st = link_recv_record(&d->link, rec);
        if (st != LINK_OK) return st;
        if (res == IDC_STATUS_OK) disk_track_write_part(rw.t, (unsigned)rw.idx, b * LINK_RECORD, rec, LINK_RECORD);
    }
    if (res == IDC_STATUS_OK) {
        d->cache[rw.tg.phys].dirty = true;
        res = flush_drive(d, rw.tg.phys);
    }
    return link_put(&d->link, res);
}

/* The 06 format byte for a drive and the disc in it [IDC3 0c55-0c71]. */
static uint8_t probe_byte(idc_t *d, unsigned phys, const disk_track_t *t) {
    uint8_t b = 0;
    if (t && t->count && !t->fm) b |= IDC_PROBE_MFM;
    if (qd_drive(d, phys)) {
        b |= IDC_PROBE_QD_DRIVE;
        /* an 80-track disc in an 80-track drive; the IDC finds it from the cylinder the ID reports [IDC3 0c35-0c4f] */
        if (d->drives->cylinders(d->drives->ctx, phys) >= IDC_QD_CYLINDERS) b |= IDC_PROBE_96TPI;
    }
    return b;
}

/* 06: probe the disc's format [IDC3 0bb5]: status; then, once an ID has been read, the status again
 * [IDC3 0c52] and the format byte (MAME trace: 00 00 0A). The IDC reads address marks on track 4 (or the
 * track it is on, if further in) in both densities; we look at the image's track. An unreadable track gives
 * one byte instead, the error status with the drive-type bits [IDC3 0bf9-0c10]. */
link_status_t idc_cmd_probe(idc_t *d) {
    target_t tg;
    link_status_t st = get_drive(d, &tg, NULL);
    if (st != LINK_OK) return st;
    idc_note_drive(d, tg.phys, tg.side, false, 0, 0);
    if (!ready(d, tg.phys)) set_sticky(d, IDC_STATUS_NOT_READY);
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK || d->sticky != IDC_STATUS_OK) return st;
    unsigned cyls = d->drives->cylinders(d->drives->ctx, tg.phys);
    unsigned probe_cyl = cyls > 4 ? 4u : 0u;
    disk_track_t *t = NULL;
    uint8_t ts = get_track(d, tg.phys, probe_cyl, tg.side, &t);
    if (ts != IDC_STATUS_OK || !t->count) {
        uint8_t err = ts != IDC_STATUS_OK ? ts : IDC_STATUS_NOT_FOUND;
        return link_put(&d->link, (uint8_t)(err | (probe_byte(d, tg.phys, NULL) & IDC_PROBE_QD_DRIVE)));
    }
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK) return st;
    return link_put(&d->link, probe_byte(d, tg.phys, t));
}

/*
 * 0C format / 0D verify a track [IDC3 0d01, 0d35]: drive, track, the sector count (must be the format's:
 * 16 FM, 9 MFM, else status 20), status, then that many sector IDs in the order they are laid down.
 * 0C writes a fresh track (E5 data; recorded C = track, H = 0 as RM's IDC writes it) at once; 0D reads
 * each listed ID and reports the first failure [IDC3 04e8].
 *
 * The sectors lie where the IDC's Write Track puts them, from its track templates: FM [IDC3 036d, 085b]
 * 73 bytes of gap 4a, index mark and gap 1, then 188 bytes a sector (gap 3 is 27 x FF); MFM [IDC3 0409,
 * 08a1] 146 bytes, then 654 a sector (gap 3 is 80 x 4E). A position is the sector's ID mark, 6 (FM) or
 * 15 (MFM) bytes into it.
 */
#define FMT_FM_FIRST_MARK  (73u + 6u)
#define FMT_FM_SECTOR      188u
#define FMT_FM_CELLS       32u /* per byte at 125 kbit/s */
#define FMT_MFM_FIRST_MARK (146u + 15u)
#define FMT_MFM_SECTOR     654u
#define FMT_MFM_CELLS      16u /* per byte at 250 kbit/s */

link_status_t idc_cmd_format(idc_t *d, bool verify_only) {
    target_t tg;
    uint8_t track, n;
    link_status_t st = get_drive(d, &tg, NULL);
    if (st == LINK_OK) st = link_get(&d->link, &track);
    if (st == LINK_OK) st = link_get(&d->link, &n);
    if (st != LINK_OK) return st;
    idc_note_drive(d, tg.phys, tg.side, true, track, n);
    bool ok = ready(d, tg.phys);
    if (!ok) set_sticky(d, IDC_STATUS_NOT_READY);
    if (ok && track >= drive_tracks(d, tg.phys)) {
        set_sticky(d, IDC_STATUS_BAD_PARAM);
        ok = false;
    }
    unsigned want = tg.mfm ? 9u : 16u;
    if (ok && n != want) set_sticky(d, IDC_STATUS_BAD_PARAM);
    st = link_put(&d->link, d->sticky);
    if (st != LINK_OK || d->sticky != IDC_STATUS_OK) return st;
    uint8_t ids[DISK_MAX_SECTORS];
    for (unsigned i = 0; i < n; i++) {
        st = link_get(&d->link, &ids[i]);
        if (st != LINK_OK) return st;
    }
    disk_track_t *t;
    uint8_t res = get_track(d, tg.phys, track, tg.side, &t);
    if (res == IDC_STATUS_OK && verify_only) {
        for (unsigned i = 0; i < n && res == IDC_STATUS_OK; i++) {
            int idx = disk_track_find(t, ids[i], 0);
            if (idx < 0 || t->fm == tg.mfm || (t->secs[idx].flags & DISK_SEC_NO_DATA)) {
                res = IDC_STATUS_NOT_FOUND;
            } else if (t->secs[idx].flags & DISK_SEC_CRC_ERROR) {
                res = IDC_STATUS_CRC;
            }
        }
    } else if (res == IDC_STATUS_OK) {
        uint16_t size = tg.mfm ? 512u : 128u;
        disk_track_clear(t, track, (uint8_t)tg.side);
        t->fm = !tg.mfm;
        t->rate_kbps = tg.mfm ? 250u : 125u;
        for (unsigned i = 0; i < n; i++) {
            disk_sector_t s = {.c = track,
                               .h = 0,
                               .r = ids[i],
                               .n = tg.mfm ? 2u : 0u,
                               .size = size,
                               .pos = tg.mfm ? (FMT_MFM_FIRST_MARK + FMT_MFM_SECTOR * i) * FMT_MFM_CELLS
                                             : (FMT_FM_FIRST_MARK + FMT_FM_SECTOR * i) * FMT_FM_CELLS};
            if (disk_track_add(t, &s, NULL) != DISK_OK) break;
            memset(t->data + t->secs[t->count - 1].data_off, 0xE5, size);
        }
        d->cache[tg.phys].dirty = true;
        res = flush_drive(d, tg.phys);
    }
    return link_put(&d->link, res);
}
