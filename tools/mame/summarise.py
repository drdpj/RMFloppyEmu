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
"""Condense an mq2trace.lua log into one line per MQ2 command.

The MQ2 main loop writes FDC command D0 (force interrupt) each time it returns to wait
for a command, so a D0 write marks the start of a new transaction. Within a transaction,
consecutive bytes in the same direction are merged into one run, and long runs are
abbreviated.

usage: summarise.py trace.txt [--full] [--hw]

--full  do not abbreviate long runs
--hw    include FDC / drive-control events (omitted by default)
"""
import sys

BAUD = {"0D": "9600", "01": "125k"}


def runs(events):
    out = []
    for ev, val in events:
        if out and out[-1][0] == ev and ev in ("RX", "TX"):
            out[-1][1].append(val)
        else:
            out.append([ev, [val]])
    return out


def fmt(ev, vals, full):
    if ev in ("RX", "TX"):
        arrow = "<" if ev == "RX" else ">"
        if len(vals) > 16 and not full:
            body = " ".join(vals[:6]) + f" ..[{len(vals)}].. " + " ".join(vals[-3:])
        else:
            body = " ".join(vals)
        return f"{arrow}{body}"
    if ev == "CTC0":
        return f"[baud {BAUD.get(vals[-1], vals[-1])}]" if vals[-1] in BAUD else ""
    if ev in ("FDC", "TRK", "SEC", "SEL", "DEN"):
        return f"{{{ev} {' '.join(vals)}}}"
    return f"{ev}:{' '.join(vals)}"


def main():
    full = "--full" in sys.argv
    hw = "--hw" in sys.argv
    path = [a for a in sys.argv[1:] if not a.startswith("--")][0]
    txns, cur, start = [], [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split(None, 2)
        if len(p) < 3:
            continue
        try:
            t = float(p[0])
        except ValueError:
            continue
        ev, val = p[1], p[2].strip()
        if ev == "KEY":
            txns.append((t, [("KEY", val)]))
            continue
        if ev == "FDC" and val == "D0":
            if cur:
                txns.append((start, cur))
            cur, start = [], t
            continue
        if not hw and ev in ("FDC", "TRK", "SEC", "SEL", "DEN"):
            continue
        if start is None:
            start = t
        cur.append((ev, val))
    if cur:
        txns.append((start, cur))

    for t, evs in txns:
        parts = [s for s in (fmt(e, v, full) for e, v in runs(evs)) if s]
        if parts:
            print(f"{t:10.6f}  " + "  ".join(parts))


if __name__ == "__main__":
    main()
