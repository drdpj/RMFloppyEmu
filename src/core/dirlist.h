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
 * One folder of the SD card as the menu lists it (docs/sd-card.md, "Choosing an image"): its folders and its
 * images (config_is_listed_image), folders first, then images, each sorted by name ignoring case
 * (config_compare_names), as FlashFloppy's native mode does by default (sort-priority = folders). Names starting
 * with "." and hidden entries are left out, folders as well as files.
 *
 * Paths are as IMAGE_x.CFG holds them: from the card's root, without a leading "/", folders separated by "/"
 * ("games/Dragon World.IMD"); the root folder is "".
 *
 * The names are kept in a pool, so a folder of short names can have many. What doesn't fit is left out and
 * noted (dirlist_t.truncated). Nothing is allocated.
 */
#ifndef RMFE_CORE_DIRLIST_H
#define RMFE_CORE_DIRLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIRLIST_MAX_ENTRIES 512u
#define DIRLIST_POOL_BYTES  32768u /* names with their NULs */

typedef struct {
    uint16_t off; /* into pool */
    bool is_dir;
} dirlist_entry_t;

typedef struct {
    char pool[DIRLIST_POOL_BYTES];
    size_t pool_used;
    dirlist_entry_t entry[DIRLIST_MAX_ENTRIES];
    unsigned count;
    bool truncated; /* entries were left out: too many, or their names too long in all */
} dirlist_t;

/* Empties the list. */
void dirlist_clear(dirlist_t *l);

/* Offers an entry of the folder, as the card lists it. Returns true if it was added; false if the listing doesn't
 * show it (an image of another kind, a "." name, a hidden entry), or it didn't fit (truncated is then set). */
bool dirlist_add(dirlist_t *l, const char *name, bool is_dir, bool hidden);

/* Sorts the list: folders first, then images, each by name ignoring case. */
void dirlist_sort(dirlist_t *l);

/* Entry i's name, and whether it is a folder. */
const char *dirlist_name(const dirlist_t *l, unsigned i);
bool dirlist_is_dir(const dirlist_t *l, unsigned i);

/* folder + "/" + name (or just name, in the root) into out (cap bytes). False if it doesn't fit. */
bool dirlist_path_join(char *out, size_t cap, const char *folder, const char *name);

/* The folder a path is in, into out (cap bytes): "games/x.imd" -> "games", "x.imd" -> "". False if it doesn't fit. */
bool dirlist_path_folder(char *out, size_t cap, const char *path);

/* The last part of a path: "games/x.imd" -> "x.imd". */
const char *dirlist_path_name(const char *path);

#endif
