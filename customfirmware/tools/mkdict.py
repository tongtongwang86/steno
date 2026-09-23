#!/usr/bin/env python3
"""
mkdict.py - compile a Plover JSON dictionary into an SDIC image.

See docs/FORMAT.md. Output is position independent: every internal reference
is a byte offset from the image base, so the blob can be linked anywhere or
moved to external flash later without recompilation.

  ./mkdict.py main.json -o dict.sdic --drop-derivable
  ./mkdict.py main.json -o dict.sdic --max-bytes 640K

SPDX-License-Identifier: MIT
"""
import argparse, json, struct, sys, re, math, collections, heapq
import numpy as np

MAGIC = 0x31434453          # 'SDC1' little-endian
VERSION = 1
HUFF_MAXLEN = 16

# ---------------------------------------------------------------- strokes

ORDER = "#STKPWHRAO*EUFRPBLGTSDZ"      # 23 keys, bit i = ORDER[i]
LEFT, VOWEL, RIGHT = "#STKPWHR", "AO*EU", "FRPBLGTSDZ"
NUMS = {'1': 'S', '2': 'T', '3': 'P', '4': 'H', '5': 'A',
        '6': 'F', '7': 'P', '8': 'L', '9': 'T', '0': 'O'}


def stroke_bits(s):
    """Parse one steno stroke into a 23-bit mask. Raises on malformed input."""
    bits = 0
    out = []
    numeric = False
    for ch in s:
        if ch in NUMS:
            numeric = True
            out.append(NUMS[ch])
        else:
            out.append(ch)
    s2 = ''.join(out)
    if numeric:
        bits |= 1

    if '-' in s2:
        l, r = s2.split('-', 1)
        parts = [(l, 'L'), (r, 'R')]
    else:
        parts = [(s2, 'A')]

    # Zones are contiguous slices of ORDER, so the position found while
    # scanning the zone IS the bit index (offset by the zone's base). Using
    # ORDER.find(ch) here instead would silently map a right-bank T, S, P or R
    # onto its left-bank twin - e.g. '1EU9' (#S-EU-T) would collide with
    # '12EU' (#ST-EU).
    RIGHT_BASE = len(LEFT) + len(VOWEL)          # ORDER.index('F') == 13
    assert ORDER[RIGHT_BASE:] == RIGHT

    for part, side in parts:
        if side == 'R':
            zone, base = RIGHT, RIGHT_BASE
        elif side == 'L':
            zone, base = LEFT + VOWEL, 0
        else:
            zone, base = ORDER, 0
        p = 0
        for ch in part:
            if ch == '#':
                bits |= 1
                continue
            found = zone.find(ch, p)
            if found < 0:
                raise ValueError("key %r out of steno order in stroke %r"
                                 % (ch, s))
            p = found + 1
            bits |= (1 << (base + found))
    return bits


def pack_key(stroke_str):
    """'TKPW/HRO' -> 6 bytes, 3 big-endian bytes per stroke.

    Big-endian so that memcmp() ordering equals numeric stroke ordering, which
    is what lets the reader binary search with a plain byte compare.
    """
    out = bytearray()
    for part in stroke_str.split('/'):
        b = stroke_bits(part)
        out += bytes([(b >> 16) & 0xff, (b >> 8) & 0xff, b & 0xff])
    return bytes(out)


# ---------------------------------------------------------------- Re-Pair

def repair(seq, group_start, max_rules, min_occ=8, log=None):
    """Re-Pair over `seq` (int32 array), never creating a pair that straddles a
    group boundary, so every group still starts on a symbol boundary.

    group_start[i] is True when position i begins a group.
    Returns (symbols, rules, n_symbols, group_start).
    """
    seq = np.asarray(seq, dtype=np.int32)
    gs = np.asarray(group_start, dtype=bool)
    nsym = 256
    rules = []

    for it in range(max_rules):
        a, b = seq[:-1], seq[1:]
        # a pair may not straddle a boundary: position i+1 must not start a group
        ok = ~gs[1:]
        if not ok.any():
            break
        pair = (a[ok].astype(np.int64) << 32) | b[ok].astype(np.int64)
        uq, cnt = np.unique(pair, return_counts=True)
        j = int(cnt.argmax())
        if cnt[j] < min_occ:
            break
        pa, pb = int(uq[j] >> 32), int(uq[j] & 0xffffffff)

        m = (seq[:-1] == pa) & (seq[1:] == pb) & ok
        idx = np.flatnonzero(m)
        # non-overlapping, left to right
        keep = []
        last = -2
        for i in idx:
            if i > last:
                keep.append(i)
                last = i + 1
        keep = np.array(keep, dtype=np.int64)
        if len(keep) < min_occ:
            break

        seq = seq.copy()
        seq[keep] = nsym
        drop = np.zeros(len(seq), dtype=bool)
        drop[keep + 1] = True
        seq = seq[~drop]
        gs = gs[~drop]

        rules.append((pa, pb))
        nsym += 1
        if log and it % 500 == 0:
            print("      %s: %d rules, %d symbols" % (log, it, len(seq)),
                  file=sys.stderr)

    return seq, rules, nsym, gs


