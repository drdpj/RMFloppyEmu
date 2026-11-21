# Testing

There are four levels of test. Each catches mistakes the others can't. Read the rules in section 1 before writing or
running any test.

| Level | What it checks | Where | Runs on | When |
|---|---|---|---|---|
| 1. Unit | each `src/core` module on its own | `tests/test_*.c` (Unity, CTest) | PC; also the Pico 2 (`tests/pico/`) | every change; on the Pico 2 at least before a release |
| 2. MAME comparison | our drive logic says exactly what the real IDC firmware says, byte for byte | `tests/mame/` (CTest label `mame`) | PC (MAME to make the traces) | every change to link or drive behaviour |
| 3. Hardware-in-the-loop | the real UART, pins and timing on the Pico 2 | `tests/hil/` | Pico 2 + probe + PC serial adapter (FT232R) | every change to `src/pico`, and before a release |
| 4. 480Z acceptance | the device works as an MD1 on a real 480Z | `docs/acceptance.md` checklist | real 480Z | before a release |

## 1. Rules

1. **Never loosen a test to make it pass.** A failing test is a regression until its cause is found. Never change an
   expected value without first establishing, from the ROM, MAME or the real machine, that the old value was wrong.
   Record that evidence in the commit message. A test that is known to fail goes in section 6 with its reason; it is
   never deleted or disabled quietly.
2. **Every bug fix comes with a test** that fails before the fix and passes after it.
3. **Every new `src/core` module comes with its unit tests**, in the same change. Test the edges as well as the normal
   case: empty input, the maximum size, the first and last track and record, wraparound, and every error status.
4. **Expected values come from an independent source**: `docs/protocol.md`, the ROM, a MAME trace, or arithmetic done
   by hand. Never take them from the code under test. A test that reads its expected value back from the
   implementation proves nothing.
5. **Wrap every test run in `timeout`** (for example `timeout 300 ctest …`). CTest also gives each test a 60-second
   limit. A hang is a failure.
6. **Tests are deterministic.** Use no wall-clock time, no random input (or a fixed seed, printed on failure), and no
   dependence on test order. `src/core` takes time as a parameter, so tests drive the clock themselves.
7. **"Couldn't run it, no data" is not a result.** A test that needs data from `testdata/` **fails** if the data is
   missing; it never skips. Report which levels were actually run.
8. **Tests never change their inputs.** Copy any disk image to a scratch location before a test, or MAME, writes to
   it.
9. **The host tests run with AddressSanitizer and UndefinedBehaviorSanitizer** (`RMFE_SANITIZE=ON`, the default).
   A sanitizer report is a failure.

## 2. Unit tests (level 1)

```
cmake -S tests -B build-tests -G Ninja      # once
ninja -C build-tests
timeout 300 ctest --test-dir build-tests --output-on-failure
```

- There is one executable per module, `tests/test_<module>.c`, registered with `rmfe_add_test(test_<module>)` in
  `tests/CMakeLists.txt`.
- Test names say what they check, as a sentence: `test_read_record_bad_track_returns_status_20`.
- Each test checks one behaviour. Use the most specific Unity assert (`TEST_ASSERT_EQUAL_HEX8_ARRAY` for link bytes),
  so a failure shows the bytes.
- `tests/support/` holds code shared by tests. `assert_hook.c` makes a failed `RMFE_ASSERT` fail the current test.
  `ibm_encode.c` builds synthetic FM/MFM tracks from the IBM System 34 layout, independently of the decoder.
  `flux_synth.c` turns those cells into flux intervals, with deterministic jitter and speed error, for the PLL and
  SCP tests.
- **Real-data tests** (CTest label `realdata`): `hfe_vs_imd_*` decode each protected disc's HFE with
  `tools/hfedump` and compare it with its IMD, read by the independent `tests/check_hfe_vs_imd.py`. They need
  `testdata/` and fail without it. `scp_4mation_*` run real flux through the SCP reader, PLL and decoder and check the protected layout
  (`tests/check_4mation_scp.py`). Dragon World must read every sector good. Flowers of Crystal's one ID that the
  scratch erased on every revolution is declared with `--damaged`, never tolerated silently. Run only these with
  `ctest -L realdata`.
- Unity's reference: `third_party/unity/unity.h`.

### On the Pico 2

