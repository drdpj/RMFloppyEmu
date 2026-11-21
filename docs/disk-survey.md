# Disc image survey

What we know about the 480Z and 380Z disc images on hand, from `src/flux`. This file holds only structural findings;
the images themselves are never committed.

## Capturing discs (Greaseweazle)

The preferred command for 480Z/380Z 5.25" discs (40 tracks, 48 tpi) on an 80-track (96 tpi) drive:

```
gw read --revs=5 --tracks=c=0-41:h=0-1:step=2 "<title>.scp"
```

- `step=2` double-steps so each 48 tpi track is read once, centred. Leave it out on a genuine 40-track drive.
- `h=0-1`: both sides. 480Z MFM discs are double-sided.
- `c=0-41`: two cylinders beyond the 40 in use, in case anything hides there.
- `--revs=5`: more revolutions to catch weak bits, and to meet both copies of a duplicated sector ID at different
  rotational positions (the 4MATION protection, `docs/protocol.md`).

Check each capture with `tools/scpdump/sweep.py build-tests/scpdump <outdir> "<title>.scp"`.

## NCEC captures: 44 SCPs, swept 2026-11-20

**Every capture holds one side only, with each physical track read twice.** The headers say 96 tpi with tracks 0–79,
and in all 44 files "side 1" is sector for sector the same as "side 0" (for example 638/638 identical sectors on
"Maths Disc"). They look like a 40-track disc read on an 80-track drive without double-stepping. The two reads land on
SCP track numbers 2k and 2k+1, which a reader takes as cylinder k sides 0 and 1. **The real side 1 of these discs was
not captured.** They are to be re-captured with the command above.

On the side that was captured:

- **Formats:** 380Z-style FM 16 × 128 (IDs 1–16) on 25 discs; 480Z MFM 9 × 512 (IDs 1–9) on 19. Track 0 is
  FM 16 × 128 on all of them.
- **No protection features:** no duplicate IDs, unusual sector numbers or sizes, wrong cylinder bytes, missing
  sectors, or weak bits (sectors CRC-good on two revolutions with different data) on any disc.
- **CRC errors** (media wear or damage): 13 sectors bad on every revolution and 35 bad on some revolutions only,
  spread over Maths Disc, HR Maths SC, Junmo 10 April, School Admin, TB Software - Economatics, Geography AUCBE
  Schools Council, Faculty Admin (the worst, 13 tracks), 380z Mosaic, RML Geography April 1981 and 380z Write.
- **Every FM data track ends with a stub:** an ID numbered 00, `E5` data and a bad data CRC, at the same place on
  every track and revolution. It is the end of the formatting write, cut off by its own start at the index. It is
  not damage and not protection. The sweep ignores it.

## 4MATION 1985: Dragon World, Flowers of Crystal

**Older images** (HFE + IMD, `testdata/images/`): both lost one of the two sectors numbered 08 on cylinder 2, and one
of the two 29s on cylinder 3. Their HFEs were synthesised from the sector images, so they lost them too.

**SCP captures of the originals** (`background-docs/*1985.scp`, 2026-11-20, 5 revolutions, 42 cylinders, both
sides). The protection layout is in `docs/protocol.md`.
- **The capture drive turns at 360 rpm** (166.7 ms per revolution), a 5.25" HD drive reading a disc written at
  300 rpm, so the cells are 1667 ns. The SCP header doesn't set its 360 rpm flag. `scpdump` now scales the cell to
  each revolution's measured time, so this needs no option.
- **Dragon World:** every sector reads good. 3 sectors differ from the old IMD, all CRC-good in both: track 0
  sector 04 on both sides (23 bytes from 0x68) and cylinder 30 sector 07 (2 bytes). The old image is presumably of
  another copy.
- **Flowers of Crystal: a radial scratch** at about ¾ of a turn crosses every cylinder. 79 sectors never read good,
  and 8 IDs are erased on every revolution (cyl 0 side 1 ID 02; ID 05 on cyl 33–38 side 1 and cyl 38 side 0). On
  cylinder 39 side 1, sector 05's data mark is gone. The older IMD has all of them, and agrees with the capture on
  all 639 sectors both can read.

**Rebuilt images** (`tools/scpdump/rebuild.py`, kept in `background-docs/repaired/` with a README, and in
`testdata/images/`):
- `*_rebuilt_300rpm.scp` / `*_rebuilt_360rpm.scp`: every track re-encoded as clean flux, each sector at its measured
  position. Flowers of Crystal's scratched or erased sectors come from the older IMD, and an erased ID goes into the
  empty slot the capture shows. Write with `gw write --tracks=c=0-39:h=0-1:step=2 <file>.scp`: use the 360 rpm
  version on the 360 rpm drive, so the disc reads back at 250 kbit/s at 300 rpm.
- `*_rebuilt.IMD` / `*_from-SCP-2026-11-20.IMD`: the same discs as IMD, for FlashFloppy or MAME.
- Verified with **three independent decoders**: HxC's `hxcfe` 2.16.13.1 finds 734 sectors and reads track 0 as
  FM 16 × 128; Greaseweazle's `gw convert --format=ibm.scan` finds 734/734, identical to ours; `src/flux` reads every
  sector good with the protection intact (`check_4mation_scp.py --all-good`). Every sector is within 17 cells (about
  one byte) of its position in the capture, and no gap is more than 1.2 times its track's usual spacing. The IMD made
  from each rebuilt SCP boots in MAME (Dragon World title screen, Flowers of Crystal menu).
- **The first build was faulty** (owner's report, from HxC). Each track started with a `0000` word: a transition
  exactly on the index gave a zero interval, which reads as "add 65536". HxC found only 702 sectors. Restored
  sectors 05 were also placed mid-gap, which put them about 3,400 cells too late on cylinders 33–38. Both are
  fixed, and `tests/check_rebuild_scp.py` (CTest `rebuild_scp_flowers_of_crystal`) catches both: on the faulty
  build it finds 80 zero words and 7 stretched gaps.
- **Still to do: write one to a real disc and boot it on the 480Z**, then read it back and sweep it.
