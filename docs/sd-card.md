# The SD card

The images, the IDC ROM and the settings live on a micro-SD card, formatted FAT32 or exFAT. The arrangement follows
[FlashFloppy](https://github.com/keirf/flashfloppy/wiki)'s, by Keir Fraser. `RMFE.CFG` is our `FF.CFG`, and
`IMAGE_A.CFG` and `INIT_A.CFG` are its files of the same names. Option names are FlashFloppy's wherever it has the
same option. There's only one system to serve, so there are far fewer options. None of FlashFloppy's code is used.

## What goes on the card

| File | What it is |
|---|---|
| `*.IMD`, `*.HFE` | disc images, in the root or in folders |
| `IDC.ROM` | your IDC ROM (8 KB): the boot block the 480Z loads comes from it. Use IDC 3.1, the double-drive units' ROM (5.1, the single-drive units', also works). Another name can be set with `idc-rom` |
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
with `#` are skipped. The menu writes `IMAGE_A.CFG` and `IMAGE_B.CFG` whenever an image is chosen, as FlashFloppy
does; emptying a drive leaves its file empty. `INIT` files are only ever edited on a PC.

The image list, used by `init` and by the menu, follows FlashFloppy's native mode. It shows `.imd` and `.hfe`
files, sorted by name ignoring case, and leaves out names starting with `.` (such as macOS's `._` files) and
hidden files.

## Choosing an image

The display shows what each drive holds, with a light under each: "reading" (green) or "writing" (orange) and
the track, while the 480Z uses that drive and for a moment after, like a real drive's light. With
`show-commands = yes` it also shows each command as the unit serves it: its byte and name, the drive and side,
the track, and the record (`R`), sector ID (`S`) or sector count (`N`), with a count of the commands so far, such
as `02 read  A/0 T12 R5  #1234`. The knob picks a drive (turn) and opens a list of images for it (press),
starting in the folder of the image it holds. The cursor is hidden until the knob is used: the first turn or press
only shows it, on the drive it was last on, and it hides again after 10 seconds without the knob. In the list:

- turn to move, and press to open a folder or choose an image;
- `../` goes up a folder, and `(eject)` empties the drive;
- `*` marks the image in this drive, and `A` or `B` the image in the other drive, which can't be chosen;
- a long press (about a second), or half a minute without touching the knob, goes back.

Folders come first, then images, each sorted by name ignoring case (FlashFloppy's `sort-priority = folders`), and
up to 512 entries a folder are shown. A name too long for the screen ends in "…"; when the cursor comes to it, it
scrolls once to its end and back, then stays.

**On an OLED** (`display-type`, below; wiring in `docs/hardware.md`) the same menu is smaller. On a 128 x 64 panel:
"A:" with a dot over the image's path, and the same for B, then a status line at the bottom. On a 128 x 32: a row
for each drive, "A", its dot and the path, and the status line under them. A drive's dot fills while the 480Z uses
it, as the light. The status line says what is happening: each drive's light (`A read T12  B idle`), or with
`show-commands = yes` the last command (`02 read A/0 T12 R5`), a missing card or ROM, or a message; while choosing,
the folder and where the cursor is (`/GAMES  3 of 12`). The list shows three entries at a time (two on a 128 x 32),
and the cursor is shown in inverse.

**What the 480Z sees.** A new image goes in at once, between two of the 480Z's commands, never during one. The next
command finds the new disc, as if it had been changed in an instant; an emptied drive answers "not ready" (`80`),
as with no disc in it. Anything the 480Z had written that was still in the unit's cache for that drive goes to the
old image first. The protocol has no way to tell the 480Z the disc has changed, so do as RM's documentation says
for changing a disc: type ^C (a warm boot) at the prompt afterwards, which logs the new disc in. On a real 480Z with
RM's CP/M 2.2F, PIP did write to a newly chosen image without one (2026-11-23), but ^C is the documented, safe way.
A ^C reloads the command processor from the boot drive, so keep a system disc there. While the unit opens the new
image, the old one carries on serving the 480Z.

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
# rotary: full | half | quarter, optionally followed by ,reverse
rotary = full
# show-commands: yes | no
show-commands = no
# fast-baud: 125000 | 62500
fast-baud = 125000
# boot-baud: 125000 | 62500
boot-baud = 125000
# display-type: auto | tft | oled-128x64 | oled-128x32, an OLED optionally followed by -rotate
display-type = auto
# usb-storage: yes | no
usb-storage = no
```

A `#` starts a comment only at the beginning of a line, because a path may contain one.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `image-on-startup` | `last`, `static`, `init` | `last` | above |
| `write-protect` | `yes`, `no` | `no` | `yes`: every image is write-protected. `no`: an image is write-protected if it has the FAT read-only attribute (as FlashFloppy) |
| `drive-a`, `drive-b` | `auto`, `40`, `80` | `auto` | the drive's type, which a real IDC finds by stepping the head when a disc is logged in. `auto`: 80-track for an image of 43 cylinders or more |
| `idc-rom` | a path | `IDC.ROM` | the IDC ROM image |
| `show-commands` | `yes`, `no` | `no` | `yes`: the display also shows each command as the unit serves it (above) |
| `fast-baud` | `125000`, `62500` | `125000` | the rate the link goes fast to. `125000` is the IDC's. The 480Z takes the rate from the unit's reply to `00` (`docs/protocol.md`, "Framing and baud rate"), so `62500` works too, at about half the transfer speed, for an RS-232 interface that can't manage 125000. The level-3 and MAME-in-the-loop tests need `125000` |
| `boot-baud` | `125000`, `62500` | `125000` | the same, only for the 480Z's ROS loading the boot block (the `00` before `FF`, which the unit tells by its parity: `docs/protocol.md`). `125000` is the IDC's, and MAME in the loop needs it. The block goes two bytes per DTR pulse, as the ROS takes it, so it takes about 0.9 s at either rate. `62500` is a fallback: before the unit paced the block, a real 480Z's ROS lost a byte of it at 125000 |
| `display-type` | `auto`, `tft`, `oled-128x64`, `oled-128x32`; an OLED type optionally followed by `-rotate` | `auto` | the display, as FlashFloppy's option (which also has LCDs and more suffixes). `auto`: an OLED if one answers on I2C (at `0x3C` or `0x3D`), taken as 128 x 64, otherwise the TFT; FlashFloppy's `auto` takes an OLED as 128 x 32, so a 128 x 32 panel needs `oled-128x32`. `-rotate` turns an OLED's picture round 180 degrees. If an OLED is asked for and none answers, the TFT is used |
| `rotary` | `full`, `half`, `quarter`, each optionally followed by `,reverse` | `full` | the knob, as FlashFloppy's option: one step per detent of an encoder with as many detents as pulses (an EC11; `full`), or per half or quarter of a pulse. `,reverse` turns it round, for an encoder that steps the wrong way |
| `usb-storage` | `yes`, `no` | `no` | ours; FlashFloppy has none. `yes`: a PC on the Pico's USB socket gets the card as a USB drive (below). Off by default, because a PC that powers the unit would otherwise take the card at every start |

## Settings

A long press of the knob at home opens the settings: the options above that are a choice from a list, with their
values. Not `idc-rom`, which is a path, or `display-type`, since the menu is on the display. Turn to pick one (the
TFT shows its key in `RMFE.CFG` under the title; an OLED, on its status line), press to change it, turn to step
through its values, and press again to save the one shown; a long press leaves it as it was. Another long press,
or half a minute without the knob, goes back home.

| Menu | Key |
|---|---|
| USB storage | `usb-storage` |
| Write-prot | `write-protect` |
| Drive A, Drive B | `drive-a`, `drive-b` |
| Startup | `image-on-startup` |
| Show cmds | `show-commands` |
| Fast baud, Boot baud | `fast-baud`, `boot-baud` |
| Knob | `rotary` (`full rev` is `full,reverse`) |

A change takes effect at once, and is written into `RMFE.CFG` (in `RMFE/` if that folder exists), the file made if
there is none. Only that option's line changes, to `key = value`; comments, blank lines and the other options stay
as they were, and an option that wasn't in the file is added at its end. When the card can't be written (or
`RMFE.CFG` is over 2 KB, more than the unit reads), the change still holds until the next start and the display
says it wasn't saved. When each takes effect:

- `write-protect`: on the images in the drives at once. Anything the 480Z had written that was still in the unit's
  cache goes to the image first.
- `drive-a`, `drive-b`: at once, as if the disc had been changed (what the 480Z had written goes to the image
  first). CP/M learns the drive's type when it logs the disc in, so type ^C afterwards.
- `fast-baud`, `boot-baud`: at the 480Z's next `00`, which it sends when it resets or boots.
- `image-on-startup`: at the next start, or when a card goes back in.
- `show-commands`, `rotary`, `usb-storage`: at once.

## USB storage

With `usb-storage = yes`, plugging the Pico 2's USB socket into a PC gives the PC the card, as a USB card reader
would: it appears as a drive, to copy images on and off or edit the settings, without taking it out. A PC and the
unit can't both use a card at once (the PC caches what it reads), so while the PC has it:

- both drives are empty: the 480Z sees no disc (`80`), as when the card is out. Anything the 480Z had written goes
  to the card first. Don't plug in a PC while the 480Z is writing;
- the display says "On USB" (on the TFT, "On USB: eject it on the PC to use it"), no image can be chosen, and only
  `usb-storage` can be changed in the settings.

The unit has the card back when the PC **ejects** it (Eject or Safely Remove; on Linux, the file manager's
eject button, which works without root where `eject` doesn't), when the USB lead comes out, or when `usb-storage` is turned off in the settings. It then
reads the card again as when one is put back: the settings, the ROM and the images. After an eject the PC doesn't
get the card again until it is unplugged and plugged in again, or `usb-storage` is turned off and on again.

Eject before unplugging, as with any USB drive: the PC may not have written everything until then. Turning
`usb-storage` off takes the card from the PC at once, as unplugging it would.

A charger or power bank on the USB socket never asks for the card (it doesn't set the device up as a PC does), so
the unit keeps it. A PC that powers the unit takes it at every start, until ejected; turn `usb-storage` off if the
unit lives on a PC's USB port.

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

**Blank discs.** Format onto an image already laid out as RM's FORMAT lays it out, made with
`tools/imd/make_blank.py` (40 or 80 cylinders, one or two sides): every track then formats in place. Measured with
drive B, MFM tracks (2026-11-21):

| Image | Format a track | Write a record, then `08` | Read a new track |
|---|---|---|---|
| blank IMD from `make_blank.py` | 0.06 s | 82 + 671 ms the first time (the one rewrite), then as below | 32 ms |
| IMD after its rewrite (uncompressed) | 0.12 s | 81 + 137 ms | 32 ms |
| HFE | 0.29 s | 173 + 188 ms | 124 ms |

An empty IMD, or one in another layout, makes the first format of each missing or different track rewrite the whole
image: a second or more each time. HFE never needs a rewrite, but every write and every track change costs more.

Don't take the card out while the 480Z is writing. The card can be taken out and put back while the unit runs. Within about two seconds both drives empty (anything
the 480Z had written that was still in the unit's cache is lost, and the console says so) and the display says "No
SD card"; when a card goes back in, its settings, the ROM and the images are read again as at start-up.
