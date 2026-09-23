# steno-fw

Standalone steno firmware for an nRF52840 keyboard, built on ZMK for BLE, USB
and power management, with the dictionary compressed into internal flash.

MIT throughout. Deliberately *not* a Javelin port — Javelin is PolyForm
Noncommercial, which cannot be combined with ZMK's MIT code or published as
open source, and its dictionary format bakes in absolute pointers and expects
2–3 MB of memory-mapped external flash. This project targets 1 MB internal.

## Status

| stage | state |
|---|---|
| **1. Dictionary format, compiler, reader** | **done, verified** |
| 2. Steno engine — segmentation, spacing, caps | not started |
| 3. Orthography, numbers, fingerspelling | not started |
| 4. ZMK integration — chord input, HID output | not started |
| 5. Display wiring | driver done, not wired |
| 6. Memory map, linker, BMP flashing | not started |

## Stage 1 — what works

The whole Plover dictionary compresses to **796 KB** for all 147,424 entries,
or **694 KB** for 121,037 once the entries an orthography engine can regenerate
are dropped. 5.5 bytes per entry, from 4.2 MB of JSON.

The reader costs **1,142 bytes of flash, zero static RAM and 744 bytes of
stack**. No allocator, no RTOS dependency, no block cache.

Every single entry in both images has been verified to look up to exactly its
original translation:

```
$ ./tools/build.sh verify
image: 815372 bytes, 147424 entries, 70353 values, max 11 strokes, group 64
verified 147424 entries: 147424 ok, 0 wrong, 0 missing
group decodes: 2096672 over 147424 probes = 14.22 per lookup
PASS
```

### Why Re-Pair rather than zstd

| codec | key stream | decoder | RAM |
|---|---|---|---|
| zstd, 4 KB blocks + trained dict | 341 KB | 30–60 KB | 32 KB block cache |
| **Re-Pair + canonical Huffman** | **313 KB** | **~1 KB** | **none** |

Grammar compression is directly addressable — expanding a record touches about
3 symbols — so there is no block to decompress and no cache to hold it. Once
the decoder counts against the same flash budget as the data, zstd is *larger*
in effect by 5,000–11,000 dictionary entries.

Full reasoning, the format spec and all measurements: [`docs/FORMAT.md`](docs/FORMAT.md).

## Layout

```
dict/sdic.{c,h}       reader — portable C11, no dependencies
tools/mkdict.py       compiler — Plover JSON -> .sdic image
tools/dump_pairs.py   emits the verification corpus
tools/test_sdic.c     exhaustive verifier + negative tests
tools/build.sh        build, compile, verify
docs/FORMAT.md        format spec and measurements
display/              OLED driver (from the earlier stage)
```

## Building a dictionary

```sh
# everything, 796 KB
./tools/mkdict.py main.json -o dict.sdic

# hand the regenerable entries to the orthography engine, 694 KB
./tools/mkdict.py main.json -o dict.sdic --drop-derivable --drop-generated

# or just name a budget and let it trim
./tools/mkdict.py main.json -o dict.sdic --max-bytes 600K --freq wordfreq.txt
```

The full build takes about four minutes — Re-Pair recounts pair frequencies
across the whole stream on each of 6,000 iterations. Lower `--max-rules` for a
faster, slightly larger image.

## Flash budget

With the bootloader removed and ~32 KB kept for the settings partition (BLE
bonds must survive a reset), 992 KB is available:

| dictionary | leaves for firmware |
|---|---|
| 796 KB (full) | 196 KB — too tight |
| 694 KB (`--drop-derivable`) | 298 KB — workable if the ZMK build stays lean |
| 600 KB | 392 KB — comfortable |

A ZMK BLE build is typically 250–350 KB before the steno engine. **The honest
read is that 694 KB plus a real engine will not fit, and the target should be
600–640 KB of dictionary.** That is ~105k entries, which the size/entry curve
says costs little in practice since the tail of `main.json` is largely
misstrokes and rare fingerspelling variants.

This gets re-measured for real once stage 4 produces a linkable image; until
then the firmware figure is an estimate from comparable ZMK builds, not a
measurement.

## Dictionary updates without reflashing firmware

The image is position independent, so it can live in its own flash region and
be written separately from the firmware. With a Black Magic Probe:

```sh
arm-none-eabi-gdb -ex 'target extended-remote /dev/ttyACM0' \
  -ex 'monitor swdp_scan' -ex 'attach 1' \
  -ex 'restore dict.sdic binary 0x60000'
```

`sdic_open()` takes the base address, validates the magic and version, and
refuses a truncated or mismatched image rather than misbehaving.