# ---------------------------------------------------------------- Huffman

def huffman_lengths(freq, maxlen=HUFF_MAXLEN):
    """Canonical Huffman code lengths, length-limited via Kraft correction."""
    syms = [s for s, f in freq.items() if f > 0]
    if len(syms) == 1:
        return {syms[0]: 1}

    h = [(freq[s], i, s) for i, s in enumerate(syms)]
    heapq.heapify(h)
    nodes = {s: None for s in syms}
    parent = {}
    nxt = len(syms)
    while len(h) > 1:
        f1, _, a = heapq.heappop(h)
        f2, _, b = heapq.heappop(h)
        node = ('n', nxt)
        nxt += 1
        parent[a] = node
        parent[b] = node
        heapq.heappush(h, (f1 + f2, nxt, node))

    # depth by walking parents
    lengths = {}
    for s in syms:
        d = 0
        cur = s
        while cur in parent:
            cur = parent[cur]
            d += 1
        lengths[s] = max(1, d)

    if max(lengths.values()) <= maxlen:
        return lengths

    # Kraft correction: clamp, then lengthen the longest codes until the
    # Kraft sum fits, which costs the least ratio.
    for s in lengths:
        lengths[s] = min(lengths[s], maxlen)
    limit = 1 << maxlen
    total = sum(1 << (maxlen - lengths[s]) for s in lengths)
    if total > limit:
        order = sorted(lengths, key=lambda s: (-lengths[s], -freq[s]))
        i = 0
        guard = 0
        while total > limit:
            s = order[i % len(order)]
            if lengths[s] < maxlen:
                total -= 1 << (maxlen - lengths[s] - 1)
                lengths[s] += 1
            i += 1
            guard += 1
            if guard > 40 * len(order):
                raise RuntimeError("Kraft correction failed to converge")
    return lengths


