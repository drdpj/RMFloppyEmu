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
#include "core/disk.h"

#include <string.h>

#include "core/rmfe_assert.h"

void disk_track_clear(disk_track_t *t, uint8_t cyl, uint8_t side) {
    t->cyl = cyl;
    t->side = side;
    t->fm = false;
    t->rate_kbps = 250;
    t->count = 0;
    t->data_used = 0;
}

disk_status_t disk_track_add(disk_track_t *t, const disk_sector_t *s, const uint8_t *data) {
    if (t->count >= DISK_MAX_SECTORS || s->size > DISK_MAX_TRACK_DATA - t->data_used) return DISK_ERR_FULL;
    disk_sector_t *d = &t->secs[t->count++];
    *d = *s;
    d->data_off = t->data_used;
    if (data) {
        memcpy(t->data + t->data_used, data, s->size);
    } else {
        memset(t->data + t->data_used, 0, s->size);
    }
    t->data_used += s->size;
    return DISK_OK;
}

int disk_track_find(const disk_track_t *t, uint8_t r, uint32_t pos) {
    int best = -1;
    uint32_t best_wait = UINT32_MAX;
    for (unsigned i = 0; i < t->count; i++) {
        if (t->secs[i].r != r) continue;
        uint32_t wait = (t->secs[i].pos + DISK_CELLS_PER_REV - pos % DISK_CELLS_PER_REV) % DISK_CELLS_PER_REV;
        if (wait < best_wait) {
            best_wait = wait;
            best = (int)i;
        }
    }
    return best;
}

void disk_track_write_part(disk_track_t *t, unsigned idx, uint32_t offset, const uint8_t *data, uint32_t len) {
    disk_sector_t *s = &t->secs[idx];
    RMFE_ASSERT(offset <= s->size && len <= s->size - offset);
    memcpy(t->data + s->data_off + offset, data, len);
    s->flags = 0;
}

void disk_track_write(disk_track_t *t, unsigned idx, const uint8_t *data) {
    disk_sector_t *s = &t->secs[idx];
    memcpy(t->data + s->data_off, data, s->size);
    s->flags = 0;
}
