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
"""Hardware-in-the-loop tests of the device firmware with its SD card (docs/testing.md section 4, level 3).

usage: run_hil_sd.py FIRMWARE.elf [--port LINK_PORT] [--console CONSOLE_PORT] [--seed N] [--log FILE]

Flashes the device's own firmware (build/rmfloppyemu.elf) and plays the 480Z through the USB serial adapter, as
run_hil.py does. The card must be set up as docs/testing.md section 4 says: drive A the CP/M system disc, drive B a
scratch copy of it, RMFE-TEST-B.IMD, which this test changes and puts back. Expected data comes from the local
copies in testdata/ (read by tests/check_hfe_vs_imd.py's independent IMD reader) and from the ROM's own layout
(docs/protocol.md: the boot block's length at 1B28, the block at 1B2A), never from the firmware. It checks:
  - the start-up report: the ROM and the two images;
  - FF sends the boot block, byte for byte;
  - every record of drive A (track 0 FM, then both sides of tracks 1-39 MFM) reads back as the local IMD has it;
  - on drive B: a track reformatted FM 16 x 128, so the IMD on the card must be rewritten, then a record written
    into it, survive a restart of the Pico; then the track is put back as it was (MFM, the original data), and
    that survives a restart too.
"""
import argparse
import pathlib
import random
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tests" / "hil"))
import check_hfe_vs_imd  # noqa: E402  (the independent IMD reader)
import run_hil  # noqa: E402  (the adapter as the 480Z, and the checks)

PORTS = run_hil.run_on_target

SYSTEM_DISC = "480Z_RML_system-cpm2.2f-basic5.3g-basic6.3b.imd"
SCRATCH_B = "RMFE-TEST-B.IMD"
ROM = ROOT / "testdata" / "roms" / "IDC3-1I.ROM"
TEST_TRACK = 5  # drive B's track that is reformatted and put back
SAVE_TIMEOUT = 10.0

# The drive byte as the 480Z sends it (docs/protocol.md, "The drive byte"): bit 5 MFM, bit 0 side, bits 1-2 drive,
# with the low two bits swapped on the way in (01 <-> 10).
def drive_byte(drive, side, mfm):
    b = (drive << 1) | side
    if b & 3 in (1, 2):
        b ^= 3
    return b | (0x20 if mfm else 0)


def record_location(track, rec):
    """(sector ID, offset) of a record (docs/protocol.md): FM records are their sector; MFM is skewed 2:1."""
    if track == 0:
        return rec, 0
    k = (rec - 1) // 4
    return run_hil.SKEW[k], ((rec - 1) % 4) * 128


class Unit:
    def __init__(self, far, c):
        self.far, self.c = far, c

    def go_fast(self):
        self.far.p.rts = True  # a new exchange: the unit goes back to 9600
        self.far.line(9600, serial.PARITY_EVEN)
        self.far.send([0x00])
        got = self.far.recv(3)
        self.far.p.rts = False
        self.far.line(125000, serial.PARITY_EVEN)
        return [b for b, _ in got] == [0x5A, 0x00, 0x01]

    def read(self, db, track, rec):
        """(status bytes, data) of 02."""
        self.far.send([0x02, db, track, rec], settle=False)
        head = self.far.recv(3)
        st = [b for b, _ in head]
        if st != [0, 0, 0]:
            return st, b""
        data = bytes(b for b, _ in self.far.recv(128))
        self.far.send([0x00], settle=False)  # ack
        return st, data

    # A save that rewrites an IMD on the card takes over a second (docs/plan.md): allow SAVE_TIMEOUT for results.
    def write(self, db, track, rec, data):
        self.far.send([0x04, db, track, rec] + list(data))
        return [b for b, _ in self.far.recv(4, timeout=SAVE_TIMEOUT)]  # accept, status, block ack, result

    def flush(self, db):
        self.far.send([0x08, db])
        return [b for b, _ in self.far.recv(3, timeout=SAVE_TIMEOUT)]

    def format(self, db, track, ids):
        self.far.send([0x0C, db, track, len(ids)] + list(ids))
        return [b for b, _ in self.far.recv(3, timeout=SAVE_TIMEOUT)]


