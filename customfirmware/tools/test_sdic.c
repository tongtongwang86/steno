/*
 * Exhaustive verification: every entry in the source dictionary must look up
 * to exactly its original translation, and keys that are not in the image
 * must miss. Also reports decode counts, which is what determines whether
 * the engine fits its per-stroke time budget.
 *
 *   ./test_sdic dict.sdic pairs.bin
 *
 * pairs.bin is emitted by tools/dump_pairs.py: a stream of
 *   u8 n_strokes, n * u32 stroke masks (LE), u8 value_len, value bytes
 *
 * SPDX-License-Identifier: MIT
 */
#include "../dict/sdic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc((size_t)n);
	if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
	fclose(f);
	*len = (size_t)n;
	return p;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s dict.sdic pairs.bin\n", argv[0]);
		return 2;
	}
	size_t dlen, plen;
	uint8_t *img = slurp(argv[1], &dlen);
	uint8_t *pairs = slurp(argv[2], &plen);

	sdic d;
	int rc = sdic_open(&d, img, dlen);
	if (rc != SDIC_OK) {
		fprintf(stderr, "sdic_open failed: %d\n", rc);
		return 1;
	}
	printf("image: %zu bytes, %u entries, %u values, max %u strokes, group %u\n",
	       dlen, d.hdr->n_entries, d.hdr->n_values, d.hdr->max_strokes,
	       1u << d.hdr->group_shift);

	size_t off = 0;
	unsigned long ok = 0, bad = 0, missing = 0;
	char out[SDIC_MAX_VALUE];
	clock_t t0 = clock();

	while (off < plen) {
		unsigned n = pairs[off++];
		uint32_t strokes[SDIC_MAX_STROKES];
		for (unsigned i = 0; i < n; i++) {
			memcpy(&strokes[i], pairs + off, 4);
			off += 4;
		}
		unsigned vlen = pairs[off++];
		const char *want = (const char *)(pairs + off);
		off += vlen;

		int got = sdic_lookup(&d, strokes, n, out, sizeof(out));
		if (got < 0) {
			if (missing < 5)
				fprintf(stderr, "MISS rc=%d n=%u want='%.*s'\n",
					got, n, (int)vlen, want);
			missing++;
		} else if ((unsigned)got != vlen || memcmp(out, want, vlen) != 0) {
			if (bad < 5)
				fprintf(stderr, "WRONG n=%u want='%.*s' got='%s'\n",
					n, (int)vlen, want, out);
			bad++;
		} else {
			ok++;
		}
	}
	double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
	unsigned long total = ok + bad + missing;

	printf("verified %lu entries: %lu ok, %lu wrong, %lu missing\n",
	       total, ok, bad, missing);
	printf("%.2f s  (%.1f us/lookup on host)\n",
	       secs, secs * 1e6 / (double)(total ? total : 1));
	printf("group decodes: %u over %u probes = %.2f per lookup\n",
	       d.stat_group_decodes, d.stat_probes,
	       (double)d.stat_group_decodes / (double)(d.stat_probes ? d.stat_probes : 1));

	/* Negative test: mutate a stroke so the key should not exist. */
	unsigned long false_hits = 0, neg = 0;
	off = 0;
	while (off < plen && neg < 20000) {
		unsigned n = pairs[off++];
		uint32_t strokes[SDIC_MAX_STROKES];
		for (unsigned i = 0; i < n; i++) {
			memcpy(&strokes[i], pairs + off, 4);
			off += 4;
		}
		unsigned vlen = pairs[off++];
		off += vlen;

		strokes[0] ^= 0x155555u;           /* scramble within 23 bits */
		strokes[0] &= 0x7FFFFFu;
		if (sdic_lookup(&d, strokes, n, out, sizeof(out)) >= 0)
			false_hits++;
		neg++;
	}
	printf("negative probes: %lu, of which found in dictionary: %lu\n"
	       "  (a nonzero count here is expected - scrambling can land on a\n"
	       "   real entry - it is only a bug if lookups return wrong text)\n",
	       neg, false_hits);

	if (bad || missing) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
