# 480Z acceptance checklist (level 4)

Run this by hand on a real 480Z before a release, and record the result at the bottom. Use **copies** of disk images.
A failed item blocks the release until it is fixed, or the owner explicitly accepts it and records it here.

## Setup

- [x] Firmware git hash and IDC ROM revision (IDC3 or IDC5) recorded (2026-11-24: 998e1f2, IDC3.1; in the results)
- [x] 480Z ROS version (sign-on message) recorded (2026-11-24: "RML 80 Character LINK 480Z V 1.2D")
- [ ] Device connected to the SIO/4 port with the production cable; the real MD1 disconnected (2026-11-24: not
      yet. The runs so far used a breadboard, two MAX3232 modules on 3V3, and RM's own MD1 lead, with the MD1
      disconnected. Tick this with the production board and cable)
- [x] SD card set up as `docs/sd-card.md` says: its `RMFE.CFG`, the images in A and B, and the IDC ROM recorded
      (2026-11-24: exFAT. `RMFE.CFG` holds the defaults but `show-commands = yes`, so `fast-baud` 125000 and
      `boot-baud` 62500. `IDC.ROM` is IDC 3.1I. Drive A: RM's CP/M 2.2F system disc. Drive B: the image each test
      names, made with `tools/imd/make_blank.py` and cpmdw, or the 4MATION discs)

## Boot and basic operation

- [x] Power-on order: device first, then 480Z. `B` boots CP/M to `A>` (2026-11-23)
- [x] Power-on order: 480Z first, then device. `B` boots CP/M to `A>` (2026-11-23)
- [x] `X` (boot from another drive) boots from drive B (2026-11-23: `05` with parameter `01`, the drives swapped)
- [x] `DIR` on A:, B:, C: and D: lists the expected files (C: and D: are the second sides of the images in drives A
      and B) (2026-11-23)
- [x] `TYPE` a text file of more than 16 KB (several extents); the contents are correct (2026-11-23: a 24 KB file of
      300 numbered, checksummed lines, two extents, made with cpmdw)

## Writing

