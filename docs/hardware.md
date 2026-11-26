# Hardware

Status (2026-11-24): working on a breadboard, the board still to come. Two MAX3232 modules drive the link, and the
real 480Z boots CP/M through them and runs everything at 125000, the ROS's boot block included ("Wiring two MAX3232
modules"; `docs/acceptance.md`); a USB serial adapter on the link pins was the stand-in before (`docs/testing.md`,
level 3). The display is the 2.8" TFT or a small I2C OLED ("An OLED instead of the TFT"). The SD card is a separate micro-SD breakout on SPI0, not the TFT module's slot.
The SD card's SPI clock is 15 MHz: with a level-shifting breakout on jumper wires, 25 MHz failed to mount and
18.75 MHz worked (`src/pico/sd_hw_config.c`). The TFT is an ILI9341 (its ID4 reads `00 93 41`). On
jumper wires its SPI1 writes were clean at 37.5 MHz and below, and garbled at 75 MHz (67 of 100 strips, window
commands included; `tests/hil/run_display_probe.py`). 37.5 MHz fills the screen in 36 ms. Clockwise on the encoder,
as wired below, is B changing before A.

## Decisions

- **Two drives**, as the MD1/MQ2 dual unit (and MAME's `rm_mq2`). Each side of a disc is a CP/M drive: the first
  drive holds A: and C:, the second B: and D: (`docs/protocol.md`, "The drive byte").
- **Images on an SD card**, IMD and HFE, chosen per drive on the display with a rotary encoder.
- **Writes supported for both formats, safely.** An IMD is rewritten to a temporary file and renamed into place. An
  HFE has the written track re-encoded in place, which needs a C FM/MFM encoder in `src/flux` (useful to Disk
  Wrangler too).
- **The IDC ROM lives on the SD card too** (owner, 2026-11-20): an 8 KB image, `IDC.ROM` in the card's root (or the
  first `*.ROM` that `idc_find_boot_block()` recognises, so IDC3 and IDC5 both work). Use IDC 3.1 (`IDC3-1I`), the
  double-drive units' ROM (owner, 2026-11-21); 5.1 (`IDC5-1J`) was the single-drive units'. It is read into RAM at start-up,
  and command `FF` sends the boot block from it. With no recognised ROM, the screen says so and `FF` is answered as
  unknown. This keeps RM's code off the device firmware and out of the repository.
- **Display:** a 2.8" SPI TFT, 240×320, "V1.2" (the board lcdwiki calls MSP2807). The controller isn't marked on
  the module. These almost always carry an **ILI9341**; the bring-up firmware reads the ID to be sure, and the
  driver keeps ST7789 possible. The module also has an SD slot on its own pins and an XPT2046 touch controller
  (`T_` pins). **Neither is used** (owner, 2026-11-21): the card is on a separate breakout, and the knob is the
  only input. Their pins stay unconnected; the touch controller's own chip select keeps it off the bus anyway.
  Or instead, a small I2C OLED, 128 x 64 or 128 x 32 (below, "An OLED instead of the TFT").
- **Backlight:** the module switches its own. Its `LED` pin goes through a 1 kΩ resistor (R6) to the base of an
  S8050 (Q1, marked `J3Y`), which switches the LEDs' cathodes through 10 Ω (R5). The anodes are fed from the
  module's 3.3 V regulator [MSP2807 schematic]. So GP22 drives `LED` directly, with PWM for brightness. It sources
  about (3.3 − 0.75) V / 1 kΩ ≈ 2.6 mA, and the backlight current comes from the module's `VCC`, never through a
  Pico pin. No external transistor is needed.

## Parts

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 | target |
| Raspberry Pi Pico running debugprobe | SWD and console during development; powers the target (VBUS → Schottky → VSYS) |
| 2.8" SPI TFT 240×320 V1.2 (ILI9341, probably) | the display. Its SD slot and touch controller aren't used |
| micro-SD breakout (SPI) | the SD card, on SPI0 |
| Rotary encoder with push switch (EC11 type) | A/B/switch to ground (common `C` and one switch pin to GND), internal pull-ups on the Pico |
| Two **MAX3232** RS-232 modules (2 drivers + 2 receivers each) | the link's 3 drivers and 3 receivers (wiring below). They run the 480Z at 125000; for a board, see "For a board" below |
| Plug for the 480Z's SIO/4 DIN socket | or reuse an MD1 cable |
| micro-SD card, FAT32 or exFAT | images in the root or in folders (`docs/sd-card.md`). exFAT because cards over 32 GB come formatted that way |