def canonical(lengths):
    """-> (codes{sym:(code,len)}, counts[1..MAXLEN], symbols in canonical order)

    first_code[1] = 0;  first_code[l] = (first_code[l-1] + counts[l-1]) << 1
    which is exactly what the bit-serial decoder in sdic.c reconstructs.
    """
    counts = [0] * (HUFF_MAXLEN + 1)
    for s, l in lengths.items():
        counts[l] += 1
    order = sorted(lengths, key=lambda s: (lengths[s], s))

    codes = {}
    first = 0
    idx = 0
    for l in range(1, HUFF_MAXLEN + 1):
        first = 0 if l == 1 else (first + counts[l - 1]) << 1
        c = first
        for _ in range(counts[l]):
            assert c < (1 << l), "canonical Huffman overflow at length %d" % l
            codes[order[idx]] = (c, l)
            c += 1
            idx += 1
    assert idx == len(order)
    return codes, counts, order


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.cur = 0
        self.n = 0

    def write(self, code, length):
        for i in range(length - 1, -1, -1):
            self.cur = (self.cur << 1) | ((code >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.buf.append(self.cur)
                self.cur = 0
                self.n = 0

    def bitpos(self):
        return len(self.buf) * 8 + self.n

    def finish(self):
        while self.n:
            self.cur <<= 1
            self.n += 1
            if self.n == 8:
                self.buf.append(self.cur)
                self.cur = 0
                self.n = 0
        return bytes(self.buf)


# ---------------------------------------------------------------- stream

class Stream:
    """One compressed, group-addressable stream."""

    def __init__(self, blobs, group_size, max_rules, name):
        self.name = name
        self.group_size = group_size
        n_groups = (len(blobs) + group_size - 1) // group_size

        data = bytearray()
        gstart = []
        for i, b in enumerate(blobs):
            if i % group_size == 0:
                gstart.append(len(data))
            data += b
        self.raw_len = len(data)

        gs = np.zeros(len(data), dtype=bool)
        for p in gstart:
            gs[p] = True

        seq, rules, nsym, gs2 = repair(
            np.frombuffer(bytes(data), dtype=np.uint8).astype(np.int32),
            gs, max_rules, log=name)

        freq = collections.Counter(seq.tolist())
        lengths = huffman_lengths(freq)
        codes, counts, order = canonical(lengths)

        bw = BitWriter()
        dirs = []
        gs_idx = np.flatnonzero(gs2)
        gset = set(gs_idx.tolist())
        for i, s in enumerate(seq.tolist()):
            if i in gset:
                dirs.append(bw.bitpos())
            c, l = codes[s]
            bw.write(c, l)
        self.bits = bw.finish()
        self.dirs = dirs
        self.rules = rules
        self.counts = counts
        self.order = order
        self.n_groups = n_groups

        assert len(dirs) == n_groups, \
            "%s: %d group offsets for %d groups" % (name, len(dirs), n_groups)

    def sections(self):
        rules = b''.join(struct.pack('<HH', a, b) for a, b in self.rules)
        huff = struct.pack('<%dH' % (HUFF_MAXLEN + 1), *self.counts)
        huff += b''.join(struct.pack('<H', s) for s in self.order)
        dirs = b''.join(struct.pack('<I', d) for d in self.dirs)
        return self.bits, rules, huff, dirs

    def report(self):
        b, r, h, d = self.sections()
        tot = len(b) + len(r) + len(h) + len(d)
        print("  %-10s raw %7d -> bits %7d  rules %6d  huff %6d  dir %6d  "
              "= %7d B (%5.1f KB)"
              % (self.name, self.raw_len, len(b), len(r), len(h), len(d),
                 tot, tot / 1024))
        return tot


# ---------------------------------------------------------------- encoding

def front_code(items, group_size):
    out = []
    prev = b''
    for i, v in enumerate(items):
        if i % group_size == 0:
            prev = b''
        c = 0
        m = min(len(v), len(prev), 255)
        while c < m and v[c] == prev[c]:
            c += 1
        assert len(v) - c < 256
        out.append(bytes([c, len(v) - c]) + v[c:])
        prev = v
    return out


def varint(n):
    o = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        o.append(b | (0x80 if n else 0))
        if not n:
            return bytes(o)


def zigzag(n):
    return (n << 1) if n >= 0 else ((-n) << 1) - 1


# ---------------------------------------------------------------- passes

SUFFIXES = [b'ing', b'est', b'es', b'ed', b'ly', b'er', b's', b'd']


# Right-bank suffix keys, by bit: -Z -D -S -G. Must match the engine's
# SUFFIX_KEYS, which are Plover's for English Stenotype.
SUFFIX_KEY_BITS = (1 << 22, 1 << 21, 1 << 20, 1 << 18)


def drop_derivable(records):
    """Remove entries the runtime can rebuild from a shorter entry.

    An entry is only droppable when BOTH halves are derivable:

      * its translation is another translation plus a regular suffix, and
      * its STROKE is another entry's stroke plus one of the suffix keys,
        so the engine's implicit-suffix lookup actually reaches it.

    Testing only the translation - which an earlier version of this did -
    drops entries whose stroke has no suffix key at all. The engine then
    cannot rebuild them and they are simply missing: measured at 26,099
    entries dropped of which only 11,769 were genuinely derivable, costing
    6.2% accuracy against Plover. Both halves are now required.
    """
    vals = set(v for _, v in records)
    by_key = {k: v for k, v in records}
    out = []
    dropped = 0

    for k, v in records:
        value_ok = False
        for suf in SUFFIXES:
            if v.endswith(suf) and len(v) - len(suf) >= 3 and v[:-len(suf)] in vals:
                value_ok = True
                break
        if not value_ok:
            out.append((k, v))
            continue

        # last stroke, big-endian 3 bytes
        last = (k[-3] << 16) | (k[-2] << 8) | k[-1]
        stroke_ok = False
        for bit in SUFFIX_KEY_BITS:
            if not (last & bit):
                continue
            base = last & ~bit
            cand = k[:-3] + bytes([(base >> 16) & 0xff,
                                   (base >> 8) & 0xff, base & 0xff])
            if cand in by_key:
                stroke_ok = True
                break

        if stroke_ok:
            dropped += 1
        else:
            out.append((k, v))
    return out, dropped


def drop_generated(records):
    """Remove pure fingerspelling and pure numeral entries."""
    out = []
    dropped = 0
    fs = re.compile(rb'\{&[A-Za-z]\}|\{>\}\{&[A-Za-z]\}')
    num = re.compile(rb'[\d,.:]+')
    for k, v in records:
        if fs.fullmatch(v) or num.fullmatch(v):
            dropped += 1
        else:
            out.append((k, v))
    return out, dropped


# ---------------------------------------------------------------- main

def build(records, group_shift, max_rules, verbose=True):
    group = 1 << group_shift
    records = sorted(records)

    vals = sorted(set(v for _, v in records))
    vid = {v: i for i, v in enumerate(vals)}

    key_blobs = front_code([k for k, _ in records], group)
    pool_blobs = front_code(vals, group)

    id_blobs = []
    prev = 0
    for i, (k, v) in enumerate(records):
        if i % group == 0:
            prev = 0
        cur = vid[v]
        id_blobs.append(varint(zigzag(cur - prev)))
        prev = cur

    if verbose:
        print("building streams (%d entries, %d values, group=%d)"
              % (len(records), len(vals), group))

    s_key = Stream(key_blobs, group, max_rules, "keys")
    s_pool = Stream(pool_blobs, group, max_rules, "pool")
    s_id = Stream(id_blobs, group, max_rules, "ids")

    max_strokes = max(len(k) for k, _ in records) // 3

    # ---- assemble ----
    HDR = 128
    parts = []
    off = HDR
    descs = []
    for s in (s_key, s_pool, s_id):
        b, r, h, d = s.sections()
        desc = []
        for blob in (b, r, h, d):
            desc.append(off)
            parts.append(blob)
            off += len(blob)
            if off & 3:                       # keep sections u32-aligned
                pad = 4 - (off & 3)
                parts.append(b'\0' * pad)
                off += pad
        descs.append((desc, len(s.rules), s.n_groups, len(s.order)))

    hdr = struct.pack('<IHHIIIIII',
                      MAGIC, VERSION, 0, off, len(records), len(vals),
                      max_strokes, group_shift, 0)
    assert len(hdr) == 32, len(hdr)
    for desc, n_rules, n_groups, n_syms in descs:
        hdr += struct.pack('<IIIIHH', desc[0], desc[1], desc[2], desc[3],
                           n_rules, n_syms)
        hdr += struct.pack('<I', n_groups)
    hdr += b'\0' * (HDR - len(hdr))
    assert len(hdr) == HDR

    image = hdr + b''.join(parts)
    assert len(image) == off, (len(image), off)

    if verbose:
        print("  %-10s %7d B" % ("header", HDR))
        total = HDR + sum(s.report() for s in (s_key, s_pool, s_id))
        print("  TOTAL %d B (%.1f KB) for %d entries = %.2f B/entry"
              % (len(image), len(image) / 1024, len(records),
                 len(image) / len(records)))
    return image


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    if s.endswith('K'):
        mult, s = 1024, s[:-1]
    elif s.endswith('M'):
        mult, s = 1024 * 1024, s[:-1]
    return int(float(s) * mult)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json', nargs='+', help='Plover dictionary JSON (later files win)')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--group-shift', type=int, default=6,
                    help='log2 records per group (default 6 = 64)')
    ap.add_argument('--max-rules', type=int, default=6000,
                    help='Re-Pair rule budget per stream')
    ap.add_argument('--drop-derivable', action='store_true',
                    help='drop entries the orthography engine can regenerate')
    ap.add_argument('--drop-generated', action='store_true',
                    help='drop pure fingerspelling and numeral entries')
    ap.add_argument('--max-entries', type=int, default=0)
    ap.add_argument('--max-bytes', type=str, default='',
                    help='trim entries until the image fits, e.g. 640K')
    ap.add_argument('--freq', help='newline-separated word frequency list, '
                                   'most frequent first, used for trimming')
    args = ap.parse_args()

    merged = {}
    for path in args.json:
        with open(path) as f:
            merged.update(json.load(f))
    print("loaded %d entries from %d file(s)" % (len(merged), len(args.json)))

    records = []
    bad = 0
    for k, v in merged.items():
        try:
            records.append((pack_key(k), v.encode('utf-8')))
        except Exception:
            bad += 1
    if bad:
        print("  skipped %d unparseable strokes" % bad)

    if args.drop_generated:
        records, n = drop_generated(records)
        print("  --drop-generated removed %d entries" % n)
    if args.drop_derivable:
        records, n = drop_derivable(records)
        print("  --drop-derivable removed %d entries" % n)

    rank = {}
    if args.freq:
        with open(args.freq) as f:
            for i, line in enumerate(f):
                rank[line.strip().encode()] = i

    def priority(rec):
        """Lower is more important: known-frequent words first, then short keys."""
        k, v = rec
        return (rank.get(v.strip(), 1 << 30), len(k))

    if args.max_entries and len(records) > args.max_entries:
        records.sort(key=priority)
        records = records[:args.max_entries]
        print("  --max-entries trimmed to %d" % len(records))

    image = build(records, args.group_shift, args.max_rules)

    if args.max_bytes:
        budget = parse_size(args.max_bytes)
        while len(image) > budget and len(records) > 1000:
            over = len(image) - budget
            drop = max(500, int(len(records) * over / len(image)) + 200)
            records.sort(key=priority)
            records = records[:len(records) - drop]
            print("  over budget by %d B, retrying with %d entries"
                  % (over, len(records)))
            image = build(records, args.group_shift, args.max_rules)

    with open(args.output, 'wb') as f:
        f.write(image)
    print("wrote %s (%d bytes)" % (args.output, len(image)))


if __name__ == '__main__':
    main()
