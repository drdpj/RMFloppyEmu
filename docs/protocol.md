# 480Z ↔ disk unit serial protocol

Working notes on the link between the RML 480Z and its serial disk unit. The unit's controller is an "IDC" board; MAME
calls the unit **MQ2**. Status (2026-11-21): every command the 480Z's software has been seen to use is decoded and
implemented, and matches MAME byte for byte; what is still open is at the end.

Each fact is tagged with its source:

- **[trace]**: observed in a MAME run (see [Tracing with MAME](#tracing-with-mame)).
- **[IDC3 xxxx]**: read from the IDC3 ROM disassembly at that address.
- **[MAME]**: stated in MAME's `src/mame/rm/rm_mq2.cpp`.
- **[sch]**: from the 480Z main board schematic, D10829 sheet 6 (`background-docs/10829-6.png`).
- **[IDC sch]**: from the IDC's circuit diagram, D12794 sheet 2 ("CPU, serial interface & clocks"), page 95 of the
  480Z service manual (`background-docs/RM_480Z_Service_Manual-PN13821B_text.pdf`). The IDC is the MD1's board.

Anything without a tag is inference and should be confirmed.

## Hardware in the disk unit

| Item | Detail |
|---|---|
| CPU | Z80 @ 4 MHz (8 MHz crystal ÷ 2) [MAME] |
| ROM | 8 KB at 0000–1FFF: `IDC3-1I.ROM` (IDC 3.1, CRC 39e2cdf0: the double-drive units' ROM, and the one MAME's MQ2 uses) or `IDC5-1J.ROM` (IDC 5.1, CRC d2ac27e2: the single-drive units', owner 2026-11-21) |
| RAM | 2 KB at E000, mirrored up to FFFF; the firmware addresses it as FExx/FFxx [MAME] |
| SIO | Z80 SIO, ports E4–E7: E4/E5 = channel A data/control, E6/E7 = channel B data/control. **Channel B is the 480Z link.** [IDC3 0a1f, 0a56] |
| CTC | Ports E0–E3. Channel 0 is clocked at 2 MHz and generates the channel B baud clock [MAME, IDC3 0116] |
| FDC | WD1793 at ports 00–03, used for 5.25" SD/DD/QD drives (8" detection exists) [MAME] |
| Drive control | Port 0E = drive select / side / motor; port 0D = density / 8" select; port 04 = drive status; port 07 = FDC sync (WAIT until DRQ/INTRQ) [MAME] |

Channel A is a second serial port. The ROM contains a monitor/debugger ("Start", "End", "Baud code (0-7)", "Trace to",
and a register dump) which probably runs on it. Command `15` passes channel A bytes through to the 480Z [IDC3 0e5f].

## Physical layer

On the 480Z this is the **SIO/4 port**, Z80 SIO channel B, which uses true RS-232 levels through 75188 drivers and
75189 receivers [sch]. Its DIN connector J5:

| J5 pin | Signal (480Z side) | Direction | Disk-unit signal [MAME, IDC sch] | IDC J1 pin [IDC sch] | MD1 socket pin |
|---|---|---|---|---|---|
| 1 | RTS | out | → DCD (also triggers the unit's **NMI**) | 6 | 5 |
| 2 | DTR | out | → CTS | 5 | 7 |
| 3 | TxD | out | → RxD | 3 | 8 |
| 4 | Signal earth | | GND | 2 or 10 (frame ground; not shown which) | 4 |
| 5 | CTS | in | ← DTR | 7 | 1 |
| 6 | n/c | | | | 6 |
| 7 | DCD | in | ← RTS | 8 | 2 |
| 8 | RxD | in | ← TxD | 1 | 3 |

**The unit's side** [IDC sch]: the IDC's SIO channel B goes through 75188 drivers and 75189 receivers to J1, a header
on the IDC board: TxDB on 1, RTSB on 8 and DTRB on 7 out; RxDB on 3, CTSB on 5, DCDB on 6 and SYNCB (labelled DSRB)
on 4 in. The inputs have 4K7 pull-ups on the line side, so an unconnected one reads asserted. DCDB also drives
`IRESET-`, the Z80's NMI (pin 17), through an open-collector inverter (IC8) and link LK3: the NMI on the 480Z's RTS
is in the hardware, as MAME has it. J1 also carries channel A (9, 11–16) and frame ground (2 and 10, each through
0.47 Ω). The firmware doesn't use DSRB.

**The MD1's socket** is reached from J1 by a loom inside the unit, which isn't on the drawing. RM's lead is a
symmetric null modem (1–5, 2–7, 3–8, 4–4; `docs/hardware.md`), so the socket's pin numbers have the 480Z's
directions (outputs on 1–3, inputs on 5, 7 and 8) but **not its signal names**: socket pin 1 is the IDC's DTR and
pin 5 its DCD, not RTS and CTS. The socket column is inferred from the lead and the real 480Z runs (the 480Z's RTS,
on socket pin 5, resets the unit; its DTR, on pin 7, paces the boot block, 525 pulses counted, 2026-11-24), not
measured inside an MD1. MAME's pairing (RTS ↔ DCD, DTR ↔ CTS) looks odd beside a standard null modem, but it is
right.

**For the Pico:** these are ±12 V RS-232 levels: the 480Z's 75188s would drive about ±12 V into the Pico's 3.3 V
pins. So the unit needs transceivers of its own, as the MD1 had. All four handshake lines are used: the unit drives
TxD, DTR(→CTS) and RTS(→DCD), and receives RxD, CTS(←DTR) and DCD(←RTS), 3 out and 3 in. That is two MAX3232
modules powered at 3.3 V (or one MAX3243); the wiring is in `docs/hardware.md`.

## Framing and baud rate

- **8 data bits, even parity, 1 stop bit**, ×16 clock (SIO WR4 = `47`) [IDC3 0123].
- **9600 baud** is the rest state. CTC0 runs in counter mode with time constant `0D`: 2 MHz / 13 / 16 = 9615 baud,
  which is +0.16% fast [IDC3 0116, trace].
- **125000 baud** is the fast state, with CTC0 time constant `01`: 2 MHz / 16 = 125000 exactly [IDC3 0b1d, trace].
- **The 480Z takes the fast rate from the unit.** The third byte of the reply to `00` (`5A 00 01`) is the time
  constant the 480Z loads into its own CTC0. The ROS reads `5A`, then `00`, then puts the third byte in H and `47`
  (CTC control: counter mode, time constant follows) in L, and writes both to port 20 [480Z ROS 1.2 1f04-1f17 ->
  3cae; ROS 2.2 3f04-3f17 -> 7cae]. Any other first or second byte is "Disc Error". So `02` gives 2 MHz / 16 / 2 =
  62500, and so on; nothing between 62500 and 125000 can be had. The boot code and the CP/M BIOS honour it too, and
  so do the 4MATION loaders, which talk to the drive themselves (on the real 480Z, ROS 1.2: Flowers of Crystal at
  62500 on 2026-11-22; Flowers of Crystal and Dragon World, from IMD and from Dragon World's rebuilt HFE, at 125000
  on 2026-11-23). The unit offers 62500 as `fast-baud` (`docs/sd-card.md`); it isn't the IDC's behaviour.
- **The ROS's SIO runs at 8N2 while it loads the boot block.** Its channel B set-up is `18 14 4C 13 E1 15 EA 12 10
  11 1E`: WR4 `4C` is ×16, no parity, 2 stop bits [480Z ROS 1.2 1cb5, ROS 2.2 3cb5]. So the ROS's `00` and `FF`
  reach an 8E1 receiver with a stop bit where the parity bit should be, and both have even parity: the unit sees a
  parity error on each, every time (seen in MAME and on the real 480Z). The boot code then sets the SIO to 8E1, and
  the `00`s of the boot code and the CP/M BIOS arrive clean. The ROS's receive interrupt stores every byte into an
  8-byte ring without looking at errors, dropping DTR at each; its byte routine raises DTR again when the ring is
  empty [ROS 1.2 157e-159e, 07b5-07dd].
- **On a real 480Z the ROS lost a byte of the boot block at 125000 until the unit paced it** (ROS 1.2D; found
  2026-11-24). Unpaced, the unit started a byte whenever DTR was up. Just after `FF`, the ROS's receive interrupt,
  which drops DTR, ran 388 us late, so five bytes went before DTR dropped. The SIO holds three plus the one it is
  assembling, so the fourth overran and was lost. The ROS waited for it with DTR up, then timed out ("Disc Error").
  The rest of the block went two bytes per pulse. Delaying the first byte 73 us, as the IDC does, booted, but the fourth
  byte only just made it, and later in that block the interrupt ran 227 us late (three bytes); at 62500 a byte takes
  twice as long, so the same delays fit. So the unit now sends the boot block **paced**, at most two bytes per DTR
  pulse, and boots at 125000 (`core/link.h`, `link_set_paced`; "Flow control"). Everything after the boot block always
  worked at 125000: the boot code and the CP/M BIOS hold DTR up through a record. Measured with a trace of the
  480Z's DTR edges and the unit's byte starts, taken by the unit itself. Ruled out before: the transceivers (V−,
  3V3 and 5 V), the edges at the 480Z's socket (rise 0.76 us, fall 1.2 us, a lone bit within 0.5 us of its 8 us),
  10 us of idle between bytes, and an 8N2 boot block. `boot-baud` (`docs/sd-card.md`) is now 125000 by default;
  62500 is kept as a fallback.
- **The boot block is checked.** The ROS adds every byte after `FF` (load addresses, counts and data) into a 16-bit
  sum and compares it with the two bytes at the end; a mismatch, or no byte for about 144 ms (256 polls), is
  "Disc Error" [480Z ROS 1.2 1f1e-1f4c, 078e; "Open questions", "The 480Z side"].
- The framing stays 8E1 at both speeds. Only the CTC divider changes [IDC3; the no-parity 2-stop table at 012D is used
  only by command `12`, for channel A].
- Parity is checked. When receiving a 128-byte block, a parity error makes the unit ask for a resend
  [IDC3 0f67–0f6f].

## Flow control

- **Unit → 480Z (unit DTR, read by the 480Z as CTS):** the unit raises DTR only while it is polling for a received
  byte. Each time it finds the receive buffer empty, it pulses DTR on and then off again (WR5 `EA` → `6A`) [IDC3 0a36].
  It works as "clear to send one byte". The 480Z must not send unless the unit is asking.
- **480Z → unit (480Z RTS, read by the unit as DCD and NMI):** the 480Z raises RTS before starting a new exchange.
  This NMIs the unit, which jumps to 0066, resets the stack, sets the SIO and CTC back to **9600 baud**, and re-enters
  the command loop [IDC3 0066, MAME, IDC sch]. Every exchange therefore starts at 9600 and renegotiates to 125000.
  - MAME suppresses this NMI for roughly the first 50 ms after power-up.
- **Auto enables:** the IDC sets channel B's WR3 to `E1`: receive 8 bits, receiver on, **auto enables** on. Its SIO
  set-up table is `18 18 14 47 15 EA 11 00 13 E1` [IDC3 0123–012c]. So the SIO transmits only while its CTS (the
  480Z's DTR) is asserted, and receives only while its DCD (the 480Z's RTS) is asserted (Z80 SIO manual, WR3 D5).
  WR5 `EA` also holds the IDC's RTS (the 480Z's DCD) asserted.
- **What the 480Z does with its lines** [MAME, the 480Z's ROS and CP/M with the real unit through `rmfe_link`,
  2026-11-21; `docs/testing.md` section 4, "MAME in the loop"]:
  - **RTS** rests asserted. The reset is a deassert pulse of 10–11 us; the unit resets as it is asserted again
    (the Pico on that edge; MAME's `rm_mq2` NMIs on the first edge). The ROS sends its `00` about 1.7 ms later.
  - **DTR** paces the unit. During the boot block (`FF`), the ROS takes two bytes, then drops DTR for about 1.3 ms
    (the IDC's own trace shows the same gaps); during records it stays up. The IDC's auto enables stop it
    within the character it is sending. In MAME, the 480Z still boots if the unit sends up to two more bytes
    after DTR drops, and fails with three: its SIO's 3-byte receive FIFO overruns and the ROS reports
    "Disc Error". The Pico now sends no more than the IDC (2026-11-22): it starts a byte only with its transmitter
    idle and DTR asserted (`src/pico/link_uart.c`); before, it could send one more, the byte waiting in its FIFO.
    The real ROS's receive interrupt drops DTR at each byte and reads into an 8-byte ring, and its byte routine
    raises DTR again only when the ring is empty [480Z ROS 2.2 357e-359e, 07b5-07dd]. A unit that sees DTR only a millisecond late (through
    USB) sends about 11, and fails.
  - **The ROS's interrupt can be late** (real 480Z, ROS 1.2D, 125000, 2026-11-24. Its channel B vectors, copied
    from ROM 0756 to FC00 with I = FC, send a received byte and a special receive condition (FC14 and FC16) both
    to 1582, so errors aren't looked at). DTR normally drops 114 us after
    the start of the pulse's first byte, during the second. Just after `FF` it dropped 386-388 us after it, in every
    run; once, at byte 92, 227 us. A unit that starts a byte whenever DTR is up sends five after `FF`, and the
    fourth overruns the SIO ("Framing and baud rate"). So the unit paces the boot block: after two bytes it waits
    for DTR to drop (counted by an edge interrupt, as a drop can last only 37 us) and rise again, or for 2 ms if
    it never drops, for a 480Z that holds DTR up [`src/pico/link_uart.c`]. Each DTR pulse then gets two bytes,
    and the boot block takes 0.92 s, as the ROS's pace sets it. The IDC isn't paced. It gets by because it starts
    its first byte about 73 us after `FF` and leaves 2-9 us between bytes.
  - **Timeouts:** only the ROS has one, while it loads the boot block: about 144 ms for each byte (144.2 ms on
    ROS 1.2D, 144–145 ms on ROS 2.2B, in MAME), after which it pulses RTS and shows "Disc Error". Once the boot
    block is in, its disc driver waits for each byte without limit, so the 480Z tolerates any delay in a command's
    answer ("Open questions", "The 480Z side"). The real unit through USB answers in 0.9 ms median, 5.8 ms at
    worst (MAME's time), well inside the boot's 144 ms.
  - **A slow unit** (`run_mame_linked.py --save-delay`, every save on the card made slower, 2026-11-21): the 480Z
    waited 4.15 s for FORMAT's `0C` and 2 s for CP/M's writes, with no reset beyond the ones MAME's own unit gets.
    That fits the boot block's driver having no timeout (above), so a card that takes a few seconds to rewrite an
    image is tolerated.
  - Through the link the 480Z sent two bytes with bad parity: its first `00` after power-up and its first `FF`
    after going fast, each the first byte after it reprograms its baud rate (it may be MAME's emulation; the
    IDC takes a command byte whatever its parity, so either way it does no harm).
- **The unit receives through the UART's 32-byte FIFO only** (`src/pico/link_uart.c`): its CTS is down while it
  works on the card (writing a track back, loading one), and a sender that ignores CTS can overrun it. The 480Z
  waits for CTS and for each answer, so it never does; a test script playing the 480Z must do the same
  (`tests/hil/run_mame_linked.py`, `send_paced`).

## Command layer

The unit idles in a loop at 0A7C. It reads one command byte `c` and dispatches on `c − FE` (mod 256), so the valid
commands are **`FE`, `FF`, and `00`–`17`**. The jump table is at 0AC2, with 26 entries [IDC3 0a93–0ab9]. Before
dispatching, the unit calls a RAM hook at FE80, which defaults to `RET`. Code downloaded later can use the hook to
intercept commands [IDC3 0055, 0a97].

- For commands **`01` and above**, the unit immediately sends one **command-accept byte**: the sticky error at FEA3,
  normally `00` [IDC3 0aa6, trace].
- An unknown command makes the unit send `40` and return to idle [IDC3 0a77].

### Status / error bytes

| Byte | Meaning | Source |
|---|---|---|
| `00` | OK | trace |
| `20` | Bad parameter (address < E000, track or sector out of range, …) | IDC3 0ecb, 0edd |
| `40` | Unknown command | IDC3 0a77 |
| `80` | Drive not ready / no disk (seen on a read with no disk inserted) | trace, IDC3 0ec7 |
| `02` | Verify failed: read-back after a write differed (`04`, `10`) | IDC3 11a1, 02f0 |
| `10` | Checksum error on an upload (`0A`) | IDC3 0ce5 |
| other | Raw WD1793 status bits (masked with `9C` on reads, `FC` on writes): `80` not ready, `40` write protect, `10` record not found, `08` CRC error, `04` lost data | IDC3 02c3, 0314; WD1793 data sheet |
| `Fn` | OK after `n` retries (n capped at 15) | IDC3 0eb0 |
| `+01` | Bit 0 is added to an error **writing the cache back**: on `08`, or when a read or write has to free a buffer. The IDC marks the buffer clean even so, and the change is lost. A write-protected disc answers `41` to `08` | IDC3 109f, 1088, 1198; trace (write_protect scenario) |

### Command table

| Cmd | Handler | Meaning | Used by CP/M? | Confidence |
|---|---|---|---|---|
| `FE` | 1393 | Enters the IDC's built-in debug monitor (register save area at FF42–FF59, prints a screen-control string, then a command loop; strings at 17FB–1A6C: "Start", "End", "Baud code (0-7)", "Trace to"). Probably on the aux port. Not needed by the 480Z | no | IDC3 |
| `FF` | 0AF6 | **Send boot block.** The unit streams 0x419 (1049) bytes from ROM 1B2A–1F42 (length word at 1B28). No accept byte | boot | trace + IDC3 |
| `00` | 0B07 | **Go fast.** Reply `5A 00 01`, wait for the transmitter to empty, switch to 125000 baud | every exchange | trace + IDC3 |
| `01` | 0B22 | **Log in a drive.** Parameter: drive byte. The unit recalibrates (steps in ×6, then restores) and detects the drive type. Replies: parameter status, then result | boot | trace + IDC3 |
| `02` | 0B2F | **Read 128-byte record** (below) | yes | trace + IDC3 |
| `03` | 0B3D | **Write 128-byte record** into the 512-byte sector cache. No verify. The record that completes a buffer's four sends the sector to the disc at once, and that write's status is this command's result (plain, no bit 0) [IDC3 10a6–10b2 → 1170] | SYSGEN, FASTCOPY | trace + IDC3 |
| `04` | 0B56 | **Write 128-byte record with verify.** As `03`, but the buffer is flagged (IX+6 = FF): when it is written to disk, the sector is read back and compared, giving status `02` on a mismatch [IDC3 10a2, 1178–11a1]. Completing a buffer writes it at once, as for `03`: a SAVE to a sector missing from the disc answers `00` to three records and `10` to the fourth [trace] | yes, all writes | trace + IDC3 |
| `05` | 0B6F | **Reset/configure.** Parameter byte `p` (drive-swapped by 0F77). High nibble ≠ F: retry count = nibble + 1 (FE9D). Low nibble bit 3 clear: reset the drive-mapping table FEA4–FEAB to identity, then swap logical drives 0/1 with `n`/`n+1` (n = low nibble; this is how `X` boots another drive). Bit 3 set: keep the mapping. Re-initialises the drive tables (0F86). Replies `00` | boot (`00`, `08`, `AF`) | trace + IDC3 |
| `06` | 0BB5 | **Probe disk format.** Parameter: drive byte. Reads address marks on track max(IY+7 & FE, 4), trying both densities. Replies: parameter status; then, once an ID has been read, the status again [IDC3 0c52] and a **format byte**: bits 0–1 = (IY+1)&3, bit 2 = side flag (FE91 bit 6), bit 3 = (IY+1) bit 4. Seen: `00 00 0A`. If no ID can be read, one byte instead: the error status \| type bits [IDC3 0bf9] | boot | trace + IDC3 |
| `07` | 0C7C | Set the retry count: parameter `n` < 10h (else `20`), FE9D = n + 1 | not seen | IDC3 |
| `08` | 0C8C | **Flush the write cache.** Parameter: drive byte. Bit 3 set: flush the dirty buffers of **all** drives (FE9E = FF skips the per-drive check at 1067). Clear: flush only this drive's. Replies: parameter status, then the result once the physical writes (and verifies) are done; an error has bit 0 added (`41` for a write-protected disc) | yes, after writes (`20`) | trace + IDC3 |
| `09` | 0CA3 | **Not implemented** (answered as an unknown command): the 7 bytes are the IDC's internal drive table, not yet decoded, and nothing traced uses `09`. Get drive parameters. Parameter: drive 0–7 (else `20`). Replies: status, then 7 bytes from the drive table (IY+8 … IY+14) | not seen | IDC3 |
| `0A` | 0CC0 | **Upload to unit RAM.** Address and length (2 bytes each, low byte first), then status `00` or `20`. On `20` (address below E000, or address + length past FFFF) one more byte follows, from `A`: `20` on overflow, otherwise the length's high byte, and the command ends. On `00`: the data, then a 16-bit check, where the data's byte sum plus the check must be 0 mod 65536; reply `00`, or `10` if not [IDC3 0cc0, 0ef6, 0ee3, 0e78]. The 2 KB RAM is mirrored through E000–FFFF. Implemented exactly (`src/core/idc.c`) | not seen | IDC3 |
| `0B` | 0CF0 | **Execute code in unit RAM.** Address (2 bytes, low byte first). Below E000 the IDC returns to its idle loop with no reply. Otherwise it replies `00`, `CALL`s the address, and sends whatever the code leaves in `A` [IDC3 0cf0, 0ce7]. **Our device can't run the code** (see "Implementation decision"): it replies `00`, reports it on the console (address only; saving the 2 KB RAM to the card is still to do), and sends `40` as the result | not seen | IDC3 |
| `0C` | 0D01 | **Format track.** Drive, track, `n` (must equal the format's sector count, else `20`), then `n` bytes into E400: the sector numbers in the order they are laid on the track (the interleave). Builds the track and writes it with FDC Write Track (`F4`) [IDC3 0317, 0363–03b7]; then re-initialises the drive tables | FORMAT, FASTCOPY | IDC3 |
| `0D` | 0D35 | **Verify track.** The same parameters as `0C` (drive, track, `n`, `n` sector numbers). Reads each listed sector in turn and stops at the first error [IDC3 04e8–0507] | FORMAT | IDC3 |
| `0E` | 0D6B | **Read a physical sector by its ID.** Drive/flags, track, **sector ID** (the ID as recorded, any value, not a record number). Reads that one sector (FDC Read Sector `80`) [IDC3 02a1], then sends the result, `n` (= 128-byte blocks in the sector: `04` for 512 bytes) and `n` × (128 bytes + ack). Seen: `<0E >00 <20 01 05 >00 >00 04 [4 × (128 <00)]`. This is how software reaches non-standard sector IDs | 4MATION games, VERIFY, FASTCOPY, SYSGEN | trace + IDC3 |
| `0F` | 0D9D | **Write a physical sector by its ID.** Drive/flags, track, sector ID. The unit sends `n` (blocks), receives `n` × 128 (with parity ack/retry), writes the sector (FDC Write Sector `A0`) [IDC3 02f2], then sends the result | FORMAT, FASTCOPY | IDC3 |
| `10` | 0D99 | **Write a physical sector by its ID, with verify.** As `0F`, then reads the sector back and compares it byte for byte [IDC3 02c6–02f1]; a mismatch sets bit 1 of the result | not seen | IDC3 |
| `11` | 0D66 | Version: replies `31` (`'1'`) | not seen | IDC3 |
| `12` | 0DCF | Aux port: set the baud rate. Parameter: code 0–7 (else `20`). CTC1/CTC2 from the table at 0DF6, SIO A set to 8N2 | not seen | IDC3 |
| `13` | 0E06 | Aux port: transmit stream. The 480Z sends `10 xx` to output `xx` on channel A, and `16` to finish (reply `00`) | not seen | IDC3 |
| `14` | 0E42 | Aux port: receive status. Replies `00`, or `FF` if a character is waiting | not seen | IDC3 |
| `15` | 0E5F | Aux port: wait for a received character and pass it to the 480Z | not seen | IDC3 |
| `16` | 0A7C | No-op; back to idle | not seen | IDC3 |
| `17` | 0E37 | Aux port: transmitter status. Replies `00`, or `FF` if ready | not seen | IDC3 |

**Commands seen so far:** CP/M uses `00`, `FF`, `01`, `02`, `04`, `05`, `06` and `08`. The 4MATION games add `0E`
and `0D`. RM's FORMAT 4.0E (formatting drive B double-sided, double density, in MAME, 2026-11-20) uses `06` to probe
the disc (it replied `0E` for a quad-density disc and `0A` for double density), then `0C` and `0D` once per track and
side (80 each), and `0F` to write cylinder 0 sector 01 on each side:

```
<0C >00 <01 00 10 >00 <01 02 03 … 10 …     format: drive 01 (side 0; 03 = side 1), track 00, 16 sectors, then their IDs
<0D >00 <01 00 10 >00 <01 02 03 … 10 …     verify the same track against the same ID list
<0F >00 <01 00 01 >00 01 <[128 bytes] …    write sector ID 01: n = 01 block of 128 (FM track 0)
```

RM's VERIFY 4.0D (drive A, both sides, in MAME) uses `01`, `05`, `06`, `0E` (32 times) and `02` (2855 records), and
finishes with "Disc verification completed".

RM's utilities, traced in MAME (2026-11-21; each is a MAME comparison scenario): **FASTCOPY 4.0E** (A to B, both
sides, reformatting) uses `06`, `0E`/`0F` and `0C` for the FM track 0, then `0C` and `03` for every other track,
with `08` after each side. **SYSGEN 4.0D** uses `0E` (track 0) and `03`. **COPYFILE 4.0C** uses `02`, `04` and `08`.
FORMAT 4.0E in quad density formats 80 tracks on each side (`0C`/`0D` for tracks up to `4F`; scenario
`format_qd`).

**`0A`/`0B` (code run inside the unit) have not been seen**: not in CP/M, the 4MATION loaders, FORMAT, VERIFY,
FASTCOPY, COPYFILE or SYSGEN. They can't be ruled out by reading the boot block either: it sends command bytes passed
in by its callers (no `ld a,04` exists in it, yet CP/M sends `04`). Other software could still use them.

**Commands needed for ordinary CP/M use** (boot, `DIR`, `TYPE`, `SAVE`, `ERA`, loading programs):
`00`, `FF`, `01`, `02`, `04`, `05`, `06` and `08` [trace, 2026-11-20]. The rest are probably used by RM's utilities
(`FORMAT`, `VERIFY`, `FASTCOPY`, `COPYFILE` and `SYSGEN` are all on the system disk). `0A`/`0B` would let 480Z software
run Z80 code inside the unit, which a C reimplementation can't do. **We need to find out whether anything uses
them.**

**The aux port:** commands `12`–`15` and `17` make SIO channel A a second serial port, presumably for a printer. In
our device it could map to a UART or to USB.

**The unit's cache:** the unit keeps 512-byte physical sectors (four records each) in a write-back cache
[IDC3 108f–10b9]. CP/M flushes it with `08` after writes. A delay between a write's reply and the physical write is
invisible to the 480Z, apart from the result of `08`.

### Implementation status (2026-11-21)

`src/core/idc.c` and `src/core/idc_disk.c` implement every command except `09` (not decoded, never seen) and the
aux-port commands `12`–`15` and `17` (the extra serial port; to be mapped to a UART or USB later). `0B` is reported,
not run. Choices made where the ROM leaves the device free, or where the image stands in for hardware:

- **Drive type is a setting** (`idc_set_drive_type`), standing in for what the IDC finds out about its drives:
  40-track, 80-track, or (the default) following the image, where more than 42 cylinders makes an 80-track drive.
  The track limit is the drive's, 80 or 40 [IDC3 028b sets IY+5 = 50h for an 80-track drive; checked at 0f14, 0d07,
  0d3b], so an 80-track drive can format and read tracks past the end of a 40-track image (MAME: FORMAT in quad
  density). `06` answers bit 1 MFM,
  bit 2 "80-track disc", bit 3 "80-track drive": `0A` for a 40-track disc and `0E` for an 80-track one in an
  80-track drive, as in MAME, whose MQ2 has 80-track drives (`525qd`); a 40-track image in a 40-track drive
  probes as `02`. The MAME comparison sets 80-track.
- **Sector IDs in `0E`/`0F`/`10` are range-checked like records** (1 to the track's record count), as the IDC's shared
  parameter routine does [IDC3 0f2b]. IDs above that (such as the 4MATION `A6`) are only reachable through `0D`,
  which is how the 4MATION loader uses them.
- **Writes:** records (`03`/`04`) go into a one-track cache per drive, which also notes, for each of the IDC's
  4-record buffers, which records have been written and whether the buffer's sector is missing [IDC3 IX+7, 1221–126e].
  The record completing a buffer writes it at once; its error is that command's result, plain (`10` for a missing
  sector, `40` write-protected). The rest reach the image on `08` or a track change, where an error has bit 0 added:
  as the result of `08`, or as the result (after a `00` status) of the read or write that changed track. A track
  whose write failed is dropped, as the IDC marks its buffer clean. `0C`, `0F` and `10` write at once, so their
  result carries a plain write-protect error (`40`) [IDC3 02f2–0316: masked with `FC`].
- **The IDC has two 512-byte buffers, freed least recently used; we keep the whole track.** The two can differ in
  *when* a part-written buffer's error shows (which later command reports it), and a failed write-through drops
  our whole track, where the IDC keeps its other part-written buffer for `08`. After every `08`, as CP/M sends it,
  and for writes of whole sectors in turn, as SAVE, PIP, FASTCOPY and SYSGEN make them, they agree.
- **`0C`** lays the sectors down in the order given, with E5 data and the ID's H byte 0, where the IDC's Write
  Track puts them. The IDC sends a template per format, run-length coded in the ROM [IDC3 04d4] and sent by
  0363–04aa; WD's `F5`/`F6`/`F7` become A1/C2 sync bytes and the two CRC bytes:
  - **FM 16 × 128** [036d, 085b]: 40 × FF, 6 × 00, index mark, 26 × FF (73 bytes); then each sector 6 × 00, ID
    mark, C H R N, CRC, 11 × FF, 6 × 00, data mark, 128 × E5, CRC, 27 × FF (188 bytes); FF to the index.
  - **MFM 9 × 512** [0409, 08a1]: 80 × 4E, 12 × 00, 3 × C2, index mark, 50 × 4E (146 bytes); then each sector
    12 × 00, 3 × A1, ID mark, ID, CRC, 22 × 4E, 12 × 00, 3 × A1, data mark, 512 × E5, CRC, 80 × 4E (654 bytes),
    the IBM layout; 4E to the index.
  - The ID's C is the track, H is 0, N is 0 (FM) or 2 (MFM). The table also has FM 26 × 128 (gap 3 29 × FF) and
    MFM 26 × 256 (gap 3 60 × 4E) [03c0, 0457]: 8-inch layouts, too long for a 5¼-inch track.
  - We always format the cylinder asked for. The IDC can miss it after a drive switches from double to quad
    density (an IDC bug we don't reproduce: "Switching a drive from double to quad density", below).
- **Changing a disc** (the menu, `docs/sd-card.md`; owner, 2026-11-21: instant). The protocol has no disc-change
  signal: the 480Z learns of a new disc only from what it reads (CP/M from its directory checksum, which makes a
  changed disc read-only until a warm boot). So the image changes between two commands, never during one, and the
  next command finds the new disc; an empty drive answers `80`, as `no_disc` does in MAME. The drive's cached track
  is first written back to the image it came from, as `08` would (`idc_change_disc`): a real IDC with a written
  buffer would write it to whatever disc was then in the drive, which is never what was meant. If that write-back
  fails, the change is lost, as with a failed `08`, and the console says so.
- **IMD tracks are laid out with standard IBM gaps** (`disk_imd.c`, `imd_lay_out`), as MAME lays them out:
  gap 4a, IAM and gap 1 (146 bytes MFM, 73 FM), then 62 (MFM) or 33 (FM) bytes plus the data and gap 3 per
  sector, gap 3 being 80 for 512-byte sectors and 50 for smaller ones, shrunk to fit. So the 4MATION loaders meet
  the two sectors 08 at the same moments as in MAME (the MAME comparison boots both games byte for byte).

### Parameter bytes

Read and write take three bytes: **`drive`, `track`, `record`** (`0E`/`0F`/`10`: a sector ID instead of a record).

**The drive byte** [IDC3 0f77, 0e83, 053b, 0b6f; traces of CP/M, FORMAT and VERIFY, 2026-11-20]. After the IDC's
parity swap (low two bits `01` ↔ `10`; `idc_param_byte()`):

| Bit | Meaning | Evidence |
|---|---|---|
| 0 | **side** | VERIFY sends `22` for side 1 (drive select port shows the side bit, `BE`); FORMAT sends `03` for drive B side 1 |
| 1–2 | **physical drive**, 0–3 = A–D | FORMAT of drive B sends `01`, which becomes `02` |
| 0–2 together | a "logical drive", one side of one drive; the 8 entries of the drive map (FEA4–FEAB) are these, which is why `05` swaps them in pairs | IDC3 0527, 0b8d |
| 5 | **MFM** (double density). Clear for FM | CP/M sends `00` for track 0 (FM) and `20` elsewhere; it sets the drive's density flag (IY+1 bit 1) [IDC3 0e94] |
| 6 | probably "double-step": a 48 tpi (40-track) disc in a 96 tpi drive. Set by the probe (`06`) on QD drives [IDC3 0c35] | inferred; seen once during boot (`40 00 02`) |

**Track** is the disc's own cylinder, checked against the drive's track count (IY+5) [IDC3 0f14]. In MAME's 80-track
drives, the IDC double-steps a 40-track disc: for logical track 3 it sets the WD1793 track register to 4, seeks to 6,
and sets it back to 3 [trace]. An image-based drive just uses the track as the cylinder, and can ignore bit 6.

**Record** is 1-based, in 128-byte CP/M records, and checked against the format's count, whatever the track holds
[IDC3 032c, table 084b, check at 0f2b]: FM has 16 records (16 × 128), MFM 36 (9 × 512). The IDC doesn't reject
record 0; what it then does is not known (we answer `20`).

- **FM:** record n is sector ID n. On track 0 the IDC reads IDs in sequence; elsewhere it applies two tables (0957,
  then 099b) whose composition is the identity [IDC3 0717, 0746].
- **MFM:** record r is in logical sector k = (r − 1) / 4, at offset ((r − 1) mod 4) × 128, and logical sectors are
  laid down with a **2:1 skew**: k = 0…8 is sector ID `1 6 2 7 3 8 4 9 5`, i.e. ID = 5k mod 9 + 1 [IDC3 table 09c7;
  trace: track 01 record 05 is ID 6]. MFM on track 0 is not traced or worked out (the IDC's cache fill reads IDs in
  sequence there [IDC3 0746]); we leave it unskewed.

**Each side of a disc is a CP/M drive of its own** [Link 480Z Disc System User Guide, ch. 5 "Disc Drives" (pp. 5.5–5.7); traces]. The low two bits of the drive byte, *as the
480Z sends them*, are the CP/M drive: A: `x0`, B: `x1`, C: `x2`, D: `x3`. After the IDC's parity swap that is
A: = drive 0 side 0, B: = drive 1 side 0, C: = drive 0 side 1, D: = drive 1 side 1. In the guide's words, A: and C:
are the left drive's label side and other side, B: and D: the right drive's. Each side is formatted as a whole disc:
track 0 FM 16 × 128 (the system track), then 1–39 MFM 9 × 512. On the system disc both sides are laid out that
way. CP/M's A: is `00` (track 0) and `20` (tracks 1–39), and never touches side 1. In the traces, only the
utilities told to do both sides (VERIFY, FASTCOPY) send `02`/`22` (C:) [traces of boot, read, write, verify,
fastcopy, 2026-11-20/21].

The guide's ROS command `X` loads CP/M from the right-hand drive and exchanges the names, so that it holds A: and
C:, and the left-hand drive B: and D:. That is presumably the drive map (`05`, and the 8-entry table at FEA4)
swapped in pairs, but `X` hasn't been traced. A one-drive 480Z uses A: and B: for the same side of whichever disc is in, and C: and D: for the other: its
firmware asks for the disc to be changed and swaps the names [same pages].
The drive types themselves (40 or 80 tracks) the IDC finds by stepping the head at login ("Open questions").

### Disk geometry: the CP/M 2.2F system disk [image, trace]

`480Z_RML_system-cpm2.2f-basic5.3g-basic6.3b.imd`, "40-19954 double density":

| Cylinders | Encoding | Per side | Sides |
|---|---|---|---|
| 0 | FM (IMD mode 2, 250 kbps) | 16 × 128 bytes | 2 |
| 1–39 | MFM (IMD mode 5, 250 kbps) | 9 × 512 bytes | 2 |

The 480Z addresses track 0 as records 01–10 (drive byte `00`) and the other tracks as records 01–24 (`20`):
36 × 128 = 4608 bytes, one side of a data track. The track byte is the cylinder (hex in the traces: the SAVE in the
write trace went to track `21`, cylinder 33), and the side comes from the drive byte, each side being a CP/M drive
of its own ("The drive byte").

### Copy-protected discs: 4MATION, 1985 [HFE + IMD, decoded by src/flux]

`480Z_4MATION_Dragon_World_1985` and `480Z_4MATION_Flowers_of_Crystal_1985` have the same protected layout. The HFE
(flux-level) and IMD images of each agree sector for sector: 730 sectors, with the same IDs, order, status and data
(the `hfe_vs_imd_*` tests).

| Where | Layout | Normal RM layout |
|---|---|---|
| Cylinder 0, both sides | FM 16 × 128, IDs `0D 0E 0F 10 09 0A 0B 0C D3 5C 57 72 01 02 03 04`: IDs 5–8 are replaced by `D3 5C 57 72` | IDs 1–16 |
| Cylinder 2, both sides | MFM **8** × 512, IDs `05 08 04 03 07 02 06 01`, then about 1.6 KB of empty gap (no 9th sector) | 9 × 512, IDs 1–9 |
| Cylinder 3, both sides | MFM 8 × 512 with arbitrary IDs `A6 67 32 09 41 29 1D 2A`: **the protection** (owner, 2026-11-20) | 9 × 512, IDs 1–9 |
| Cylinders 4–39 | normal | |

A full scan of every track (each A1 sync run and mark, checked independently of `src/flux`) finds no hidden or
malformed sectors. Every sync run is 3 × A1, every mark is FE or FB, and every ID CRC is good. The protection is in
the sector numbers alone, plus cylinder 2's missing ninth sector. **Every side-1 sector records head `00` in its ID**
(all 349 of them), so the emulator must match sectors on R (and C), never on the ID's H byte.

**Confirmed on the original discs (SCP captures, 2026-11-20).** Both discs have the same mastering:

| Where | Real layout (rotational order) | What the old IMD/HFE kept |
|---|---|---|
| Cylinder 2, both sides | 9 × 512, evenly spaced: `05 08 04 08 03 07 02 06 01`. The two 08s are 2nd and 4th. On side 0 the 2nd is the copy in the old Dragon World image (byte 2 `BB`) and the 4th is the one in the old Flowers image (`3B`); on side 1 they are the other way round | one 08 (different in each), no 9th sector |
| Cylinder 3, both sides | 9 × 512: `A6 67 32 09 41 29 29 1D 2A`, **two sectors numbered 29**, all `E5` | 8 sectors, one 29 |
| Cylinder 0 | FM 16 × 128, IDs as in the table above | the same |
| Cylinders 40–41 | blank | (not imaged) |

The "1.6 KB gap" in the old images was where the lost sector belonged. The 2026-11-20 images built from the captures
(`tools/scpdump/rebuild.py`) keep this layout and boot in MAME.

**The check that matters is two sectors numbered 08 on cylinder 2** (found 2026-11-20 by tracing the loader in MAME;
the owner had pointed at an odd sector number).

- After loading cylinder 1, the loader reads cylinder 2 sector 08 with `0E` into 0300. It decrypts it in place with
  self-modifying code at D791: per byte `a = (hl); rlc a; xor b`, with `b` counting down from 80h in each 128-byte
  block (the opcodes `CB 07 A8 00` are patched in from D7F0). It then checks byte 2: Dragon World's loader does
  `cp 08` at 0127, Flowers of Crystal's does `cp 09`. On failure it waits (ROS `RST 30h` delay, with a counter
  stepping by 15h so the wait changes each time) and reads again, for ever.
- **Each loader wants the copy its own image doesn't have.** Dragon World's sector 08 has byte 2 = `BB`, which
  decrypts to `09`; Flowers of Crystal's has `3B`, which decrypts to `08`. The two sectors differ in 7 bytes per
  256-byte half (each sector is two identical halves): offsets 02, 28, 46, 48, 4D, 4E, 4F. Both have good CRCs, so
  this is not a weak bit. **Each original disc carried both copies**, one where the imaged sector 08 is and the other
  presumably in the 1.6 KB gap at the end of the track. The loader's changing delay makes the drive meet the other
  copy first sooner or later. Both surviving images (IMD, and the HFEs made from them: their sector positions are
  identical on every track, so they are synthesised, not flux captures) kept only one copy each, a different one in
  each case. **The images are incomplete, not corrupt.**
- **Repaired images**, made with `tools/imd/add_duplicate_sector.py` (each game's missing copy taken from the other
  game's image, appended as a ninth sector 08 at the end of cylinder 2, both heads), boot in MAME. Dragon World
  reaches its title screen ("Press SPACE BAR to continue") and Flowers of Crystal its menu. They are kept in
  `background-docs/repaired/` and `testdata/images/` as `*_both-sector-08.IMD`.
- After the check, both games also use `0D` (verify track against a list of sector IDs), presumably the cylinder 3 IDs.
- **Why one copy can never pass:** the loader is itself the decrypted sector 08 (it runs from 0100/0300), and offset
  28 of the sector is the operand of its own `cp` (`FE xx` at offset 27, executed at 0127). Offsets 02 and 28 are two
  of the 7 that differ between the copies. Each copy's byte 2 decrypts to the value the *other* copy's code compares
  against, so the first read loads a loader that insists on getting the second copy.
- **The HFEs don't have the second copy either** (checked 2026-11-20). Their cylinder 2 tracks are bit-identical to each
  other apart from sector 08's data and CRC, and the end gap is plain `4E` filler.
- There is a report of the Dragon World HFE loading on real hardware from a FlashFloppy Gotek, and Flowers of
  Crystal's not. That contradicts the above for *these* files. The working Gotek HFE may be a different file (for example a
  flux capture with both copies). Compare it with the HFE here (md5 `5ce28980f7c8446971e7ae183459cea0`), and test the repaired IMDs on the Gotek
  (FlashFloppy reads IMD directly).

**What this means for the emulator:** a track can hold several sectors with the same ID, and *which one a read
returns must depend on where the disc is in its rotation when the command arrives*, as on a real drive. The device
therefore has to model rotation: time since the index, the bit rate, and each sector's position on the track (from
an HFE, or laid out with standard gaps from an IMD). It must not always return the first match. Otherwise this protection
loops for ever, exactly as it does with a single-copy image.

The loader reads by physical sector ID with `0E`, which is how a protected title reaches sectors that CP/M's
record-based `02` can't address. **The emulator must therefore keep each track's real sector IDs, in rotational order,
and probably their positions.** An IMD keeps the IDs and order but not the positions or gap sizes. An HFE keeps all
of them.

### Read record (`02`) [trace, IDC3 0b2f, 0f3d]

```
480Z: 02                     unit: 00            command accepted
480Z: <drv> <trk> <rec>      unit: 00            parameters OK  (else 20 and abort)
                             unit: <result>      00 / Fn = OK, 80 = not ready, …
                             unit: 128 data bytes      (only if result OK)
480Z: <ack>                                      00 = good; anything else = send the 128 bytes again
```

### Write record (`04`; `03` the same) [trace, IDC3 0b3d, 0b56, 0f58]

```
480Z: 04                     unit: 00
480Z: <drv> <trk> <rec>      unit: 00
480Z: 128 data bytes         unit: 00 = good  /  FF = parity error, send the 128 bytes again
                             unit: <result>      00 (into the cache)
```

Seen during `SAVE 2 T.COM`: `<04 >00 <20 21 19 >00 <[128 × FF] >00 00`, then `<08 >00 <20 >00 … >00`.

## Observed boot (CP/M 2.2F, IDC3, MAME)

This is the summarised output of `tools/mame/summarise.py`. `<` is 480Z → unit and `>` is unit → 480Z.

```
<00  >5A 00 01  [125k]                      go fast
<FF  >00 E0 95 00 F3 ED .. [1049 bytes]     download boot code into the 480Z
[9600]  <00  >5A 00 01  [125k]              (RTS/NMI) new exchange
<05  >00  <00  >00                          set option 00
<01  >00  <00  >00 >00                      log in drive 0
<02  >00  <00 00 01  >00 >[128]  <00        read T0 R1 (boot sector)
<06  >00  <00  >00 >00 0A                   format query
<02  >00  <00 00 02  >00 >[128]  <00        read T0 R2
... T0 R3–R10, then T1–T2 R1–R24 (CP/M system tracks), then T3 directory ...
→ "56k CP/M vers 2.2F … For 480Z Disc Systems / Quad and Double Density"  A>
```

## Implementation decision (2026-11-20)

We **reimplement the unit's command set in C** (high-level emulation) and serve disk images directly. We don't run the
IDC ROM. This keeps the firmware free of RM code.

The one piece of RM code the 480Z needs from the unit is the **boot block** sent by `FF`: 1049 bytes of Z80 code that
run on the 480Z. We handle it in two stages:

1. **First:** the firmware reads just that block from an IDC ROM file the user puts on the SD card (`docs/hardware.md`).
   `idc_find_boot_block()` finds it by locating the `FF` handler's code (`ld bc,(len) / ld hl,block`), so any
   revision laid out that way works: IDC3 gives 1049 bytes at 1B2A, IDC5 1181 bytes at 1B2A. Everything else is our own code.
2. **Then:** a clean-room GPL Z80 replacement, written from a specification of what the block must do (the ROS hooks it
   installs, the entry points and RAM it provides to the CP/M BIOS), never from its code. Once it boots CP/M in MAME
   and on a real 480Z, it becomes the default, and the ROM file becomes optional.

**`0A`/`0B` (2026-11-20):** `0A` is implemented exactly, because it only stores bytes. Running what `0B` points at
would need a Z80 plus the IDC ROM (the uploaded code would call its routines, such as `RST 30h` services) plus the
WD1793, SIO and CTC it drives: in effect running the real ROM, for this one path. Nothing traced uses either command
(CP/M, the 4MATION loaders, FORMAT, VERIFY). So `0B` is reported, not run: the console shows its address (saving the uploaded RAM to the SD card is still to
do), and the 480Z gets `40` as the result so it doesn't hang. If real software turns out to use it,
the saved code shows what it does. Then either reimplement that routine in C (recognised by a checksum) or add a Z80
core with the ROM from the SD card and a WD1793 model on `src/core/disk`.

The Pico UART needs: 8E1, live switching between 9600 and 125000 baud, DTR gating done in software (a GPIO, not
hardware flow control), and an edge interrupt on DCD (the 480Z's RTS) in place of the NMI.

**How the Pico does it** (`src/pico/link_uart.c`, 2026-11-21): UART1 at 8E1. Its divisors give exactly 9600 and
125000 baud from the 150 MHz peripheral clock; the IDC's 9600 is really 9615. The 480Z's CTS is asserted while
`recv()` waits, rather than pulsed at each poll; either way, the 480Z may send only while the unit is waiting.
The 480Z's RTS going asserted interrupts: CTS drops at once, and the next link call returns a reset with both FIFOs
emptied. The PL011 can't abort the character it is shifting out, so that one byte can still reach the 480Z after
its RTS. The unit is back at 9600 and receiving within 50 us of RTS; the SDK's own UART calls would leave the
receiver off for 1.6 ms. A byte is started only while the 480Z's DTR is asserted and the transmitter is idle, which is the transmit half
of auto enables: the SIO checks CTS as each character starts. The receive half (ignoring bytes while the 480Z's RTS is down) is **not**
reproduced. The 480Z sends only when the unit asserts CTS, and dropping a byte wrongly would hang the exchange.
Whether the 480Z ever sends with RTS down is an open question.

## Open questions

- [x] Which IDC revision: **3.1 (`IDC3-1I`) was in the double-drive units, 5.1 (`IDC5-1J`) in the single-drive
      ones** (owner, 2026-11-21). This is a double-drive unit, so 3.1, which is also MAME's. The ROMs differ in about
      3900 of 8192 bytes; the differences found so far are the version byte (`51`), the drive-type test stepping
      up to 49 times instead of 44, and a drive-byte mask in 0f77 (below). Both give a boot block the firmware
      finds.
- [x] Why the ROS (1.2, on a real 480Z) couldn't load the boot block from the unit at 125000: its receive interrupt
      runs up to 388 us late just after `FF`, and the unit's fourth byte overran its SIO. Fixed by pacing the block,
      two bytes per DTR pulse (2026-11-24; "Framing and baud rate", "Flow control").
- [ ] The drive byte's bits 3, 4 and 7, and bit 6 (inferred as double-step; "The drive byte"). Bits 0–2 and 5 are
      decoded.
- [x] Trace the utilities (`FORMAT`, `VERIFY`, `FASTCOPY`, `COPYFILE`, `SYSGEN`): done, 2026-11-21 ("Commands seen
      so far"). `03` is used (SYSGEN, FASTCOPY); `07`, `09` and `10` are not.
- [ ] **Does anything use `0A`/`0B` (code executed inside the unit)?** Not CP/M, the 4MATION loaders, or any of RM's
      utilities above. Still to check: other software, other protected titles.
- [x] **How the IDC finds each drive's type** (40 or 80 tracks): **by stepping the head**, each time a disc is
      logged in. It steps in 45 times, then out 44; if the drive's track-0 sensor isn't on at the end (WD1793
      status bit 2), the drive has more than 44 tracks, and it is marked 80-track: IY+1 bit 4, IY+5 = 50h
      [IDC3 0255–0297; IDC5 the same, stepping out 49]. The ROM reads no links for it: port 04 is read once, its
      bit 0, before a drive is selected [IDC3 05ef], which is not a type. (The owner's recollection of links on the
      board isn't borne out by IDC 3.1's ROM, 2026-11-24.) Our device has no head to step, so the type is a setting
      that stands in for the test's result (`drive-a`, `drive-b`): by default it follows the image, 43 cylinders or
      more making an 80-track drive. The 480Z sees the type only in `06`'s answer and the track limit, so it gets
      the same answer as from a real IDC with that drive. Command `09` returns the IDC's drive table, still not
      decoded; nothing traced uses it ("Command table").
- [x] **Switching a drive from double to quad density sends the next seek to the wrong track (an IDC bug; we
      don't reproduce it).** Decided by the owner, 2026-11-21. It leaves stale data; it doesn't break discs.
      - *What happens:* in MAME's quad-density FORMAT (2026-11-21), drive B had last been used in double density
        at cylinder 2, with the head on physical track 4. `0C 41 00` then seeks to cylinder 0 in single steps
        (`TRK 02`, `FDC 18`), so the FM track for cylinder 0 side 0 goes to **physical track 2** (logged in
        MAME's `floppy_image_device::write_flux`). The `0D` verify that follows reads physical track 0. On a used
        disc it finds the old sectors 01–10 and passes, so the disc keeps its old cylinder 0 side 0 (valid FM
        16 × 128, stale data in the system area) except sector 01, which `0F` rewrites. If the track has no good
        sectors, the verify fails (`10`) and FORMAT 4.0E formats it again, now correctly; the disc comes out
        fully formatted (MAME, 2026-11-21: cylinder 0 side 0 emptied first; `0D` gave `10`, then `0C` again, `0D`
        `00`). Physical track 2 is reformatted later, so nothing else is affected. Our link bytes match MAME's
        in the used-disc case; in the blank-track case MAME sends the retry and we don't need one.
      - *Why:* the seek [IDC3 0667–06df] loads the track register with the drive's remembered cylinder (IY+7). It
        doubles both registers only for a 48 tpi disc in an 80-track drive (06bb–06d5: IY+1 bit 4 set, bit 0
        clear, drive byte bit 6 clear). IY+7 doesn't record which way the head got there, and the seek re-homes
        only a drive whose remembered cylinder is 0 [067c–068d]. `0C` re-initialises the drive tables *after*
        its Write Track [031e → 0f86], so the `0D` that follows runs the stepping test and restore, and every
        later seek is right. FORMAT 4.0E's own `06` probe leaves the head at cylinder 2, so quad-density FORMAT
        over a used double-density disc should always hit it.
      - *IDC5 is the same:* its seek routine is byte-for-byte IDC3's. Its other changes are the version byte
        (51), the drive-type test stepping up to 49 times instead of 44 [IDC5 0255, 0261], and a drive-byte mask
        (`AND F9`) in 0f77, which `0C` doesn't use.
      - *What we do:* what was meant to happen. We don't model head position, so `0C` formats the cylinder it
        is asked for, and the disc comes out properly formatted. Nothing relies on the bug, it's an edge case,
        and its only lasting effect is stale data where FORMAT should leave E5, so it's unlikely anyone ever
        noticed. This is a known difference from MAME (and, presumably, from the real unit): the used-disc case
        differs only in those sectors' contents; the blank-track case also differs on the link (MAME's verify
        fails once and FORMAT retries). The MAME comparison `format_qd` (the used-disc case) declares it
        (`differs B 0 0 02-10`). The link bytes and the rest of the disc match MAME's, so our quad-density disc
        is otherwise exactly what MAME's FORMAT leaves. Still worth a check on the real unit: format
        a used DD disc as QD, then read cylinder 0 side 0.
- [x] Dragon World / Flowers of Crystal looping in MAME: solved. The images lack the second sector 08 on cylinder 2
      (see "Copy-protected discs"); the repaired images boot.
- [x] Where on the original track the second sector 08 sits, and the exact interleave. It only mattered if a real
      480Z's loader, against our emulated rotation, failed to reach both copies. It doesn't: on the real 480Z
      (2026-11-23) Flowers of Crystal booted from the repaired IMD, and Dragon World from its IMD and from the HFE
      rebuilt from its flux capture (`docs/acceptance.md`). The exact position on the original disc is still
      unknown, and no longer needed.
- [x] Why the Dragon World HFE is reported to load from a Gotek. Dropped (owner, 2026-11-24): the HFEs rebuilt
      from the flux captures hold both sector 08s and both 29s, match the rebuilt IMDs in all 734 sectors
      (`tests/check_hfe_vs_imd.py`), and load and run, in MAME and on the real 480Z
      (`gw convert --tracks c=0-39:h=0-1 …_rebuilt_300rpm.scp …_rebuilt.hfe::bitrate=250`, Greaseweazle). The old
      HFEs here, synthesised from the incomplete IMDs, lack the second 08 and don't load.
- [x] How logical track numbers map to cylinder and side: the track byte is the cylinder, and the side is bit 0 of the
      drive byte, each side being a CP/M drive of its own (A:/C:, B:/D:; "The drive byte" above, from the user guide
      and the traces, 2026-11-21).
- [x] **The structure of the boot block** (2026-11-24, from IDC 3.1 and 5.1): records of a load address and a
      count (two bytes each, low byte first) and that many bytes, ended by a count of 0, then the 16-bit sum the
      ROS checks ("Framing and baud rate"). IDC 3.1's block is two records: 149 bytes at E000, a start-up stub,
      and 886 at F800, the 480Z's disc driver (IDC 5.1: 149 and 1018). The ROS jumps to E000. The stub hooks the
      driver into the ROS: its jump table over the ROS's vectors at FF6B and FECD–FEFF (so ROS service 48, the
      disc call that the CP/M BIOS makes with IX at a parameter block, goes to the driver), its own receive
      interrupt in the SIO's vectors (FC14, FC16), and a hook for the ROS's `B` and `X` commands (FF39). The driver
      takes received bytes into a 256-byte ring at E000, over the stub.
- [x] **The 480Z side: its timeouts** (2026-11-24, from ROS 1.2D, its CP/M BIOS and IDC 3.1's boot block). Only
      the ROS has one, and only while it loads the boot block: each byte, from the go-fast reply to the checksum,
      gets 256 calls of its "is a byte waiting?" service (2F) [ROS 1.2D 3eea–3ef2 for the first, 078e for the
      rest]. That is **144.2 ms** on ROS 1.2D, measured in MAME's `rm480za` with no unit (`00` sent, then
      "Disc Error" 144.2 ms later, with no retry); 144–145 ms on ROS 2.2B (`rm480z`). After that the 480Z's disc
      driver is the boot block's, and it has **no timeout**: it waits for each byte (f816) and for its transmitter
      (f828) for as long as it takes [IDC3 boot block]. So a slow or silent unit makes CP/M wait, not fail, until
      the 480Z is reset; MAME's 480Z waited 4.15 s for a slow FORMAT and 2 s for slow writes. Its one timed wait
      is after a parity error in a record it receives: it discards bytes until 256 polls in a row find none, then
      sends `FF` for the record again (f8d8). The CP/M BIOS doesn't time the link itself: it goes through
      service 48. RTS and DTR: "Flow control". Not checked on the real 480Z, whose ROS is the same 1.2D.
- [x] Image formats and geometries: IMD and HFE (v1 writable, v3 read-only), FM track 0 with DD or QD data tracks,
      40 or 80 cylinders, one or two sides (`docs/sd-card.md`; blank ones from `tools/imd/make_blank.py`).
- [x] A record written (`03`/`04`) to a sector that isn't on the disc (MAME, 2026-11-21): the IDC takes it into its
      buffer; the fourth record of the sector writes it and answers `10`. A part-written buffer fails on `08` with
      `11` [IDC3 112a, 109f]. Implemented; the `missing_sector` scenario matches.
- [ ] Record mapping for MFM on track 0, and what the IDC does with record 0 (see "Parameter bytes").

## Tracing with MAME

`tools/mame/mq2trace.lua` taps the MQ2 CPU's I/O space in MAME's `rm480z` driver. It logs link bytes, baud changes and
FDC activity, and can type keys on the 480Z. It runs without opening a window:

```
cp image.imd /tmp/work.imd     # MAME may write to the image, so use a copy
MQ2_KEYS="2.0:b B" MQ2_SNAP=29 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  $RMFE_MAME rm480z -rompath background-docs/mame-romsets -flop1 /tmp/work.imd \
  -autoboot_script tools/mame/mq2trace.lua -video none -sound none -nothrottle \
  -seconds_to_run 30 -skip_gameinfo > trace.txt
tools/mame/summarise.py trace.txt          # add --hw for FDC/drive-select detail, --full for every byte
```

With `rmfe_link` in the port instead (`-rs232 rmfe_link`, a real unit through `tools/mame/link_bridge.py`), there
is no MQ2 to tap: the script only types keys and takes snapshots, and the device logs the link itself to
`RMFE_LINK_LOG`, from the 480Z's side, with the same `RX`/`TX` names (`docs/testing.md` section 4).

`tools/mame/keys.py` types shifted symbols (`=`, `*`, …) as `Shift+<key>`, which `mq2trace.lua` turns into Shift
pressed 50 ms before the key (pressed together, the 480Z sees the unshifted key). `MQ2_SNAP` snapshots work
headless (`snap/rm480z/*.png` under MAME's working directory). An image file MAME can't write is mounted
write-protected. **MAME saves a 40-track image in its 80-track drives' geometry**: disc cylinder c is written at
cylinder 2c, the odd cylinders blank.

**MAME's rotation** (needed to compare the 4MATION loaders): a drive's index passes when the IDC first selects it
(`rm_mq2.cpp` port0_w → `floppy.cpp` mon_w starts the revolution clock) and every 0.2 s after; MAME lays an IMD track
out with standard IBM gaps (`flopimg.cpp` build_pc_track_mfm/fm). Measured in a trace: every read of the k-th
sector of a 9 × 512 track returns at the same phase, 0.0503 + 0.10464k of a turn after the first select time, plus
about 80 µs of IDC work.

`$RMFE_MAME` is a single-driver build of MAME, made in MAME's source tree with
`make SUBTARGET=rm480z SOURCES=src/mame/rm/rm480z.cpp -j$(nproc)`. Keys are pressed by ioport field name (for example
`b B` or `Carriage Return`), because this driver doesn't support MAME's natural keyboard.
