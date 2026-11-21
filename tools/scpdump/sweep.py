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
"""Sweep SCP flux images for anything a copy-protection scheme (or damage) would leave behind.

usage: sweep.py SCPDUMP OUTDIR image.scp [image.scp ...]

Runs scpdump --data on each image (in parallel), works out each disc's normal layout (encoding and
sector count/size/IDs of the most common track shape, with track 0 judged separately), then lists
every track that departs from it. Per disc it writes OUTDIR/<name>.txt and prints a summary.

Reported per track (each revolution decoded separately; 'all revs' means seen on every one):
  dup-id      two good IDs with the same sector number on one revolution
  odd-id      sector numbers outside the disc's normal set
  odd-size    size code different from the disc's normal one
  id-cyl      the ID's cylinder byte differs from the physical cylinder (head byte is not
              checked: RM discs record head 0 on side 1)
  missing     normal sector numbers absent on every revolution
  id-crc / data-crc   CRC errors, with whether every revolution shows them
  weak        a sector read CRC-good on two revolutions with different data (a real weak/fuzzy area)
  marginal    a sector with CRC errors on some revolutions only (worn or marginal media)
  nothing     no sectors decoded at all
Per disc it also checks whether 'side 1' is just side 0 again (every sector identical): the mark of
a 40-track disc read on an 80-track drive without double-stepping, one side only, with each
physical track stored twice. Those discs are reported as 'SIDE 1 = SIDE 0'.
Not reported as anomalies: an ID with sector number 00 and a bad data CRC as the last thing before
the index ("stub": the end of the formatting write, cut off by its own start).
"""
import collections
import concurrent.futures
import os
import subprocess
import sys


def run_dump(scpdump, path):
    out = subprocess.run([scpdump, "--data", path], capture_output=True, text=True, timeout=600)
    return out.stdout


def parse(text):
    header, tracks = "", collections.defaultdict(lambda: collections.defaultdict(list))
    tinfo = {}
    for line in text.splitlines():
        p = line.split()
        if not p:
            continue
        if p[0] == "H":
            header = line
        elif p[0] == "T":
            key = (int(p[1]), int(p[2]))
            f = dict(x.split("=", 1) for x in p[4:] if "=" in x)
            tinfo.setdefault(key, []).append((f["enc"], int(f["step"]), "nothing" in p))
        elif p[0] == "S":
            key, rev = (int(p[1]), int(p[2])), int(p[3])
            f = dict(x.split("=", 1) for x in p[5:] if "=" in x)
            data = p[-1] if "=" not in p[-1] else None
            tracks[key][rev].append({"id": int(f["id"]), "c": int(f["c"], 16), "r": int(f["r"], 16),
                                     "n": int(f["n"], 16), "flags": int(f["flags"], 16), "data": data})
    return header, tracks, tinfo


def is_stub(s, sectors):
    return s["r"] == 0 and s["flags"] & 0x04 and s is sectors[-1]


