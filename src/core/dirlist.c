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

/* A folder's listing for the menu: see dirlist.h. */
#include "core/dirlist.h"

#include "core/config.h"
#include "core/rmfe_assert.h"

#include <string.h>

void dirlist_clear(dirlist_t *l) {
    RMFE_ASSERT(l);
    l->pool_used = 0;
    l->count = 0;
    l->truncated = false;
}

bool dirlist_add(dirlist_t *l, const char *name, bool is_dir, bool hidden) {
    RMFE_ASSERT(l && name);
    if (is_dir) {
        if (hidden || name[0] == '.' || name[0] == '\0') return false;
    } else if (!config_is_listed_image(name, hidden)) {
        return false;
    }
    size_t n = strlen(name) + 1;
    if (l->count >= DIRLIST_MAX_ENTRIES || n > DIRLIST_POOL_BYTES - l->pool_used) {
        l->truncated = true;
        return false;
    }
    memcpy(l->pool + l->pool_used, name, n);
    l->entry[l->count].off = (uint16_t)l->pool_used;
    l->entry[l->count].is_dir = is_dir;
    l->count++;
    l->pool_used += n;
    return true;
}

static int compare(const dirlist_t *l, const dirlist_entry_t *a, const dirlist_entry_t *b) {
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return config_compare_names(l->pool + a->off, l->pool + b->off);
}

void dirlist_sort(dirlist_t *l) {
    RMFE_ASSERT(l);
    /* insertion sort: stable, no recursion, and a few hundred entries at most */
    for (unsigned i = 1; i < l->count; i++) {
        dirlist_entry_t e = l->entry[i];
        unsigned j = i;
        while (j > 0 && compare(l, &l->entry[j - 1], &e) > 0) {
            l->entry[j] = l->entry[j - 1];
            j--;
        }
        l->entry[j] = e;
    }
}

const char *dirlist_name(const dirlist_t *l, unsigned i) {
    RMFE_ASSERT(l && i < l->count);
    return l->pool + l->entry[i].off;
}

bool dirlist_is_dir(const dirlist_t *l, unsigned i) {
    RMFE_ASSERT(l && i < l->count);
    return l->entry[i].is_dir;
}

bool dirlist_path_join(char *out, size_t cap, const char *folder, const char *name) {
    RMFE_ASSERT(out && cap > 0 && folder && name);
    size_t f = strlen(folder), n = strlen(name), need = (f ? f + 1 : 0) + n + 1;
    if (need > cap) return false;
    memmove(out, folder, f); /* out may be folder itself */
    if (f) out[f++] = '/';
    memcpy(out + f, name, n + 1);
    return true;
}

bool dirlist_path_folder(char *out, size_t cap, const char *path) {
    RMFE_ASSERT(out && cap > 0 && path);
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n + 1 > cap) return false;
    memmove(out, path, n);
    out[n] = '\0';
    return true;
}

const char *dirlist_path_name(const char *path) {
    RMFE_ASSERT(path);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
