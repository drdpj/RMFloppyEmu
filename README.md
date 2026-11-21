# RMFloppyEmu

A Raspberry Pi Pico 2 (RP2350) replacement for the MD1 disk unit of the Research Machines 480Z. The MD1 talks to
the 480Z over a serial link, so this emulates the unit at the protocol level, serving disc images (IMD and HFE)
from an SD card. There is no flux or drive-timing emulation.

## Status

- **Done:** the protocol, decoded from the IDC ROM and MAME traces (`docs/protocol.md`). The command layer, in C,
  says byte for byte what the real IDC firmware says in MAME, for CP/M and RM's utilities (`docs/testing.md`).
- **Working on development hardware:** the firmware serves the link on a UART at the 480Z's 9600/125000 baud with its
  handshake lines. It serves the drives from images on an SD card, configured the way
  [FlashFloppy](https://github.com/keirf/flashfloppy) does it (`docs/sd-card.md`). Hardware-in-the-loop tests
  play the 480Z through a USB serial adapter.
- **To come:** the display and rotary encoder; MAME's emulated 480Z connected to the real unit; the RS-232
  interface and cable; trial on a real 480Z (`docs/acceptance.md`); a clean-room boot block (`docs/plan.md`).

## What you need

- A Pico 2, an SD card adapter, and (later) a MAX3243 RS-232 transceiver and a 2.8" ILI9341 TFT (`docs/hardware.md`).
- An IDC ROM image from your own disk unit, on the SD card: the 480Z loads its boot block from it. ROM images and
  disc images are RM copyright and are not distributed with this project.

## Building

Requires the Pico SDK 2.x (`PICO_SDK_PATH` set), `arm-none-eabi-gcc`, CMake and Ninja.

```
cmake -S . -B build -G Ninja
ninja -C build                    # build/rmfloppyemu.uf2
```

## Testing

Four levels (`docs/testing.md`): unit tests on the PC, and on the Pico 2 itself; a byte-for-byte comparison with
MAME running the real IDC firmware; hardware-in-the-loop tests through a USB serial adapter; and a checklist for a
real 480Z.

```
cmake -S tests -B build-tests -G Ninja
ninja -C build-tests && ctest --test-dir build-tests --output-on-failure
```

The real-data and MAME tests need your own ROMs and images in `testdata/` (`docs/testing.md` section 5), and fail
without them.

## Documentation

- [docs/plan.md](docs/plan.md): milestones and progress
- [docs/protocol.md](docs/protocol.md): the 480Z ↔ disk unit serial protocol, with the evidence for each fact
- [docs/sd-card.md](docs/sd-card.md): the SD card's files and settings
- [docs/hardware.md](docs/hardware.md): parts, pin plan and the RS-232 link to the 480Z
- [docs/coding.md](docs/coding.md) and [docs/testing.md](docs/testing.md): how the code is written and tested
- [docs/acceptance.md](docs/acceptance.md): the release checklist for a real 480Z
- [docs/disk-survey.md](docs/disk-survey.md): what the disc images on hand contain, and how to capture more
- [tools/mame/](tools/mame/): MAME scripts for tracing the link in emulation

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party code, and what to pass on with a built firmware, is in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
