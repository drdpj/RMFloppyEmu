# Plan

The approach is a C reimplementation of the disk unit's command set, with a staged boot block (`docs/protocol.md`,
"Implementation decision"). Each milestone ends with clean builds and passing tests, as `docs/testing.md` sets out.

| # | Milestone | Done when |
|---|---|---|
| 1 | **Protocol fully specified.** Decode every command (`FE`, `FF`, `00`–`17`), the parameter byte, status codes and the disk formats. Use the IDC3 disassembly plus MAME traces of writes, formatting, errors and both drives | every command in `docs/protocol.md` has framing, meaning and evidence, and the open questions are answered or explicitly parked |
| 2 | **Link layer** (`src/core`): the byte state machine for command framing, the baud negotiation, the RTS reset, DTR gating and block ack/retry, behind a small platform interface | unit tests cover every exchange in `docs/protocol.md` |
| 3 | **Disk images** (`src/core`): one track-at-a-time model (sectors in rotational order, with positions and data) fed from IMD and HFE (via `src/flux`); writes back to both (IMD by safe rewrite, HFE via a C FM/MFM encoder in `src/flux`); geometry; records ↔ physical sectors | unit tests on synthetic images, plus a check against the real images in `testdata/` |
| 4 | **Command handlers** (`src/core`): each command on top of 2 and 3, with the boot block read from the user's ROM file | unit tests per command |
| 5 | **MAME comparison** (test level 2): scenario runner, generation of the reference traces, byte-for-byte comparison | the CP/M boot plus the read, write and error scenarios match MAME |
| 6 | **Pico platform** (`src/pico`): UART at 8E1 with 9600/125000 switching, DTR/RTS GPIO handling, SD card (FatFs), TFT and encoder UI, console (`docs/hardware.md`) | hardware-in-the-loop tests (level 3) pass with a USB serial adapter |
| 7 | **Interface hardware:** RS-232 transceiver, connector and cable to the SIO/4 port | the real 480Z boots CP/M (`docs/acceptance.md`) |
| 8 | **Clean-room boot block:** specify what the 480Z needs from it, then write our own Z80 code from that specification | it boots CP/M in MAME and on the real 480Z; the ROM file becomes optional |

## Progress

- **Milestone 1** (protocol): done. Every command is in `docs/protocol.md`.
- **Milestone 2** (link layer): done (2026-11-20). `src/core/link`: 9600/125000 baud, the RTS reset, go-fast, record
  send/receive with ack and parity retry. `src/core/idc`: command framing, status echo, unknown-command `40`, and the
  commands that need no disc (`00`, `FF`, `05`, `07`, `11`, `16`). The boot block is found in the user's IDC ROM by
  its handler code (IDC3 and IDC5).
- **Milestone 3** (disc images): done (2026-11-20). `src/core/disk` is the track model (rotational order, positions,
  find-by-rotation for duplicate IDs). `disk_imd` reads, updates in place when every record keeps its length, and
  otherwise rewrites the whole file through a sink (the platform writes a new file and renames it; this also covers
  formatting and new tracks). `disk_hfe` reads through `src/flux`, and writes by re-encoding the track with
  `flux/ibm_encode` and writing it in place (v1; v3 is read-only). Checked on the real discs; an HFE fully rewritten
  by this code reads back at 730/730 in HxC and Greaseweazle.
- **Milestone 4** (commands): started. `0A` upload is implemented exactly. `0B` is reported to the platform and
  answered `40`: no Z80 unless real software needs it (`docs/protocol.md`, "Implementation decision"). The drive byte
  and the record mapping are decoded. `01` log in, `02` read, `03`/`04` write and `08` flush are done
  (`src/core/idc_disk.c`): one cached track per drive, written back on flush or a track change, duplicate IDs
  chosen by rotational position. The platform supplies the discs through `idc_drives_t`. `idc_t` holds the two
  track caches and the upload RAM (19,556 bytes on the RP2350), so the firmware keeps it in static storage. `06`, `0C`, `0D`, `0E`, `0F` and `10` are
  done too. Every command is implemented except `09` (not decoded, never seen) and the aux-port commands; see
  `docs/protocol.md`, "Implementation status".
