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
"""Rebuild a disc from an SCP capture, keeping its real layout: as an IMD, or as a clean SCP to write
back to a real disc.

usage: rebuild.py imd SCPDUMP capture.scp out.IMD [--patch older.IMD] [--comment TEXT]
       rebuild.py scp SCPDUMP capture.scp out.scp [--patch older.IMD] [--rpm 300|360] [--revs N]

The layout of each track comes from scpdump's decode of the capture: the revolution with the most
good IDs gives the sectors, in rotational order, with their positions; duplicate IDs are kept. Each
sector's data is a CRC-good copy from whichever revolution has one (matched by position, so
duplicates keep their own data). A sector unreadable on every revolution, or an ID the damage has
erased, is taken from --patch (an older image of the same disc), but only when that ID is unique on
the track there, so a duplicate is never guessed; an erased ID goes into the empty slot the capture
shows (a gap at least 1.6 times the usual spacing), one usual spacing after the sector before it;
never by the patch image's order. Anything still missing is written with a bad CRC and reported.

imd  writes an ImageDisk image with the recorded C and H bytes (RM discs record head 0 on side 1).
     IMD keeps the sector order but not positions; MAME and FlashFloppy space sectors evenly.
scp  re-encodes every track as ideal FM/MFM flux (IBM System 34 layout: 12x00 + 3xA1 sync before
     each MFM field, 6x00 before FM ones, 4E/FF gaps) with each sector at its measured position, one
     index-aligned track of 100000 2 us cells per side, and writes an SCP for 'gw write':
       --rpm 300   timing for a 300 rpm drive (the disc's own: 2 us cells, 200 ms per turn), default
       --rpm 360   the same disc timing as a 360 rpm drive (a 5.25" HD drive) must write it: 1667 ns
                   cells, 166.7 ms per turn, so it reads back at 250 kbit/s at 300 rpm
       --revs N    revolutions per track (default 2)
     Only cylinders that hold data are written (no blank tracks).
Every sector also good in --patch is compared with it; disagreements are listed.
"""
import datetime
import struct
import subprocess
import sys

ERR = 0x01 | 0x02 | 0x04 | 0x10 | 0x20
NOMINAL_CELLS = 100000  # one turn at 300 rpm in 2 us cells


# ---- input ---------------------------------------------------------------------------------------
def read_imd(path):
    b = open(path, "rb").read()
    i = b.index(b"\x1a") + 1
    tracks = {}
    while i < len(b):
        mode, cyl, head, n, sz = b[i:i + 5]
        i += 5
        ids = list(b[i:i + n])
        i += n
        if head & 0x80:
            i += n
        if head & 0x40:
            i += n
        size = 128 << sz
        secs = []
        for r in ids:
            t = b[i]
            i += 1
            if t % 2 == 1:
                data = b[i:i + size]
                i += size
            elif t:
                data = bytes([b[i]]) * size
                i += 1
            else:
                data = None
            secs.append((r, data, t in (1, 2)))
        tracks[(cyl, head & 1)] = secs
    return tracks


def read_capture(scpdump, path):
    out = subprocess.run([scpdump, "--data", path], capture_output=True, text=True, check=True).stdout
    revs, enc = {}, {}
    for line in out.splitlines():
        p = line.split()
        if p[0] == "T":
            key, rev = (int(p[1]), int(p[2])), int(p[3])
            f = dict(x.split("=", 1) for x in p[4:] if "=" in x)
            enc.setdefault(key, f["enc"])
            revs.setdefault(key, {})[rev] = {"cells": int(f["cells"]), "step": int(f["step"]), "secs": []}
        elif p[0] == "S":
            key, rev = (int(p[1]), int(p[2])), int(p[3])
            f = dict(x.split("=", 1) for x in p[5:] if "=" in x)
            tr = revs[key][rev]
            scale = NOMINAL_CELLS / tr["cells"]
            tr["secs"].append({
                "pos": int(f["id"]) * scale,
                "dpos": int(f["data"]) * scale if f["data"] != "-" else None,
                "c": int(f["c"], 16), "h": int(f["h"], 16), "r": int(f["r"], 16), "n": int(f["n"], 16),
                "mark": int(f["mark"], 16), "flags": int(f["flags"], 16),
                "data": bytes.fromhex(p[-1]) if "=" not in p[-1] else None})
    return revs, enc


