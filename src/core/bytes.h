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
 * Little-endian load/store helpers for parsing disk images and ROM tables. The Z80 and the
 * image formats (IMD, and so on) are little-endian; these work on byte buffers so they never
 * depend on the host's endianness or alignment.
 */
#ifndef RMFE_CORE_BYTES_H
#define RMFE_CORE_BYTES_H

#include <stdint.h>

uint16_t bytes_get_le16(const uint8_t *p);
uint32_t bytes_get_le32(const uint8_t *p);
void bytes_put_le16(uint8_t *p, uint16_t v);
void bytes_put_le32(uint8_t *p, uint32_t v);

#endif