The same tests also run on the target, so `src/core` is checked as the RP2350 compiles it (32-bit, `-O3`,
newlib) and its RAM, stack and time can be measured there.

```
cmake -S tests/pico -B build-pico-tests -G Ninja        # once
ninja -C build-pico-tests
timeout 900 tests/pico/run_on_target.py build-pico-tests [test_<module> ...] [--log FILE]
```

- Each `tests/test_<module>.c` is built, unchanged, as its own firmware, `pico_test_<module>.elf`: together the
  fixtures wouldn't fit in 520 KB of SRAM. Its `main()` is renamed `rmfe_test_main()` and called by
  `tests/pico/runner.c`, which prints Unity's output and a `RMFE-TARGET-RESULT` line on the debug console. That line
  gives the time with the console output taken out, the peak stack (by painting the 64 KB below the top of core 0's
  stack), the peak heap, the static RAM, and `sizeof(idc_t)`.
- `run_on_target.py` flashes each module through the probe (through the OpenOCD server if one is running, otherwise
  through a new OpenOCD), reads the console and prints a table. It fails if a test fails, if a module doesn't finish,
  if Unity's test count isn't the number of `RUN_TEST` lines in the source, or if a test is ignored for any reason
  other than missing test data.
- **Tests that need `testdata/` can't run on the target.** They call `RMFE_NEEDS_TESTDATA()`
  (`tests/support/testdata.h`) before reading it. On the PC this does nothing, and missing data still fails. On the
  target, Unity reports the test as `IGNORE: needs testdata/: not run on the target`, and the script lists each one
  by name. There are 8: the `test_real_*` tests in `test_disk`, `test_disk_write` and `test_idc`.
- **Tests of `src/pico`** are in `tests/pico/test_<module>.c`, run only there, and are built with the `src/pico` files
  they test. `test_link_uart` checks the 480Z link driver with no wiring. The UART runs in the PL011's internal
  loopback, and the test plays the 480Z's RTS and DTR by driving those input pins. It checks: 8E1; both baud rates;
  every byte value at both rates; CTS only while waiting; DCD held; sending held off until the 480Z's DTR;
  a record back to back at 125000; go-fast; the RTS reset (once, from any wait, with both FIFOs emptied and the
  link back at 9600); and that undriven inputs read idle. Parity errors and the real pin levels need level 3.
- Fixtures must fit in the RP2350's RAM, so they are sized for the synthetic data and check their own capacity
  (`test_idc_disk` makes tracks on demand and keeps only the ones a test changes). Where only the real-data tests need
  a big buffer, `RMFE_TEST_ON_TARGET` (defined only in the target build) gives it a smaller size there
  (`test_disk_write`).

## 3. MAME comparison tests (level 2)

MAME's `rm480z` driver runs the real IDC3 firmware on an emulated Z80. It is our reference for what the drive says on
the link.

```
tests/mame/make_traces.py                                  # make the reference traces (needs MAME; ~10 minutes)
timeout 300 ctest --test-dir build-tests -L mame --output-on-failure
```

