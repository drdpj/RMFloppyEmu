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
"""Bring-up of a small I2C OLED (128 x 64 or 128 x 32, SSD1306 or SH1106) on I2C1, GP10 SDA / GP11 SCL.

usage: run_oled_probe.py BUILD_DIR [--keys KEYS] [--console CONSOLE_PORT] [--log FILE]
       run_oled_probe.py --keys KEYS --no-flash        (change the settings of a probe already running)

Flashes BUILD_DIR/oled_probe.elf (tests/pico/) through the probe and reads the console. It checks:
  - something answers on the bus at 0x3C or 0x3D, the addresses these modules use;
  - the set-up and the test screen were acknowledged;
  - at least one clock (100, 400, 1000 kHz) writes 50 full frames without a NAK; the probe uses the fastest.
KEYS are then sent to the probe, a setting a key (tests/hil/oled_probe.c): h height 64/32, m SSD1306/SH1106, f turn
180 degrees, c contrast, i inverse, k clock, s scan again, z next font, l layout, t test screen. What only you can see is listed at the end.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "pico"))
import run_on_target  # noqa: E402  (ports and flashing)

SCREEN = """On the OLED, check:
  - a one-pixel border all the way round, touching every edge. A side missing, a band of noise down one side, or
    the picture squashed, doubled or only half the panel tall means the wrong controller (m) or height (h);
  - "RMFE OLED 128xH" at the top, reading normally (not mirrored or upside down: f turns it round);
  - the controller, address and I2C clock on the line below, as in OLED-STATE;
  - every text row whole and sharp, with no stray pixels: the panel's memory can't be read back over I2C, so this
    is the only check that the bytes arrived intact at that clock;
  - the last row says "bottom row 8" (128 x 64) or "bottom row 4" (128 x 32)."""


def read_until(console, stop, timeout, log):
    lines, end = [], time.monotonic() + timeout
    while time.monotonic() < end:
        raw = console.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip("\r\n")
        lines.append(line)
        if log:
            log.write(line + "\n")
        if stop and line.startswith(stop):
            return lines, True
    return lines, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=pathlib.Path, nargs="?")
    ap.add_argument("--keys", default="", help="settings keys to send once the probe is ready")
    ap.add_argument("--no-flash", action="store_true", help="don't flash: send KEYS to a probe already running")
    ap.add_argument("--console", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--log", type=pathlib.Path)
    args = ap.parse_args()
    if not args.no_flash and not args.build:
        ap.error("BUILD_DIR is needed unless --no-flash")
    args.console = run_on_target.find_port("console", args.console)
    git = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                         text=True).stdout.strip()

    failures = []

    def check(ok, what, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    log = args.log.open("w") if args.log else None
    with serial.Serial(args.console, 115200, timeout=0.2) as console:
        console.reset_input_buffer()
        if not args.no_flash:
            elf = (args.build / "oled_probe.elf").resolve()
            if not elf.exists():
                sys.exit(f"missing {elf}: build tests/pico first")
            print(f"git {git}; {elf.name}")
            try:
                server = run_on_target.socket.create_connection(("localhost", run_on_target.TCL_PORT), timeout=60)
            except OSError:
                server = None
            run_on_target.flash(elf, server)
            lines, ready = read_until(console, "OLED-READY", 30, log)
            report = [l for l in lines if l.startswith("OLED-")]
            print("\n".join("  " + l for l in report))
            if not ready:
                sys.exit("the probe didn't reach OLED-READY")
            scan = next((l for l in report if l.startswith("OLED-SCAN")), "")
            addrs = re.findall(r" ([0-9a-f]{2}):status=", scan)
            check(any(a in ("3c", "3d") for a in addrs), "an OLED answers at 0x3C or 0x3D", scan)
            states = [l for l in report if l.startswith("OLED-STATE")]
            check(bool(states) and states[-1].endswith(" acked"), "the set-up and test screen were acknowledged",
                  states[-1] if states else "no OLED-STATE")
            clean = [m.group(1) for m in (re.match(r"OLED-I2C khz=(\d+) frames=\d+ nak=0 ", l) for l in report) if m]
            check(bool(clean), "at least one clock writes 50 frames without a NAK", "see the OLED-I2C lines")
            print(f"        clean clocks: {', '.join(k + ' kHz' for k in clean) or 'none'}")
        for key in args.keys:
            console.write(key.encode())
            lines, ok = read_until(console, "OLED-", 3, log)
            print("  " + (lines[-1] if ok else f"no answer to {key!r}"))
            if not ok:
                failures.append(f"key {key!r}")

    print("\n" + SCREEN)
    print(f"\ngit {git}; {len(failures)} failed" + "".join(f"\nFAIL {f}" for f in failures))
    if log:
        log.close()
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
