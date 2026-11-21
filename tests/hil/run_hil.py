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
"""Hardware-in-the-loop tests (docs/testing.md section 4, level 3).

usage: run_hil.py BUILD_DIR [--port LINK_PORT] [--console CONSOLE_PORT] [--log FILE]

Flashes BUILD_DIR/hil_firmware.elf (tests/hil/hil_firmware.c, built by tests/pico/) through the probe, then plays
the 480Z through a USB serial adapter wired to the link pins at 3.3 V (docs/testing.md section 4): TXD, RXD, RTS,
DTR, CTS and DCD. It checks, over the real UART and pins:
  - the unit sends nothing at power-up;
  - 8E1 both ways, at 9600 and 125000 baud, and the switch after 00 -> 5A 00 01;
  - single bytes with bad parity are taken (the IDC ignores parity there), and every byte the unit sends has even
    parity (the adapter, set to odd, flags each one);
  - CTS asserted only while the unit waits for a byte, and the unit sending only while DTR is asserted;
  - records read (02) with the right data, and the time each takes;
  - a record written (04) with one bad-parity byte is answered FF and taken again, then reads back right;
  - RTS asserted mid-command abandons it and puts the unit back at 9600.
Expected data comes from the disc's pattern (hil_firmware.c, copied here) and the 2:1 MFM skew
(docs/protocol.md), not from the unit. Fails on any mismatch; prints the git hash with the results.
"""
import argparse
import pathlib
import subprocess
import sys
import termios
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "pico"))
import run_on_target  # noqa: E402  (flashing through the probe)

PORTS = run_on_target

SKEW = [1, 6, 2, 7, 3, 8, 4, 9, 5]  # MFM records' logical sector -> ID (docs/protocol.md)
DRIVE_A = 0x20                       # the drive/flags byte CP/M sends for drive A, side 0
TIMEOUT = 1.0


def pattern(c, s, r, k):  # hil_firmware.c, hil_pattern()
    return (c * 7 + s * 50 + r * 13 + k) & 0xFF