def analyse(tracks, tinfo):
    # Normal shape: from tracks other than cylinder 0, the commonest (encoding, sorted good IDs, n).
    shape = collections.Counter()
    for key, revs in tracks.items():
        if key[0] == 0:
            continue
        secs = [s for s in revs.get(0, []) if not s["flags"] & 0x01 and not is_stub(s, revs.get(0, []))]
        shape[(tinfo[key][0][0], tuple(sorted(s["r"] for s in secs)), tuple(sorted({s["n"] for s in secs})))] += 1
    if not shape:
        return None, []
    (enc, ids, ns), count = shape.most_common(1)[0]
    normal_ids, normal_n = set(ids), (ns[0] if len(ns) == 1 else None)

    findings = []
    for key in sorted(set(tracks) | set(tinfo)):
        revs = tracks.get(key, {})
        nrevs = len(tinfo.get(key, []))
        notes = []
        if all(t[2] for t in tinfo.get(key, [])):
            findings.append((key, ["nothing"]))
            continue
        per_rev_ids = []
        crc = collections.defaultdict(set)  # (kind, r) -> revs
        datas = collections.defaultdict(lambda: collections.defaultdict(set))  # (r, occurrence) -> rev -> data
        for rev, secs in revs.items():
            seen = collections.Counter()
            good = [s for s in secs if not s["flags"] & 0x01 and not is_stub(s, secs)]
            for s in good:
                seen[s["r"]] += 1
                datas[(s["r"], seen[s["r"]])][rev].add((s["data"], bool(s["flags"] & 0x04)))
            per_rev_ids.append(seen)
            for s in secs:
                if is_stub(s, secs):
                    continue
                if s["flags"] & 0x01:
                    crc[("id-crc", s["r"])].add(rev)
                elif s["flags"] & 0x04:
                    crc[("data-crc", s["r"])].add(rev)
        cyl0 = key[0] == 0
        allids = set().union(*[set(x) for x in per_rev_ids]) if per_rev_ids else set()
        dups = sorted({r for x in per_rev_ids for r, n in x.items() if n > 1})
        if dups:
            notes.append("dup-id " + " ".join("%02X" % r for r in dups))
        if not cyl0:
            odd = sorted(allids - normal_ids)
            if odd:
                notes.append("odd-id " + " ".join("%02X" % r for r in odd))
            missing = sorted(normal_ids - allids)
            if missing:
                notes.append("missing " + " ".join("%02X" % r for r in missing))
            sizes = sorted({s["n"] for secs in revs.values() for s in secs if not s["flags"] & 0x01
                            and not is_stub(s, secs)})
            if normal_n is not None and sizes and sizes != [normal_n]:
                notes.append("odd-size " + " ".join(str(n) for n in sizes))
        cyls = sorted({s["c"] for secs in revs.values() for s in secs if not s["flags"] & 0x01
                       and not is_stub(s, secs)} - {key[0]})
        if cyls:
            notes.append("id-cyl " + " ".join("%02X" % c for c in cyls))
        for (kind, r), rv in sorted(crc.items()):
            if len(rv) == nrevs:
                notes.append(f"{kind} {r:02X} (all revs)")
            else:
                notes.append(f"marginal {r:02X} ({kind} on revs {','.join(map(str, sorted(rv)))})")
        for (r, occ), byrev in sorted(datas.items()):
            good = {d for v in byrev.values() for d, bad in v if not bad}
            if len(good) > 1:
                notes.append(f"weak {r:02X}" + (f"#{occ}" if occ > 1 else ""))
        if notes:
            findings.append((key, notes))
    same = diff = 0
    for (c, side), revs in tracks.items():
        if side != 0 or (c, 1) not in tracks:
            continue
        a = {x["r"]: x["data"] for x in revs.get(0, []) if x["flags"] == 0}
        b = {x["r"]: x["data"] for x in tracks[(c, 1)].get(0, []) if x["flags"] == 0}
        for r in a.keys() & b.keys():  # sectors good on both reads
            if a[r] == b[r]:
                same += 1
            else:
                diff += 1
    mirrored = same > 0 and diff == 0
    summary = ("SIDE 1 = SIDE 0 (one side, each track read twice); " if mirrored else "") + f"{enc} {len(normal_ids)} x {128 << normal_n if normal_n is not None else '?'} "               f"(IDs {min(normal_ids):02X}-{max(normal_ids):02X}) on {count} tracks"
    return summary, findings


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    scpdump, outdir, images = sys.argv[1], sys.argv[2], sys.argv[3:]
    os.makedirs(outdir, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        dumps = dict(zip(images, pool.map(lambda p: run_dump(scpdump, p), images)))
    for path in images:
        name = os.path.splitext(os.path.basename(path))[0]
        header, tracks, tinfo = parse(dumps[path])
        summary, findings = analyse(tracks, tinfo)
        with open(os.path.join(outdir, name + ".txt"), "w") as f:
            f.write(f"{path}\n{header}\nnormal: {summary}\n")
            for (c, s), notes in findings:
                f.write(f"cyl {c:2d} side {s}: {'; '.join(notes)}\n")
        flag = "" if "checksum=ok" in header else "  [SCP checksum BAD]"
        interesting = [(k, n) for k, n in findings if k[0] != 0 or any(not x.startswith("data-crc") for x in n)]
        print(f"{name}: {summary}; {len(findings)} tracks noted{flag}")
        for (c, s), notes in interesting[:12]:
            print(f"    cyl {c:2d} side {s}: {'; '.join(notes)}")
        if len(interesting) > 12:
            print(f"    ... {len(interesting) - 12} more (see {name}.txt)")


if __name__ == "__main__":
    main()
