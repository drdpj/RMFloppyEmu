# RMFloppyEmu

A Raspberry Pi Pico 2 (RP2350) replacement for the MD1 disk unit of the Research Machines 480Z. The MD1 talks to
the 480Z over a serial link, so this emulates the unit at the protocol level, serving disc images (IMD and HFE)
from an SD card. There is no flux or drive-timing emulation.

## Status

- **Done:** the protocol, decoded from the IDC ROM and MAME traces (`docs/protocol.md`). The command layer, in C,
  says byte for byte what the real IDC firmware says in MAME, for CP/M and RM's utilities (`docs/testing.md`).
- **Working on development hardware:** the firmware serves the link on a UART at the 480Z's 9600/125000 baud with
  its handshake lines, and the two drives from IMD and HFE images on an SD card, configured the way
  [FlashFloppy](https://github.com/keirf/flashfloppy) does it (`docs/sd-card.md`). A 2.8" TFT and a rotary encoder
  choose each drive's image, show each drive's activity and, optionally, each command. The card can be changed while
  it runs. Tested through a USB serial adapter playing the 480Z, and with MAME's emulated 480Z, running CP/M and
  RM's utilities, using the real unit as its disc unit.
- **To come:** the RS-232 transceivers and cable (`docs/hardware.md`); trial on a real 480Z (`docs/acceptance.md`);
  a clean-room boot block, so the IDC ROM becomes optional (`docs/plan.md`).

## What you need

- A Pico 2, a micro-SD card breakout (SPI), a 2.8" SPI TFT (ILI9341), an EC11 rotary encoder with a push switch, two
  MAX3232 RS-232 modules (or one MAX3243), a plug for the 480Z's SIO/4 socket, and a 5 V supply. Wiring, pin plan and
  parts are in `docs/hardware.md`.
- An IDC ROM image from your own disk unit, on the SD card: the 480Z loads its boot block from it. Use **IDC 3.1**
  (`IDC3-1I`), from the double-drive units: this is a double-drive unit, and 3.1 is what MAME's MQ2 runs and what
  the project is tested against. IDC 5.1 (`IDC5-1J`), from the single-drive units, is also recognised. ROM images
  and disc images are RM copyright and are not distributed with this project.
- Blank discs: `tools/imd/make_blank.py` writes images in the layout RM's FORMAT lays down, so formatting them is
  fast (`docs/sd-card.md`).

## Building

Requires the Pico SDK 2.x (`PICO_SDK_PATH` set, or `-DPICO_SDK_PATH=…`), `arm-none-eabi-gcc`, CMake and Ninja. The
board is the Pico 2 (`PICO_BOARD=pico2`, the default here).

```
cmake -S . -B build -G Ninja
ninja -C build                    # build/rmfloppyemu.uf2
```

## Testing

Four levels (`docs/testing.md`): unit tests on the PC, and on the Pico 2 itself; a byte-for-byte comparison with
MAME running the real IDC firmware; hardware-in-the-loop tests through a USB serial adapter, including MAME's 480Z
using the real unit; and a checklist for a real 480Z. On the bench, flash and restart the Pico 2 as
`tests/pico/run_on_target.py` does, never with OpenOCD's own `reset` (`docs/testing.md`, "Resetting the Pico 2").

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
- [tools/mame/](tools/mame/): MAME scripts for tracing the link, and the bridge connecting MAME's 480Z to the
  real unit
- [tools/imd/](tools/imd/): blank RM-format images, and repairing protected ones; [tools/hfedump/](tools/hfedump/) and
  [tools/scpdump/](tools/scpdump/): what `src/flux` decodes from HFE and SCP images; [tools/font/](tools/font/): the
  display's font

## Licence

GPL-3.0-or-later; see [LICENSE](LICENSE). Third-party code, and what to pass on with a built firmware, is in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
