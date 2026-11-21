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
"""Turn text into an MQ2_KEYS sequence for mq2trace.lua.

usage: keys.py START_SECONDS 'TEXT' ['TEXT' ...]
Each TEXT is typed starting at START (later texts follow on); '\\r' or a newline is Return.
Example: MQ2_KEYS="$(tools/mame/keys.py 12 'SAVE 2 T.COM\\r' 'DIR\\r')"

Letters are typed unshifted (CP/M upper-cases commands anyway); the shifted symbols below are typed
with Shift held ("Shift+<key>", which mq2trace.lua understands). Key names are the rm480z driver's
ioport field names (MQ2_LISTKEYS=1 prints them).
"""
import sys

STEP = 0.25  # seconds between key presses; mq2trace holds each key for 0.1 s

NAMED = {" ": "Space", "\r": "Carriage Return", "\n": "Carriage Return",
         ".": ". >", ",": ", <", ":": ": *", ";": "; +", "/": "/ ?", "-": "- =",
         "@": "@ `", "[": "[ {", "]": "] }", "\\": "\\ |", "^": "^ ~"}
DIGITS = {"1": "1 !", "2": '2 "', "3": "3 #", "4": "4 $", "5": "5 %", "6": "6 &",
          "7": "7 '", "8": "8 (", "9": "9 )", "0": "0 _"}


# The symbol on the shifted half of each key (the second half of its name).
SHIFTED = {"=": "- =", "*": ": *", "+": "; +", ">": ". >", "<": ", <", "?": "/ ?", "`": "@ `",
           "{": "[ {", "}": "] }", "|": "\\ |", "~": "^ ~", "!": "1 !", '"': '2 "', "#": "3 #",
           "$": "4 $", "%": "5 %", "&": "6 &", "'": "7 '", "(": "8 (", ")": "9 )", "_": "0 _"}


def key_name(ch):
    if ch.isalpha():
        return f"{ch.lower()} {ch.upper()}"
    if ch in DIGITS:
        return DIGITS[ch]
    if ch in NAMED:
        return NAMED[ch]
    if ch in SHIFTED:
        return "Shift+" + SHIFTED[ch]
    raise SystemExit(f"keys.py: no key for {ch!r}")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    t = float(sys.argv[1])
    out = []
    for text in sys.argv[2:]:
        for ch in text.replace("\\r", "\r"):
            out.append(f"{t:.2f}:{key_name(ch)}")
            t += STEP
        t += 2.0  # let the command run before the next text
    print(";".join(out))


if __name__ == "__main__":
    main()
