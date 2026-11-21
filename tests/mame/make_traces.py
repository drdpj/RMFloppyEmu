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
"""Make the MAME reference traces for the comparison tests (docs/testing.md section 3).

usage: make_traces.py [--explore DIR] [SCENARIO.scn ...]   (default: every tests/mame/scenarios/*.scn)

--explore DIR writes everything to DIR instead and skips the check that the link was idle at the end: for
working out a new scenario (add "snap" lines to see the screen).

Runs each scenario in MAME's rm480z driver, headless, with tools/mame/mq2trace.lua, on copies of its
images. Writes testdata/traces/<name>.trace and the images as MAME left them (<name>.A.imd, ...).
The traces hold RM bytes (the boot block, disc contents), so they stay in testdata/, never in git.

Environment: RMFE_MAME (the rm480z single-driver MAME build; default: rm480z on the PATH), RMFE_MAME_ROMS (default background-docs/mame-romsets),
RMFE_TESTDATA (default testdata/).
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scenario as scn  # noqa: E402

QUIET_SECONDS = 1.0  # the link must be idle this long before the end, so nothing is cut off


def last_link_time(trace):
    last = None
    for line in trace.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("RX", "TX"):
            last = float(p[0])
    return last


def run(s, mame, roms, testdata, explore=None):
    out_dir = Path(explore) if explore else Path(testdata) / "traces"
    out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"rmfe-{s.name}-") as tmp:
        tmp = Path(tmp)
        args = [str(mame), "rm480z", "-rompath", str(roms)]
        for letter, image in sorted(s.drives.items()):
            src = Path(testdata) / "images" / image
            if not src.is_file():
                raise SystemExit(f"{s.name}: missing image {src}")
            work = tmp / f"{letter}{src.suffix.lower()}"
            scn.prepare_image(s, letter, src, work)  # MAME writes to its images: never give it the original
            if letter in s.readonly:
                work.chmod(0o444)  # MAME mounts a file it can't write as a write-protected disc
            args += ["-flop1" if letter == "A" else "-flop2", str(work)]
        args += ["-autoboot_script", str(scn.REPO / "tools" / "mame" / "mq2trace.lua"), "-video", "none",
                 "-sound", "none", "-nothrottle", "-seconds_to_run", f"{s.seconds:g}", "-skip_gameinfo"]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", MQ2_KEYS=s.mq2_keys(),
                   MQ2_SNAP=",".join(f"{t:g}" for t in s.snaps))
        r = subprocess.run(args, cwd=tmp, env=env, capture_output=True, text=True, timeout=1800)
        if r.returncode != 0:
            raise SystemExit(f"{s.name}: MAME failed ({r.returncode}):\n{r.stderr}")
        trace = r.stdout
        last = last_link_time(trace)
        if last is None and not explore:
            raise SystemExit(f"{s.name}: no link traffic in the trace")
        if not explore and last > s.seconds - QUIET_SECONDS:
            raise SystemExit(f"{s.name}: the link was still busy at {last:.3f} s of {s.seconds:g} s: "
                             "make the scenario longer")
        header = f"# MAME reference trace for tests/mame/scenarios/{s.path.name} (make_traces.py)\n"
        (out_dir / f"{s.name}.trace").write_text(header + trace)
        for letter, image in s.drives.items():
            shutil.copyfile(tmp / f"{letter}{Path(image).suffix.lower()}",
                            out_dir / s.mame_image_path(testdata, letter).name)
        snaps = out_dir / f"{s.name}.snap"
        shutil.rmtree(snaps, ignore_errors=True)
        if (tmp / "snap" / "rm480z").is_dir():
            shutil.copytree(tmp / "snap" / "rm480z", snaps)  # 0000.png is MAME's own, at the start
        print(f"{s.name}: {out_dir / (s.name + '.trace')} (last link byte at {last or 0:.3f} s)")


def main():
    found = os.environ.get("RMFE_MAME") or shutil.which("rm480z")
    if not found or not Path(found).is_file():
        raise SystemExit("MAME's rm480z build not found: set RMFE_MAME to it, or put it on the PATH")
    mame = Path(found)
    roms = Path(os.environ.get("RMFE_MAME_ROMS", scn.REPO / "background-docs" / "mame-romsets"))
    testdata = Path(os.environ.get("RMFE_TESTDATA", scn.REPO / "testdata"))
    args, explore = sys.argv[1:], None
    if args[:1] == ["--explore"]:
        if len(args) < 2:
            raise SystemExit(__doc__)
        explore, args = args[1], args[2:]
    files = args or sorted(str(p) for p in (scn.HERE / "scenarios").glob("*.scn"))
    if not files:
        raise SystemExit("no scenarios")
    for f in files:
        run(scn.Scenario(f), mame, roms, testdata, explore)


if __name__ == "__main__":
    main()
