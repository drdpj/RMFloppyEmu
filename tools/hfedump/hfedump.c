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
 * hfedump: decodes every track of an HFE image with src/flux and prints what it finds, one line per
 * track and per sector. Used by tests/check_hfe_vs_imd.py and for looking at protected discs.
 *
 *   hfedump [--data] image.hfe
 *
 *   H version=1 cyls=40 sides=2 enc=00 rate=250 track0=02,02
 *   T <cyl> <side> enc=MFM step=1 cells=100352 count=9 lost=0 weak=0
 *   S <cyl> <side> <idx> id=<pos> data=<pos|-> c=.. h=.. r=.. n=.. mark=.. flags=.. [hex data]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flux/hfe.h"
#include "flux/ibm_track.h"

static bool file_read(void *ctx, uint32_t offset, void *buf, uint32_t len) {
    FILE *f = ctx;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return false;
    return fread(buf, 1, len, f) == len;
}

int main(int argc, char **argv) {
    bool with_data = argc == 3 && strcmp(argv[1], "--data") == 0;
    if (argc != 2 && !with_data) {
        fprintf(stderr, "usage: hfedump [--data] image.hfe\n");
        return 2;
    }
    FILE *f = fopen(argv[argc - 1], "rb");
    if (!f) {
        perror(argv[argc - 1]);
        return 1;
    }
    hfe_image_t img;
    hfe_status_t st = hfe_open(&img, file_read, f);
    if (st != HFE_OK) {
        fprintf(stderr, "hfedump: %s\n", hfe_status_name(st));
        return 1;
    }
    printf("H version=%u cyls=%u sides=%u enc=%02X rate=%u track0=%02X,%02X\n", img.version, img.cylinders,
           img.sides, img.encoding, img.bitrate_kbps, img.track0_encoding[0], img.track0_encoding[1]);

    static uint8_t cells[64 * 1024];
    static ibm_sector_t secs[64];
    static uint8_t data[64 * 1024];
    int rc = 0;
    for (unsigned c = 0; c < img.cylinders; c++) {
        for (unsigned s = 0; s < img.sides; s++) {
            hfe_track_t tr;
            st = hfe_read_track(&img, c, s, cells, sizeof cells, &tr);
            if (st != HFE_OK) {
                printf("E %u %u %s\n", c, s, hfe_status_name(st));
                rc = 1;
                continue;
            }
            ibm_track_t t;
            ibm_track_init(&t, secs, 64, data, sizeof data);
            ibm_status_t ist = ibm_track_decode_auto(&t, cells, tr.ncells);
            printf("T %u %u enc=%s step=%u cells=%u count=%u lost=%u weak=%u%s\n", c, s,
                   t.encoding == IBM_ENC_FM ? "FM" : "MFM", t.cell_step, tr.ncells, t.count, t.lost, tr.weak_cells,
                   ist == IBM_ERR_NOTHING ? " nothing" : "");
            for (unsigned i = 0; i < t.count; i++) {
                const ibm_sector_t *x = &secs[i];
                printf("S %u %u %u id=%u data=", c, s, i, x->id_pos);
                if (x->data_pos == IBM_NO_POS) {
                    printf("-");
                } else {
                    printf("%u", x->data_pos);
                }
                printf(" c=%02X h=%02X r=%02X n=%02X mark=%02X flags=%02X", x->c, x->h, x->r, x->n, x->mark, x->flags);
                if (with_data && !(x->flags & (IBM_SEC_NO_DATA | IBM_SEC_DATA_DROPPED | IBM_SEC_BAD_SIZE))) {
                    printf(" ");
                    for (unsigned k = 0; k < x->size; k++) printf("%02X", data[x->data_off + k]);
                }
                printf("\n");
            }
        }
    }
    fclose(f);
    return rc;
}
