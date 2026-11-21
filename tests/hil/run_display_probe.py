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
"""Checks the display and encoder wiring with tests/hil/display_probe.c (docs/testing.md section 4).

usage: run_display_probe.py BUILD_DIR [--console CONSOLE_PORT] [--clicks N] [--log FILE]

Flashes BUILD_DIR/display_probe.elf (tests/pico/) through the probe and reads the console. Interactive: it asks
you to turn the knob and press it. It checks:
  - the controller answers over MISO (its power mode changes from reset to on), and says which it is;
  - a pattern written over SPI reads back intact at the slowest clock tried at least;
  - the soak: at least one clock writes 100 full-width strips, each with its window commands, at random rows, with
    no pixel wrong on reading back. It lists the clean clocks; the probe uses the fastest;
  - the backlight PWM ran;
  - N clicks one way give N steps of one sign, N the other way give N of the other, and one press gives one press
    and one release.
What only you can see is listed at the end: check the screen against it.
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

SCREEN = """On the screen (landscape, 320 x 240), check:
  - the backlight was off until the picture was drawn, dimmed to off and came back;
  - "RMFE display probe" in large text at the top left, reading normally (not mirrored or upside down);
  - the controller and SPI clock on the line below;
  - eight bars, left to right: white, yellow, cyan, green, magenta, red, blue, black, each named under it;
  - a small red square in the top right corner, touching both edges;
  - the status line at the bottom: the brightness, knob position and presses change as you use the knob;
  - turning the knob changes the brightness, 5% a click."""


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


def collect(console, log, quiet=0.5):
    """Everything on the console until it has been quiet for a while."""
    lines = []
    while True:
        got, _ = read_until(console, None, quiet, log)
        if not got:
            return lines
        lines += got


def steps(lines):
    return [int(m.group(1)) for m in (re.match(r"ENC step ([+-]1) ", l) for l in lines) if m]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("--console", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--clicks", type=int, default=5, help="clicks to turn each way (default 5)")
    ap.add_argument("--log", type=pathlib.Path)
    args = ap.parse_args()
    args.console = run_on_target.find_port("console", args.console)
    elf = (args.build / "display_probe.elf").resolve()
    if not elf.exists():
        sys.exit(f"missing {elf}: build tests/pico first")
    git = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                         text=True).stdout.strip()
    print(f"git {git}; {elf.name}")

    failures = []

    def check(ok, what, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {what}" + (f": {detail}" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    log = args.log.open("w") if args.log else None
    with serial.Serial(args.console, 115200, timeout=0.2) as console:
        console.reset_input_buffer()
        try:
            server = run_on_target.socket.create_connection(("localhost", run_on_target.TCL_PORT), timeout=60)
        except OSError:
            server = None
        run_on_target.flash(elf, server)
        lines, ready = read_until(console, "DISP-READY", 90, log)
        report = [l for l in lines if l.startswith("DISP-")]
        print("\n".join("  " + l for l in report))
        if not ready:
            sys.exit("the probe didn't reach DISP-READY")

        ident = next((l for l in report if l.startswith("DISP-ID")), "")
        check(" miso=ok" in ident, "the controller answers over MISO (power mode reset -> on)", ident)
        controller = re.search(r"controller=(\S+)", ident)
        check(controller and controller.group(1) != "unknown", "the controller is identified",
              "neither ILI9341's ID4 nor ST7789's RDDID: see the raw bytes")
        spi = [l for l in report if l.startswith("DISP-SPI write_khz")]
        check(spi and spi[-1].split()[3] == "readback=ok", "a pattern read back intact at the slowest clock",
              spi[-1] if spi else "no DISP-SPI lines")
        clean = [l.split()[1].split("=")[1] for l in spi if "soak_bad_strips=0/" in l]
        check(bool(clean), "at least one clock writes 100 strips, window commands and all, without an error",
              "see the DISP-SPI lines")
        print(f"        clean clocks: {', '.join(k + ' kHz' for k in clean) or 'none'}")
        check(sum(l.startswith("DISP-BL") for l in report) == 21, "the backlight ran down and up")

        print(f"\nTurn the knob {args.clicks} clicks CLOCKWISE, slowly, then press Enter.")
        console.reset_input_buffer()
        input()
        cw = steps(collect(console, log))
        print(f"Now {args.clicks} clicks ANTICLOCKWISE, then press Enter.")
        console.reset_input_buffer()
        input()
        acw_lines = collect(console, log)
        acw = steps(acw_lines)
        acw_counts = [re.sub(r".*(transitions=\d+ invalid=\d+).*", r"since start: \1 (invalid = a state missed)", l)
                      for l in acw_lines if l.startswith("ENC step")]
        print("Now press the knob once and let go, then press Enter.")
        console.reset_input_buffer()
        input()
        sw = collect(console, log)

        sign = cw[0] if cw else 0
        check(len(cw) == args.clicks and all(s == sign for s in cw), f"{args.clicks} clockwise clicks: "
              f"{args.clicks} steps of one sign", f"got {cw}")
        check(len(acw) == args.clicks and all(s == -sign for s in acw) and sign,
              f"{args.clicks} anticlockwise clicks: {args.clicks} steps of the other sign", f"got {acw}")
        presses = [l for l in sw if l in ("ENC press", "ENC release")]
        check(presses == ["ENC press", "ENC release"], "one press: one press and one release", f"got {presses}")
        if sign:  # +1 is A changing before B (display_probe.c, enc_poll)
            print(f"        clockwise is {sign:+d}: {'A' if sign > 0 else 'B'} leads clockwise")
        print(f"        {acw_counts[-1] if acw_counts else 'no steps'}")

    print("\n" + SCREEN)
    lines = [f"git {git}", f"{len(failures)} failed"] + [f"FAIL {f}" for f in failures]
    print("\n" + "\n".join(lines[1:]))
    if log:
        log.close()
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
