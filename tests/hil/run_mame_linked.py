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
"""MAME's emulated 480Z with the real unit: the MAME scenarios, played through the Pico 2 (docs/testing.md
section 4, "MAME in the loop").

usage: run_mame_linked.py SCENARIO.scn ... [--port LINK_PORT] [--console CONSOLE_PORT] [--out DIR] [--keep-card]
       run_mame_linked.py --restore      (put the card's discs back as the original, after --keep-card or a failure)

Each scenario (tests/mame/scenarios/) runs in MAME's rm480z at real speed, windowless, with the rmfe_link device
(tools/mame/rmfe_link.patch) in place of MAME's own disc unit, and tools/mame/link_bridge.py relaying the link to the
FT232R on the Pico's link pins. The Pico runs the device firmware (build/rmfloppyemu.elf) with its card set up as
for run_hil_sd.py: the system disc in drive A and RMFE-TEST-B.IMD, a copy of it, in drive B. So only scenarios
with those discs can run; the others are refused.

For each scenario it:
  - restarts the Pico through the OpenOCD server, if one is running (else uses it as it is);
  - runs the scenario, logging the link from the 480Z's side (MAME's emulated time) and the bridge's turnaround;
  - checks MAME kept real speed and the link was idle for the last second;
  - compares the bytes the 480Z sent and received with the MAME reference trace (testdata/traces/<name>.trace),
    reporting each difference. These are not failures: the 480Z may take another path if, say, the drive type
    differs (rm_mq2's drives are 80-track; the card's are what RMFE.CFG says);
  - reads every record of both drives back through the link and compares them with the discs MAME's own unit
    left (testdata/traces/<name>.<drive>.imd), or with the original for a drive the scenario doesn't use. A
    difference fails;
  - puts both drives back as they were (the original image's records), unless --keep-card, so the card stays set
    up for run_hil_sd.py and the next scenario.

Writes the link log, the bridge's log, MAME's output and the screen snapshots to OUT/<name>/ (default
testdata/linked/: they hold RM bytes, so they stay out of git).

Environment: RMFE_MAME (the rm480z build with the rmfe_link patch), RMFE_MAME_ROMS (default
background-docs/mame-romsets), RMFE_TESTDATA (default testdata/), RMFE_LINK_PORT, RMFE_CONSOLE_PORT.
"""
import argparse
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "hil"))
sys.path.insert(0, str(ROOT / "tests" / "mame"))
import compare  # noqa: E402  (undo_double_step: MAME's 80-track geometry)
import run_hil  # noqa: E402
import run_hil_sd  # noqa: E402  (Unit, drive_byte, record_location, the card's images)
import scenario as scn  # noqa: E402

SYSTEM_DISC = run_hil_sd.SYSTEM_DISC
QUIET_SECONDS = 1.0
MIN_SPEED = 95.0  # percent: MAME must keep up with real time, or the 480Z's timing isn't real


def records(track):
    return 16 if track == 0 else 36  # FM 16 x 128 on track 0, MFM 9 x 512 elsewhere (docs/protocol.md)


def expected_records(disc):
    """{(track, side, record): 128 bytes} for a disc in the system disc's layout."""
    out = {}
    for (track, side), (_, secs) in disc.items():
        by_id = {r: d for r, d, _ in secs}
        for rec in range(1, records(track) + 1):
            sid, off = run_hil_sd.record_location(track, rec)
            if sid in by_id and by_id[sid] is not None:
                out[(track, side, rec)] = by_id[sid][off:off + 128]
    return out


def read_drive(u, drive, keys):
    """{(track, side, record): data or status list}, read through the link."""
    got = {}
    for track, side, rec in sorted(keys):
        st, data = u.read(run_hil_sd.drive_byte(drive, side, track != 0), track, rec)
        got[(track, side, rec)] = data if st == [0, 0, 0] else st
    return got


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def link_events(path):
    out = []
    for line in path.read_text().splitlines():
        p = line.split()
        if len(p) == 3:
            out.append((float(p[0]), p[1], int(p[2], 16)))
    return out


