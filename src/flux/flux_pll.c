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
#include "flux/flux_pll.h"

#include <stddef.h>

/* Times are Q16 sample ticks, signed: the phase residue swings either side of zero. */
#define Q16(x) ((int64_t)(x) << 16)

void flux_pll_decode(const flux_pll_params_t *p, flux_next_fn next, void *ctx, uint8_t *cells, uint32_t cap_cells,
                     flux_pll_result_t *out) {
    out->ncells_rev = 0;
    out->ncells = 0;
    out->nflux = 0;
    out->truncated = false;
    if (!p || !next || !cells || cap_cells == 0 || p->sample_hz == 0 || p->cell_ns == 0) return;

    const int64_t period_pct = p->period_pct ? p->period_pct : FLUX_PLL_PERIOD_PCT;
    const int64_t phase_pct = p->phase_pct ? p->phase_pct : FLUX_PLL_PHASE_PCT;
    const int64_t centre = (int64_t)(((uint64_t)p->sample_hz * p->cell_ns << 16) / 1000000000u);
    const int64_t clock_min = centre * (100 - FLUX_PLL_RANGE_PCT) / 100;
    const int64_t clock_max = centre * (100 + FLUX_PLL_RANGE_PCT) / 100;
    const int64_t rev_end = Q16(p->rev_ticks);

    int64_t clock = centre, ticks = 0, elapsed = 0;
    bool rev_done = p->rev_ticks == 0;
    uint32_t n = 0;
    uint32_t x;

    while (next(ctx, &x)) {
        out->nflux++;
        ticks += Q16(x);
        if (ticks < clock / 2) continue;

        /* Zero or more 0 cells, then a 1. */
        unsigned zeros = 0;
        for (;;) {
            ticks -= clock;
            if (ticks < clock / 2) break;
            zeros++;
        }
        if (n + zeros + 1 > cap_cells) {
            out->truncated = true;
            break;
        }
        for (unsigned k = 0; k < zeros; k++, n++) cells[n >> 3] &= (uint8_t)~(0x80u >> (n & 7u));
        cells[n >> 3] |= (uint8_t)(0x80u >> (n & 7u));

        /* Phase: keep part of the mismatch, spread the rest over the cells just emitted. */
        int64_t new_ticks = ticks * (100 - phase_pct) / 100;
        int64_t cell_time = clock + (ticks - new_ticks) / (int64_t)(zeros + 1);
        for (unsigned k = 0; k <= zeros; k++) {
            if (!rev_done && elapsed + cell_time > rev_end) {
                out->ncells_rev = n - zeros + k; /* cells before this one belong to the revolution */
                rev_done = true;
            }
            elapsed += cell_time;
        }
        n++;

        /* Frequency: follow the mismatch while in sync, otherwise relax towards nominal. */
        if (zeros <= 3) {
            clock += ticks * period_pct / 100;
        } else {
            clock += (centre - clock) * period_pct / 100;
        }
        if (clock < clock_min) clock = clock_min;
        if (clock > clock_max) clock = clock_max;
        ticks = new_ticks;
    }
    out->ncells = n;
    if (!rev_done) out->ncells_rev = n;
}