- **Milestone 5** (MAME comparison): done (2026-11-20). `tests/mame/`: scenarios, `make_traces.py` (reference traces
  from MAME), `mamecmp` (replays them into `src/core` and compares every byte) and `compare.py` (also compares the
  discs left behind). The CP/M boot, read, write, disc-full, no-disc and write-protect scenarios match MAME byte for
  byte, discs included (`docs/testing.md` section 3). The comparison found four differences, all fixed in
  `src/core` with ROM evidence: `06` sends the status twice before the format byte; MFM records are skewed 2:1
  (IDs `1 6 2 7 3 8 4 9 5`) and the record limit comes from the format; a cache write-back error has bit 0 set
  (`41`), is the result rather than the status, and loses the change; and the drive type is now a setting (MAME's
  drives are 80-track).
  Follow-up (2026-11-21): scenarios for RM's VERIFY, FORMAT, FASTCOPY, SYSGEN and COPYFILE, a write to a missing
  sector, both 4MATION games on both kinds of image, and FORMAT in quad density: 18 MAME tests, all matching
  (one with a declared difference, below). Fixed on the way: the track
  limit is the drive's (80 in an 80-track drive); a record completing its 512-byte buffer is written at once and
  its error is that write's result; IMD tracks are laid out with standard IBM gaps, as MAME does, and each drive
  has its own index, so the 4MATION loaders meet the same copy of sector 08 as in MAME. None of the utilities uses
  `0A`/`0B`. `0C` places sectors from the IDC's own track templates. An IDC bug that sends quad-density
  FORMAT's first track to the wrong physical track is documented and deliberately not reproduced (`format_qd`
  declares it). Left open: how the IDC learns the drive type (its stepping test or board links). **Next:
  milestone 6**, the Pico platform.