def expected_record(cyl, rec):
    sector_id = SKEW[(rec - 1) // 4]
    offset = ((rec - 1) % 4) * 128
    return bytes(pattern(cyl, 0, sector_id, offset + k) for k in range(128))


class Far480Z:
    """The adapter, as the 480Z. Received bytes come with a parity-error flag (termios PARMRK)."""

    def __init__(self, port):
        self.p = serial.Serial()
        self.p.port = port
        self.p.timeout = 0.01
        self.p.rts = False  # set before opening, so the 480Z's RTS doesn't pulse
        self.p.dtr = True
        self.p.open()
        try:
            self.p.set_low_latency_mode(True)
        except (OSError, ValueError, NotImplementedError) as e:
            print(f"note: low-latency mode not available ({e}); timings include the adapter's latency timer")
        self.raw = b""
        self.line(9600, serial.PARITY_EVEN)

    def line(self, baud, parity):
        self.p.flush()
        self.p.baudrate = baud
        self.p.parity = parity
        # pyserial resets the input flags on every change: ask the kernel to mark bad parity (FF 00 x) again.
        attrs = termios.tcgetattr(self.p.fd)
        attrs[0] |= termios.INPCK | termios.PARMRK
        attrs[0] &= ~(termios.IGNPAR | termios.ISTRIP | termios.IGNBRK | termios.BRKINT)
        termios.tcsetattr(self.p.fd, termios.TCSANOW, attrs)

    def send(self, data, settle=True):
        self.p.write(bytes(data))
        self.p.flush()
        if settle:  # tcdrain returns when the adapter has the bytes, not when they are on the wire
            time.sleep(len(data) * 11 / self.p.baudrate + 0.003)

    def recv(self, n, timeout=TIMEOUT):
        """Up to n (byte, parity_error) pairs, waiting up to timeout."""
        out, end = [], time.monotonic() + timeout
        while len(out) < n and time.monotonic() < end:
            self.raw += self.p.read(4096)
            i = 0
            while i < len(self.raw) and len(out) < n:
                b = self.raw[i]
                if b != 0xFF:
                    out.append((b, False))
                    i += 1
                elif i + 1 < len(self.raw) and self.raw[i + 1] == 0xFF:
                    out.append((0xFF, False))
                    i += 2
                elif i + 2 < len(self.raw) and self.raw[i + 1] == 0x00:
                    out.append((self.raw[i + 2], True))
                    i += 3
                else:
                    break  # an incomplete mark: wait for the rest
            self.raw = self.raw[i:]
        return out

    def drop_input(self):
        time.sleep(0.02)
        self.p.reset_input_buffer()
        self.raw = b""


class Checks:
    def __init__(self):
        self.failures, self.passed = [], 0

    def check(self, ok, what, detail=""):
        if ok:
            self.passed += 1
            print(f"  ok    {what}")
        else:
            self.failures.append(f"{what}: {detail}")
            print(f"  FAIL  {what}: {detail}")


def hexs(pairs):
    return " ".join(f"{b:02X}{'!' if pe else ''}" for b, pe in pairs)


def expect(c, far, n, want, what):
    got = far.recv(n)
    c.check([b for b, _ in got] == list(want) and not any(pe for _, pe in got), what,
            f"got {hexs(got)} (! = parity error), want {bytes(want).hex(' ').upper()}")
    return got


def run(far, c):
    print("framing at 9600")
    far.send([0x11])
    expect(c, far, 2, [0x00, 0x31], "11 at 9600: 00 31, even parity")
    far.line(9600, serial.PARITY_ODD)
    far.send([0x11])
    got = far.recv(2)
    c.check([b for b, _ in got] == [0x00, 0x31] and all(pe for _, pe in got),
            "a command byte with bad parity is still taken, and the reply has even parity (flagged at odd)", hexs(got))
    far.line(9600, serial.PARITY_EVEN)

    print("go fast")
    far.send([0x00])
    expect(c, far, 3, [0x5A, 0x00, 0x01], "00 at 9600: 5A 00 01")
    far.line(125000, serial.PARITY_EVEN)
    far.send([0x11])
    expect(c, far, 2, [0x00, 0x31], "11 at 125000: 00 31, even parity")
    far.line(125000, serial.PARITY_ODD)
    far.send([0x11])
    got = far.recv(2)
    c.check([b for b, _ in got] == [0x00, 0x31] and all(pe for _, pe in got),
            "at 125000 too: bad parity taken, replies even", hexs(got))
    far.line(125000, serial.PARITY_EVEN)

    print("handshake")
    time.sleep(0.02)
    c.check(far.p.cts, "CTS asserted while the unit waits for a command")
    c.check(far.p.cd, "DCD asserted")
    far.p.dtr = False
    far.send([0x11])
    time.sleep(0.05)
    c.check(not far.p.cts, "CTS down while the unit has a reply to send")
    c.check(far.recv(2, timeout=0.1) == [], "nothing sent while DTR is down")
    far.p.dtr = True
    expect(c, far, 2, [0x00, 0x31], "the reply goes as soon as DTR is up")
    time.sleep(0.02)
    c.check(far.p.cts, "CTS up again, waiting for the next command")

    print("reading records (02), cylinder 5")
    times, bad = [], []
    for rec in range(1, 37):
        t0 = time.monotonic()
        far.send([0x02, DRIVE_A, 5, rec], settle=False)
        got = far.recv(3 + 128)
        times.append(time.monotonic() - t0)
        far.send([0x00], settle=False)  # ack
        data = bytes(b for b, _ in got[3:])
        if [b for b, _ in got[:3]] != [0, 0, 0] or data != expected_record(5, rec) or any(pe for _, pe in got):
            bad.append(rec)
    c.check(not bad, "36 records read with the right data and even parity", f"records {bad}")
    wire = (4 + 3 + 128) * 11 / 125000
    print(f"        per record: {min(times) * 1000:.1f}-{max(times) * 1000:.1f} ms from command to the last byte "
          f"(the bytes alone take {wire * 1000:.1f} ms; the rest is USB)")
    c.check(max(times) < wire + 0.020, "every record arrives within 20 ms of its wire time",
            f"slowest {max(times) * 1000:.1f} ms")

    print("writing a record (04) with a parity error")
    record = bytes((i * 37 + 11) & 0xFF for i in range(128))
    far.send([0x04, DRIVE_A, 7, 1] + list(record[:50]))
    far.line(125000, serial.PARITY_ODD)
    far.send([record[50]])
    far.line(125000, serial.PARITY_EVEN)
    far.send(record[51:])
    expect(c, far, 3, [0x00, 0x00, 0xFF], "accept, status, then FF: send the record again")
    far.send(record)
    expect(c, far, 2, [0x00, 0x00], "the clean copy: block ack 00, result 00")
    far.send([0x08, DRIVE_A])
    expect(c, far, 3, [0x00, 0x00, 0x00], "08 flush: 00 00 00")
    far.send([0x02, DRIVE_A, 7, 1], settle=False)
    got = far.recv(3 + 128)
    far.send([0x00])
    c.check(bytes(b for b, _ in got[3:]) == record, "the record reads back as written")

    print("RTS mid-command")
    far.send([0x04, DRIVE_A, 8, 2] + [0xEE] * 60)
    far.drop_input()
    t0 = time.monotonic()
    far.p.rts = True  # the 480Z starts a new exchange
    far.line(9600, serial.PARITY_EVEN)
    far.send([0x00])
    expect(c, far, 3, [0x5A, 0x00, 0x01], "the unit is back at 9600: 00 -> 5A 00 01")
    print(f"        RTS to the 5A reply: {(time.monotonic() - t0) * 1000:.1f} ms (mostly USB)")
    far.line(125000, serial.PARITY_EVEN)
    far.p.rts = False
    far.send([0x02, DRIVE_A, 8, 2], settle=False)
    got = far.recv(3 + 128)
    far.send([0x00])
    c.check(bytes(b for b, _ in got[3:]) == expected_record(8, 2), "the abandoned write changed nothing")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("--port", help="the adapter playing the 480Z (default: found by USB ID, or RMFE_LINK_PORT)")
    ap.add_argument("--console", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--log", type=pathlib.Path)
    args = ap.parse_args()
    args.port = PORTS.find_port("link", args.port)
    args.console = PORTS.find_port("console", args.console)

    elf = (args.build / "hil_firmware.elf").resolve()
    if not elf.exists():
        sys.exit(f"missing {elf}: build tests/pico first")
    git = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                         text=True).stdout.strip()
    print(f"git {git}; {elf.name}; 480Z played by {args.port}")

    far = Far480Z(args.port)  # first, so the 480Z's lines are steady when the unit starts
    with serial.Serial(args.console, 115200, timeout=0.2) as console:
        console.reset_input_buffer()
        try:
            server = run_on_target.socket.create_connection(("localhost", run_on_target.TCL_PORT), timeout=60)
        except OSError:
            server = None
        run_on_target.flash(elf, server)
        end, ready = time.monotonic() + 5, False
        while time.monotonic() < end and not ready:
            ready = b"RMFE-HIL ready" in console.readline()
        if not ready:
            sys.exit("the HIL firmware didn't start (no 'RMFE-HIL ready' on the console)")

    c = Checks()
    time.sleep(0.1)
    said = far.recv(16, timeout=0.1)  # DTR has been up all along, so the unit could have sent
    c.check(said == [], "the unit says nothing at power-up", f"it sent {hexs(said)}")
    far.drop_input()
    run(far, c)
    far.p.close()
    lines = [f"git {git}", f"{c.passed} passed, {len(c.failures)} failed"] + [f"FAIL {f}" for f in c.failures]
    print("\n" + "\n".join(lines[1:]))
    if args.log:
        args.log.write_text("\n".join(lines) + "\n")
    sys.exit(1 if c.failures else 0)


if __name__ == "__main__":
    main()