- [x] `PIP` a file from A: to B:, then compare the copy with the original using a checksum utility (2026-11-23:
      the whole system disc to a blank image, `PIP B:=A:*.*[V]`, PIP's own verify; same files in `DIR B:`)
- [x] `ERA` a file; the directory updates, and the space is reused by the next write (2026-11-24: a 24 KB file
      erased from a full disc, then a 24 KB PIP copy into its space; read back on a PC byte for byte)
- [x] Fill a disk until "disk full"; the image is still consistent afterwards (2026-11-24: PIP copies until CP/M's
      "DISK WRITE ERROR =B:BIG.TXT". On a PC the filesystem checks clean, every complete copy is identical, and PIP's
      unfinished `.$$$` file is an exact prefix of the original)
- [x] Power off the device during a write. On restart the image is either unchanged or holds the new data (never a
      half-written sector), or the failure mode is documented (2026-11-24: power removed during `PIP B:=A:*.*`.
      On a PC the filesystem checks clean, the finished files are identical, and each of the 107 sectors the 480Z
      changed holds only `E5` and whole records of the files written: no half-written sector. The image holds
      what CP/M had committed, as a real disc would)
- [x] Changes survive a device power cycle, and the image read back on a PC matches (2026-11-24: a 24 KB PIP copy,
      then the unit's power removed and restored; the 480Z read the file back, and so did a PC, byte for byte)
- [x] An image with the FAT read-only attribute (or `write-protect = yes`) gives the 480Z a write-protect error,
      and is unchanged afterwards (2026-11-24: the attribute set on a PC; "Disc write protected"; the image byte
      for byte unchanged)

## Formats and media

- [x] Every disk format we support boots or reads (SD, DD, QD; single and double sided), one image of each
      (2026-11-24, at 125000: single, double and quad density, each one- and two-sided, six images. On each side:
      `DIR`, `STAT`, `TYPE` of a 24 KB file, and a PIP copy with verify, which read back on a PC byte for byte.
      The DD and QD images were made with `tools/imd/make_blank.py` and cpmdw; the SD ones with cpmdw, whose IMDs
      were wrongly MFM and were corrected to FM by hand)
- [x] RM's FORMAT and VERIFY on an IMD and on an HFE in drive B; the formatted image reads back on a PC
      (2026-11-23, at 125000. On a PC: the IMD reads as an empty RM double-density disc in cpmdw; the HFE, decoded
      by `tools/hfedump`, has RM's layout on all 80 tracks, no CRC errors, every sector `E5` but FORMAT's boot
      sector on track 0 of each side)
- [x] Changing disk images while the 480Z is at `A>` behaves as a disk change does (`^C` logs in the new disk, as
      RM's documentation says) (2026-11-23; PIP also wrote to a newly chosen image without a `^C`)
- [x] No image in a drive gives the same error on the 480Z as an empty real drive (2026-11-24: `(eject)`, then
      `DIR B:` gives "Disc Err on B: Select", the usual message for an empty drive)

## Settings and USB storage

- [x] A long press at home opens the settings; each option's value matches `RMFE.CFG`; a change made with the knob
      is in `RMFE.CFG` afterwards, with the file's comments and other lines as they were (2026-11-26, on the bench
      through the console: `usb-storage = yes` added at the end, the rest unchanged, read back over USB)
- [x] `write-protect` turned on in the settings: `SAVE` to A: gives CP/M's write-protect error at once; turned off,
      it works again (2026-11-26)
- [x] With `usb-storage = yes`, plugging into a PC: the card appears there as a drive, the display says "On USB",
      and `DIR` on the 480Z gives the empty-drive error (2026-11-26: mounted on Linux; on the 480Z, a boot error
      and a disc error on `DIR`)
- [x] An image copied onto the card from the PC, then the card ejected: the unit has the card back within a few
      seconds, and the new image can be chosen and read on the 480Z (2026-11-26)
- [x] The USB lead pulled out without ejecting: the unit has the card back, and it still mounts on a PC afterwards
      (2026-11-26: the display went back to both drives at once, and the card mounted cleanly on the PC again)
- [x] `usb-storage` turned off in the settings while the PC has the card: the PC loses it, the unit has it back,
      and `RMFE.CFG` says `usb-storage = no` (2026-11-26, on the bench: the drive went from the PC, the unit
      re-read the card and put both images back)
- [x] A USB charger on the socket: the unit keeps the card (2026-11-26)

## Robustness

- [x] 480Z reset in the middle of a transfer: the next command works, with no device reset needed (2026-11-24: RESET
      during a PIP copy; `B` booted and `DIR` worked at once)
- [x] A long session (copy a whole disk three times) completes without errors or retries (2026-11-24: three
      `PIP B:=A:*.*[V]` copies, `ERA` between, at 125000: no errors, no parity errors on the link, about 500 KB
      written and 1.2 MB read; the last copy read back on a PC, all 25 files identical)
- [x] Software that talks to the drive directly (list any found, for example FORMAT and disk utilities) works
      (2026-11-23: RM's FORMAT and VERIFY; the 4MATION loaders, `0E` sector reads at 125000: Flowers of Crystal
      from the repaired IMD with both sector 08s, Dragon World from its IMD and from the HFE rebuilt from its flux
      capture)

## Results

| Date | Firmware | IDC ROM | ROS | Tester | Result / notes |
|---|---|---|---|---|---|
| 2026-11-22 | 7f6f741 | IDC3.1 | 1.2 | owner | First run, not a full pass. Breadboard: two MAX3232 modules and RM's own lead. At 125000 the ROS's boot block fails ("Disc Error"). With `fast-baud = 62500`: `B` boots CP/M 2.2F to `A>`, and 4MATION's Flowers of Crystal boots |
| 2026-11-23 | 998e1f2 (defaults: `boot-baud` 62500, `fast-baud` 125000) | IDC3.1 | 1.2 | owner | The ROS's boot block at 62500 and everything after it at 125000: `B` boots CP/M 2.2F to `A>`, and `DIR` and `TYPE` run at 125000 (with a test build of the same rule). Modules on 3V3: `PIP B:=A:*.*[V]` to a blank image, no errors; swapping images from the knob at `A>`; FORMAT and VERIFY of drive B (an IMD); 4MATION's Flowers of Crystal and Dragon World boot, Dragon World also from its rebuilt HFE; FORMAT and VERIFY of an HFE in B, and both formatted images read back on a PC; a 24 KB PIP copy read back on a PC byte for byte; `X` boots from B; both power-on orders boot; `TYPE` of a 24 KB file; single, double and quad density, one- and two-sided, read and written (2026-11-24); `DIR` on A:, B:, C: and D:; all at 125000 with no parity errors on the link. The rest of the checklist is still to do |
| 2026-11-24 | 998e1f2 (later commits are docs only; defaults: `boot-baud` 62500, `fast-baud` 125000) | IDC3.1 | 1.2 | owner | Every format (SD, DD, QD; one and two sides), `ERA` and a full disc, write protection, an empty drive, a power cycle and power off during a write, a 480Z reset in mid-transfer, and a long session: all pass, checked on a PC where the card can show it. ROS sign-on "V 1.2D". Every item passes but the production cable, still to come (the breadboard and RM's own lead were used) |
| 2026-11-24 | 30b331c: the boot block paced (defaults: `boot-baud` and `fast-baud` 125000) | IDC3.1 | 1.2D | owner | The ROS's boot block at 125000, two bytes per DTR pulse (1049 bytes, 525 pulses, 0.93 s), then `B` boots CP/M 2.2F to `A>`, all at 125000 with no parity errors (`docs/protocol.md`, "Framing and baud rate"). Unpaced, the same firmware lost a byte at 125000 ("Disc Error") |