## The 480Z link (RS-232)

The 480Z SIO/4 port is Z80 SIO channel B through 75188 drivers and 75189 receivers [schematic D10829 sheet 6], so
it is true ±12 V RS-232. The unit's side of the cable, as wired on the IDC (its circuit diagram, D12794 sheet 2), in
MAME (`rm_mq2.cpp`) and used by the IDC firmware (`docs/protocol.md`, "Physical layer"):

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
J5 pin numbers are from the 480Z main board schematic, and RM's LINK 480Z Information File ("Peripheral Interfaces",
SERIAL I/O 4) gives the same list. The socket is an 8-pin DIN with pin 8 in the centre. Seen from the back panel, with
the key at the bottom:

```
        2
    4       5
  1     8     3
    6       7
       key
```

Some published drawings show the mirror image of this (the socket from behind), so go by the numbers moulded on the
connectors, and check with a meter before connecting: with the 480Z on and nothing plugged in, pin 4 is earth (about
10 Ω to the 480Z's 0 V, through R10), pins 5 and 7 sit a few volts positive (their pull-ups), pin 3 (TxD, idle) at
about −10 V, and pin 6 is not connected.

### Wiring two MAX3232 modules

A MAX3232 has two drivers and two receivers; the link needs three of each, so two modules. The data lines and the
480Z's RTS (the reset) share the first. TIN/ROUT are the logic side (to the Pico), TOUT/RIN the RS-232 side (to the
480Z). Take the handshake lines from the modules' pins, not their DB9s, which often carry only TXD and RXD.

**The cable.** RM's disc unit interface lead (480Z Serial I/O 4 to the MD1's "480Z" socket) is a symmetric null
modem: 1–5, 2–7, 3–8 and 4–4, the same lead either way round (checked on an original, 2026-11-22). The MD1's socket
therefore has the 480Z's own layout, outputs on 1, 2 and 3 and inputs on 5, 7 and 8, and the lead crosses each
output to the other end's input. The unit's 8-pin DIN socket is wired the MD1's way, so it takes RM's lead:

| Socket pin | Direction | Module | Pico 2 | Lead takes it to 480Z J5 | 480Z signal |
|---|---|---|---|---|---|
| 1 | out | 1: T2IN ← GP6 (pin 9), T2OUT → socket | GP6 | 5 | CTS (480Z input) |
| 2 | out | 2: T1IN ← GP7 (pin 10), T1OUT → socket | GP7 | 7 | DCD (480Z input) |
| 3 | out | 1: T1IN ← GP4 (pin 6), T1OUT → socket | GP4, UART1 TX | 8 | RxD (480Z input) |
| 4 | | both GNDs | GND (pin 8) | 4 | signal earth |
| 5 | in | 1: R2IN ← socket, R2OUT → GP8 (pin 11) | GP8 | 1 | RTS (480Z output) |
| 6 | | not connected | | | |
| 7 | in | 2: R1IN ← socket, R1OUT → GP9 (pin 12) | GP9 | 2 | DTR (480Z output) |
| 8 | in | 1: R1IN ← socket, R1OUT → GP5 (pin 7) | GP5, UART1 RX | 3 | TxD (480Z output) |

Module 2's T2IN goes to GND (a floating CMOS input can oscillate), and its R2OUT is not connected. The transceivers
invert as the firmware expects (`LINK_UART_ASSERTED_LEVEL`), as the FT232R's active-low handshake pins did.

**Checking it**, before the 480Z is connected: with the Pico running, at the far end of RM's lead, pin 4 is 0 V,
pins 5 and 7 (CTS and DCD, both asserted while the unit waits for a command) are positive, and pin 8 (our TxD,
idle at mark) is negative.

### Two ways to connect it

- **A DIN socket, wired as the back of the MD1** (the table above). It takes a crossover lead: RM's own, or any lead
  wired 1–5, 2–7, 3–8, 4–4 between two 8-pin DIN plugs. This is what the breadboard uses and what was tested on
  the real 480Z.