def byte_stream(events):
    """The link as the 480Z saw it: '<xx' from the 480Z, '>xx' to it, in order."""
    return [("<" if k.startswith("RX") else ">") + f"{v:02X}" for _, k, v in events if k in ("RX", "RX!", "TX")]


def compare_with_reference(trace, events):
    import difflib
    ref = byte_stream(link_events_from_trace(trace))
    ours = byte_stream(events)
    if ref == ours:
        return f"identical to the MAME reference ({len(ref)} bytes)", []
    sm = difflib.SequenceMatcher(None, ref, ours, autojunk=False)
    diffs = []
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        ctx = " ".join(ours[max(0, j1 - 6):j1])
        diffs.append(f"{op} at byte {j1} (after ...{ctx}): MAME {' '.join(ref[i1:i2][:16])}"
                     f"{' ...' if i2 - i1 > 16 else ''} ({i2 - i1}), here {' '.join(ours[j1:j2][:16])}"
                     f"{' ...' if j2 - j1 > 16 else ''} ({j2 - j1})")
    same = sum(b.size for b in sm.get_matching_blocks())
    return f"{same} of {len(ref)} reference bytes in the same order; {len(ours)} here", diffs


def link_events_from_trace(trace):
    out = []
    for line in trace.read_text().splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in ("RX", "TX"):
            try:
                out.append((float(p[0]), p[1], int(p[2], 16)))
            except ValueError:
                pass
    return out


def restart_pico(server, console, far):
    report = run_hil_sd.restart(server, console, far)
    if not any("serving the 480Z link" in l for l in report):
        raise SystemExit("the Pico didn't come back after a reset:\n  " + "\n  ".join(report))
    return report


