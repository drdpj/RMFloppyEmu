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
"""One MAME comparison test (docs/testing.md section 3, level 2).

usage: compare.py MAMECMP SCENARIO.scn TESTDATA WORKDIR

1. mamecmp replays the scenario's reference trace (made by make_traces.py) into our command layer:
   every byte our unit sends must be the IDC's.
2. The discs our unit leaves must hold what MAME's do: every track's sector IDs in order, and each
   sector's data and status, read by the independent IMD reader in tests/check_hfe_vs_imd.py. The only
   exception is a scenario's 'differs' line (a documented difference): those sectors' data must differ,
   and the difference is printed on every run.

Fails if the trace, the ROM or an image is missing: run tests/mame/make_traces.py first.
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scenario as scn  # noqa: E402

ROM = "IDC3-1I.ROM"  # the IDC ROM MAME's rm_mq2 runs (CRC 39e2cdf0)


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def undo_double_step(mame, ours):
    """MAME's drives are 80-track: when it saves a 40-track disc it writes it in the drive's geometry, disc
    cylinder c at track 2c, the odd tracks blank (seen in every image MAME writes back). Map it back."""
    if max(c for c, _ in mame) < 2 * max(c for c, _ in ours):
        return mame
    if any(c % 2 and secs for (c, _), (_, secs) in mame.items()):
        return mame
    return {(c // 2, h): v for (c, h), v in mame.items() if c % 2 == 0}


def compare_discs(ours, mames, letter, differs):
    """differs: the scenario's known differences, (drive, cylinder, side, first ID, last ID, reason): those
    sectors' data may differ, and must (else the exception is stale). Everything else is compared."""
    a = scn.read_imd(ours)
    b = undo_double_step(scn.read_imd(mames), a)
    problems = []
    excused = {}  # (cylinder, head, sector ID) -> reason
    for d, c, h, lo, hi, why in differs:
        if d == letter:
            for r in range(lo, hi + 1):
                excused[(c, h, r)] = why
    seen = set()
    for key in sorted(set(a) | set(b)):
        if key not in a or key not in b:
            problems.append(f"{letter}: cylinder {key[0]} head {key[1]} only in {'MAME' if key in b else 'ours'}")
            continue
        sa, sb = a[key][1], b[key][1]
        if [s[0] for s in sa] != [s[0] for s in sb]:
            problems.append(f"{letter}: cylinder {key[0]} head {key[1]}: IDs {[s[0] for s in sa]} vs MAME "
                            f"{[s[0] for s in sb]}")
            continue
        for (r, da, ka), (_, db, kb) in zip(sa, sb):
            if ka == kb and da != db and (key[0], key[1], r) in excused:
                seen.add((key[0], key[1], r))
                continue
            if (da, ka) != (db, kb):
                diff = next((i for i in range(min(len(da or b""), len(db or b""))) if da[i] != db[i]), None)
                problems.append(f"{letter}: cylinder {key[0]} head {key[1]} sector {r:02X}: ours {ka}, MAME {kb}"
                                + (f", first difference at byte {diff}" if diff is not None else ""))
    for d, c, h, lo, hi, why in differs:
        if d != letter:
            continue
        hit = sorted(r for cc, hh, r in seen if (cc, hh) == (c, h) and lo <= r <= hi)
        if hit:
            print(f"KNOWN DIFFERENCE {letter}: cylinder {c} head {h} sectors {hit[0]:02X}-{hit[-1]:02X} "
                  f"({len(hit)}) data differ from MAME's: {why}")
        else:
            problems.append(f"{letter}: cylinder {c} head {h}: the declared difference ({why}) is gone; "
                            "remove the 'differs' line")
    return problems


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    mamecmp, s, testdata, work = sys.argv[1], scn.Scenario(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    trace = s.trace_path(testdata)
    if not trace.is_file():
        fail(f"no reference trace {trace}: run tests/mame/make_traces.py")
    rom = testdata / "roms" / ROM
    if not rom.is_file():
        fail(f"no IDC ROM {rom}")
    work = work / s.name
    work.mkdir(parents=True, exist_ok=True)
    args = [mamecmp, str(rom), str(trace), str(work), "--80-track"]
    args += [f"--read-only={letter}" for letter in sorted(s.readonly)]
    for letter, image in sorted(s.drives.items()):
        src = testdata / "images" / image
        if not src.is_file():
            fail(f"no image {src}")
        if any(d == letter for d, *_ in s.drops):
            prepared = work / f"input.{letter}{src.suffix.lower()}"
            scn.prepare_image(s, letter, src, prepared)
            src = prepared
        args.append(f"{letter}={src}")
    r = subprocess.run(args, capture_output=True, text=True, timeout=600)
    sys.stdout.write(r.stdout)
    sys.stdout.write(r.stderr)
    if r.returncode != 0:
        fail("the link bytes differ from MAME's")

    problems = []
    for letter, image in sorted(s.drives.items()):
        if Path(image).suffix.lower() != ".imd":
            fail(f"comparing {image} with MAME's copy needs an IMD reader for its format")
        mames = s.mame_image_path(testdata, letter)
        if not mames.is_file():
            fail(f"no MAME image {mames}: run tests/mame/make_traces.py")
        problems += compare_discs(work / f"{letter}.imd", mames, letter, s.differs)
    if problems:
        print("\n".join(problems[:40]))
        fail(f"{len(problems)} differences between our discs and MAME's")
    print(f"{s.name}: link and discs match MAME")


if __name__ == "__main__":
    main()