- **A 7-pin header** (one row, 2.54 mm), which feeds a lead ending in an 8-pin DIN plug that goes straight into the
  480Z's SIO/4, with no other lead. The crossing is done on the board, so the header is numbered from the 480Z's
  side and the lead is straight through: header pins 1–5 go to plug pins 1–5, header 6 to plug 7 and header 7 to
  plug 8 (the 480Z's pin 6 is not connected). The 480Z's outputs then arrive on 1–3, ground on 4 and its inputs
  on 5–7.

The signal names are the IDC's own [D12794 sheet 2; `docs/protocol.md`, "Physical layer"]; the module wiring is
as in the table above.

| Header pin | Plug pin (480Z J5) | 480Z signal | Direction (ours) | Pico 2 | Our signal (IDC name) |
|---|---|---|---|---|---|
| 1 | 1 | RTS | in | GP8 | DCD: the 480Z's reset (the IDC's NMI) |
| 2 | 2 | DTR | in | GP9 | CTS: the 480Z's pace |
| 3 | 3 | TxD | in | GP5, UART1 RX | RxD |
| 4 | 4 | signal earth | | GND | signal earth |
| 5 | 5 | CTS | out | GP6 | DTR: "send me a byte" |
| 6 | 7 | DCD | out | GP7 | RTS: held asserted |
| 7 | 8 | RxD | out | GP4, UART1 TX | TxD |

Check the lead with a meter against the J5 diagram above (the plug is the mirror image of the socket, seen from its
pins), then make the check above ("Checking it") at the plug: the same pins give the same readings.

With nothing connected, the IDC's inputs read asserted (it pulls them up on the line side) and ours read deasserted
(an RS-232 receiver's input pulls down to mark, and the Pico's pull-ups on GP8 and GP9 do the same with no
transceiver). This only matters with no 480Z connected, because a connected 480Z drives both lines.

**Power: 3V3 (pin 36).** That is enough for the 480Z at 125000 (below), and keeps 5 V off the Pico's pins. The
drivers then give about ±5.5 V. VSYS (pin 39) works too: the RP2350's GPIOs are 5 V tolerant while the chip is
powered (not the ADC pins, GP26–GP29, which the link doesn't use), unlike the RP2040's, so 5 V on GP5, GP8 and GP9
is safe as long as the modules are powered from the Pico's own supply and never while it is off. At 5 V the MAX3232's datasheet asks for C1 ≥ 0.047 µF and C2–C4 ≥ 0.33 µF; modules are
usually fitted with 0.1 µF for 3.3 V.

**At 125000** (real 480Z, ROS 1.2, 2026-11-23), two inexpensive MAX3232 modules run CP/M at full speed, from VSYS
and from 3V3 alike. From 3V3, `PIP B:=A:*.*[V]` copied the whole system disc to a blank image in B and verified it,
with no errors: 162 KB to the unit and 387 KB back in one exchange, at 125000, with no parity errors or resets.
The ROS's loading of the boot block failed there at first, which was not the transceivers: the ROS's receive
interrupt runs late just after `FF`, and the unit overran its SIO. The unit now sends the block two bytes per DTR
pulse, and the 480Z loads it at 125000 (2026-11-24; `docs/protocol.md`, "Framing and baud rate"). Measured on the way: with the
modules' own capacitors V+ was +8.5 V but V− only −4.2 V; 910 nF ceramics across C1–C4 brought V− to −8.0 V. At
the 480Z's socket the data line then rose in 0.76 us and fell in 1.2 us, swinging about +8.7 V to −6 V; a lone bit
was within 0.5 us of its 8 us.

### For a board

- **TI MAX3243E (or TRS3243E):** 3 drivers and 5 receivers in one chip from 3.3 V, guaranteed 250 kbps into 3 kΩ ‖
  1000 pF, ±15 kV ESD. Not Maxim's MAX3243, guaranteed only 120 kbps. Tie FORCEON and FORCEOFF high so it never
  powers down. Use capacitors that keep their value under bias (X7R or X5R, ≥ 0.47 µF): the modules' own let V−
  sag. The modules from 3V3, at about ±5.5 V, ran the 480Z at 125000, so a ±5.4 V driver is enough.
- **±12 V drivers, as the MD1 had,** are not needed, but would do: a DS14C88 (a low-power 1488) or a 75188 for
  the 3 drivers, fed from a small 5 V to ±12 V DC-DC converter (for example a Murata NMA0512SC); the receivers can
  stay a 3.3 V part, or be a 1489, whose 5 V output the RP2350 tolerates.

## Pin plan (Pico 2)

The physical pin is the Pico 2's header pin (pin 1 is GP0, at the USB end; the GNDs are 3, 8, 13, 18, 23, 28, 33
and 38).

