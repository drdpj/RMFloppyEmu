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
 * CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF, no reflection, no final XOR): the CRC
 * of the IBM System 34 / WD177x / uPD765 FM and MFM formats. Over a whole field including its
 * recorded CRC (and, for MFM, the three A1 sync bytes) the result is 0 when the field is good.
 */
#ifndef FLUX_CRC16_H
#define FLUX_CRC16_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUX_CRC16_INIT 0xFFFFu

/* Continues crc over n bytes at p. Start with FLUX_CRC16_INIT. */
uint16_t flux_crc16(uint16_t crc, const uint8_t *p, size_t n);

/* Continues crc over one byte. */
uint16_t flux_crc16_byte(uint16_t crc, uint8_t b);

#ifdef __cplusplus
}
#endif

#endif
