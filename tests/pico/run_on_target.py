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
"""Runs the unit tests on the Pico 2 (docs/testing.md section 2, "On the Pico 2").

usage: run_on_target.py BUILD_DIR [MODULE ...] [--port CONSOLE_PORT] [--log FILE]

Flashes each pico_test_<module>.elf from BUILD_DIR (tests/pico/) through the probe, reads the debug
console until the runner says RMFE-TARGET-DONE, and prints a table: tests, failures, the tests that need
testdata/ (which can't run on the target and are listed by name), time, stack and heap. It flashes through
an OpenOCD server already running on the probe (its Tcl port, 6666) if there is one, and otherwise starts
OpenOCD for each module.

It fails if any test fails, if a module doesn't finish, if Unity's test count isn't the number of
RUN_TEST lines in tests/test_<module>.c (or tests/pico/test_<module>.c), or if a test is ignored for any reason but missing testdata/.
"""
import argparse
import os
import pathlib
import re
import socket
import subprocess
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[2]
TESTDATA_MSG = "needs testdata/: not run on the target"  # tests/support/testdata.h
OPENOCD = ["openocd", "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg", "-c", "adapter speed 5000"]
TCL_PORT = 6666

PROBE_USB_ID = (0x2E8A, 0x000C)  # Raspberry Pi Debugprobe (CMSIS-DAP): its USB serial port bridges the console

# Resetting the RP2350 (docs/testing.md, "Resetting the Pico 2"). OpenOCD's `reset` asserts SYSRESETREQ on each core in
# turn, 50 ms apart, and on the RP2350 SYSRESETREQ resets only its own core (datasheet; raspberrypi/openocd PR 153).
# With core 1 running from flash meanwhile, it collides with core 0's boot ROM driving the flash in direct mode:
# measured on this firmware, about 1 reset in 6 leaves the QMI stuck busy, and the chip then needs a power cycle
# or the rescue reset. So nothing here resets a running chip with OpenOCD's `reset`:
#   - RESET_CHIP is the reset the SDK's watchdog_reboot() does: every block but the oscillators (PSM WDSEL),
#     both cores and the QMI together, with watchdog scratch 4 cleared so the boot ROM doesn't take it for a
#     reboot-to-address. 0 of 90 such resets failed where 5 of 30 OpenOCD resets did.
#   - flashing halts both cores first, so neither is running when OpenOCD resets them (as hathach/agentrc
#     found for `program`), then starts the new firmware with RESET_CHIP.
PSM_WDSEL, WDSEL_ALL_BUT_OSCILLATORS = 0x40018008, 0x01FFFFF3
WATCHDOG_CTRL, WATCHDOG_SCRATCH4, WATCHDOG_TRIGGER = 0x400D8000, 0x400D801C, 0x80000000
RESET_CHIP = (f"mww 0x{PSM_WDSEL:08x} 0x{WDSEL_ALL_BUT_OSCILLATORS:08x}; mww 0x{WATCHDOG_SCRATCH4:08x} 0; "
              f"mww 0x{WATCHDOG_CTRL:08x} 0x{WATCHDOG_TRIGGER:08x}")


def find_port(what, given=None):
    """The serial port for 'console' (the probe's bridge to the target's debug console) or 'link' (the USB serial
    adapter playing the 480Z): as given, else from RMFE_CONSOLE_PORT / RMFE_LINK_PORT, else found by USB ID (the
    probe's, and the one other USB serial port). Stops if that isn't clear."""
    env = "RMFE_CONSOLE_PORT" if what == "console" else "RMFE_LINK_PORT"
    if given or os.environ.get(env):
        return given or os.environ[env]
    from serial.tools import list_ports
    usb = [p for p in list_ports.comports() if p.vid is not None]
    found = [p for p in usb if ((p.vid, p.pid) == PROBE_USB_ID) == (what == "console")]
    if len(found) != 1:
        sys.exit(f"can't tell which serial port is the {'probe' if what == 'console' else '480Z adapter'}: "
                 f"found {[p.device for p in found] or 'none'}. Name it with the script's port option or {env}.")
    return found[0].device


UNITY_LINE = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<test>\w+):(?P<what>PASS|FAIL|IGNORE)(?::\s*(?P<msg>.*))?$")
UNITY_SUMMARY = re.compile(r"^(\d+) Tests (\d+) Failures (\d+) Ignored\s*$")


def tcl(sock, cmd):
    sock.sendall(cmd.encode() + b"\x1a")
    out = b""
    while not out.endswith(b"\x1a"):
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("OpenOCD closed the Tcl connection")
        out += chunk
    return out[:-1].decode(errors="replace").strip()


def flash(elf, server):
    """Programs elf and starts it: both cores halted first, then the whole chip reset (RESET_CHIP, above)."""
    program = f"targets rp2350.dap.core0; halt; program {{{elf}}} verify"
    if server:
        if tcl(server, "catch {%s}" % program) != "0":
            raise RuntimeError(f"OpenOCD could not program {elf.name}")
        reset_chip(server)
    else:
        # the trigger's own write is cut short by the reset it causes, so it is in a catch
        cmds = ["init", program, "catch {%s}" % RESET_CHIP, "sleep 100", "catch {targets rp2350.dap.core0; resume}",
                "exit"]
        subprocess.run(OPENOCD + [a for c in cmds for a in ("-c", c)], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=120)


def reset_chip(server):
    """Restarts the firmware: the whole chip at once, never OpenOCD's core-by-core `reset`. The trigger's own write is
    cut short by the reset it causes, so its result means nothing."""
    tcl(server, "catch {%s}" % RESET_CHIP)
    resume_if_halted(server)


