# SDIC — steno dictionary image format v1

A compressed, randomly-addressable steno dictionary sized to live in the
nRF52840's internal flash alongside the firmware.

## Why this shape

Measured on Plover's `main.json` (147,424 entries, 337,251 strokes, 70,353
unique translations):

| representation | size | notes |
|---|---|---|
| `main.json` as shipped | 4.2 MB | |
| 3-byte packed strokes + raw strings | 2,580 KB | |
| front-coded, sorted | 2,097 KB | |
| whole-image LZMA | 690 KB | **not randomly addressable** |
| zstd, 4 KB blocks + trained dict | 341 KB keys | + **30–60 KB decoder**, + 32 KB RAM block cache |
| **Re-Pair + canonical Huffman** | **313 KB keys** | + **~0.2 KB decoder**, no block cache |

Re-Pair wins on every axis that matters here. Grammar compression is directly
addressable — expanding one record touches ~3 symbols to a maximum rule depth
of 9 — so there is no block to decompress, no LRU cache, and no 32 KB of RAM
spent on buffers. Against zstd it is *smaller in effect*, because 30–60 KB of
decoder is 5,000–11,000 dictionary entries we would not be able to ship.

Three streams rather than one, because keys and English text have unrelated
statistics and interleaving them costs dearly:

| layout | size |
|---|---|
| **value pool + id stream** | **404 KB** |
| values inlined per entry | 666 KB |
| key and value interleaved | 1,061 KB |

## Design rules

1. **Position independent.** Every internal reference is a byte offset from the
   image base. No absolute pointers. The image can be linked at any address,
   relocated, or moved to external QSPI later without recompiling it. (This is
   the one thing Javelin's format gets wrong — its blob is only valid at the
   address it was compiled for, which is why its compiler must know your
   memory map.)
2. **Read through a seam.** All flash access goes through one accessor. On
   internal flash that compiles to a direct pointer dereference; pointing it at
   QSPI later is a one-function change.
3. **Little-endian**, matching the target. The compiler asserts this.

## Layout

```
+---------------------------+ 0
| header (128 B)            |
+---------------------------+
| key stream    (bits)      |   sorted packed stroke sequences
| key rules                 |
| key huffman table         |
| key group directory       |
+---------------------------+
| value pool stream (bits)  |   70,353 unique translations, sorted
| value pool rules          |
| value pool huffman table  |
| value pool directory      |
+---------------------------+
| id stream     (bits)      |   one value-id per entry, in key order
| id rules                  |
| id huffman table          |
| id group directory        |
+---------------------------+
```

### Header

| off | type | field |
|---|---|---|
| 0 | u32 | magic `'SDC1'` = 0x31434453 |
| 4 | u16 | version = 1 |
| 6 | u16 | flags |
| 8 | u32 | image_size |
| 12 | u32 | n_entries |
| 16 | u32 | n_values |
| 20 | u32 | max_strokes |
| 24 | u32 | group_shift (log2 of records per group) |
| 28 | u32 | reserved |
| 32 | 3 × `sdic_stream_desc` (24 B each) | key, pool, id |

Header is 128 bytes; the 24 bytes after the descriptors are reserved.

`sdic_stream_desc`:

| off | type | field |
|---|---|---|
| 0 | u32 | off_bits — Huffman-coded Re-Pair symbols |
| 4 | u32 | off_rules — `n_rules` × 2 × u16 |
| 8 | u32 | off_huff — 17 × u16 length counts, then `n_syms` × u16 canonical symbols |
| 12 | u32 | off_dir — `n_groups` × u32 bit offsets |
| 16 | u16 | n_rules |
| 18 | u16 | n_syms |
| 20 | u32 | n_groups |

### Compression pipeline

Each stream is built identically:

1. **Records.** Keys and pool entries are front-coded (`[common_prefix_len,
   suffix_len, suffix…]`); ids are zigzag varint deltas.
2. **Group restart.** Front-coding and delta chains restart every `1 <<
   group_shift` records (default 64), so a group can be decoded without
   touching the one before it. 64 was measured as the sweet spot: larger groups
   compress better *and* shrink the directory, while decode cost stays trivial.
