# 480Z acceptance checklist (level 4)

Run this by hand on a real 480Z before a release, and record the result at the bottom. Use **copies** of disk images.
A failed item blocks the release until it is fixed, or the owner explicitly accepts it and records it here.

## Setup

- [ ] Firmware git hash and IDC ROM revision (IDC3 or IDC5) recorded
- [ ] 480Z ROS version (sign-on message) recorded
- [ ] Device connected to the SIO/4 port with the production cable; the real MD1 disconnected
- [ ] SD card set up as `docs/sd-card.md` says: its `RMFE.CFG`, the images in A and B, and the IDC ROM recorded

## Boot and basic operation

- [ ] Power-on order: device first, then 480Z. `B` boots CP/M to `A>`
- [ ] Power-on order: 480Z first, then device. `B` boots CP/M to `A>`
- [ ] `X` (boot from another drive) boots from drive B
- [ ] `DIR` on A:, B:, C: and D: lists the expected files (C: and D: are the second sides of the images in drives A
      and B)
- [ ] `TYPE` a text file of more than 16 KB (several extents); the contents are correct

## Writing

- [x] `PIP` a file from A: to B:, then compare the copy with the original using a checksum utility (2026-11-23:
      the whole system disc to a blank image, `PIP B:=A:*.*[V]`, PIP's own verify; same files in `DIR B:`)
- [ ] `ERA` a file; the directory updates, and the space is reused by the next write
- [ ] Fill a disk until "disk full"; the image is still consistent afterwards
- [ ] Power off the device during a write. On restart the image is either unchanged or holds the new data (never a
      half-written sector), or the failure mode is documented
- [ ] Changes survive a device power cycle, and the image read back on a PC matches
- [ ] An image with the FAT read-only attribute (or `write-protect = yes`) gives the 480Z a write-protect error,
      and is unchanged afterwards

## Formats and media

- [ ] Every disk format we support boots or reads (SD, DD, QD; single and double sided), one image of each
- [ ] RM's FORMAT and VERIFY on an IMD and on an HFE in drive B; the formatted image reads back on a PC
      (2026-11-23: the IMD half passes, on a blank IMD from `tools/imd/make_blank.py`, at 125000; the HFE and the
      read-back on a PC are still to do)
- [x] Changing disk images while the 480Z is at `A>` behaves as a disk change does (`^C` logs in the new disk)
      (2026-11-23)
- [ ] No image in a drive gives the same error on the 480Z as an empty real drive

## Robustness

- [ ] 480Z reset in the middle of a transfer: the next command works, with no device reset needed
- [ ] A long session (copy a whole disk three times) completes without errors or retries
- [x] Software that talks to the drive directly (list any found, for example FORMAT and disk utilities) works
      (2026-11-23: RM's FORMAT and VERIFY; the 4MATION loaders, `0E` sector reads at 125000: Flowers of Crystal
      from the repaired IMD with both sector 08s, Dragon World from its IMD and from the HFE rebuilt from its flux
      capture)

## Results

| Date | Firmware | IDC ROM | ROS | Tester | Result / notes |
|---|---|---|---|---|---|
| 2026-11-22 | 7f6f741 | IDC3.1 | 1.2 | owner | First run, not a full pass. Breadboard: two MAX3232 modules and RM's own lead. At 125000 the ROS's boot block fails ("Disc Error"). With `fast-baud = 62500`: `B` boots CP/M 2.2F to `A>`, and 4MATION's Flowers of Crystal boots |
| 2026-11-23 | 998e1f2 (defaults: `boot-baud` 62500, `fast-baud` 125000) | IDC3.1 | 1.2 | owner | The ROS's boot block at 62500 and everything after it at 125000: `B` boots CP/M 2.2F to `A>`, and `DIR` and `TYPE` run at 125000 (with a test build of the same rule). Modules on 3V3: `PIP B:=A:*.*[V]` to a blank image, no errors; swapping images from the knob at `A>`; FORMAT and VERIFY of drive B (an IMD); 4MATION's Flowers of Crystal and Dragon World boot, Dragon World also from its rebuilt HFE; all at 125000 with no parity errors on the link. The rest of the checklist is still to do |