| GPIO | Pin | Use |
|---|---|---|
| GP0 / GP1 | 1 / 2 | UART0 TX / RX: debug console (to the probe's GP5 / GP4) |
| GP2 / GP3 | 4 / 5 | rotary encoder A / B |
| GP4 / GP5 | 6 / 7 | UART1 TX / RX: the 480Z link (via the transceiver) |
| GP6 / GP7 | 9 / 10 | link outputs: 480Z CTS, 480Z DCD |
| GP8 / GP9 | 11 / 12 | link inputs: 480Z RTS (edge interrupt), 480Z DTR |
| GP12 | 16 | SPI1 RX (MISO): the TFT's `SDO(MISO)`, to read the controller's ID and check writes |
| GP13 | 17 | TFT `CS` (SPI1's CSn pin, but driven as a plain GPIO: a command and its data share one select) |
| GP14 / GP15 | 19 / 20 | SPI1 SCK / TX (MOSI): TFT `SCK` / `SDI(MOSI)` |
| GP16 / GP17 / GP18 / GP19 | 21 / 22 / 24 / 25 | SPI0 RX / CS / SCK / TX: the SD card breakout, on its own bus so slow SD transfers never hold up the display |
| GP20 | 26 | TFT `DC` (D/C) |
| GP21 | 27 | TFT `RESET` |
| GP22 | 29 | TFT `LED`: backlight, PWM (slice 3, channel A) |
| GP24 | (on board) | VBUS sense: high while the Pico 2's USB socket has a lead with power in it (`usb-storage`, `docs/sd-card.md`) |
| GP25 | (on board) | on-board LED |
| GP26 | 31 | rotary encoder push switch |

Free: GP10, GP11 (14, 15), GP27, GP28 (32, 34). GP10/GP11 are I2C1's SDA and SCL, for an OLED (below). GP27/GP28 were for the touch controller, which isn't used. The SWD
pins are on the Pico 2's own debug header, so no GPIOs go to debugging.

Checked against the RP2350 datasheet's function table (2026-11-21): GP12–15 are SPI1 RX, CSn, SCK and TX, and
GP16–19 are SPI0's, so each bus is on its own pins with no overlap with the console (GP0/1), the link (GP4–9) or the
encoder. GP6 is also PWM slice 3 channel A, like GP22, but a pin follows PWM only when its function is set to PWM, so
GP6 as a GPIO is unaffected; never select PWM on GP6. The encoder uses pull-ups, which RP2350 erratum E9 (a pin
held by a pull-down can latch at about 2 V) doesn't affect. Every signal is 3.3 V: the module's logic runs from its
own 3.3 V regulator.

### Wiring the display and encoder

| From | To | Pico 2 pin | GPIO |
|---|---|---|---|
| TFT `VCC` | VSYS | 39 | |
| TFT `GND` | GND | 38 | |
| TFT `CS` | | 17 | GP13 |
| TFT `RESET` | | 27 | GP21 |
| TFT `DC` | | 26 | GP20 |
| TFT `SDI(MOSI)` | | 20 | GP15 |
| TFT `SCK` | | 19 | GP14 |
| TFT `LED` | | 29 | GP22 |
| TFT `SDO(MISO)` | | 16 | GP12 |
| TFT `T_CLK`, `T_CS`, `T_DIN`, `T_DO`, `T_IRQ` | not connected | | |
| TFT `SD_CS`, `SD_MOSI`, `SD_MISO`, `SD_SCK` | not connected, unless the card is in the module's slot ("Wiring the SD card") | | |
| encoder `A` | | 4 | GP2 |
| encoder `B` | | 5 | GP3 |
| encoder `C` (the middle pin of the three) | GND | 3 | |
| encoder switch, one pin | | 31 | GP26 |
| encoder switch, other pin | GND | 28 | |

Leave the module's `J1` open: it bypasses the module's regulator, for a 3.3 V supply only. Keep the SPI1 wires short
(under 15 cm) and together, with the GND wire alongside them.

### An OLED instead of the TFT

A small 4-pin I2C OLED can be the display instead of the TFT (`src/pico/oled.c`; `display-type` in
`docs/sd-card.md`): an SSD1306, 128 x 64 (0.96") or 128 x 32 (0.91"). Both kinds tried answered at 0x3C and took
the I2C bus at 1 MHz, which the firmware uses (`tests/hil/oled_probe.c`, `docs/testing.md`, "The OLED"); a full
frame takes 12 ms. They go on I2C1, the free GP10/GP11, so nothing else moves, and can be wired alongside the TFT:
with `display-type = auto` the OLED is used when one answers. The two sizes can't be told apart over I2C, so a
128 x 32 needs `display-type = oled-128x32`. Plug in one at a time: both usually answer at 0x3C. The 128 x 32's
picture fills only the top half of its glass: that is the panel, not a fault.

| From | To | Pico 2 pin | GPIO |
|---|---|---|---|
| OLED `VCC` | 3V3(OUT) | 36 | |
| OLED `GND` | GND | 13 | |
| OLED `SDA` | | 14 | GP10 (I2C1 SDA) |
| OLED `SCL` (`SCK` on some) | | 15 | GP11 (I2C1 SCL) |

The modules have their own pull-ups on SDA and SCL (to their `VCC`, so 3.3 V here) and take 3.3 V or 5 V; from 3.3 V
the bus can't go above it. Unplug the probe's USB before changing modules.

### Wiring the SD card

The card is on SPI0 by itself (`src/pico/sd_hw_config.c`), at 15 MHz, with no card-detect pin: the firmware
checks every few seconds whether the card answers. It runs from 3.3 V. There are three ways to fit it, all on the
same four GPIOs, so the firmware doesn't change; fit only one at a time.

**A micro-SD breakout** (on the bench now), from 3V3(OUT). A breakout with its own 3.3 V regulator is meant for
5 V, and from 3.3 V leaves the card below 3 V; one without a regulator, or one that has run reliably from 3.3 V,
is fine there.

| From | To | Pico 2 pin | GPIO |
|---|---|---|---|
| breakout `VCC` | 3V3(OUT) | 36 | |
| breakout `GND` | GND | 23 | |
| breakout `MISO` (`DO`) | | 21 | GP16 (SPI0 RX) |
| breakout `CS` | | 22 | GP17 |
| breakout `SCK` (`CLK`) | | 24 | GP18 (SPI0 SCK) |
| breakout `MOSI` (`DI`) | | 25 | GP19 (SPI0 TX) |

**A bare micro-SD socket**, for a board. The card's pins in SPI mode (SD Physical Layer Simplified Specification):
note that **VDD is the supply, +3.3 V, and VSS is ground**.

| Card pin | Name | SPI role | To |
|---|---|---|---|
| 1 | DAT2 | unused | 3.3 V through 47 kΩ |
| 2 | CD/DAT3 | CS | GP17 (pin 22), and 3.3 V through 47 kΩ |
| 3 | CMD | MOSI (DI) | GP19 (pin 25), and 3.3 V through 47 kΩ |
| 4 | VDD | supply | 3V3(OUT) (pin 36) |
| 5 | CLK | SCK | GP18 (pin 24) |
| 6 | VSS | ground | GND |
| 7 | DAT0 | MISO (DO) | GP16 (pin 21), and 3.3 V through 47 kΩ |
| 8 | DAT1 | unused | 3.3 V through 47 kΩ |

The pull-ups hold the lines the card samples at power-up and while CS is high; 10–47 kΩ all work. Put 100 nF and
about 10 µF from VDD to VSS at the socket: a card draws its current in bursts while it writes. A socket's own
card-detect switch, if it has one, needn't be wired.

**The MSP2806's own SD slot** (the TFT module's): its `SD_CS`, `SD_MOSI`, `SD_MISO` and `SD_SCK` go to GP17, GP19,
GP16 and GP18 as above. The slot is powered from the module's own supply, so it needs no wire of its own, but
then the card is on the TFT module and leaves with it.

## Power

During development the probe's VBUS powers the target through a Schottky diode into VSYS. The TFT module takes 3.3 V
or 5 V on `VCC`: it has its own 3.3 V regulator (with `J1` open), and its logic is 3.3 V. Take its supply from
**VSYS** (pin 39), not the Pico's 3V3 pin, because of the backlight: the module's regulator then carries the
backlight and the panel. Not VBUS (pin 40) either: VBUS is live only when the Pico 2's own USB socket is plugged in,
and on the bench it isn't, so the module would get nothing. VSYS is about 4.7 V after the diode, enough for the
module's regulator. The backlight is four LEDs in parallel from 3.3 V through 10 Ω, a few tens of mA; lcdwiki gives
0.31 W for the whole module. The 480Z's SIO/4 socket supplies no power. The finished unit needs its own
5 V (USB is enough).

**The finished unit** runs from one USB lead into the Pico 2's own socket: the Pico's on-board diode takes VBUS to
VSYS, so VSYS is then an output as well, at about 4.7 V. The TFT stays on VSYS (pin 39) and an OLED on 3V3(OUT)
(pin 36), so the same wiring works from the probe, from the Pico 2's socket, or from both at once (each supply
comes through its own diode, so neither feeds the other). A unit only ever powered by its own socket can take the
TFT from VBUS (pin 40) instead, for the full 5 V, but then the display is dark whenever the Pico 2 is powered any
other way. Never feed 5 V into VBUS from elsewhere while the socket goes to a PC: it would drive the PC's port. The
whole unit draws a few hundred mA at most with the TFT lit, within what a USB port gives. On a PC, the same lead
gives the PC the card if `usb-storage = yes` (`docs/sd-card.md`).