3. **Re-Pair.** Repeatedly replace the most frequent byte/symbol pair with a new
   symbol. Pairs that straddle a group boundary are forbidden, so every group
   still begins on a symbol boundary after compression.
4. **Canonical Huffman**, length-limited to 16 bits, over the resulting symbol
   alphabet. Decoded bit-serially from per-length count/offset arrays — no
   large lookup table, ~300 bytes of code.
5. **Directory.** One u32 bit-offset per group.

### Lookup

```
lookup(strokes[], n):
    key = pack(strokes)                      # 3n bytes, big-endian per stroke
    g   = binary search group directory      # first_key[g] <= key
    decode group g, linear scan for key      # <= 64 records
    id  = decode_id(entry_ordinal)
    return decode_value(id)
```

The binary search decodes each candidate group's first key on the fly rather
than caching them. Storing first keys uncompressed would cost ~30 KB of flash
(about 5,000 entries), and caching them in RAM costs 18 KB plus boot time — but
a group-first decode is only a handful of Huffman symbols, and the measured
cost is 13.9 group decodes per lookup total. Paying microseconds to save flash
is the right side of this trade on a part where flash is the binding
constraint.

### Why no Bloom filter

Most probes miss: steno tries the trailing window longest-first, so 7 of ~8
probes per stroke are expected misses. A Bloom filter in front would cost no
correctness (a false positive costs one wasted decode, not a wrong answer), but
at 4 bits/entry it is 74 KB of flash — about 13,000 entries. A miss already
costs only the binary search, which terminates without touching the id or pool
streams. Spending 13,000 entries to save ~3 group decodes on a 10 ms budget is
not a trade worth making. So: no filter.

## Measured sizes

Built from Plover `main.json`, `group_shift = 6`, `max_rules = 6000`. These are
emitted section sizes from a real image, not estimates, and every entry in both
images has been verified to look up to exactly its original translation.

### Full dictionary — 147,424 entries

| section | bits | rules | huffman | dir | total |
|---|---|---|---|---|---|
| keys | 339,551 | 20,484 | 10,772 | 9,216 | 371.1 KB |
| value pool | 172,689 | 13,036 | 6,854 | 4,400 | 192.4 KB |
| value ids | 218,076 | 6,932 | 4,012 | 9,216 | 232.7 KB |
| | | | | **total** | **796.3 KB** |

5.53 bytes per entry, from 4.2 MB of JSON — a 5.3× reduction against the
packed binary form and 27× against the source.

### With `--drop-derivable --drop-generated` — 121,037 entries

| section | total |
|---|---|
| keys | 327.0 KB |
| value pool | 176.7 KB |
| value ids | 190.5 KB |
| **total** | **694.3 KB** |

26,099 entries handed to the orthography engine, 288 to the number and
fingerspelling generators.

### Reader cost

`arm-none-eabi-gcc -Os -mcpu=cortex-m4`:

| | |
|---|---|
| flash (`.text` + `.rodata`) | **1,142 B** |
| static RAM | **0 B** |
| peak stack (`sdic_lookup`) | **744 B** |
| group decodes per lookup | 13.9 |

Zero static RAM and no allocator: the image is read in place from flash and
every buffer is on the caller's stack. The 13.9 group decodes are ~11 for the
binary search plus one each for the key, id and pool groups.

## Budget

nRF52840 with no bootloader: 1,024 KB, less ~32 KB of settings partition for
BLE bonds, leaves **992 KB** for firmware plus dictionary.

| dictionary | leaves for firmware |
|---|---|
| 796 KB (full) | 196 KB — too tight |
| 694 KB (derivable dropped) | 298 KB — workable if the ZMK build is lean |
| ~600 KB (`--max-bytes 600K`) | 392 KB — comfortable |

`--max-bytes` trims entries and rebuilds until the image fits, using a supplied
`--freq` word list if you have one, else dropping the longest stroke sequences
first. The size/entry curve is close to linear at 5.5-5.9 bytes per entry, so
the tradeoff is easy to reason about.