def restart(server, console, far):
    """Resets the Pico and waits for it to serve again; returns its start-up report."""
    console.reset_input_buffer()
    run_hil.run_on_target.tcl(server, "reset run")
    lines, end = [], time.monotonic() + 10
    while time.monotonic() < end:
        line = console.readline().decode(errors="replace").rstrip()
        if line:
            lines.append(line)
        if "serving the 480Z link" in line:
            break
    far.drop_input()
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", type=pathlib.Path)
    ap.add_argument("--port", help="the adapter playing the 480Z (default: found by USB ID, or RMFE_LINK_PORT)")
    ap.add_argument("--console", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--seed", type=int, default=None, help="for the record written to drive B (default: random)")
    ap.add_argument("--log", type=pathlib.Path)
    args = ap.parse_args()
    args.port = PORTS.find_port("link", args.port)
    args.console = PORTS.find_port("console", args.console)

    for p in (args.firmware, ROM, ROOT / "testdata" / "images" / SYSTEM_DISC):
        if not p.exists():
            sys.exit(f"missing {p}")  # missing data fails (docs/testing.md rule 7)
    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    git = run_hil.subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                                 text=True).stdout.strip()
    print(f"git {git}; {args.firmware.name}; seed {seed}")

    disc = check_hfe_vs_imd.read_imd(ROOT / "testdata" / "images" / SYSTEM_DISC)
    rom = ROM.read_bytes()
    boot_len = rom[0x1B28] | rom[0x1B29] << 8
    boot = rom[0x1B2A:0x1B2A + boot_len]

    def expected(track, side, rec):
        sid, off = record_location(track, rec)
        _, secs = disc[(track, side)]
        data = next(d for r, d, kind in secs if r == sid)
        return data[off:off + 128]

    far = run_hil.Far480Z(args.port)
    c = run_hil.Checks()
    u = Unit(far, c)
    server = run_hil.run_on_target.socket.create_connection(("localhost", run_hil.run_on_target.TCL_PORT),
                                                            timeout=60)
    with serial.Serial(args.console, 115200, timeout=0.2) as console:
        console.reset_input_buffer()
        run_hil.run_on_target.flash(args.firmware.resolve(), server)
        report = restart(server, console, far)
        print("start-up report:\n  " + "\n  ".join(report))
        c.check(any(l.startswith("SD: IDC ROM") for l in report), "the IDC ROM was found")
        c.check(any(l.startswith(f"SD: A: {SYSTEM_DISC} (IMD") for l in report), f"A is {SYSTEM_DISC}",
                "set up the card as docs/testing.md section 4 says")
        c.check(any(l.startswith(f"SD: B: {SCRATCH_B} (IMD") for l in report), f"B is {SCRATCH_B}",
                "set up the card as docs/testing.md section 4 says")
        if c.failures:
            sys.exit("the card isn't set up for this test:\n  " + "\n  ".join(c.failures))

        print("boot block")
        c.check(u.go_fast(), "go fast")
        far.send([0xFF], settle=False)
        got = far.recv(len(boot), timeout=2.0)
        c.check(bytes(b for b, _ in got) == boot, f"FF sends the ROM's {len(boot)}-byte boot block")

        print("drive A, every record")
        bad, t0, n = [], time.monotonic(), 0
        for track in range(40):
            for side in ((0,) if track == 0 else (0, 1)):
                db = drive_byte(0, side, track != 0)
                for rec in range(1, (16 if track == 0 else 36) + 1):
                    st, data = u.read(db, track, rec)
                    n += 1
                    if st != [0, 0, 0] or data != expected(track, side, rec):
                        bad.append(f"{track}/{side}/{rec}: {st}")
        c.check(not bad, f"{n} records match the local {SYSTEM_DISC}", "; ".join(bad[:5]))
        print(f"        {(time.monotonic() - t0) / n * 1000:.1f} ms a record")

        print(f"drive B, track {TEST_TRACK}: reformat and write, then restart")
        fm, mfm = drive_byte(1, 0, False), drive_byte(1, 0, True)
        record = bytes(rng.randrange(256) for _ in range(128))
        t0 = time.monotonic()
        got = u.format(fm, TEST_TRACK, range(1, 17))
        c.check(got == [0, 0, 0], "0C: formatted FM 16 x 128", got)
        print(f"        {time.monotonic() - t0:.2f} s")
        got = u.write(fm, TEST_TRACK, 3, record)
        c.check(got == [0, 0, 0, 0], "04: record 3 written", got)
        got = u.flush(fm)
        c.check(got == [0, 0, 0], "08: flushed", got)
        report = restart(server, console, far)
        c.check(not any("interrupted" in l or "unfinished" in l or "isn't" in l for l in report),
                "a clean restart", " | ".join(report))
        c.check(u.go_fast(), "go fast")
        st, data = u.read(fm, TEST_TRACK, 3)
        c.check(st == [0, 0, 0] and data == record, "after the restart: the record is there")
        st, data = u.read(fm, TEST_TRACK, 16)
        c.check(st == [0, 0, 0] and data == bytes([0xE5]) * 128, "and the rest of the track is freshly formatted")

        print(f"drive B, track {TEST_TRACK}: put back as it was, then restart")
        _, secs = disc[(TEST_TRACK, 0)]
        got = u.format(mfm, TEST_TRACK, [r for r, _, _ in secs])
        c.check(got == [0, 0, 0], "0C: formatted MFM as the original", got)
        replies = [u.write(mfm, TEST_TRACK, rec, expected(TEST_TRACK, 0, rec)) for rec in range(1, 37)]
        c.check(all(r == [0, 0, 0, 0] for r in replies), "04: the original 36 records written",
                [r for r in replies if r != [0, 0, 0, 0]][:3])
        got = u.flush(mfm)
        c.check(got == [0, 0, 0], "08: flushed", got)
        restart(server, console, far)
        c.check(u.go_fast(), "go fast")
        bad = [rec for rec in range(1, 37) if u.read(mfm, TEST_TRACK, rec) != ([0, 0, 0], expected(TEST_TRACK, 0, rec))]
        c.check(not bad, "after the restart: the track is as it was", f"records {bad}")

    far.p.close()
    lines = [f"git {git}", f"seed {seed}", f"{c.passed} passed, {len(c.failures)} failed"]
    lines += [f"FAIL {f}" for f in c.failures]
    print("\n" + "\n".join(lines[2:]))
    if args.log:
        args.log.write_text("\n".join(lines) + "\n")
    sys.exit(1 if c.failures else 0)


if __name__ == "__main__":
    main()