def run_scenario(s, args, mame, roms, testdata, server, console):
    print(f"== {s.name}")
    if s.drives.get("A") != SYSTEM_DISC or s.drives.get("B", SYSTEM_DISC) != SYSTEM_DISC or s.readonly or s.drops:
        print(f"   refused: needs discs other than the card's (A {SYSTEM_DISC}, B a copy of it)")
        return None
    out = args.out / s.name
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    problems = []

    far = run_hil.Far480Z(args.port)
    if server:
        restart_pico(server, console, far)
    far.p.close()

    port = free_port()
    bridge = subprocess.Popen([sys.executable, str(ROOT / "tools" / "mame" / "link_bridge.py"), "--once",
                               "--listen", f"127.0.0.1:{port}", "--port", args.port, "--log", str(out / "bridge.log")],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    first = bridge.stdout.readline()
    if not first.startswith("bridge:"):
        raise SystemExit(f"the bridge didn't start: {first}{bridge.stdout.read()}")

    snaps = sorted(set(s.snaps) | {s.seconds - 0.5})
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", MQ2_KEYS=s.mq2_keys(),
               MQ2_SNAP=",".join(f"{t:g}" for t in snaps), RMFE_LINK_LOG=str(out / "link.log"))
    cmd = [str(mame), "rm480z", "-rompath", str(roms), "-rs232", "rmfe_link", "-bitb", f"socket.127.0.0.1:{port}",
           "-autoboot_script", str(ROOT / "tools" / "mame" / "mq2trace.lua"), "-video", "none", "-sound", "none",
           "-seconds_to_run", f"{s.seconds:g}", "-skip_gameinfo"]  # no -nothrottle: real speed
    t0 = time.monotonic()
    r = subprocess.run(cmd, cwd=out, env=env, capture_output=True, text=True, timeout=s.seconds * 3 + 60)
    (out / "mame.out").write_text(r.stdout + r.stderr)
    bridge_out = bridge.communicate(timeout=30)[0]
    (out / "bridge.out").write_text(first + bridge_out)
    print(f"   MAME ran {s.seconds:g} s in {time.monotonic() - t0:.1f} s")
    for line in bridge_out.splitlines():
        if "turnaround" in line or "bytes to the unit" in line:
            print("   " + line)
    if r.returncode != 0:
        problems.append(f"MAME failed ({r.returncode}): {r.stderr.strip()[-300:]}")
    m = re.search(r"Average speed: ([\d.]+)%", r.stdout + r.stderr)
    speed = float(m.group(1)) if m else 0.0
    if speed < MIN_SPEED:
        problems.append(f"MAME ran at {speed:.1f}% of real speed (needs {MIN_SPEED:g}%)")

    events = link_events(out / "link.log")
    link = [t for t, k, _ in events if k in ("RX", "RX!", "TX")]
    if not link:
        problems.append("no link traffic")
    elif link[-1] > s.seconds - QUIET_SECONDS:
        problems.append(f"the link was still busy at {link[-1]:.3f} s of {s.seconds:g} s")
    flagged = [(t, v) for t, k, v in events if k == "RX!"]
    if flagged:
        print(f"   {len(flagged)} bytes from the 480Z had bad parity as MAME sent them (relayed with good parity): "
              + ", ".join(f"{v:02X} at {t:.3f}" for t, v in flagged[:6]))

    trace = s.trace_path(testdata)
    if trace.exists():
        summary, diffs = compare_with_reference(trace, events)
        print(f"   link: {summary}")
        for d in diffs[:8]:
            print(f"     {d}")
        if len(diffs) > 8:
            print(f"     ... {len(diffs) - 8} more")
    else:
        print(f"   link: no reference trace ({trace.name}); run tests/mame/make_traces.py for one")

    original = run_hil_sd.check_hfe_vs_imd.read_imd(testdata / "images" / SYSTEM_DISC)
    orig = expected_records(original)
    far = run_hil.Far480Z(args.port)
    u = run_hil_sd.Unit(far, run_hil.Checks())
    if not u.go_fast():
        problems.append("the unit didn't answer 00 after the scenario")
        far.p.close()
        return problems
    for drive, letter in ((0, "A"), (1, "B")):
        want = orig
        if letter in s.drives:
            path = s.mame_image_path(testdata, letter)
            if not path.exists():
                problems.append(f"no disc from MAME's run: {path}")
                continue
            want = expected_records(compare.undo_double_step(run_hil_sd.check_hfe_vs_imd.read_imd(path), original))
        t1 = time.monotonic()
        got = read_drive(u, drive, orig.keys())
        bad = [k for k in sorted(orig) if got.get(k) != want.get(k)]
        changed = sum(1 for k in orig if want.get(k) != orig[k])
        print(f"   drive {letter}: {len(got)} records read in {time.monotonic() - t1:.0f} s; "
              f"{changed} differ from the original in MAME's disc")
        if bad:
            problems.append(f"drive {letter}: {len(bad)} records differ from MAME's disc, first "
                            + ", ".join(f"T{t} S{sd} R{rc}" for t, sd, rc in bad[:5]))
        if not args.keep_card:
            restore(u, drive, letter, got, orig, problems)
    far.p.close()
    return problems


def send_paced(far, data, chunk=16):
    """Sends as the 480Z does, only while the unit asserts CTS (it is waiting for a byte). The unit has only the
    UART's 32-byte receive FIFO, and its CTS is down while it works on the card (a track written back or loaded),
    so a sender that ignores CTS overruns it. TIOCMGET is slower than a byte, so CTS is checked every 16 bytes:
    half the FIFO."""
    for i in range(0, len(data), chunk):
        end = time.monotonic() + run_hil_sd.SAVE_TIMEOUT
        while not far.p.cts and time.monotonic() < end:
            time.sleep(0.0005)
        far.send(data[i:i + chunk])


def write_record(u, db, track, rec, data):
    """04 as the 480Z sends it: the record after the unit has answered the command, paced by CTS (run_hil_sd's
    Unit.write sends everything at once, which only works when the unit has the track already). Returns accept,
    status, block ack, result."""
    send_paced(u.far, [0x04, db, track, rec])
    head = u.far.recv(2, timeout=run_hil_sd.SAVE_TIMEOUT)
    if [b for b, _ in head] != [0, 0]:
        return [b for b, _ in head]
    send_paced(u.far, list(data))
    return [0, 0] + [b for b, _ in u.far.recv(2, timeout=run_hil_sd.SAVE_TIMEOUT)]