- **Scenarios** (`tests/mame/scenarios/*.scn`, format in `tests/mame/scenario.py`) say what the 480Z does: the
  images in drives A and B (from `testdata/images/`), which are write-protected, any sector to remove from the
  working copy (`drop`), the keys typed and when, and how long to run. They hold no RM material. Each one is a CTest
  test, `mame_<name>`:

  | Scenario | What it covers |
  |---|---|
  | `boot` | CP/M 2.2F boots to `A>`: `00`, `FF`, `05`, `01`, `02`, `06`, `08` |
  | `read` | `DIR`, `TYPE` of a 27-record file, `STAT` |
  | `write` | `SAVE`, a 41-record `PIP` copy, `ERA`, `DIR`: `04` and `08`, and the disc afterwards |
  | `pip_b` | a `PIP` copy from A: to B:, then `DIR B:` and `STAT B:T.COM`: two drives, and drive B's disc afterwards |
  | `disc_full` | a `PIP` copy that fills the disc (223 writes, then CP/M's DISK WRITE ERROR) |
  | `no_disc` | `DIR B:` with drive B empty: `80` |
  | `write_protect` | `SAVE` to a write-protected disc: `08` answers `41` |
  | `missing_sector` | `SAVE` to a sector removed from the disc: the fourth record answers `10` |
  | `verify` | RM's VERIFY, both sides (A: and C:): `06`, `0E`, and `02` for every record |
  | `format` | RM's FORMAT of drive B, double density: `0C`, `0D`, `0F`, and the formatted disc |
  | `format_qd` | the same in quad density: 80 tracks a side, drive bytes `41`/`43`; one declared difference (below) |
  | `fastcopy` | RM's FASTCOPY A to B, reformatting: `0C`, `0E`, `0F`, `03` |
  | `sysgen` | RM's SYSGEN A to B: `0E`, `03` |
  | `copyfile` | RM's COPYFILE A to B |
  | `dragon_world`, `flowers_of_crystal` | the 4MATION loaders on the repaired images: two sectors 08, found by rotation |
  | `dragon_world_from_scp`, `flowers_of_crystal_rebuilt` | the same, on the images rebuilt from the SCP captures |

- **Declared differences from MAME** (`differs` in a scenario) are the only exception to "the discs must
  match", and need the owner's decision and a `docs/protocol.md` entry giving the cause. A `differs` line names
  one track and a range of sector IDs whose *data* may differ. IDs, order, sizes, status and every other sector
  are still compared. `compare.py` prints the difference on every run and fails if it has gone away, and the
  self-check (`mame_selfcheck_detects_mismatches`) moves the line to another track and expects both failures.
  There is one: `format_qd`, B cylinder 0 side 0 sectors 02–10, from the IDC seek bug we deliberately don't
  reproduce ("Switching a drive from double to quad density", `docs/protocol.md`).

- **Reference traces** are made by `tests/mame/make_traces.py`, which runs each scenario in MAME (headless, on
  copies of the images) with `tools/mame/mq2trace.lua`. It writes `testdata/traces/<name>.trace` and the images as
  MAME left them (`<name>.A.imd`, …). They contain RM bytes, so they stay in `testdata/` and are never committed. The
  script refuses a trace whose link is still busy within a second of the end (the scenario needs to run longer).
- **The comparison** (`tests/mame/compare.py`): `mamecmp` (`tests/mame/mamecmp.c`) replays everything the 480Z sent
  into `src/core`, built for the PC, with copies of the same images and the boot block from
  `testdata/roms/IDC3-1I.ROM`. Every byte our unit sends, and every baud change, must be the IDC's, in the same
  order. Then every track of the discs it leaves must match MAME's: IDs in order, data and status, read by the
  independent IMD reader in `tests/check_hfe_vs_imd.py`.
- What the replay takes from the trace, apart from bytes: the IDC's RTS resets (each NMI reloads CTC0 with `0D`)
  and the rotational position, for discs with duplicate sector IDs. That is the time of the IDC's next read or
  write command to its FDC, measured from when the IDC first selected the drive (MAME's index), with the IMD laid
  out as MAME lays it out (`docs/protocol.md`, "Tracing with MAME"). When an NMI arrived while the IDC still had bytes to send, our next send is reset too; `mamecmp`
  counts these. MAME's drives are 80-track, so our drives are set 80-track (`--80-track`), and MAME's saved
  images are mapped back from its double-stepped geometry before comparing.
- What it can't cover: parity errors (the trace doesn't show them, and the 480Z's software never makes them; the
  unit tests cover them), timing, and the IDC's retries (MAME's discs never fail).
- `mame_selfcheck_detects_mismatches` proves the comparison can fail: a wrong, missing or extra byte in a copy of
  the boot trace must each be reported at its line.
- When MAME and the real 480Z disagree, the real machine wins. Record the difference in `docs/protocol.md`.
- **Writing a scenario:** `tests/mame/make_traces.py --explore DIR new.scn` runs it into `DIR`, with `snap` lines
  giving screen snapshots (`DIR/new.snap/`), and doesn't insist the link was idle at the end. RM's utilities lose
  keys typed while they are busy, so leave time between answers.

## 4. Hardware-in-the-loop tests (level 3)

```
ninja -C build-pico-tests                                   # builds hil_firmware.elf too
timeout 300 tests/hil/run_hil.py build-pico-tests [--port LINK_PORT] [--console CONSOLE_PORT] [--log FILE]
```

