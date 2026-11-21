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
 * Flux to bitcells: a software PLL turning flux-transition intervals (as a flux capture such as SCP
 * records them) into bitcells for ibm_track_decode_span().
 *
 * The algorithm is Greaseweazle's (src/greaseweazle/track.py, flux_to_bitcells), reimplemented in
 * fixed point (Q16 sample ticks, no floating point) so it runs the same on a microcontroller:
 *   each flux interval clocks out zero or more 0 cells and then a 1; the phase error left over is
 *   cut by phase_pct; while in sync (3 or fewer 0s) the cell period follows the phase error by
 *   period_pct, otherwise it drifts back to the nominal period; it never strays more than +-10%.
 * Defaults (FLUX_PLL_PERIOD_PCT, FLUX_PLL_PHASE_PCT) are Greaseweazle's default PLL.
 *
 * Flux comes from a callback, so the caller can stream it from a file without holding it all.
 */
#ifndef FLUX_FLUX_PLL_H
#define FLUX_FLUX_PLL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLUX_PLL_PERIOD_PCT 5u
#define FLUX_PLL_PHASE_PCT  60u
#define FLUX_PLL_RANGE_PCT  10u

/* Gives the next flux interval in sample ticks. Returns false at the end of the data. */
typedef bool (*flux_next_fn)(void *ctx, uint32_t *ticks);

typedef struct {
    uint32_t sample_hz;  /* flux sample clock (SCP: 40 MHz) */
    uint32_t cell_ns;    /* nominal bitcell: 2000 for 250 kbit/s MFM (and FM, at double rate) */
    uint32_t rev_ticks;  /* length of the revolution being decoded, in sample ticks (from the index) */
    unsigned period_pct; /* 0 = FLUX_PLL_PERIOD_PCT */
    unsigned phase_pct;  /* 0 = FLUX_PLL_PHASE_PCT */
} flux_pll_params_t;

typedef struct {
    uint32_t ncells_rev;   /* cells up to the end of the revolution (the next index) */
    uint32_t ncells;       /* cells written, including any beyond the index */
    uint32_t nflux;        /* flux intervals consumed */
    bool truncated;        /* the buffer filled before the flux ran out */
} flux_pll_result_t;

/*
 * Converts flux to cells, MSB first, into cells[] (cap_cells cells; cleared as written). Decoding
 * continues past the end of the revolution until the flux or the buffer runs out, so a sector
 * across the index can be read from real data: pass ncells_rev and ncells to
 * ibm_track_decode_span(). If the flux ends before the index, ncells_rev = ncells.
 */
void flux_pll_decode(const flux_pll_params_t *p, flux_next_fn next, void *ctx, uint8_t *cells, uint32_t cap_cells,
                     flux_pll_result_t *out);

#ifdef __cplusplus
}
#endif

#endif