def restore_card(args, testdata):
    """Reads both drives and puts back every record that differs from the original image."""
    orig = expected_records(run_hil_sd.check_hfe_vs_imd.read_imd(testdata / "images" / SYSTEM_DISC))
    far = run_hil.Far480Z(args.port)
    u = run_hil_sd.Unit(far, run_hil.Checks())
    problems = []
    if not u.go_fast():
        problems.append("the unit didn't answer 00")
    else:
        for drive, letter in ((0, "A"), (1, "B")):
            got = read_drive(u, drive, orig.keys())
            print(f"   drive {letter}: {sum(1 for k in orig if got[k] != orig[k])} records differ from the original")
            restore(u, drive, letter, got, orig, problems)
    far.p.close()
    return problems


def restore(u, drive, letter, got, orig, problems):
    """Writes back every record that differs from the original image, a track at a time."""
    todo = sorted(k for k in orig if got.get(k) != orig[k])
    if not todo:
        return
    for track, side, rec in todo:
        db = run_hil_sd.drive_byte(drive, side, track != 0)
        res = write_record(u, db, track, rec, orig[(track, side, rec)])
        if res != [0, 0, 0, 0]:
            problems.append(f"restoring drive {letter} T{track} S{side} R{rec}: {res}")
            return
    for side in (0, 1):
        for mfm in (False, True):
            res = u.flush(run_hil_sd.drive_byte(drive, side, mfm))
            if res != [0, 0, 0]:
                problems.append(f"restoring drive {letter}: 08 answered {res}")
    back = read_drive(u, drive, todo)
    wrong = [k for k in todo if back[k] != orig[k]]
    print(f"   drive {letter}: {len(todo)} records put back" + (f", {len(wrong)} still wrong" if wrong else ""))
    if wrong:
        problems.append(f"drive {letter}: {len(wrong)} records couldn't be put back")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenarios", nargs="*", type=pathlib.Path)
    ap.add_argument("--port", help="the adapter on the link pins (default: found by USB ID, or RMFE_LINK_PORT)")
    ap.add_argument("--console", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--out", type=pathlib.Path, help="where to put the logs (default RMFE_TESTDATA/linked)")
    ap.add_argument("--keep-card", action="store_true", help="leave the discs as the scenario left them")
    ap.add_argument("--restore", action="store_true", help="only put the card's discs back as the original")
    args = ap.parse_args()

    roms = pathlib.Path(os.environ.get("RMFE_MAME_ROMS", ROOT / "background-docs" / "mame-romsets"))
    testdata = pathlib.Path(os.environ.get("RMFE_TESTDATA", ROOT / "testdata"))
    args.out = (args.out or testdata / "linked").resolve()
    args.port = run_hil.PORTS.find_port("link", args.port)
    if not (testdata / "images" / SYSTEM_DISC).exists():
        sys.exit(f"missing {testdata / 'images' / SYSTEM_DISC}")  # missing data fails (docs/testing.md rule 7)

    if args.restore:
        problems = restore_card(args, testdata)
        for p in problems:
            print(f"   FAIL  {p}")
        sys.exit(1 if problems else 0)
    if not args.scenarios:
        ap.error("no scenarios")
    found = os.environ.get("RMFE_MAME") or shutil.which("rm480z")
    if not found or not pathlib.Path(found).is_file():
        sys.exit("MAME's rm480z build not found: set RMFE_MAME to it, or put it on the PATH")
    mame = pathlib.Path(found)
    git = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                         text=True).stdout.strip()
    print(f"git {git}; MAME {mame}")
    try:
        server = socket.create_connection(("localhost", run_hil.run_on_target.TCL_PORT), timeout=60)
        console = serial.Serial(run_hil.PORTS.find_port("console", args.console), 115200, timeout=0.2)
    except OSError:
        server = console = None
        print("no OpenOCD server: the Pico isn't restarted between scenarios")

    results = {}
    for path in args.scenarios:
        results[path.stem] = run_scenario(scn.Scenario(path), args, mame, roms, testdata, server, console)
        for p in results[path.stem] or []:
            print(f"   FAIL  {p}")

    print("\nsummary")
    for name, problems in results.items():
        print(f"  {name:20} {'refused' if problems is None else 'FAIL' if problems else 'ok'}")
    sys.exit(1 if any(results.values()) else 0)


if __name__ == "__main__":
    main()
