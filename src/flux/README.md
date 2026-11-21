# flux: floppy track decoding and image containers

A small C11 library that turns floppy disk images into sectors. It is written for RMFloppyEmu and intended for
reuse in Disk Wrangler (the planned `device/flux` module in DiskWranglerCore: "HFE, SCP, WOZ as container types that
present a `Physical`").

| File | What it does |
|---|---|
| `crc16.h/.c` | CRC-16/CCITT-FALSE, the FM/MFM field CRC |
| `ibm_track.h/.c` | decodes one revolution of FM or MFM bitcells into sectors. Each sector has its ID, data, ID/data CRC status, deleted mark, and its position in cells. Sectors come in rotational order, duplicate IDs are kept, a sector across the index is handled, and FM recorded at the MFM rate is supported |
| `hfe.h/.c` | reads HxC HFE images (v1 `HXCPICFE`, v3 `HXCHFEV3` with opcodes) and returns a track side as bitcells; writes a track side back in place (v1) |
| `scp.h/.c` | reads SuperCard Pro / Greaseweazle `.scp` flux images: header, track headers, revolutions, checksum. It **streams** flux through a 256-byte buffer, so an 18 MB file is never held in memory |
| `ibm_encode.h/.c` | the inverse of `ibm_track`: sectors (ID, data, mark, CRC error or no data) at given positions → FM or MFM bitcells for one revolution, FM optionally at double rate |
| `flux_pll.h/.c` | flux intervals → bitcells: Greaseweazle's PLL in fixed point (no floating point), with the index position reported and decoding carried on past it |

The pipelines:

- **HFE:** `hfe_read_track()` → bitcells → `ibm_track_decode_auto()` → sectors.
- **SCP:** `scp_read_track()` → `scp_flux_open()` per revolution → `flux_pll_decode(scp_flux_next)` → bitcells
  (one revolution, plus run-on past the index) → `ibm_track_decode_span_auto()` → sectors.

Other flux formats (WOZ, KryoFlux, A2R) need only a container that supplies flux through a `flux_next_fn`.
`tools/scpdump/scpdump.c` shows the SCP pipeline in full.

## Rules that keep it reusable

- **Self-contained.** It includes only its own headers and the C standard library: never `src/core`, the Pico SDK,
  or Qt. It has its own status enums, and no asserts that depend on the project.
- **No allocation.** The caller supplies every buffer: track cells, the sector table and the data area. It runs
  unchanged on the RP2350.
- **I/O by callback** (`hfe_read_fn`, `scp_read_fn`; flux by `flux_next_fn`), so the file can be a `FILE*`, a `QFile`,
  memory, or an SD card.
- **C++-friendly headers** (`extern "C"`), with fixed-width types and no macros that collide with Qt.
- **GPL-3.0-or-later**, with the full header naming Daniel Jameson on every file, the same as Disk Wrangler.
- Everything is reported, nothing is merged: protected discs depend on duplicate IDs, CRC errors, deleted marks,
  missing data and sector positions.

To use it in DiskWranglerCore, copy `src/flux/` as it stands. A `dw::device` container class then wraps `hfe_open`
and `hfe_read_track`, and maps each `ibm_sector_t` onto `dw::device::Sector`:

- `c`/`h`/`r`/`n` map directly.
- `IBM_SEC_DELETED` → `deleted`.
- `IBM_SEC_DATA_CRC_ERROR` → `crcError`.
- `IBM_SEC_NO_DATA` → `missing`.
- Duplicate IDs on a track → `Track::duplicateIds`.

## Tests

In RMFloppyEmu (`tests/`, run by CTest):

- `test_crc16` checks against the published check values.
- `test_ibm_track` decodes synthetic FM and MFM tracks built by a separate test encoder
  (`tests/support/ibm_encode.c`, written from the IBM System 34 layout). It covers every flag, duplicate IDs, the
  index wrap, capacity limits and FM at double rate.
- `test_flux_pll` turns synthetic tracks into flux (`tests/support/flux_synth.c`) with jitter and ±2–3% speed
  error, and decodes them back to the original sectors.
- `test_scp` uses SCP files built in memory, covering the header, track headers, empty trailing revolutions, overflow
  words, run-on across revolutions, truncation, the checksum, and the whole SCP → PLL → sectors path.
- `test_ibm_encode` encodes tracks and decodes them with `ibm_track` (positions, FM at double rate, deleted, CRC errors,
  no data, duplicate IDs, overlap refused). RMFloppyEmu's `test_disk_write` writes real HFEs; HxC and Greaseweazle read
  them back in full.
- `test_hfe` uses HFE files built in memory, covering v1 interleaving and partial blocks, the track 0 encoding
  override, v3 opcodes, and errors.
- `hfe_vs_imd_*` decode real discs with `tools/hfedump` and compare them with ImageDisk images of the same discs,
  read by an independent Python parser (`tests/check_hfe_vs_imd.py`).

## References

- SCP: Jim Drew's `scp_image_specs.txt`, as implemented by Greaseweazle (`src/greaseweazle/image/scp.py`). The PLL
  is Greaseweazle's `flux_to_bitcells` (`src/greaseweazle/track.py`).
- HFE: the HxC documentation as quoted in MAME (`src/lib/formats/hxchfe_dsk.cpp`), and Greaseweazle
  (`src/greaseweazle/image/hfe.py`) for the v3 opcodes.
- FM/MFM: the IBM System 34 format and the WD1793 and uPD765 data sheets, cross-checked with Greaseweazle
  (`src/greaseweazle/codec/ibm/ibm.py`).

No code was copied from these. They were used to check facts about the formats. The PLL follows Greaseweazle's
algorithm (Greaseweazle is public domain, under the Unlicense), reimplemented in fixed-point C.
