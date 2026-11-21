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
#include "flux/crc16.h"

uint16_t flux_crc16_byte(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)(b << 8);
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

uint16_t flux_crc16(uint16_t crc, const uint8_t *p, size_t n) {
    while (n--) crc = flux_crc16_byte(crc, *p++);
    return crc;
}
