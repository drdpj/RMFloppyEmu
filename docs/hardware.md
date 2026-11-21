# Hardware

Status (2026-11-21): in development. A USB serial adapter on the link pins stands in for the transceiver
(`docs/testing.md`, level 3), and a separate micro-SD breakout can stand in for the TFT's slot on the SD pins below.
The SD card's SPI clock is 15 MHz: with a level-shifting breakout on jumper wires, 25 MHz failed to mount and
18.75 MHz worked (`src/pico/sd_hw_config.c`).

## Decisions

- **Two drives**, as the MD1/MQ2 dual unit (and MAME's `rm_mq2`). Each side of a disc is a CP/M drive: the first
  drive holds A: and C:, the second B: and D: (`docs/protocol.md`, "The drive byte").
- **Images on an SD card**, IMD and HFE, chosen per drive on the display with a rotary encoder.
- **Writes supported for both formats, safely.** An IMD is rewritten to a temporary file and renamed into place. An
  HFE has the written track re-encoded in place, which needs a C FM/MFM encoder in `src/flux` (useful to Disk
  Wrangler too).
- **The IDC ROM lives on the SD card too** (owner, 2026-11-20): an 8 KB image, `IDC.ROM` in the card's root (or the
  first `*.ROM` that `idc_find_boot_block()` recognises, so IDC3 and IDC5 both work). It is read into RAM at start-up,
  and command `FF` sends the boot block from it. With no recognised ROM, the screen says so and `FF` is answered as
  unknown. This keeps RM's code off the device firmware and out of the repository.
- **Display:** a 2.8" SPI TFT, 240×320, "V1.2". These modules carry an **ILI9341** controller in almost
  every case (check the silkscreen; ST7789 is close enough to support too), an SD card slot on its own pins, and
  usually an XPT2046 resistive touch controller (`T_` pins).

## Parts

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 | target |
| Raspberry Pi Pico running debugprobe | SWD and console during development; powers the target (VBUS → Schottky → VSYS) |
| 2.8" SPI TFT 240×320 V1.2 (ILI9341) with SD slot | the display, and the SD slot |
| Rotary encoder with push switch (EC11 type) | A/B/switch to ground, internal pull-ups on the Pico |
| **MAX3243 / SP3243** RS-232 transceiver (3 drivers, 5 receivers, 3.0–5.5 V) + 4 × 0.1 µF | or two MAX3232s (2 + 2 each). All 3.3 V parts: no ±12 V supply needed |
| Plug for the 480Z's SIO/4 DIN socket | or reuse an MD1 cable |
| Small NPN or N-MOSFET + resistor | TFT backlight switch (some modules' `LED` pin draws 50–100 mA) |
| micro-SD card, FAT32 or exFAT | images in the root or in folders (`docs/sd-card.md`). exFAT because cards over 32 GB come formatted that way |

## The 480Z link (RS-232)

The 480Z SIO/4 port is Z80 SIO channel B through 75188 drivers and 75189 receivers [schematic D10829 sheet 6], so
it is true ±12 V RS-232. The unit's side of the cable, as wired in MAME (`rm_mq2.cpp`) and used by the IDC firmware
(`docs/protocol.md`):

| Pico GPIO | Direction | Transceiver | 480Z J5 pin | 480Z signal | Role |
|---|---|---|---|---|---|
| GP4 (UART1 TX) | out | driver 1 | 8 | RxD | data to the 480Z |
| GP5 (UART1 RX) | in | receiver 1 | 3 | TxD | data from the 480Z |
| GP6 | out | driver 2 | 5 | CTS | "send me a byte": up only while we wait for one (IDC DTR) |
| GP7 | out | driver 3 | 7 | DCD | IDC RTS (held active) |
| GP8 | in, IRQ | receiver 2 | 1 | RTS | the 480Z starting an exchange: back to 9600 baud (IDC NMI) |
| GP9 | in | receiver 3 | 2 | DTR | 480Z ready (IDC CTS) |
| GND | | GND | 4 | signal earth | |

RS-232 drivers and receivers invert. The firmware works in logical terms (asserted or not) and puts the inversion in
one place (`src/pico/link_uart.c`). GP5, GP8 and GP9 have the Pico's pull-ups on, so with nothing connected RX
idles at mark and RTS and DTR read deasserted. A floating RX would read as breaks, which come in as `00` bytes.
J5 pin numbers are from the 480Z main board schematic. Check the DIN pinout against the socket before soldering.

## Pin plan (Pico 2)

| GPIO | Use |
|---|---|
| GP0 / GP1 | UART0 TX / RX: debug console (to the probe's GP5 / GP4) |
| GP2 / GP3 | rotary encoder A / B |
| GP4 / GP5 | UART1 TX / RX: the 480Z link (via the transceiver) |
| GP6 / GP7 | link outputs: 480Z CTS, 480Z DCD |
| GP8 / GP9 | link inputs: 480Z RTS (edge interrupt), 480Z DTR |
| GP12 | SPI1 RX (MISO): touch controller (and the TFT's SDO if ever needed) |
| GP13 | TFT CS (SPI1) |
| GP14 / GP15 | SPI1 SCK / TX (MOSI): TFT (and touch) |
| GP16 / GP17 / GP18 / GP19 | SPI0 RX / CS / SCK / TX: SD card (the module's `SD_` pins), on its own bus so slow SD transfers never hold up the display |
| GP20 | TFT D/C |
| GP21 | TFT RESET |
| GP22 | TFT backlight, through the transistor (PWM for brightness) |
| GP25 | on-board LED |
| GP26 | rotary encoder push switch |
| GP27 / GP28 | touch T_CS / T_IRQ (optional) |

Free: GP10, GP11. The SWD pins are on the Pico 2's own debug header, so no GPIOs go to debugging.

## Power

During development the probe's VBUS powers the target through a Schottky diode into VSYS. The TFT module takes 3.3 V
or 5 V on `VCC`: it has its own regulator, but its logic is 3.3 V. Take its supply from VSYS/VBUS rather than the
Pico's 3V3 pin, because of the backlight. The 480Z's SIO/4 socket supplies no power. The finished unit needs its own
5 V (USB is enough).

## Software pieces this needs (milestone 6 in docs/plan.md)

- **SD/FAT:** FatFs, with exFAT enabled as well as FAT32, through carlk3's `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` (Apache-2.0, and FatFs's BSD-style licence:
  both compatible with GPL-3.0). Vendored in `third_party/` and listed in `THIRD-PARTY-NOTICES.md`.
- **Display:** a small ILI9341/ST7789 driver in `src/pico/`, drawing text with a bitmap font a line at a time (no
  frame buffer), using DMA.
- **Encoder:** a quadrature state machine (PIO or GPIO interrupts) with debouncing. The menu logic is in `src/core`,
  so it can be tested on the PC.
- **Images:** each drive presents one track at a time, loaded from the SD card on demand: sectors in rotational
  order, with positions and data. HFE goes through `src/flux` decoding; IMD is parsed and its sectors placed with
  standard IBM gaps. A whole disc (368 KB of data) doesn't fit twice in 520 KB of RAM.
