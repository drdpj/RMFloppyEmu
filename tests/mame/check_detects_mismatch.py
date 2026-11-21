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
"""The MAME comparison must be able to fail: mamecmp is run on copies of the boot trace with one thing
wrong, and must report a mismatch at exactly that line each time. And a scenario's 'differs' line must
excuse only what it names: compare.py is run on format_qd with that line moved to another track, and must
report both the real difference and the stale declaration.

usage: check_detects_mismatch.py MAMECMP TESTDATA WORKDIR
"""
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scenario as scn  # noqa: E402


def run(mamecmp, rom, trace, work, image):
    return subprocess.run([mamecmp, str(rom), str(trace), str(work), "--80-track", f"A={image}"],
                          capture_output=True, text=True, timeout=300)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    mamecmp, testdata, work = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]) / "selfcheck"
    s = scn.Scenario(scn.HERE / "scenarios" / "boot.scn")
    trace, rom = s.trace_path(testdata), testdata / "roms" / "IDC3-1I.ROM"
    image = testdata / "images" / s.drives["A"]
    for f in (trace, rom, image):
        if not f.is_file():
            sys.exit(f"FAIL: missing {f} (run tests/mame/make_traces.py)")
    work.mkdir(parents=True, exist_ok=True)
    lines = trace.read_text().splitlines(keepends=True)

    r = run(mamecmp, rom, trace, work, image)
    if r.returncode != 0:
        sys.exit(f"FAIL: the unaltered boot trace doesn't match:\n{r.stdout}{r.stderr}")

    tx = [i for i, ln in enumerate(lines) if ln.split()[1:2] == ["TX"]]
    rx = [i for i, ln in enumerate(lines) if ln.split()[1:2] == ["RX"]]
    cases = {
        "a wrong unit byte": (tx[len(tx) // 2], lambda p: f"{p[0]} TX {int(p[2], 16) ^ 0x01:02X}\n"),
        "a missing unit byte": (tx[len(tx) // 3], lambda p: ""),
        "an extra unit byte": (rx[len(rx) // 2], lambda p: f"{p[0]} TX 00\n{' '.join(p)}\n"),
    }
    for what, (at, change) in cases.items():
        bad = list(lines)
        bad[at] = change(lines[at].split())
        path = work / "altered.trace"
        path.write_text("".join(bad))
        r = run(mamecmp, rom, path, work, image)
        if r.returncode != 1 or "MISMATCH" not in r.stderr:
            sys.exit(f"FAIL: {what} at line {at + 1} was not detected:\n{r.stdout}{r.stderr}")
        reported = [ln for ln in r.stderr.splitlines() if ln.startswith("  ->")]
        # the line numbers move by one when a line is added; the mismatch is at the altered line or the next
        if not reported or int(reported[0].split()[2]) not in (at + 1, at + 2):
            sys.exit(f"FAIL: {what} at line {at + 1} was reported elsewhere:\n{r.stderr}")
        print(f"{what} at line {at + 1}: detected")

    check_differs_is_narrow(mamecmp, testdata, work)


def check_differs_is_narrow(mamecmp, testdata, work):
    src = scn.HERE / "scenarios" / "format_qd.scn"
    text = src.read_text()
    if "\ndiffers B 0 0 02-10 " not in text:
        sys.exit(f"FAIL: {src} no longer declares 'differs B 0 0 02-10': update this check")
    moved = work / "format_qd.scn"  # same name, so compare.py finds format_qd's trace and MAME image
    moved.write_text(text.replace("\ndiffers B 0 0 02-10 ", "\ndiffers B 1 0 01-09 "))
    r = subprocess.run([sys.executable, "-I", str(scn.HERE / "compare.py"), mamecmp, str(moved), str(testdata),
                        str(work / "differs")], capture_output=True, text=True, timeout=300)
    out = r.stdout + r.stderr
    if (r.returncode != 1 or "cylinder 0 head 0 sector 02: ours ok, MAME ok" not in out
            or "cylinder 1 head 0: the declared difference" not in out):
        sys.exit(f"FAIL: a 'differs' line on the wrong track was not caught both ways:\n{out}")
    print("a 'differs' line on the wrong track: the real difference and the stale line both reported")


if __name__ == "__main__":
    main()
