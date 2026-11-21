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
#include "flux_synth.h"

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

uint32_t synth_flux(const uint8_t *cells, uint32_t ncells, unsigned repeat, uint32_t cell_ticks,
                    uint32_t speed_permille, uint32_t jitter, uint32_t *flux, uint32_t cap, uint32_t *total) {
    uint32_t seed = 12345, n = 0, gap = 0;
    uint64_t rev_ticks = 0;
    for (unsigned r = 0; r < repeat; r++) {
        for (uint32_t i = 0; i < ncells; i++) {
            gap++;
            if (!(cells[i >> 3] & (0x80u >> (i & 7u)))) continue;
            int64_t t = (int64_t)gap * cell_ticks * speed_permille / 1000;
            if (jitter) t += (int64_t)(lcg(&seed) % (2 * jitter + 1)) - (int64_t)jitter;
            if (n >= cap) return 0;
            flux[n++] = (uint32_t)t;
            if (r == 0) rev_ticks += (uint64_t)t;
            gap = 0;
        }
    }
    *total = (uint32_t)rev_ticks;
    return n;
}