- **Milestone 6** (Pico platform): started (2026-11-21).
  - Step 1 is done: the unit tests run on the Pico 2 (`tests/pico/`, `docs/testing.md` section 2). There is one
    firmware per module, flashed and read back through the probe. 129 of the 137 tests pass there. The other 8 need
    `testdata/`, and are listed by name rather than run. The fixtures of `test_idc_disk`, `test_scp`, `test_flux_pll`
    and `test_disk_write` were resized to fit in RAM, without changing what they check.
  - Measured on the target, with the SDK defaults (150 MHz, `-O3`):
    - `sizeof(idc_t)` is 19,556 bytes, and `sizeof(disk_track_t)` is 8,716.
    - The core's largest stack frames are `scp_verify_checksum` (552 bytes) and `disk_imd_rewrite` (376). The peak
      stack of any module, counting Unity and the test's own locals, is 2,740 bytes (`test_disk_write`, whose tests
      keep `disk_imd_t`s on the stack). `test_idc_disk` peaks at 1,840.
    - The SDK gives core 0 a nominal 2 KB of stack (`PICO_STACK_SIZE`) in the 4 KB `SCRATCH_Y`, with no guard.
      Decided (owner, 2026-11-21): the firmware uses all 4 KB, with the Cortex-M33's hardware stack limit (MSPLIM)
      at its bottom (`PICO_USE_STACK_GUARDS`). An overflow faults at once. `src/pico/fault.c` then reports it on the
      console and halts with the LED flashing, as `RMFE_ASSERT` does. This was checked on the Pico with SP forced
      to the limit (CFSR `00100000`, STKOF). `idc_t` and the image state stay static. The firmware's real peak
      stack is still to be measured once FatFs and the display are in.
    - The command-layer tests take milliseconds there. The flux tests take longer, but the firmware doesn't run
      them: `test_flux_pll` takes 1.6 s, `test_ibm_track` 1.2 s and `test_scp` 0.5 s, against 19, 17 and 7 ms on
      the PC (`-O3`, no sanitizers).
  - Step 2 is done: the 480Z link on UART1 (`src/pico/link_uart.c`), and the firmware serves the command layer over
    it. There are no discs and no ROM until the SD card is added. The driver is checked on the target with no
    wiring (`tests/pico/test_link_uart.c`, 17 tests, `docs/testing.md` section 2). The IDC's SIO has auto enables on
    (`docs/protocol.md`, "Flow control"), so the unit sends only while the 480Z's DTR is asserted.
  - The level-3 tests are built and pass (2026-11-21): an FT232R plays the 480Z on the link pins
    (`tests/hil/`, `docs/testing.md` section 4). They found two bugs in the driver, both fixed and now tested:
    - The unit answered a phantom `5A 00 01` at power-up. The UART was enabled before GP5 was connected to it, and
      read a break as `00`.
    - After an RTS reset the receiver was off for 1.8 ms. The SDK's baud and FIFO calls wait 15 bit times, which
      could garble a `00` the 480Z sends soon after. The driver now reprograms the UART directly, in under 50 us.
  - Step 3, the SD card: working (2026-11-21).
    - The firmware mounts the card (FAT32 or exFAT), reads the IDC ROM for the boot block, and puts images in
      drives A and B the FlashFloppy way: `RMFE.CFG`, `IMAGE_x.CFG`, `INIT_x.CFG` (`docs/sd-card.md`). The library
      is carlk3's FatFs/SD v3.6.2, vendored. Its `crash.c` isn't built, and FatFs uses our `ffconf.h`, which puts
      the long-name buffer in static memory instead of on the heap.
    - New in `src/core`, unit-tested: `config` (the configuration files and the image list) and `disk_image` (one
      image of either kind, with the safe save: in place, or a new file renamed over the old). `mamecmp` uses
      `disk_image` too.
    - Level 3 with the card (`tests/hil/run_hil_sd.py`, 18 checks) passes:
      - the boot block, byte for byte;
      - all 2824 records of the system disc;
      - a reformat and a write on drive B, and the track put back, each surviving a restart.
    - IMD saves (owner, 2026-11-21: work with uncompressed IMDs). Rewriting the file is slow on the card, about
      1 s for 300 KB, against a real IDC's 0.2 s. So the first rewrite leaves the whole image uncompressed
      (`disk_imd_rewrite`), and every later write that keeps a track's layout goes in place. RM's FORMAT sends
      sector IDs in order, so reformatting an RM disc goes in place, at 0.12 s a track. A rewrite is still needed
      for a change of layout (FM/MFM, sector count or order), and for the first write that grows a compressed
      sector. On the way, the platform's replacement writes were batched in 16 KB, and its reads cached 4 KB at a
      time (4.4 s down to 1 s). The SPI clock went from 12.5 to 15 MHz.
  - Step 4, MAME's 480Z with the real unit: working (2026-11-21). MAME's emulated 480Z, running the real ROS,
    CP/M and RM's utilities, talks through the FT232R to the Pico and its card instead of MAME's own disc unit
    (`docs/testing.md` section 4, "MAME in the loop"). CP/M boots to `A>`, and `boot`, `read`, `write`, `pip_b`
    (a new scenario, a PIP copy to drive B), `verify` and `format` all pass at real speed: every record of both
    drives afterwards matches the disc MAME's own unit left. The link differs from MAME's only where the drive
    type does (the card's drives are 40-track, MAME's 80).
    - New: `tools/mame/rmfe_link.patch` (an RS-232 slot device for MAME that relays bytes and the four handshake
      lines over a socket), `tools/mame/link_bridge.py` (socket to FT232R) and `tests/hil/run_mame_linked.py`.
    - The bridge adds 0.6 ms median at 125000 (bridge's turnaround 0.55–0.64 ms against 0.18 ms of wire time;
      90% under 1.2 ms), 3 ms at 9600. The 480Z, in MAME's time, sees replies 0.9 ms median and 5.8 ms at worst
      after its byte, against 0.05 ms from `rm_mq2`. Its ROS waits about 145 ms, so this is well within it.
    - Found: the 480Z paces the unit with DTR, and the unit must stop within about a byte. Through USB the Pico
      learns too late, so the MAME device does the IDC's DTR gating itself (`docs/protocol.md`, "Flow control").
      In MAME the 480Z tolerates two bytes beyond the IDC's; the Pico sends at most one.
  - Step 5, the display and encoder: started (2026-11-21). The pin plan was checked against the RP2350's function
    table and the module's schematic, and corrected (`docs/hardware.md`): the module switches its own backlight
    (an S8050 behind a 1 kΩ resistor), so GP22 drives `LED` directly and no transistor is needed; `VCC` comes from
    VSYS, because VBUS is dead with the Pico's own USB unplugged; touch and the module's SD slot aren't used, which
    frees GP27/GP28. The bring-up firmware is `tests/hil/display_probe.c`, run by `tests/hil/run_display_probe.py`
    (`docs/testing.md` section 4). The font is misc-fixed 8x13 (public domain), made into C by `tools/font/pcf2c.py`.
    - Wired and probed (2026-11-21): ILI9341, MISO working, colour bars and text right, backlight PWM and the
      knob's clicks and press counted. Writes at 75 MHz were garbled (the probe first picked it from a short
      read-back, and the status line showed it); a soak of 100 full-width strips is clean at 37.5 MHz and below.
    - The display, knob and menu: working (2026-11-21).
      - New in `src/core`, unit-tested: `rotary` (quadrature, switch debounce, long press; `rotary = full|half|
        quarter[,reverse]` in `RMFE.CFG`, FlashFloppy's option), `dirlist` (a folder's listing, FlashFloppy's
        order) and `ui` (the menu, driven by knob events and a tick count). `disk_imd`/`disk_image` open in steps
        (one track record each), and `idc` reports the gap between commands (`idle_hook`), changes a disc
        (`idc_change_disc`: the cached track goes to the old image first) and reports what it is doing
        (`activity_hook`).
      - The two cores (`src/pico/main.c`): core 0 keeps the link, the command layer and the SD card, and does the
        menu's card work a step at a time while it waits for a command (`menu_host.c`); core 1 has the display
        (`tft.c`: ILI9341 at 37.5 MHz, DMA, a 16-pixel text row at a time), the knob (GPIO interrupts) and the
        menu (`panel.c`). They meet only in `mailbox.c`.
      - Changing an image (owner, 2026-11-21): instant, between commands (`docs/protocol.md`, "Changing a disc";
        `docs/sd-card.md`, "Choosing an image"). The display shows a light per drive (reading or writing, and the
        track), and with `show-commands = yes` each command as it is served.
      - Measured with the menu in use (`run_mame_linked.py --use-menu`: about ten keys a second, 535 folder lists
        over the six scenarios): all six pass. The longest step of menu work on core 0 was 1.2 ms (a command could
        have waited that long for it; the ROS waits about 145 ms). The bridge's turnaround at 125000 is as before
        (median 0.47-0.55 ms) except in `format`, whose 90th percentile of 111 ms and maximum of 179 ms are the
        formatting itself (each `0C` writes a track to the card), not the menu.
      - Peak stack, measured by painting, with FatFs, the display and the menu all in: core 0 2156 of 4096 bytes,
        core 1 2044 of 4096.
      - Swapping drive B's image through the menu, an IMD for an HFE and back: the longest step was 15 ms.
      - Found on the way: OpenOCD's `reset` resets the RP2350's cores one at a time, and with core 1 running from
        flash in the gap, the QMI can stick busy, leaving the chip stuck in the boot ROM until a power cycle or a
        rescue reset (5 of 30 resets). The test scripts now restart with a whole-chip watchdog reset (0 of 90) and
        flash with both cores halted (`docs/testing.md`, "Resetting the Pico 2"). Only the debugger's reset is
        affected; power-on, the RUN pin and the watchdog reset both cores together.
      - Closed afterwards: the card can be taken out and put back while running (checked every 2 s between
        commands; the remount is one step of 89 ms); the 480Z waits at least 4 s for a slow format and 2 s for a
        slow write (`docs/protocol.md`, "Flow control"), so an image's whole rewrite (at most about 2.5 s) is
        tolerated; `format`'s long waits are the formatting itself, as without the menu (90% 111 ms, max 273 ms);
        and `tools/imd/make_blank.py` makes blank discs that format in place (`docs/sd-card.md`).
    - **Next:** test on the real 480Z (`docs/acceptance.md`), and milestone 7, the transceiver.
