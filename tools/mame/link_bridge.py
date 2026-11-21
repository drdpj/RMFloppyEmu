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
"""Bridge MAME's emulated 480Z to a real disc unit through a USB serial adapter.

usage: link_bridge.py [--listen HOST:PORT] [--port LINK_PORT] [--log FILE] [--once]

MAME's rm480z, with the rmfe_link slot device (tools/mame/rmfe_link.patch) in its disc-unit port, connects to
this bridge as a socket:

    mame rm480z -rs232 rmfe_link -bitb socket.127.0.0.1:4480 ...

The bridge relays the 480Z's bytes to the adapter and the unit's back, mirrors the 480Z's RTS and DTR onto the
adapter's, and reports the adapter's CTS and DCD (the unit's DTR and RTS) back to MAME. The adapter is wired to the
Pico's link pins as for the level-3 tests (docs/testing.md section 4), and follows the link's rate (9600 or
125000, 8E1) as MAME's device tells it. Messages are two bytes, a type and a value (rmfe_link.cpp):

    MAME -> bridge   'T' b  byte from the 480Z     'H' l  bit 0 RTS, bit 1 DTR     'B' f  0 = 9600, 1 = 125000
    bridge -> MAME   'R' b  byte from the unit     'H' l  bit 0 CTS, bit 1 DCD

On exit (MAME disconnecting, or ^C) it prints the turnaround it saw: from writing a byte the 480Z sent to the
adapter, to reading the first byte of the unit's answer.

The adapter is found by USB ID as in tests/pico/run_on_target.py (the one USB serial port that isn't the probe),
or named with --port or RMFE_LINK_PORT.
"""
import argparse
import fcntl
import pathlib
import socket
import struct
import sys
import termios
import threading
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "hil"))
import run_hil  # noqa: E402  (Far480Z: the adapter set up as the 480Z; run_on_target's port discovery)

RATES = {0: 9600, 1: 125000}


