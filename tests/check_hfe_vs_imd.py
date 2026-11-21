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
"""Independent check of src/flux: compare hfedump's decode of an HFE image with an ImageDisk (.IMD)
image of the same disc, read here by a separate IMD parser that shares no code with src/flux.

usage: check_hfe_vs_imd.py HFEDUMP image.hfe image.imd

Every sector is matched by (cylinder, head, position on the track, sector ID). For each track the
sector IDs must be the same in the same rotational order (IMD records them in that order), and the
data must be identical. Exit status 0 only if everything matches; differences are listed.
IMD format: ImageDisk 1.18 documentation (Dave Dunfield).
"""
import subprocess
import sys

IMD_SIZES = [128 << n for n in range(7)]


def read_imd(path):
    """{(cyl, head): [(id, data or None, kind)]} in recorded order. kind: 'ok', 'deleted', 'error', 'missing'."""
    d = open(path, "rb").read()
    i = d.index(b"\x1a") + 1
    tracks = {}
    while i < len(d):
        mode, cyl, head, nsec, size_code = d[i:i + 5]
        i += 5
        ids = list(d[i:i + nsec])
        i += nsec
        if head & 0x80:
            i += nsec  # cylinder map
        if head & 0x40:
            i += nsec  # head map
        size = IMD_SIZES[size_code]
        secs = []
        for r in ids:
            t = d[i]
            i += 1
            if t == 0:
                secs.append((r, None, "missing"))
                continue
            if t % 2 == 1:
                data = d[i:i + size]
                i += size
            else:
                data = bytes([d[i]]) * size
                i += 1
            kind = {1: "ok", 2: "ok", 3: "deleted", 4: "deleted", 5: "error", 6: "error", 7: "deleted-error",
                    8: "deleted-error"}[t]
            secs.append((r, data, kind))
        tracks[(cyl, head & 1)] = (mode, secs)
    return tracks


def read_hfedump(hfedump, path):
    out = subprocess.run([hfedump, "--data", path], capture_output=True, text=True, check=True).stdout
    tracks = {}
    for line in out.splitlines():
        p = line.split()
        if p[0] == "T":
            tracks[(int(p[1]), int(p[2]))] = []
        elif p[0] == "S":
            f = dict(x.split("=", 1) for x in p[4:] if "=" in x)
            flags = int(f["flags"], 16)
            data = bytes.fromhex(p[-1]) if "=" not in p[-1] else None
            kind = "ok"
            if flags & 0x02:
                kind = "missing"
            elif flags & 0x04:
                kind = "error"
            if flags & 0x08:
                kind = "deleted" if kind == "ok" else "deleted-error"
            tracks[(int(p[1]), int(p[2]))].append((int(f["r"], 16), data, kind))
    return tracks


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    hfe = read_hfedump(sys.argv[1], sys.argv[2])
    imd = read_imd(sys.argv[3])
    problems = 0
    sectors = 0
    for key in sorted(set(hfe) | set(imd)):
        h = hfe.get(key, [])
        _, m = imd.get(key, (None, []))
        if not m and not h:
            continue
        hid, mid = [s[0] for s in h], [s[0] for s in m]
        if hid != mid:
            print(f"cyl {key[0]} head {key[1]}: IDs differ: hfe {' '.join('%02X' % x for x in hid)}"
                  f" / imd {' '.join('%02X' % x for x in mid)}")
            problems += 1
            continue
        for (r, hd, hk), (_, md, mk) in zip(h, m):
            sectors += 1
            if hk != mk or hd != md:
                print(f"cyl {key[0]} head {key[1]} sector {r:02X}: hfe {hk} / imd {mk}"
                      + ("" if hd == md else ", data differs"))
                problems += 1
    print(f"{sectors} sectors compared, {problems} differences")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
