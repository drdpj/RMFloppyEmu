# Third-party code

RMFloppyEmu is GPL-3.0-or-later (`LICENSE`). It includes, or is built with, the following. Every one of these
licences allows that; Apache-2.0 code may be combined into a GPL-3.0 work.

| Component | Version | Licence | Where | Used for |
|---|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) | v2.7.0 (`b6763fbd`) | MIT, see `third_party/unity/LICENSE.txt` | `third_party/unity/` | the unit tests only, on the PC and on the Pico 2; not in the device firmware |
| [no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico) (Carl John Kugler III) | v3.6.2 (`6c644cfc`) | Apache-2.0, see `third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/LICENSE` | `third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/` | firmware: the SD card (SPI) and its filesystem |
| ↳ FatFs R0.15 (ChaN), inside it | R0.15 | FatFs licence (BSD-style, one clause), see `src/ff15/LICENSE.txt` there | `…/src/ff15/` | FAT32 and exFAT |
| ↳ SDIO driver, inside it, derived from ZuluSCSI / SdFat / Mbed OS | as shipped | GPL-3.0-or-later (ZuluSCSI), MIT (SdFat), Apache-2.0 (Mbed OS); licences in `src/ZuluSCSI-firmware/`, `src/SdFat/`, `src/mbed-os/` there | `…/src/sd_driver/SDIO/` | compiled into the firmware, not used (the card is on SPI) |
| FatFs configuration, derived from the library's `src/include/ffconf.h` | as above | as FatFs (the file says so, and marks our one change) | `src/pico/fatfs/ffconf.h` | firmware |
| [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | 2.3.1 | BSD-3-Clause, see `third_party/pico-sdk/LICENSE.TXT` | outside the repository (`PICO_SDK_PATH`), except `pico_sdk_import.cmake`, a copy of the SDK's that keeps its notice | firmware and the on-target tests |

**Ideas, not code.** The SD card's configuration files follow [FlashFloppy](https://github.com/keirf/flashfloppy)'s
(`FF.CFG`, `IMAGE_A.CFG`, `INIT_A.CFG`, and its option names; `docs/sd-card.md`). None of its code is used. The
MAME tooling in `tools/mame/` and `tests/mame/` drives a separately built MAME (GPL-2.0-or-later) and contains no
MAME code.

## Keeping them up to date

Only `unity.c`, `unity.h` and `unity_internals.h` are vendored, unmodified. Update them as a set and record the new
version and commit here.

The FatFs/SD library is vendored unmodified: the release's `LICENSE`, `README.md` and `src/`, without FatFs's HTML
documentation (`src/ff15/documents/`). `cmake/fatfs_sd.cmake` builds it from its own list of sources, leaving out
`src/crash.c`, whose HardFault handler would replace ours; `src/pico/sd_hw_config.c` supplies its hooks. To update
it, replace the directory with a new release's files, check `src/pico/fatfs/ffconf.h` against the new
`src/include/ffconf.h`, and record the version and commit here.

## Distributing a built firmware (.uf2 / .elf)

The firmware contains code under each licence above except Unity's. Whoever passes on a built firmware has to pass
on, with it, the licence texts: this project's `LICENSE` (GPL-3.0, which also means offering the corresponding
source), `third_party/pico-sdk/LICENSE.TXT`, `third_party/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/LICENSE` (Apache-2.0; the
library has no NOTICE file), its `src/ff15/LICENSE.txt`, `src/SdFat/LICENSE.md`, `src/mbed-os/LICENSE.md` and
`src/ZuluSCSI-firmware/LICENSE`, and this file. Never with an IDC ROM or disc images: those are RM's.
