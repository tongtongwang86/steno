/*
 * Replay a Plover reference trace through the C engine and diff every action.
 *
 *   ./test_engine dict_full.sdic trace.txt [-v]
 *
 * Trace format (from tools/plover_ref.py):
 *   steno \t 0xMASK \t backspaces \t text \t other
 *
 * SPDX-License-Identifier: MIT
 */
#include "../dict/sdic.h"
#include "../engine/steno_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sdic g_dict;

static int lookup_cb(void *ctx, const uint32_t *strokes, unsigned n,
		     char *out, size_t outsz)
{
	(void)ctx;
	return sdic_lookup(&g_dict, strokes, n, out, outsz);
}

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = malloc((size_t)n + 1);
	if (fread(p, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
	((char *)p)[n] = 0;
	fclose(f);
	*len = (size_t)n;
	return p;
}

static void unescape(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			*w++ = (*r == 't') ? '\t' : (*r == 'n') ? '\n' : *r;
		} else {
			*w++ = *r;
		}
	}
	*w = 0;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s dict.sdic trace.txt [-v]\n", argv[0]);
		return 2;
	}
	bool verbose = (argc > 3 && strcmp(argv[3], "-v") == 0);

	size_t dlen, tlen;
	uint8_t *img = slurp(argv[1], &dlen);
	char *trace = slurp(argv[2], &tlen);

	int rc = sdic_open(&g_dict, img, dlen);
	if (rc) { fprintf(stderr, "sdic_open: %d\n", rc); return 1; }

	steno_engine e;
	steno_engine_init(&e, lookup_cb, NULL, g_dict.hdr->max_strokes);

	unsigned long n = 0, match = 0, bs_bad = 0, txt_bad = 0, shown = 0;

	for (char *line = strtok(trace, "\n"); line; line = strtok(NULL, "\n")) {
		char *f1 = line;
		char *f2 = strchr(f1, '\t'); if (!f2) continue; *f2++ = 0;
		char *f3 = strchr(f2, '\t'); if (!f3) continue; *f3++ = 0;
		char *f4 = strchr(f3, '\t'); if (!f4) continue; *f4++ = 0;
		char *f5 = strchr(f4, '\t'); if (f5) *f5++ = 0;

		uint32_t mask = (uint32_t)strtoul(f2, NULL, 0);
		unsigned want_bs = (unsigned)strtoul(f3, NULL, 10);
		char *want_txt = f4;
		unescape(want_txt);

		steno_action a = steno_engine_stroke(&e, mask);
		n++;

		bool ok_bs = (a.backspaces == want_bs);
		bool ok_tx = (strcmp(a.text, want_txt) == 0);

		if (ok_bs && ok_tx) {
			match++;
		} else {
			if (!ok_bs) bs_bad++;
			if (!ok_tx) txt_bad++;
			if (verbose && shown < 40) {
				fprintf(stderr,
					"%-12s want bs=%-3u '%s'\n"
					"%-12s got  bs=%-3u '%s'\n\n",
					f1, want_bs, want_txt,
					"", a.backspaces, a.text);
				shown++;
			}
		}
	}

	printf("strokes replayed: %lu\n", n);
	printf("exact matches:    %lu (%.1f%%)\n", match, 100.0 * match / n);
	printf("  backspace mismatches: %lu\n", bs_bad);
	printf("  text mismatches:      %lu\n", txt_bad);
	printf("engine: %u lookups, %u untranslated strokes\n",
	       e.stat_lookups, e.stat_untranslated);
	return match == n ? 0 : 1;
}
