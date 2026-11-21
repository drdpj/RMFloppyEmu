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
"""Regression check for tools/scpdump/rebuild.py's SCP writer, on the scratched Flowers of Crystal capture
(the hardest case: scratched sectors, erased IDs, a lost data mark, duplicate IDs).

usage: check_rebuild_scp.py SCPDUMP capture.scp patch.IMD workdir

Builds the 360 rpm SCP and checks:
  - no flux word is 0000 (a zero interval, as a transition exactly on the index once produced, is
    read as an overflow word; HxC rejected those files);
  - no sector gap is more than 1.2 times its track's usual spacing (restored sectors once sat too late);
  - every sector reads good and the protection layout is intact (check_4mation_scp.py --all-good).
"""
import os
import struct
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    scpdump, capture, patch, work = sys.argv[1:]
    os.makedirs(work, exist_ok=True)
    out = os.path.join(work, "rebuilt_360.scp")
    subprocess.run([sys.executable, "-I", os.path.join(here, "..", "tools", "scpdump", "rebuild.py"), "scp", scpdump,
                    capture, out, "--patch", patch, "--rpm", "360"], check=True, capture_output=True)
    fails = []
    d = open(out, "rb").read()
    revs = d[5]
    for tn, off in enumerate(struct.unpack("<168I", d[0x10:0x2B0])):
        if not off:
            continue
        for r in range(revs):
            _, n, doff = struct.unpack("<3I", d[off + 4 + 12 * r:off + 16 + 12 * r])
            words = struct.unpack(f">{n}H", d[off + doff:off + doff + 2 * n])
            if 0 in words:
                fails.append(f"track {tn} rev {r}: 0000 word at flux {words.index(0)}")
    dump = subprocess.run([scpdump, out], capture_output=True, text=True, check=True).stdout
    pos, cells = {}, {}
    for line in dump.splitlines():
        p = line.split()
        if p[0] == "T" and p[3] == "0":
            cells[(p[1], p[2])] = int(next(x for x in p if x.startswith("cells="))[6:])
        elif p[0] == "S" and p[3] == "0" and not int(p[12].split("=")[1], 16) & 1:
            pos.setdefault((p[1], p[2]), []).append(int(p[5].split("=")[1]))
    for key, ps in pos.items():
        gaps = [b - a for a, b in zip(ps, ps[1:])]
        med = sorted(gaps)[len(gaps) // 2]
        for g in gaps:
            if g > 1.2 * med:
                fails.append(f"cyl {key[0]} side {key[1]}: gap {g} cells against the usual {med}")
    chk = subprocess.run([sys.executable, "-I", os.path.join(here, "check_4mation_scp.py"), scpdump, out, "--all-good"],
                         capture_output=True, text=True)
    if chk.returncode:
        fails.append("check_4mation_scp --all-good: " + chk.stdout.strip().splitlines()[-1])
    for f in fails:
        print("FAIL", f)
    print(f"{out}: {'ok' if not fails else str(len(fails)) + ' failures'}")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