## Software pieces this needs (milestone 6 in docs/plan.md)

- **SD/FAT:** FatFs, with exFAT enabled as well as FAT32, through carlk3's `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` (Apache-2.0, and FatFs's BSD-style licence:
  both compatible with GPL-3.0). Vendored in `third_party/` and listed in `THIRD-PARTY-NOTICES.md`.
- **Display:** `src/pico/tft.c`, ILI9341 (ST7789 recognised by its ID and driven too), landscape. Text only, a
  16-pixel row at a time with the misc-fixed 8x13 font (`src/pico/font_8x13.c`), from two line buffers, one sent
  by DMA while the next is drawn: no frame buffer. At start-up it identifies the controller and picks the fastest
  clock (37.5 MHz down to 12.5) at which 16 full-width strips, window commands and all, read back intact.
  Or `src/pico/oled.c`: an SSD1306 OLED on I2C1 at 1 MHz, with a 1 KB frame buffer of which only the changed
  8-row pages are sent; misc-fixed 7x13 bold for the menu's rows, 5x8 or 4x6 for its status line.
- **Encoder:** `src/core/rotary.c` decodes the quadrature and debounces the switch (tested on the PC); on core 1,
  GPIO interrupts on both edges of A, B and the switch note the pins' levels, so no state is missed while a row is
  drawn. GPIO interrupts rather than PIO: the knob changes a few hundred times a second at most, and core 1 has
  nothing more urgent to do. Direction and steps per detent are set by `rotary` in `RMFE.CFG` (`docs/sd-card.md`).
- **Menu:** `src/core/ui.c` and `src/core/dirlist.c`, driven by knob events and a tick count, tested on the PC.
  Core 1 runs it with the display and the knob; core 0, which owns the link and the card, does what it asks on the
  card a short step at a time between the 480Z's commands (`src/pico/main.c`, `menu_host.c`, `mailbox.c`).
- **USB storage:** `src/pico/usb_disk.c`, a TinyUSB (the SDK's) mass-storage device on the Pico 2's own USB
  socket, whose sectors are the SD card's, read and written through the SD driver under FatFs. The card is the
  PC's or the unit's, never both (`src/core/usb_share.c` decides, `src/pico/menu_host.c` does it); `docs/sd-card.md`,
  "USB storage". The SDK tells the USB controller VBUS is always there, so an unplugged lead looks like a suspend:
  GP24, the board's VBUS sense, tells them apart. The device uses pid.codes' test IDs (`1209:0001`) for now.
- **Images:** each drive presents one track at a time, loaded from the SD card on demand: sectors in rotational
  order, with positions and data. HFE goes through `src/flux` decoding; IMD is parsed and its sectors placed with
  standard IBM gaps. A whole disc (368 KB of data) doesn't fit twice in 520 KB of RAM.
