/*
 * flux - floppy track decoding and image containers (RMFloppyEmu, Disk Wrangler)
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
 * Decodes one revolution of an IBM-format (System 34) FM or MFM track - the format of the WD177x/
 * WD279x, uPD765 and their kin - from its bitcells into sectors.
 *
 * Input: bitcells, MSB first, one revolution starting at the index, as an image container
 * (hfe.h, and later SCP/WOZ after flux-to-bitcell conversion) provides them. A 1 is a flux
 * transition. The track is treated as circular, so a sector that straddles the index is
 * decoded, once.
 *
 * Output: every ID address mark found, in rotational order, each with the data field that
 * follows it (if any). Nothing is merged or hidden: duplicate IDs, missing data, bad CRCs and
 * deleted-data marks are all reported, because copy-protected discs depend on exactly these.
 *
 * Marks recognised (references: IBM System 34 format; WD1793 and uPD765 data sheets; checked
 * against Greaseweazle's codec/ibm/ibm.py):
 *   MFM: three A1 sync bytes with a missing clock (cells 4489 4489 4489), then the mark byte.
 *   FM:  the mark byte with clock C7 (no sync bytes).
 *   Mark bytes: FE = ID; FB/FA = data; F8/F9 = deleted data.
 *
 * Memory: none of its own. The caller supplies the sector table and the data buffer
 * (ibm_track_init), so it runs unchanged on a microcontroller.
 */
#ifndef FLUX_IBM_TRACK_H
#define FLUX_IBM_TRACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IBM_ENC_MFM = 0,
    IBM_ENC_FM = 1,
} ibm_encoding_t;

typedef enum {
    IBM_OK = 0,
    IBM_ERR_ARG,     /* NULL pointer, zero cells, or a bad cell_step */
    IBM_ERR_NOTHING, /* (decode_auto only) no ID with a good CRC in any encoding tried */
} ibm_status_t;

#define IBM_MARK_ID           0xFE
#define IBM_MARK_DATA         0xFB
#define IBM_MARK_DELETED      0xF8
#define IBM_SECTOR_MAX_N      6          /* largest size code decoded: 128 << 6 = 8192 bytes */
#define IBM_NO_POS            0xFFFFFFFFu
#define IBM_DATA_WINDOW_CELLS 1000u      /* a data mark belongs to the ID it follows within this many cells */

/* ibm_sector_t.flags */
#define IBM_SEC_ID_CRC_ERROR   0x01u /* the ID field's CRC is wrong; c/h/r/n are as read */
#define IBM_SEC_NO_DATA        0x02u /* no data mark follows the ID within IBM_DATA_WINDOW_CELLS */
#define IBM_SEC_DATA_CRC_ERROR 0x04u /* the data field's CRC is wrong; the data is as read */
#define IBM_SEC_DELETED        0x08u /* deleted-data mark (F8/F9) */
#define IBM_SEC_DATA_DROPPED   0x10u /* the data didn't fit the caller's buffer (CRC still checked) */
#define IBM_SEC_BAD_SIZE       0x20u /* n > IBM_SECTOR_MAX_N: data not decoded */

typedef struct {
    uint32_t id_pos;   /* cell index (in the input, before cell_step) of the ID mark byte's first cell */
    uint32_t data_pos; /* likewise for the data mark byte; IBM_NO_POS if there is no data */
    uint32_t data_off; /* where the data starts in ibm_track_t.data; valid unless NO_DATA/DROPPED/BAD_SIZE */
    uint16_t size;     /* data length, 128 << n; 0 if n > IBM_SECTOR_MAX_N */
    uint16_t id_crc;   /* CRC as recorded in the ID field */
    uint16_t data_crc; /* CRC as recorded in the data field (0 if no data) */
    uint8_t c, h, r, n;
    uint8_t mark;      /* the data mark byte (FB, FA, F8, F9); 0 if no data */
    uint8_t flags;     /* IBM_SEC_* */
} ibm_sector_t;

typedef struct {
    /* Storage, set by ibm_track_init and owned by the caller. */
    ibm_sector_t *sectors;
    uint16_t max_sectors;
    uint8_t *data;
    uint32_t data_cap;

    /* Results of the last decode. */
    ibm_encoding_t encoding;
    unsigned cell_step;
    uint16_t count;     /* sectors stored in sectors[], in rotational order */
    uint16_t lost;      /* further IDs found that didn't fit sectors[] */
    uint32_t data_used; /* bytes of data[] in use */
} ibm_track_t;

void ibm_track_init(ibm_track_t *t, ibm_sector_t *sectors, uint16_t max_sectors, uint8_t *data, uint32_t data_cap);

/*
 * Decodes one revolution. ncells is the number of input bitcells. cell_step is the number of input
 * cells per encoded cell: 1 normally, 2 for FM recorded at twice its cell rate (HFE images store
 * an FM track at the MFM rate of the same drive, so a 250 kbit/s MFM image holds FM at step 2).
 * At step 2 each pair of input cells is OR-ed, which works whichever cell of the pair the
 * transition fell in. Any previous results in t are replaced.
 */
ibm_status_t ibm_track_decode(ibm_track_t *t, const uint8_t *cells, uint32_t ncells, ibm_encoding_t enc,
                              unsigned cell_step);

/*
 * As ibm_track_decode, for input that runs on past the end of the revolution: ncells_rev cells make
 * one revolution (marks are looked for there) and ncells (>= ncells_rev) are present, so a field
 * that crosses the index is read from the real cells that follow (as a flux decode gives them;
 * see flux_pll.h) instead of wrapping round to the start.
 */
ibm_status_t ibm_track_decode_span(ibm_track_t *t, const uint8_t *cells, uint32_t ncells_rev, uint32_t ncells,
                                   ibm_encoding_t enc, unsigned cell_step);

/*
 * Tries MFM (step 1), then FM at step 2, then FM at step 1, and keeps the first that finds at least
 * one ID with a good CRC. Returns IBM_ERR_NOTHING (with t holding the MFM attempt) if none does.
 */
ibm_status_t ibm_track_decode_auto(ibm_track_t *t, const uint8_t *cells, uint32_t ncells);
ibm_status_t ibm_track_decode_span_auto(ibm_track_t *t, const uint8_t *cells, uint32_t ncells_rev, uint32_t ncells);

/* The index in t->sectors of the first sector (rotationally) with ID r and a good ID CRC, or -1. */
int ibm_track_find(const ibm_track_t *t, uint8_t r);

#ifdef __cplusplus
}
#endif

#endif
