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
"""Real-data check of the SCP path (scp + flux_pll + ibm_track) on the 4MATION captures, which were
read at 360 rpm. The expected layout is the protection described in docs/protocol.md, "Copy-protected
discs", as decoded from these captures and confirmed by booting the images built from them in MAME.

usage: check_4mation_scp.py SCPDUMP image.scp [--all-good] [--damaged CYL:SIDE:ID ...]
--all-good  also require every sector to read CRC-good on some revolution (Dragon World; the Flowers
            of Crystal disc has a radial scratch)
--damaged   an ID known to be unreadable on every revolution because of physical damage; it is
            expected to be missing from the layout (and the test fails if it reads after all)
"""
import subprocess
import sys

EXPECT = {
    0: [0x0D, 0x0E, 0x0F, 0x10, 0x09, 0x0A, 0x0B, 0x0C, 0xD3, 0x5C, 0x57, 0x72, 0x01, 0x02, 0x03, 0x04],
    2: [0x05, 0x08, 0x04, 0x08, 0x03, 0x07, 0x02, 0x06, 0x01],
    3: [0xA6, 0x67, 0x32, 0x09, 0x41, 0x29, 0x29, 0x1D, 0x2A],
}
SECTOR8_DIFFS = {o + h for o in (0x02, 0x28, 0x46, 0x48, 0x4D, 0x4E, 0x4F) for h in (0, 0x100)}


def main():
    args = sys.argv[1:]
    damaged = set()
    while "--damaged" in args:
        k = args.index("--damaged")
        c, sd, r = args[k + 1].split(":")
        damaged.add((int(c), int(sd), int(r, 16)))
        del args[k:k + 2]
    all_good = "--all-good" in args
    args = [a for a in args if a != "--all-good"]
    if len(args) != 2:
        raise SystemExit(__doc__)
    sys.argv = [sys.argv[0]] + args
    out = subprocess.run([sys.argv[1], "--data", sys.argv[2]], capture_output=True, text=True, check=True).stdout
    secs, tracks = {}, {}
    for line in out.splitlines():
        p = line.split()
        if p[0] == "T":
            tracks[(int(p[1]), int(p[2]), int(p[3]))] = line
        elif p[0] == "S":
            f = dict(x.split("=", 1) for x in p[5:] if "=" in x)
            secs.setdefault((int(p[1]), int(p[2]), int(p[3])), []).append(
                (int(f["r"], 16), int(f["flags"], 16), p[-1] if "=" not in p[-1] else None))
    fails = []
    for side in (0, 1):
        for cyl, want in EXPECT.items():
            # The layout must show in full on at least one revolution (a scratch can hide an ID on another).
            seen = []
            for rev in range(8):
                got = [r for r, fl, _ in secs.get((cyl, side, rev), []) if not fl & 0x01 and not (r == 0 and fl & 0x04)]
                if got:
                    seen.append(got)
            want = [r for r in want if (cyl, side, r) not in damaged]
            if want not in seen:
                fails.append(f"cyl {cyl} side {side}: IDs {' / '.join(' '.join('%02X' % r for r in g) for g in seen)}")
        copies = []
        for rev in range(8):
            c = [d for r, fl, d in secs.get((2, side, rev), []) if r == 8 and not fl & 0x05]
            if len(c) == 2:
                copies = c
                break
        if len(copies) != 2:
            fails.append(f"cyl 2 side {side}: no revolution with two good sector 08s")
        else:
            a, b = bytes.fromhex(copies[0]), bytes.fromhex(copies[1])
            diffs = {i for i in range(512) if a[i] != b[i]}
            if diffs != SECTOR8_DIFFS:
                fails.append(f"cyl 2 side {side}: sector 08 copies differ at {sorted(diffs)}")
        for cyl in (40, 41):
            if secs.get((cyl, side, 0)):
                fails.append(f"cyl {cyl} side {side}: expected blank")
    if all_good:
        bad = {}
        for (c, s, rev), lst in secs.items():
            for i, (r, fl, _) in enumerate(lst):
                key = (c, s, i, r)
                if r == 0 and fl & 0x04 and i == len(lst) - 1:
                    continue
                # any failure counts: ID or data CRC, no data field, dropped or undecodable data
                bad[key] = bad.get(key, True) and bool(fl & 0x37)
        never = [k for k, v in bad.items() if v]
        if never:
            fails.append(f"{len(never)} sectors never read good, e.g. {never[:3]}")
    for f in fails:
        print("FAIL", f)
    print(f"{sys.argv[2]}: {'ok' if not fails else str(len(fails)) + ' failures'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
