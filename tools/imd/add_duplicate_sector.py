#!/usr/bin/env python3
# RMFloppyEmu - a Raspberry Pi Pico 2 replacement for the RML 480Z's disk unit
# Copyright (C) 2026 Daniel Jameson
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Add a second copy of a sector to an ImageDisk (.IMD) image, taking its data from another image.

usage: add_duplicate_sector.py IN.IMD DONOR.IMD OUT.IMD CYLINDER SECTOR_ID

On every head of CYLINDER, appends the donor's sector SECTOR_ID (same cylinder and head) after the
existing sectors, so the track then holds two sectors with that ID: the original first, the
donor's copy last (in the gap at the end of the track). IN is not changed.

Made for the 4MATION 1985 protection (docs/protocol.md, "Copy-protected discs"): each original disc
held two different sectors numbered 08 on cylinder 2, and each surviving image kept only one.
  add_duplicate_sector.py Dragon_World.IMD Flowers_of_Crystal.IMD Dragon_World_fixed.IMD 2 8
"""
import sys


def parse(path):
    d = open(path, "rb").read()
    i = d.index(b"\x1a") + 1
    header, tracks = d[:i], []
    while i < len(d):
        mode, cyl, head, n, sz = d[i:i + 5]
        i += 5
        smap = d[i:i + n]
        i += n
        cmap = hmap = b""
        if head & 0x80:
            cmap = d[i:i + n]
            i += n
        if head & 0x40:
            hmap = d[i:i + n]
            i += n
        size = 128 << sz
        recs = []
        for _ in smap:
            t = d[i]
            i += 1
            if t % 2 == 1:
                recs.append(bytes([t]) + d[i:i + size])
                i += size
            elif t:
                recs.append(bytes([t, d[i]]))
                i += 1
            else:
                recs.append(b"\x00")
        tracks.append([mode, cyl, head, n, sz, smap, cmap, hmap, recs])
    return header, tracks


def main():
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    src, donor_path, out_path = sys.argv[1:4]
    cylinder, sector = int(sys.argv[4], 0), int(sys.argv[5], 0)
    header, tracks = parse(src)
    _, donor = parse(donor_path)
    by_pos = {(t[1], t[2] & 1): t for t in donor}
    out = bytearray(header)
    added = 0
    for mode, cyl, head, n, sz, smap, cmap, hmap, recs in tracks:
        if cyl == cylinder:
            dt = by_pos.get((cyl, head & 1))
            if dt is None or sector not in dt[5]:
                raise SystemExit(f"donor has no sector {sector:02X} on cylinder {cyl} head {head & 1}")
            rec = dt[8][list(dt[5]).index(sector)]
            smap += bytes([sector])
            recs = recs + [rec]
            n += 1
            if cmap:
                cmap += bytes([cyl])
            if hmap:
                hmap += bytes([head & 1])
            added += 1
            print(f"cylinder {cyl} head {head & 1}: added a second sector {sector:02X}")
        out += bytes([mode, cyl, head, n, sz]) + smap + cmap + hmap + b"".join(recs)
    if not added:
        raise SystemExit(f"no tracks on cylinder {cylinder}")
    open(out_path, "wb").write(out)


if __name__ == "__main__":
    main()
