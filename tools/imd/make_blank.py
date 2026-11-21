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
"""Write a blank disc image in the layout RM's FORMAT lays down, so formatting it on the unit goes in place.

usage: make_blank.py OUT.IMD [--cylinders 40|80] [--sides 1|2] [--uncompressed]

Each side: cylinder 0 FM 16 x 128 (IMD mode 2), the rest MFM 9 x 512 (mode 5), sector IDs in order, C = the
cylinder, H = 0 on both sides (a head map on side 1), every sector E5: as the CP/M system disc is laid out and as
the IDC's 0C writes a track (docs/protocol.md, "Implementation status": 0C). With the same IDs, order and sizes, a
format rewrites each sector in place (disk_imd_update), so the image is never rewritten whole. Compressed (one fill
byte a sector) by default; --uncompressed stores every sector in full, as an image is after its first rewrite.
"""
import argparse
import pathlib


def track(cyl, side, uncompressed):
    fm = cyl == 0
    n, code, size = (16, 0, 128) if fm else (9, 2, 512)
    out = bytes([2 if fm else 5, cyl, (0x40 | side) if side else 0, n, code]) + bytes(range(1, n + 1))
    if side:
        out += bytes(n)  # head map: H = 0, as the IDC writes it
    for _ in range(n):
        out += (b"\x01" + b"\xe5" * size) if uncompressed else b"\x02\xe5"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", type=pathlib.Path)
    ap.add_argument("--cylinders", type=int, choices=(40, 80), default=40)
    ap.add_argument("--sides", type=int, choices=(1, 2), default=2)
    ap.add_argument("--uncompressed", action="store_true")
    a = ap.parse_args()
    data = b"IMD 1.18: 01/01/2026 00:00:00\r\nRMFloppyEmu blank disc (tools/imd/make_blank.py)\r\n\x1a"
    for cyl in range(a.cylinders):
        for side in range(a.sides):
            data += track(cyl, side, a.uncompressed)
    a.out.write_bytes(data)
    print(f"{a.out}: {a.cylinders} cylinders, {a.sides} side(s), {len(data)} bytes")


if __name__ == "__main__":
    main()
