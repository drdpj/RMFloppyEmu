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
"""MAME comparison scenarios (docs/testing.md section 3): parsing, shared by make_traces.py and compare.py.

A scenario file (tests/mame/scenarios/<name>.scn) says what the 480Z does in MAME. It holds no RM
material: image names, timings and keystrokes only.

    # comment
    drive A 480Z_RML_system-cpm2.2f-basic5.3g-basic6.3b.imd    image in testdata/images/ (A or B)
    readonly B                                                 that drive's disc is write-protected
    drop A 33 0 4                                              remove sector ID 4 from cylinder 33 side 0 of
                                                               that drive's copy (an IMD), before MAME and
                                                               our replay both see it
    differs B 0 0 02-10 why...                                 a known, documented difference from MAME: the
                                                               data of those sectors (IDs in hex) on cylinder
                                                               0 side 0 of B may differ; everything else on
                                                               the track is still compared. compare.py prints
                                                               it on every run and fails if it goes away
    seconds 30                                                 emulated time to run
    type 2.0 B                                                 text typed from that time; \\r is Return
    type 12 DIR\\r
    snap 20 30                                                 screen snapshots at those times, kept by
                                                               make_traces.py (for writing scenarios)

The trace and the images MAME leaves are kept in testdata/traces/ as <name>.trace and <name>.<drive>.<ext>.
"""
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Scenario:
    def __init__(self, path):
        self.path = Path(path)
        self.name = self.path.stem
        self.drives = {}  # 'A'/'B' -> image file name
        self.readonly = set()  # drive letters
        self.seconds = None
        self.typing = []  # (start seconds, text)
        self.snaps = []  # seconds
        self.drops = []  # (drive, cylinder, side, sector ID)
        self.differs = []  # (drive, cylinder, side, first ID, last ID, reason)
        for n, line in enumerate(self.path.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            word, _, rest = line.partition(" ")
            try:
                if word == "drive":
                    letter, image = rest.split(None, 1)
                    if letter not in ("A", "B"):
                        raise ValueError("drive must be A or B")
                    self.drives[letter] = image.strip()
                elif word == "readonly":
                    if rest not in ("A", "B"):
                        raise ValueError("readonly takes A or B")
                    self.readonly.add(rest)
                elif word == "drop":
                    letter, cyl, side, sec = rest.split()
                    self.drops.append((letter, int(cyl), int(side), int(sec, 0)))
                elif word == "differs":
                    letter, cyl, side, ids, reason = rest.split(None, 4)
                    first, _, last = ids.partition("-")
                    if letter not in ("A", "B"):
                        raise ValueError("differs: drive must be A or B")
                    self.differs.append((letter, int(cyl), int(side), int(first, 16), int(last or first, 16),
                                         reason))
                elif word == "snap":
                    self.snaps += [float(x) for x in rest.split()]
                elif word == "seconds":
                    self.seconds = float(rest)
                elif word == "type":
                    t, text = rest.split(" ", 1)
                    self.typing.append((float(t), text.replace("\\r", "\r")))
                else:
                    raise ValueError(f"unknown keyword {word!r}")
            except ValueError as e:
                raise SystemExit(f"{self.path}:{n}: {e}")
        if self.seconds is None:
            raise SystemExit(f"{self.path}: no 'seconds' line")
        if not self.readonly <= set(self.drives):
            raise SystemExit(f"{self.path}: readonly names a drive with no disc")

    def mq2_keys(self):
        """The MQ2_KEYS value for tools/mame/mq2trace.lua."""
        keys = _load("keys", REPO / "tools" / "mame" / "keys.py")
        presses = []  # mq2trace.lua plays them in list order, so they go in time order
        for start, text in self.typing:
            t = start
            for ch in text:
                presses.append((t, keys.key_name(ch)))
                t += keys.STEP
        presses.sort(key=lambda p: p[0])
        return ";".join(f"{t:.2f}:{name}" for t, name in presses)

    def trace_path(self, testdata):
        return Path(testdata) / "traces" / f"{self.name}.trace"

    def mame_image_path(self, testdata, letter):
        return Path(testdata) / "traces" / f"{self.name}.{letter}{Path(self.drives[letter]).suffix.lower()}"


def read_imd(path):
    """The independent IMD reader of tests/check_hfe_vs_imd.py."""
    return _load("check_hfe_vs_imd", REPO / "tests" / "check_hfe_vs_imd.py").read_imd(path)


IMD_SIZES = [128 << n for n in range(7)]


def imd_drop_sector(data, cyl, side, sector_id):
    """An IMD image with one sector removed from one track (every other byte as it was)."""
    i = data.index(b"\x1a") + 1
    out = bytearray(data[:i])
    found = False
    while i < len(data):
        mode, c, head, nsec, size_code = data[i:i + 5]
        p = i + 5
        ids = data[p:p + nsec]
        p += nsec
        cmap = data[p:p + nsec] if head & 0x80 else b""
        p += len(cmap)
        hmap = data[p:p + nsec] if head & 0x40 else b""
        p += len(hmap)
        if size_code == 0xFF:
            raise SystemExit("imd_drop_sector: per-sector size tables are not supported")
        recs = []
        for _ in range(nsec):
            kind = data[p]
            n = 1 + (0 if kind == 0 else IMD_SIZES[size_code] if kind % 2 else 1)
            recs.append(data[p:p + n])
            p += n
        keep = [k for k in range(nsec) if not (c == cyl and (head & 1) == side and ids[k] == sector_id)]
        if len(keep) != nsec:
            found = True
        out += bytes([mode, c, head, len(keep), size_code]) + bytes(ids[k] for k in keep)
        out += bytes(cmap[k] for k in keep) if cmap else b""
        out += bytes(hmap[k] for k in keep) if hmap else b""
        for k in keep:
            out += recs[k]
        i = p
    if not found:
        raise SystemExit(f"imd_drop_sector: no sector {sector_id:02X} on cylinder {cyl} side {side}")
    return bytes(out)


def prepare_image(s, letter, src, dst):
    """Copies drive letter's image to dst with the scenario's drops applied."""
    data = Path(src).read_bytes()
    for d, cyl, side, sec in s.drops:
        if d == letter:
            if Path(src).suffix.lower() != ".imd":
                raise SystemExit(f"{s.path}: drop needs an IMD image")
            data = imd_drop_sector(data, cyl, side, sec)
    Path(dst).write_bytes(data)
