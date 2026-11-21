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
"""src/flux must stay reusable (src/flux/README.md): its files may include only flux/ headers and the
C standard library. Exit 1 and list any other include."""
import pathlib
import re
import sys

STD = {"stdint.h", "stddef.h", "stdbool.h", "string.h", "limits.h"}
root = pathlib.Path(sys.argv[1])
bad = []
for f in sorted(root.glob("*.[ch]")):
    for n, line in enumerate(f.read_text().splitlines(), 1):
        m = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
        if m and not (m.group(1).startswith("flux/") or m.group(1) in STD):
            bad.append(f"{f.name}:{n}: {m.group(1)}")
print("\n".join(bad) if bad else f"{root}: self-contained")
sys.exit(1 if bad else 0)