- **Milestone 7** (interface hardware): started (2026-11-22).
  - Two MAX3232 modules on the link pins, an 8-pin DIN socket wired the MD1's way, and RM's own disc unit lead,
    which is a symmetric null modem (1–5, 2–7, 3–8, 4–4; `docs/hardware.md`, "Wiring two MAX3232 modules").
  - On the real 480Z (ROS 1.2), at 125000 the boot block went out whole and correctly paced by DTR, but the 480Z lost
    bytes and showed "Disc Error". Found from the ROS: the third byte of the go-fast reply is the time constant the
    480Z loads into its CTC0, so the unit sets the 480Z's fast rate (`docs/protocol.md`, "Framing and baud rate").
    Answering `5A 00 02`, at 62500, the 480Z loaded the boot block, the CP/M system tracks and the directory, and
    then booted CP/M to `A>` and 4MATION's Flowers of Crystal, whose loader talks to the drive itself.
  - It isn't the modules (2026-11-23). Fitting 910 nF across their pump capacitors brought V− from −4.2 V to
    −8.0 V, and the line at the 480Z's socket was clean (rise 0.76 us, fall 1.2 us), yet the boot block still failed
    at 125000; so did 10 us of idle between bytes and an 8N2 boot block. Then, with only the ROS's exchange at 62500
    (its `00` arrives with a parity error, its SIO being at 8N2) and every later one at 125000, CP/M booted and ran
    `DIR` and `TYPE` at 125000. Only the ROS's loading of the boot block fails at 125000; why is an open question
    (`docs/protocol.md`). Decided (owner, 2026-11-23): keep the split, as a setting, and find the cause later.
  - With the modules back on 3V3 (about ±5.5 V), `PIP B:=A:*.*[V]` copied the whole system disc to a blank image
    and verified it at 125000, with no errors: 550 KB across the link, no parity errors, no resets. So a ±5.4 V
    transceiver from 3.3 V (the TI MAX3243E) is enough for the board (`docs/hardware.md`, "For a board").
  - Also passed on the 480Z (2026-11-23): swapping images from the knob at `A>`, RM's FORMAT and VERIFY of an IMD in
    drive B, and the 4MATION games with their own loaders: Flowers of Crystal from the repaired IMD, and Dragon World
    from its IMD and from the HFE rebuilt from its flux capture, the first HFE on the real machine. All at 125000
    with no parity errors (`docs/acceptance.md`).
  - New: `fast-baud = 125000 | 62500` (default 125000, the IDC's) and `boot-baud = 62500 | 125000` (default
    62500) in `RMFE.CFG` (`docs/sd-card.md`, `core/link.h`), unit-tested in `test_link`, `test_config` and
    `test_idc`. The send side now starts a byte only with the transmitter idle and DTR asserted,
    as the IDC's SIO does; before, the byte waiting in the FIFO could go out after DTR dropped (it wasn't the cause
    here, but it was one byte beyond the IDC). The console's `k` key prints the link's last exchanges: bytes each
    way, bad parity, waits for DTR and how each ended, which is what found the problem.
  - The FT232R harness is gone (the link pins go through the transceivers), so level 3 (`run_hil.py`,
    `run_hil_sd.py`) and MAME in the loop (`run_mame_linked.py`) can't run on this set-up; the real 480Z stands in.
    Level 2, the MAME comparison from recorded traces, runs on the PC as before.
  - **Next:** the rest of `docs/acceptance.md` at 62500; then the board.

## Decided (2026-11-20)

- **Images live on an SD card**, IMD and HFE. They are chosen per drive (two drives: A:/C: and B:/D:, a CP/M drive per side) on the 2.8" TFT with
  a rotary encoder. **Both formats are writable, safely.** See `docs/hardware.md`.

## Decisions still to make

- **A clean-room process for milestone 8.** The block's specification should be written by one person or agent, and
  the code by another who has never seen the RM block.
