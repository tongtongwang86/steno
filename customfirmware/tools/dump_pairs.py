#!/usr/bin/env python3
"""Emit the (strokes, translation) pairs of a dictionary in the binary form
test_sdic expects, applying the same elimination passes as the compiler so
the two agree on what should be present.

  ./dump_pairs.py main.json -o pairs.bin [--drop-derivable] [--drop-generated]

SPDX-License-Identifier: MIT
"""
import argparse, json, struct, sys
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import mkdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json', nargs='+')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--drop-derivable', action='store_true')
    ap.add_argument('--drop-generated', action='store_true')
    ap.add_argument('--max-entries', type=int, default=0)
    args = ap.parse_args()

    merged = {}
    for p in args.json:
        merged.update(json.load(open(p)))

    records = []
    for k, v in merged.items():
        try:
            records.append((mkdict.pack_key(k), v.encode('utf-8')))
        except Exception:
            pass

    if args.drop_generated:
        records, n = mkdict.drop_generated(records)
        print("dropped %d generated" % n)
    if args.drop_derivable:
        records, n = mkdict.drop_derivable(records)
        print("dropped %d derivable" % n)
    if args.max_entries and len(records) > args.max_entries:
        records.sort(key=lambda r: (1 << 30, len(r[0])))
        records = records[:args.max_entries]

    out = bytearray()
    skipped = 0
    for k, v in sorted(records):
        n = len(k) // 3
        if len(v) > 255:
            skipped += 1
            continue
        out.append(n)
        for i in range(n):
            b = k[3 * i:3 * i + 3]
            out += struct.pack('<I', (b[0] << 16) | (b[1] << 8) | b[2])
        out.append(len(v))
        out += v

    open(args.output, 'wb').write(out)
    print("wrote %s: %d pairs, %d bytes (%d skipped for length)"
          % (args.output, len(records) - skipped, len(out), skipped))


if __name__ == '__main__':
    main()