class Bridge:
    def __init__(self, far, conn, log):
        self.far, self.p, self.conn, self.log = far, far.p, conn, log
        self.t0 = time.monotonic()
        self.lock = threading.Lock()  # serialises writes to the socket (two threads send)
        self.stop = threading.Event()
        self.rate = 9600
        self.last_tx = None      # time the last 480Z byte was written to the adapter, until an answer comes
        self.turnaround = {9600: [], 125000: []}
        self.counts = {"T": 0, "R": 0, "parity": 0}

    def event(self, what):
        if self.log:
            self.log.write(f"{time.monotonic() - self.t0:11.6f} {what}\n")

    def send(self, msg):
        with self.lock:
            self.conn.sendall(msg)

    def from_mame(self):
        """Socket -> adapter. Runs on the main thread."""
        buf = b""
        while not self.stop.is_set():
            chunk = self.conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            out = bytearray()
            while len(buf) >= 2:
                kind, value, buf = chr(buf[0]), buf[1], buf[2:]
                if kind == "T":
                    out.append(value)
                    continue
                if out:  # keep bytes and line changes in order
                    self.write(out)
                    out = bytearray()
                if kind == "H":
                    rts, dtr = bool(value & 1), bool(value & 2)
                    self.event(f"480Z RTS={int(rts)} DTR={int(dtr)}")
                    self.p.rts, self.p.dtr = rts, dtr
                elif kind == "B":
                    self.rate = RATES[value]
                    self.event(f"rate {self.rate}")
                    self.far.line(self.rate, serial.PARITY_EVEN)
                else:
                    print(f"bridge: unknown message {buf[:0]!r} {kind!r} {value:02X}", file=sys.stderr)
            if out:
                self.write(out)
        self.stop.set()

    def write(self, data):
        self.p.write(bytes(data))
        now = time.monotonic()
        self.counts["T"] += len(data)
        self.last_tx = (now, self.rate, len(data))
        self.event("T " + data.hex(" ").upper())

    def from_unit(self):
        """Adapter -> socket. Bad parity is marked FF 00 x (termios PARMRK, set by Far480Z.line)."""
        raw = b""
        while not self.stop.is_set():
            try:
                raw += self.p.read(max(1, self.p.in_waiting))
            except (serial.SerialException, OSError, TypeError):
                if self.stop.is_set():
                    break
                raise
            if not raw:
                continue
            now = time.monotonic()
            out, i = bytearray(), 0
            while i < len(raw):
                b = raw[i]
                if b != 0xFF:
                    out.append(b)
                    i += 1
                elif i + 1 < len(raw) and raw[i + 1] == 0xFF:
                    out.append(0xFF)
                    i += 2
                elif i + 2 < len(raw) and raw[i + 1] == 0x00:
                    self.counts["parity"] += 1
                    self.event(f"parity error on {raw[i + 2]:02X}")
                    out.append(raw[i + 2])
                    i += 3
                else:
                    break
            raw = raw[i:]
            if not out:
                continue
            if self.last_tx:
                sent, rate, n = self.last_tx
                self.turnaround[rate].append(now - sent)
                self.last_tx = None
            self.counts["R"] += len(out)
            self.event("R " + out.hex(" ").upper())
            self.send(b"".join(bytes([ord("R"), b]) for b in out))

    def watch_lines(self):
        """The unit's lines, polled: each TIOCMGET asks the FT232R for its modem status (a USB control transfer,
        a few hundred microseconds). TIOCMIWAIT would be cheaper, but ftdi_sio misses changes with it: it never
        woke for the unit's CTS coming back after go-fast, so the 480Z waited for ever."""
        buf = struct.pack("I", 0)
        last = None
        while not self.stop.is_set():
            try:
                bits = struct.unpack("I", fcntl.ioctl(self.p.fd, termios.TIOCMGET, buf))[0]
            except OSError:
                if self.stop.is_set():
                    break
                raise
            lines = (1 if bits & termios.TIOCM_CTS else 0) | (2 if bits & termios.TIOCM_CD else 0)
            if lines != last:
                last = lines
                self.event(f"lines CTS={lines & 1} DCD={lines >> 1}")
                self.send(bytes([ord("H"), lines]))

    def report(self):
        print(f"bridge: {self.counts['T']} bytes to the unit, {self.counts['R']} back, "
              f"{self.counts['parity']} with bad parity")
        for rate, times in self.turnaround.items():
            if not times:
                continue
            times = sorted(t * 1000 for t in times)
            wire = 2 * 11 / rate * 1000  # the 480Z's byte and the first byte of the answer, 8E1
            pct = lambda q: times[min(len(times) - 1, int(q * len(times)))]  # noqa: E731
            print(f"bridge: turnaround at {rate}: {len(times)} answers, median {pct(0.5):.2f} ms, "
                  f"90% {pct(0.9):.2f} ms, max {times[-1]:.2f} ms (the two bytes take {wire:.2f} ms on the wire)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:4480", help="where MAME connects (default 127.0.0.1:4480)")
    ap.add_argument("--port", help="the adapter on the link pins (default: found by USB ID, or RMFE_LINK_PORT)")
    ap.add_argument("--log", type=pathlib.Path, help="log every event with its time")
    ap.add_argument("--once", action="store_true", help="exit when MAME disconnects (default: wait for another)")
    args = ap.parse_args()
    port = run_hil.PORTS.find_port("link", args.port)
    host, _, num = args.listen.rpartition(":")

    srv = socket.create_server((host, int(num)))
    log = open(args.log, "w", buffering=1) if args.log else None
    print(f"bridge: {port} <-> {args.listen}", flush=True)
    try:
        while True:
            conn, _ = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print("bridge: MAME connected", flush=True)
            far = run_hil.Far480Z(port)  # opens at 9600 8E1, RTS down, DTR up: replaced by MAME's lines at once
            b = Bridge(far, conn, log)
            threads = [threading.Thread(target=f, daemon=True) for f in (b.from_unit, b.watch_lines)]
            for t in threads:
                t.start()
            try:
                b.from_mame()
            except (ConnectionResetError, KeyboardInterrupt):
                pass
            finally:
                b.stop.set()
                b.report()
                far.p.close()
                conn.close()
            print("bridge: MAME disconnected", flush=True)
            if args.once:
                break
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