A USB serial adapter plays the 480Z on the link pins, at 3.3 V TTL, before the RS-232 transceiver. The tests were
developed with an FTDI FT232R, which does 8E1 at exactly 125000. Its handshake pins are active low, like the
transceiver's logic side, so the firmware is unchanged:

| FT232R | Pico 2 | 480Z role |
|---|---|---|
| TXD → | GP5 (UART1 RX), pin 7 | TxD |
| RXD ← | GP4 (UART1 TX), pin 6 | RxD |
| RTS# → | GP8, pin 11 | RTS (reset) |
| DTR# → | GP9, pin 12 | DTR (the unit may send) |
| CTS# ← | GP6, pin 9 | CTS (the unit is waiting) |
| DCD# ← | GP7, pin 10 | DCD |
| GND | GND, pin 8 | |

The I/O voltage jumper is at 3.3 V, and VCC isn't connected.

The scripts find the probe's console port and the adapter by USB ID (the probe is `2e8a:000c`; the adapter is the
one other USB serial port); `--console`/`--port`, or `RMFE_CONSOLE_PORT`/`RMFE_LINK_PORT`, name them instead.

The scripts find the probe's console port and the adapter by USB ID: the probe is `2e8a:000c`, and the adapter is
the one other USB serial port. `--console` and `--port`, or `RMFE_CONSOLE_PORT` and `RMFE_LINK_PORT`, name them
instead.

`run_hil.py` flashes `hil_firmware.elf` (`tests/hil/hil_firmware.c`), which is the real link driver and command layer
with a made-up disc in drive A, shaped like the RM system disc and in the same pattern as `test_idc_disk`. The
adapter flags each received byte that has bad parity (termios `PARMRK`). The script checks:

- the unit sends nothing at power-up
- 8E1 at 9600 and at 125000, and the switch after `00` → `5A 00 01`
- a single byte with bad parity is taken, as the IDC does; everything the unit sends has even parity, so the adapter,
  set to odd, flags every byte
- CTS asserted only while the unit waits; the unit sends only while DTR is asserted; DCD asserted
- 36 records read (`02`), with the data checked against the pattern and the 2:1 skew, and their times
- a record written (`04`) with one bad-parity byte is answered `FF` and taken again, and reads back after `08`
- RTS asserted in the middle of a write abandons it, and a `00` sent at 9600 straight away is answered


**With the SD card** (`tests/hil/run_hil_sd.py build/rmfloppyemu.elf`): the device's own firmware, with its
card, through the same adapter. The card has to be set up as follows (`docs/sd-card.md`):

- `IDC.ROM`: a copy of `testdata/roms/IDC3-1I.ROM`.
- `480Z_RML_system-cpm2.2f-basic5.3g-basic6.3b.imd`, the same as in `testdata/images/`.
- `RMFE-TEST-B.IMD`: a copy of it, for the test to change.
- `IMAGE_A.CFG` naming the first, and `IMAGE_B.CFG` naming the second.

The test fails at once if the start-up report doesn't show these. It checks:

- `FF` sends the boot block, byte for byte against the ROM's own layout (length at `1B28`, block at `1B2A`)
- every record of CP/M's A: and C:, the two sides of drive A's disc (2840: on each, track 0 FM, then 1–39 MFM),
  matches the local IMD, read by
  `tests/check_hfe_vs_imd.py`'s independent reader
- on drive B, track 5 is reformatted FM 16 × 128 (so the IMD on the card is rewritten) and a record is written;
  both survive a restart of the Pico. Then the track is put back as it was, and that survives a restart too

The record written to B comes from a random seed, printed with the results (`--seed` repeats a run).

Both scripts print the git hash with the results, and `--log` keeps them. The timings include USB: a record takes 12–23 ms
from command to last byte, against 11.9 ms on the wire. The 480Z's own timeouts are still an open question
(`docs/protocol.md`).

### MAME in the loop

MAME's emulated 480Z, running the real ROS, CP/M and RM's utilities, in place of the adapter's script: the device
firmware with its card, as for `run_hil_sd.py`, plays the disc unit instead of MAME's `rm_mq2`.

```
cd $RMFE_MAME_SRC && git apply $REPO/tools/mame/rmfe_link.patch     # once; MAME's tree, then rebuild rm480z
RMFE_MAME=.../rm480z timeout 3000 tests/hil/run_mame_linked.py tests/mame/scenarios/{boot,read,pip_b,write,verify,format}.scn
tests/hil/run_mame_linked.py --restore                              # put the card back after --keep-card or a failure
```