def is_stub(s, secs):
    return s["r"] == 0 and s["flags"] & 0x04 and s is secs[-1]


def build_tracks(revs, enc, patch, log):
    """{(cyl, side): (encoding, [sector dicts with 'data' and 'status'])}"""
    result, stats = {}, {"good": 0, "patched": 0, "bad": 0, "agree": 0, "differ": 0}
    for key in sorted(revs):
        good_ids = {}
        for rev, tr in revs[key].items():
            good_ids[rev] = [s for s in tr["secs"] if not s["flags"] & 0x01 and not is_stub(s, tr["secs"])]
        best = max(good_ids, key=lambda r: len(good_ids[r]))
        base = [dict(s) for s in good_ids[best]]
        if not base:
            continue
        old = patch.get(key, [])
        old_count = {}
        for r, _, _ in old:
            old_count[r] = old_count.get(r, 0) + 1
        # IDs the damage erased on every revolution: restored from the patch image into the empty slot
        # the capture shows (a gap about twice the usual sector spacing). The patch image's own sector
        # order is not used: some IMDs store tracks sorted by ID rather than in rotational order.
        have = {s["r"] for s in base}
        for r, data, ok in old:
            if r in have or old_count[r] != 1 or not ok:
                continue
            pos = sorted(s["pos"] for s in base)
            gaps = [(pos[i + 1] - pos[i], i) for i in range(len(pos) - 1)] + [(pos[0] + NOMINAL_CELLS - pos[-1], len(pos) - 1)]
            spacing = sorted(g for g, _ in gaps)[len(gaps) // 2]
            widest, i = max(gaps)
            if len(base) < 2 or widest < 1.6 * spacing:
                log(f"cyl {key[0]} side {key[1]}: ID {r:02X} is in the patch image but the capture has no empty "
                    f"slot for it; not restored")
                continue
            a_ = min(base, key=lambda x: abs(x["pos"] - pos[i]))
            # One normal spacing after the sector before the slot (not the middle of the gap: when the
            # missing sector is the last before the index, the gap also holds the end-of-track gap).
            npos = (pos[i] + spacing) % NOMINAL_CELLS
            off = a_["dpos"] - a_["pos"] if a_["dpos"] is not None else None
            new = dict(a_, r=r, pos=npos, dpos=(npos + off) if off is not None else None, data=None, flags=0x04,
                       restored=True)
            base.append(new)
            base.sort(key=lambda x: x["pos"])
            have.add(r)
            log(f"cyl {key[0]} side {key[1]}: ID {r:02X} erased on every revolution; restored from the patch "
                f"image into the empty slot at cell {round(npos)}")
        # A sector whose data mark the damage destroyed still gets its data field, at the track's usual
        # distance from the ID.
        offs = sorted(x["dpos"] - x["pos"] for x in base if x["dpos"] is not None and not x.get("restored"))
        data_off = offs[len(offs) // 2] if offs else None
        ids_here = [s["r"] for s in base]
        for s in base:
            data, good = s["data"], not (s["flags"] & ERR) and not s.get("restored")
            if not good:
                for tr in revs[key].values():
                    for x in tr["secs"]:
                        if x["r"] == s["r"] and abs(x["pos"] - s["pos"]) < 400 and not x["flags"] & ERR:
                            data, good = x["data"], True
                            break
                    if good:
                        break
            olds = [d for r, d, ok in old if r == s["r"] and ok]
            unique = ids_here.count(s["r"]) == 1 and old_count.get(s["r"], 0) == 1
            if good and unique and olds:
                if olds[0] == data:
                    stats["agree"] += 1
                else:
                    stats["differ"] += 1
                    log(f"cyl {key[0]} side {key[1]} sector {s['r']:02X}: capture and patch image differ")
            if s["dpos"] is None and data_off is not None:
                s["dpos"] = s["pos"] + data_off
            if good:
                s["data"], s["status"] = data, "good"
                stats["good"] += 1
            elif unique and olds:
                s["data"], s["status"] = olds[0], "patched"
                stats["patched"] += 1
                log(f"cyl {key[0]} side {key[1]} sector {s['r']:02X}: unreadable, data from the patch image")
            else:
                s["data"] = data or bytes([0xE5]) * (128 << s["n"])
                s["status"] = "bad"
                stats["bad"] += 1
                log(f"cyl {key[0]} side {key[1]} sector {s['r']:02X}: unreadable and not patchable; bad CRC")
        result[key] = (enc[key], base)
    return result, stats


# ---- IMD output ----------------------------------------------------------------------------------
def write_imd(tracks, path, comment):
    stamp = datetime.datetime.now().strftime("%d/%m/%Y %H:%M:%S")
    out = bytearray(f"IMD 1.18: {stamp}\r\n{comment}\r\n".encode("ascii", "replace") + b"\x1a")
    for (cyl, side), (enc, secs) in sorted(tracks.items()):
        n = secs[0]["n"]
        if any(s["n"] != n for s in secs):
            raise SystemExit(f"cyl {cyl} side {side}: mixed sector sizes, not representable in IMD")
        recs = b"".join((b"\x01" if s["status"] != "bad" else b"\x05") + s["data"] for s in secs)
        out += bytes([2 if enc == "FM" else 5, cyl, side | 0xC0, len(secs), n]) + bytes(s["r"] for s in secs)
        out += bytes(s["c"] for s in secs) + bytes(s["h"] for s in secs) + recs
    open(path, "wb").write(out)


# ---- track encoding ------------------------------------------------------------------------------
def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Mfm:
    def __init__(self):
        self.cells, self.prev = bytearray(), 0

    def byte(self, b):
        for k in range(7, -1, -1):
            d = (b >> k) & 1
            self.cells += bytes([0 if (self.prev | d) else 1, d])
            self.prev = d

    def a1(self):
        self.cells += bytes(int(c) for c in "0100010010001001")
        self.prev = 1


class Fm:
    def __init__(self):
        self.cells = bytearray()  # at the FM cell rate; doubled at the end

    def byte(self, b, clock=0xFF):
        for k in range(7, -1, -1):
            self.cells += bytes([(clock >> k) & 1, (b >> k) & 1])


def field(mark, body, mfm, bad=False):
    crc = crc16(bytes([mark]) + body, crc16(b"\xA1\xA1\xA1") if mfm else 0xFFFF)
    if bad:
        crc ^= 0x5A5A
    return body + bytes([crc >> 8, crc & 0xFF])


def encode_track(enc, secs):
    """One revolution of 2 us cells (FM at double rate), sectors at their measured positions."""
    mfm = enc == "MFM"
    unit = 16 if mfm else 32  # stored cells per byte
    pre = (12 * 16 + 3 * 16) if mfm else 6 * 32  # preamble before a mark
    e = Mfm() if mfm else Fm()
    gap = 0x4E if mfm else 0xFF

    def length():
        return len(e.cells) * (1 if mfm else 2)

    def fill_to(pos):
        while length() + unit <= pos:
            e.byte(gap)

    def mark_field(mark, body, bad):
        if mfm:
            for _ in range(12):
                e.byte(0x00)
            for _ in range(3):
                e.a1()
            e.byte(mark)
            for b in field(mark, body, True, bad):
                e.byte(b)
        else:
            for _ in range(6):
                e.byte(0x00)
            e.byte(mark, 0xC7)
            for b in field(mark, body, False, bad):
                e.byte(b)

    for s in secs:
        fill_to(round((s["pos"] - pre) / unit) * unit)
        mark_field(0xFE, bytes([s["c"], s["h"], s["r"], s["n"]]), False)
        if s["dpos"] is not None:
            fill_to(round((s["dpos"] - pre) / unit) * unit)
            mark_field(s["mark"] or 0xFB, s["data"], s["status"] == "bad")
    fill_to(NOMINAL_CELLS)
    cells = e.cells if mfm else bytes(c for x in e.cells for c in (x, 0))
    if len(cells) > NOMINAL_CELLS:
        raise SystemExit("track too long for one revolution")
    return cells + bytes(NOMINAL_CELLS - len(cells))


def write_scp(tracks, path, rpm, nrevs):
    cell_ticks = 40e6 * (2e-6 * 300 / rpm)  # 40 MHz samples per cell
    rev_ticks = round(NOMINAL_CELLS * cell_ticks)
    table = [0] * 168
    body = bytearray()
    base = 0x10 + 168 * 4
    for (cyl, side), (enc, secs) in sorted(tracks.items()):
        cells = encode_track(enc, secs)
        ones = [i for i, c in enumerate(cells) if c]
        tnr = cyl * 2 + side
        table[tnr] = base + len(body)
        hdr = bytearray(b"TRK" + bytes([tnr]))
        data_parts, last_t = [], 0
        for r in range(nrevs):
            words = bytearray()
            nflux = 0
            for i in ones:
                t = round((r * NOMINAL_CELLS + i) * cell_ticks)
                v = t - last_t
                if v <= 0:
                    # A transition exactly on the index (cell 0) would give a zero interval, which SCP
                    # can't hold: 0000 means "add 65536 to the next value". Leave it out; it only
                    # trims the first gap byte.
                    continue
                last_t = t
                while v > 0xFFFF:
                    words += b"\x00\x00"
                    v -= 0x10000
                    nflux += 1
                words += struct.pack(">H", v)
                nflux += 1
            data_parts.append((rev_ticks, nflux, words))
        off = 4 + 12 * nrevs
        for ticks, nflux, words in data_parts:
            if words[-2:] == b"\0\0":  # an overflow word must always have a value after it
                raise SystemExit(f"cyl {cyl} side {side}: flux data ends in an overflow word")
            hdr += struct.pack("<3I", ticks, nflux, off)
            off += len(words)
        body += hdr + b"".join(w for _, _, w in data_parts)
    ncyl = max(c for c, _ in tracks) + 1
    # Header as Greaseweazle writes it (which HxC and FlashFloppy accept): version 0, disk type 'other',
    # index-cued. The rpm flag is left clear as Greaseweazle leaves it; the timing is in the flux.
    flags = 0x01
    rest = b"".join(struct.pack("<I", t) for t in table) + bytes(body)
    head = b"SCP" + bytes([0x00, 0x80, nrevs, 0, ncyl * 2 - 1, flags, 0, 0, 0])
    open(path, "wb").write(head + struct.pack("<I", sum(rest) & 0xFFFFFFFF) + rest)


def main():
    args = sys.argv[1:]
    opts = {"--patch": None, "--comment": None, "--rpm": "300", "--revs": "2"}
    for o in list(opts):
        if o in args:
            k = args.index(o)
            opts[o] = args[k + 1]
            del args[k:k + 2]
    if len(args) != 4 or args[0] not in ("imd", "scp"):
        raise SystemExit(__doc__)
    what, scpdump, capture, out = args
    revs, enc = read_capture(scpdump, capture)
    patch = read_imd(opts["--patch"]) if opts["--patch"] else {}
    tracks, st = build_tracks(revs, enc, patch, print)
    if what == "imd":
        write_imd(tracks, out, opts["--comment"] or f"from {capture.split('/')[-1]}")
    else:
        rpm = int(opts["--rpm"])
        if rpm not in (300, 360):
            raise SystemExit("--rpm must be 300 or 360")
        write_scp(tracks, out, rpm, int(opts["--revs"]))
    print(f"{out}: {st['good']} sectors from the capture, {st['patched']} from the patch image, {st['bad']} bad; "
          f"against the patch image {st['agree']} agree, {st['differ']} differ")


if __name__ == "__main__":
    main()
