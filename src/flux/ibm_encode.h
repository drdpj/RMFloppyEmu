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
 * Encodes one revolution of an IBM-format (System 34) FM or MFM track into bitcells, MSB first: the
 * inverse of ibm_track.h, used to write tracks back to HFE (and later other bitcell formats).
 *
 * Each sector is placed at the cell position given (rounded to a byte boundary), so a layout read
 * from a disc - duplicate IDs and odd spacing included - can be written back as it was. Layout
 * (IBM System 34; WD1793 and uPD765 data sheets):
 *   MFM: 12 x 00, 3 x A1 (missing clock), mark, field, CRC; gaps 4E; ID to data: 22 bytes of 4E
 *   FM:  6 x 00, mark with clock C7, field, CRC; gaps FF; ID to data: 11 bytes of FF
 * The CRC is CRC-16/CCITT-FALSE over (A1 A1 A1 for MFM) mark and field (crc16.h).
 */
#ifndef FLUX_IBM_ENCODE_H
#define FLUX_IBM_ENCODE_H

#include <stdbool.h>
#include <stdint.h>

#include "flux/ibm_track.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t c, h, r, n;    /* the ID field */
    uint8_t mark;          /* data mark: FB normal, F8 deleted; 0 = FB */
    bool no_data;          /* write the ID field only */
    bool bad_crc;          /* write the data field with a wrong CRC (to keep a sector's recorded error) */
    const uint8_t *data;   /* size bytes */
    uint16_t size;
    uint32_t pos;          /* output cell where the ID field's mark byte should start (cells as written) */
} ibm_enc_sector_t;

/*
 * Writes ncells cells (the whole revolution) into cells[] (ncells/8 bytes, rounded up). cell_step:
 * output cells per encoded cell (1, or 2 for FM at the MFM rate, as HFE stores it). Sectors must be
 * in rotational order. Returns IBM_ERR_ARG if a sector would overlap the one before or run past the
 * end of the revolution.
 */
ibm_status_t ibm_track_encode(const ibm_enc_sector_t *secs, unsigned count, ibm_encoding_t enc, unsigned cell_step,
                              uint8_t *cells, uint32_t ncells);

#ifdef __cplusplus
}
#endif

#endif