- `tools/mame/rmfe_link.patch` adds `rmfe_link`, an RS-232 slot device for the 480Z's disc port. It relays bytes
  and all four handshake lines over a socket (MAME's bitbanger, `-bitb socket.HOST:PORT`), and serialises the
  unit's bytes into the 480Z's SIO at 9600 or 125000, following the IDC's rule: back to 9600 when the 480Z asserts
  RTS, 125000 once `5A 00 01` has been relayed for a `00`. It also does the transmit half of the IDC's auto
  enables, sending to the 480Z only while its DTR is asserted: the 480Z paces the unit with DTR, and the real
  unit only learns of a change a millisecond or so later through USB (`docs/protocol.md`, "Flow control").
  `RMFE_LINK_LOG` logs the link with MAME's emulated time; `RMFE_LINK_DTR_SLACK=n` lets n more bytes through after
  DTR drops, to test what the 480Z tolerates.
- `tools/mame/link_bridge.py` connects that socket to the FT232R (low-latency mode), mirrors the 480Z's RTS and DTR
  onto the adapter's, and polls the adapter's CTS and DCD back into MAME. It can be used on its own, with MAME
  started by hand (with a window, to use the 480Z interactively).
- `run_mame_linked.py` runs each scenario at real speed, windowless, and restarts the Pico first if an OpenOCD
  server is running. It fails if MAME falls below 95% of real speed, if the link is still busy in the last
  second, or if any record of either drive, read back through the link afterwards, differs from the disc MAME's
  own unit left (`testdata/traces/<name>.<drive>.imd`; the original for a drive the scenario doesn't use). It
  then puts every changed record back, paced by CTS as the 480Z sends, so the card stays set up. Only scenarios
  whose discs are the card's (the system disc in A, a copy in B) can run; the others are refused.
- It compares the bytes the 480Z sent and received with the MAME reference trace, and prints each difference, but
  they aren't failures: with the card's drives 40-track (`drive-a = auto`), `06`'s format byte is `02`, not
  MAME's `0A`, and CP/M then takes a slightly different path (an extra log-in and read after each warm boot).
- It writes MAME's output, the screen snapshots, the link log and the bridge's log to `testdata/linked/<name>/`.
- The turnaround the bridge adds is measured both ways: by the bridge (from writing the 480Z's byte to the
  adapter to reading the first byte back) and by the 480Z, in MAME's time.

## 5. Test data

| Data | Where | Used by |
|---|---|---|
| IDC ROMs (`IDC3-1I.ROM`, `IDC5-1J.ROM`) | `testdata/roms/` (copied from `background-docs/`) | levels 2 and 3 |
| 480Z disk images | `testdata/images/` (copies of your own) | levels 2–4 |
| Repaired 4MATION IMDs that boot (`*_both-sector-08.IMD`, made with `tools/imd/add_duplicate_sector.py`); masters in `background-docs/repaired/` | `testdata/images/` | level 2 (MAME boot), level 4 |
| SCP captures of the 4MATION originals (`480Z_4MATION_*_1985.scp`, read at 360 rpm) | `testdata/images/` (copied from `background-docs/`) | `scp_4mation_*` (level 1, label `realdata`) |
| Rebuilt 4MATION images (`*_rebuilt_*rpm.scp`, `*_rebuilt.IMD`); masters in `background-docs/repaired/` | `testdata/images/` | level 2 (MAME boot), level 4 |
| Protected 4MATION discs, HFE and IMD of each (`480Z_4MATION_Dragon_World_1985`, `480Z_4MATION_Flowers_of_Crystal_1985`) | `testdata/images/` (copied from `background-docs/`) | `hfe_vs_imd_*` (level 1, label `realdata`) |
| MAME reference traces and the images MAME left (`<scenario>.trace`, `<scenario>.A.imd`) | `testdata/traces/` (made by `tests/mame/make_traces.py`) | level 2 |

`testdata/` is gitignored and must never be committed (docs/coding.md section 11). Tests find it through the CMake
cache variable `RMFE_TESTDATA`, which defaults to `<repo>/testdata`.

## 6. Known failures

None.
