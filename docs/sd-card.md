# The SD card

The images, the IDC ROM and the settings live on a micro-SD card, formatted FAT32 or exFAT. The arrangement follows
[FlashFloppy](https://github.com/keirf/flashfloppy/wiki)'s, by Keir Fraser. `RMFE.CFG` is our `FF.CFG`, and
`IMAGE_A.CFG` and `INIT_A.CFG` are its files of the same names. Option names are FlashFloppy's wherever it has the
same option. There's only one system to serve, so there are far fewer options. None of FlashFloppy's code is used.

## What goes on the card

| File | What it is |
|---|---|
| `*.IMD`, `*.HFE` | disc images, in the root or in folders |
| `IDC.ROM` | your IDC ROM (8 KB): the boot block the 480Z loads comes from it. Another name can be set with `idc-rom` |
| `RMFE.CFG` | settings (optional) |
| `IMAGE_A.CFG`, `IMAGE_B.CFG` | the image last chosen for each drive (optional) |
| `INIT_A.CFG`, `INIT_B.CFG` | a fixed image for each drive, for `image-on-startup = static` (optional) |

`RMFE.CFG` and the `IMAGE`/`INIT` files go in the root, or in an `RMFE/` folder if there is one, the way FlashFloppy
uses `FF/`. Other files on the card are left alone.

If `idc-rom` doesn't name a ROM that contains a boot block, the first `*.ROM` in the root (by name) that does is
used. With no ROM at all, the 480Z can't load the boot block and won't boot, but the drives still work.

## Which images are in the drives

The unit has two drives, A and B. On the 480Z each side of a disc is a CP/M drive of its own: the image in drive A
is CP/M's **A:** (its first side) and **C:** (its second), and the image in drive B is **B:** and **D:**. A
single-sided image's second side reads as an unformatted disc. Booting with the ROS's `X` command exchanges the
names, so that drive B holds A: and C:.

At start-up, `image-on-startup` decides:

| Value | Drive A | Drive B |
|---|---|---|
| `last` (default) | the image named in `IMAGE_A.CFG` | the image named in `IMAGE_B.CFG` |
| `static` | the image named in `INIT_A.CFG` | the image named in `INIT_B.CFG` |
| `init` | the first image in the root | empty |

If the file is missing, names no image, or names one that isn't on the card, the drive falls back to `init`. The
same image can't go in both drives.

An `IMAGE` or `INIT` file holds one line: the image's path from the root of the card, such as
`480Z_RML_system-cpm2.2f-basic5.3g-basic6.3b.imd` or `games/Dragon World.IMD`. Blank lines and lines starting
with `#` are skipped. Until the display and knob arrive, you edit these files on a PC. The display will then write
`IMAGE_A.CFG` and `IMAGE_B.CFG` itself, as FlashFloppy does.

The image list, used by `init` and later by the display, follows FlashFloppy's native mode. It shows `.imd` and
`.hfe` files, sorted by name ignoring case, and leaves out names starting with `.` (such as macOS's `._` files) and
hidden files.

## RMFE.CFG

One setting per line, `key = value`. Blank lines and lines starting with `#` are ignored. Keys and keywords can be in
any case, and a value can be put in double quotes. A line that can't be read is reported on the debug console with
its line number, and that setting keeps its default.

```
# RMFE.CFG: the defaults
# image-on-startup: last | static | init
image-on-startup = last
# write-protect: yes | no
write-protect = no
# drive-a, drive-b: auto | 40 | 80
drive-a = auto
drive-b = auto
idc-rom = IDC.ROM
```

A `#` starts a comment only at the beginning of a line, because a path may contain one.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `image-on-startup` | `last`, `static`, `init` | `last` | above |
| `write-protect` | `yes`, `no` | `no` | `yes`: every image is write-protected. `no`: an image is write-protected if it has the FAT read-only attribute (as FlashFloppy) |
| `drive-a`, `drive-b` | `auto`, `40`, `80` | `auto` | the drive's type, which a real IDC took from links on its board. `auto`: 80-track for an image with more than 44 cylinders |
| `idc-rom` | a path | `IDC.ROM` | the IDC ROM image |

## Writes

Writes go to the image on the card as the 480Z makes them, and are on the card before the 480Z is told they worked.

- **HFE:** the track is re-encoded and written in place.
- **IMD:** in place when it fits. If a sector's record has to change length (a compressed sector given varied data,
  or a track reformatted to a different layout), the whole image is written to `<image>.new` and renamed over the
  old one. That takes about a second, so the new image is written **uncompressed**: every sector stored in full.
  After that, writes go in place, and so does reformatting a track to the layout it already has, as RM's FORMAT
  does. The file grows by about a fifth, and it is still a standard IMD. An image that is only ever read is
  never changed. The old image
  goes only once the new one is complete. If the power fails in the gap between deleting the old and renaming the
  new, the next start finishes the rename; a `.new` left beside an intact image is an unfinished one and is
  deleted. Both are reported on the console.

Don't take the card out while the 480Z is writing. Changing the card while the unit is running isn't supported yet.
