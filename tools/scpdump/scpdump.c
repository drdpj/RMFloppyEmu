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
 * scpdump: decodes every revolution of every track of an SCP flux image with src/flux (SCP reader,
 * PLL, FM/MFM decoder) and prints what it finds. Used by tools/scpdump/sweep.py.
 *
 *   scpdump [--data] [--cell NS] image.scp
 *
 * The bitcell is 2000 ns at 300 rpm (250 kbit/s MFM, and FM at double rate). By default it is
 * scaled by each revolution's measured time, since a disc written at 300 rpm read in a 360 rpm
 * drive (a 5.25" HD drive) comes past 20% faster: 166.7 ms revolutions, 1667 ns cells. SCP's
 * 360 rpm header flag can't be relied on to say so. --cell NS fixes the cell length instead.
 *
 *   H revs=3 tracks=0-79 flags=23 heads=0 hz=40000000 checksum=ok
 *   T <cyl> <side> <rev> enc=MFM step=1 cells=<rev cells> span=<cells> ms=<rev ms*1000> count=.. lost=..
 *   S <cyl> <side> <rev> <idx> id=<cell> data=<cell|-> c=.. h=.. r=.. n=.. mark=.. flags=.. [hex data]
 *   E <cyl> <side> <message>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flux/flux_pll.h"
#include "flux/ibm_track.h"
#include "flux/scp.h"

static bool file_read(void *ctx, uint32_t offset, void *buf, uint32_t len) {
    FILE *f = ctx;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, f) == len;
}

int main(int argc, char **argv) {
    bool with_data = false;
    uint32_t cell_ns = 0; /* 0: scale 2000 ns by each revolution's time against 200 ms (300 rpm) */
    int a = 1;
    for (; a < argc - 1; a++) {
        if (strcmp(argv[a], "--data") == 0) {
            with_data = true;
        } else if (strcmp(argv[a], "--cell") == 0 && a + 1 < argc - 1) {
            cell_ns = (uint32_t)strtoul(argv[++a], NULL, 10);
        } else {
            break;
        }
    }
    if (a != argc - 1) {
        fprintf(stderr, "usage: scpdump [--data] [--cell NS] image.scp\n");
        return 2;
    }
    FILE *f = fopen(argv[a], "rb");
    if (!f) {
        perror(argv[a]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    uint32_t file_len = (uint32_t)ftell(f);

    scp_image_t img;
    scp_status_t st = scp_open(&img, file_read, f);
    if (st != SCP_OK) {
        fprintf(stderr, "scpdump: %s\n", scp_status_name(st));
        return 1;
    }
    bool sum_ok = false;
    scp_verify_checksum(&img, file_len, &sum_ok);
    printf("H revs=%u tracks=%u-%u flags=%02X heads=%u hz=%u checksum=%s\n", img.revolutions, img.start_track,
           img.end_track, img.flags, img.heads, img.sample_hz, sum_ok ? "ok" : "BAD");

    static uint8_t cells[160 * 1024];
    static ibm_sector_t secs[64];
    static uint8_t data[64 * 1024];
    for (unsigned tn = img.start_track; tn <= img.end_track && tn < SCP_MAX_TRACKS; tn++) {
        unsigned c = tn / 2, s = tn % 2;
        scp_track_t tr;
        st = scp_read_track(&img, c, s, &tr);
        if (st == SCP_ERR_NO_TRACK) continue;
        if (st != SCP_OK) {
            printf("E %u %u %s\n", c, s, scp_status_name(st));
            continue;
        }
        for (unsigned r = 0; r < tr.nrevs; r++) {
            scp_flux_stream_t fs;
            scp_flux_open(&fs, &img, &tr, r);
            uint32_t rev_us = (uint32_t)((uint64_t)tr.revs[r].index_ticks * 1000000u / img.sample_hz);
            uint32_t cell = cell_ns;
            if (cell == 0) cell = (rev_us > 150000u && rev_us < 230000u) ? rev_us / 100u : 2000u;
            flux_pll_params_t p = {img.sample_hz, cell, tr.revs[r].index_ticks, 0, 0};
            flux_pll_result_t pr;
            flux_pll_decode(&p, scp_flux_next, &fs, cells, (uint32_t)sizeof cells * 8u, &pr);
            if (fs.failed) {
                printf("E %u %u read error\n", c, s);
                break;
            }
            /* Decode one revolution, with up to an eighth of a turn of real run-on past the index. */
            uint32_t span = pr.ncells;
            if (span > pr.ncells_rev + pr.ncells_rev / 8) span = pr.ncells_rev + pr.ncells_rev / 8;
            ibm_track_t t;
            ibm_track_init(&t, secs, 64, data, sizeof data);
            ibm_status_t ist = ibm_track_decode_span_auto(&t, cells, pr.ncells_rev, span);
            printf("T %u %u %u enc=%s step=%u cells=%u span=%u us=%u cell_ns=%u count=%u lost=%u%s\n", c, s, r,
                   t.encoding == IBM_ENC_FM ? "FM" : "MFM", t.cell_step, pr.ncells_rev, span, rev_us, cell, t.count,
                   t.lost,
                   ist == IBM_ERR_NOTHING ? " nothing" : "");
            for (unsigned i = 0; i < t.count; i++) {
                const ibm_sector_t *x = &secs[i];
                printf("S %u %u %u %u id=%u data=", c, s, r, i, x->id_pos);
                if (x->data_pos == IBM_NO_POS) {
                    printf("-");
                } else {
                    printf("%u", x->data_pos);
                }
                printf(" c=%02X h=%02X r=%02X n=%02X mark=%02X flags=%02X", x->c, x->h, x->r, x->n, x->mark,
                       x->flags);
                if (with_data && !(x->flags & (IBM_SEC_NO_DATA | IBM_SEC_DATA_DROPPED | IBM_SEC_BAD_SIZE))) {
                    printf(" ");
                    for (unsigned k = 0; k < x->size; k++) printf("%02X", data[x->data_off + k]);
                }
                printf("\n");
            }
        }
    }
    fclose(f);
    return 0;
}