def resume_if_halted(server):
    """After RESET_CHIP the cores run, unless the debugger had halted them, as flash() does: then resume them."""
    time.sleep(0.1)
    tcl(server, "catch {targets rp2350.dap.core0; resume}")


def run_module(name, elf, port, server, timeout, log):
    port.reset_input_buffer()
    flash(elf, server)
    lines, result, began = [], None, False
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        line = raw.decode(errors="replace").rstrip("\r\n")
        if line.startswith("RMFE-TARGET-BEGIN"):
            began, lines = True, []  # anything before it is the previous firmware's
        if not began:
            continue
        lines.append(line)
        if log:
            log.write(line + "\n")
        if line.startswith("RMFE-TARGET-RESULT"):
            result = dict(kv.split("=", 1) for kv in line.split()[1:])
        if line == "RMFE-TARGET-DONE":
            return lines, result
    return lines, None


def check(name, lines, result):
    """Returns (row, problems) for one module's console output."""
    problems, testdata, failed = [], [], []
    summary = None
    for line in lines:
        m = UNITY_LINE.match(line)
        if m:
            if m["what"] == "FAIL":
                failed.append(f'{m["test"]}: {m["msg"]}')
            elif m["what"] == "IGNORE":
                if m["msg"] == TESTDATA_MSG:
                    testdata.append(m["test"])
                else:
                    problems.append(f'{name}: {m["test"]} ignored: {m["msg"]}')
        m = UNITY_SUMMARY.match(line)
        if m:
            summary = tuple(int(x) for x in m.groups())
    if result is None:
        problems.append(f"{name}: did not finish (no RMFE-TARGET-DONE)")
        return None, problems, testdata
    if summary is None:
        problems.append(f"{name}: no Unity summary")
        return None, problems, testdata
    tests, failures, ignored = summary
    src = ROOT / "tests" / f"{name}.c"
    if not src.exists():
        src = ROOT / "tests" / "pico" / f"{name}.c"  # a test of src/pico, on the target only
    want = len(re.findall(r"^\s*RUN_TEST\(", src.read_text(), re.M))
    if tests != want:
        problems.append(f"{name}: Unity ran {tests} tests, the source has {want} RUN_TEST lines")
    if failures or int(result["failures"]):
        problems.append(f"{name}: {failures} failed: " + "; ".join(failed))
    if result.get("heap_met_stack") == "1":
        problems.append(f"{name}: the heap reached the painted stack area; the stack figure is unreliable")
    row = (name, tests, failures, ignored, int(result["time_us"]) - int(result["output_us"]),
           int(result["output_us"]), result["stack_peak"],
           int(result["heap_peak"]), int(result["static_ram"]))
    return row, problems, testdata


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", type=pathlib.Path)
    ap.add_argument("modules", nargs="*", help="test_<module> names (default: every pico_test_*.elf)")
    ap.add_argument("--port", help="the probe's console port (default: found by USB ID, or RMFE_CONSOLE_PORT)")
    ap.add_argument("--timeout", type=float, default=300, help="seconds per module")
    ap.add_argument("--log", type=pathlib.Path, help="also write the console output here")
    args = ap.parse_args()

    if args.modules:
        elfs = [(m, args.build / f"pico_{m}.elf") for m in args.modules]
    else:
        elfs = [(p.stem[len("pico_"):], p) for p in sorted(args.build.glob("pico_test_*.elf"))]
    if not elfs:
        sys.exit(f"no pico_test_*.elf in {args.build}")
    for _, elf in elfs:
        if not elf.exists():
            sys.exit(f"missing {elf}")

    git = subprocess.run(["git", "-C", str(ROOT), "describe", "--always", "--dirty"], capture_output=True,
                         text=True).stdout.strip()
    try:
        server = socket.create_connection(("localhost", TCL_PORT), timeout=60)
        how = f"the running OpenOCD server (port {TCL_PORT})"
    except OSError:
        server, how = None, "a new OpenOCD for each module"
    args.port = find_port("console", args.port)
    print(f"git {git}; flashing through {how}; console {args.port}")

    log = args.log.open("w") if args.log else None
    if log:
        log.write(f"git {git}\n")
    rows, problems, testdata = [], [], []
    with serial.Serial(args.port, 115200, timeout=1) as port:
        for name, elf in elfs:
            print(f"  {name} ...", end="", flush=True)
            lines, result = run_module(name, elf.resolve(), port, server, args.timeout, log)
            row, p, td = check(name, lines, result)
            print(" ok" if not p else " FAILED")
            if row:
                rows.append(row)
            problems += p
            testdata += [f"{name}: {t}" for t in td]
    if log:
        log.close()

    print()
    # time: the tests' own, without writing Unity's output (console), in ms. Memory in bytes; a stack
    # figure ending in + filled the 64 KB painted, so is at least that.
    print(f"{'module':<18} {'tests':>5} {'fail':>4} {'testdata':>8} {'time ms':>9} {'console':>8} {'stack':>7} "
          f"{'heap':>7} {'static':>7}")
    for name, tests, failures, ignored, us, out_us, stack, heap, static in rows:
        print(f"{name:<18} {tests:>5} {failures:>4} {ignored:>8} {us / 1000:>9.1f} {out_us / 1000:>8.1f} "
              f"{stack:>7} {heap:>7} {static:>7}")
    total = sum(r[1] for r in rows)
    print(f"{len(rows)} modules, {total} tests, {total - len(testdata)} run on the target")
    if testdata:
        print(f"\nNot run on the target, because they need testdata/ ({len(testdata)}; run them on the PC):")
        for t in testdata:
            print(f"  {t}")
    if problems:
        print("\nFAILED:")
        for p in problems:
            print(f"  {p}")
        sys.exit(1)
    print("\nPASSED")


if __name__ == "__main__":
    main()
