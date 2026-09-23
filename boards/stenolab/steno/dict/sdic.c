/*
 * sdic - compressed steno dictionary reader. See docs/FORMAT.md.
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdic.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Flash accessor                                                      */
/*                                                                     */
/* The single seam through which every dictionary byte is read. On     */
/* nRF52840 internal flash this is a plain dereference and compiles    */
/* away. Redirect it for QSPI without touching anything else.          */
/* ------------------------------------------------------------------ */

#ifndef sdic_fetch
#define sdic_fetch(p) (*(const uint8_t *)(p))
#endif

/* ------------------------------------------------------------------ */
/* Bit reader                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	const uint8_t *base;
	uint32_t bit;
} bitreader;

static inline unsigned br_bit(bitreader *br)
{
	unsigned b = (sdic_fetch(br->base + (br->bit >> 3)) >> (7u - (br->bit & 7u))) & 1u;
	br->bit++;
	return b;
}

/* ------------------------------------------------------------------ */
/* Canonical Huffman                                                   */
/* ------------------------------------------------------------------ */

static void huff_init(sdic_stream *s)
{
	uint32_t code = 0, idx = 0;

	for (int l = 1; l <= 16; l++) {
		code = (l == 1) ? 0u : ((code + s->hcount[l - 1]) << 1);
		s->first_code[l] = code;
		s->first_index[l] = idx;
		idx += s->hcount[l];
	}
}

static int huff_decode(const sdic_stream *s, bitreader *br)
{
	uint32_t code = 0;

	for (int l = 1; l <= 16; l++) {
		code = (code << 1) | br_bit(br);
		uint32_t cnt = s->hcount[l];

		if (cnt && code >= s->first_code[l] &&
		    (code - s->first_code[l]) < cnt) {
			uint32_t i = s->first_index[l] + (code - s->first_code[l]);

			if (i >= s->n_syms_total)
				return SDIC_E_CORRUPT;
			return (int)s->hsyms[i];
		}
	}
	return SDIC_E_CORRUPT;
}

/* ------------------------------------------------------------------ */
/* Re-Pair expansion -> byte stream                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	const sdic_stream *s;
	bitreader br;
	uint16_t stack[SDIC_EXPAND_STACK];
	int sp;
} expander;

static void exp_seek(expander *e, const sdic_stream *s, uint32_t bitpos)
{
	e->s = s;
	e->br.base = s->bits;
	e->br.bit = bitpos;
	e->sp = 0;
}

/* Returns the next plaintext byte, or negative on error. */
static int exp_byte(expander *e)
{
	for (;;) {
		int sym;

		if (e->sp > 0) {
			sym = e->stack[--e->sp];
		} else {
			sym = huff_decode(e->s, &e->br);
			if (sym < 0)
				return sym;
		}

		if (sym < 256)
			return sym;

		uint32_t r = (uint32_t)sym - 256u;

		if (r >= e->s->n_rules)
			return SDIC_E_CORRUPT;
		if (e->sp + 2 > SDIC_EXPAND_STACK)
			return SDIC_E_CORRUPT;

		/* push right then left so the left child pops first */
		e->stack[e->sp++] = e->s->rules[2 * r + 1];
		e->stack[e->sp++] = e->s->rules[2 * r + 0];
	}
}

/* ------------------------------------------------------------------ */
/* Record decoding                                                     */
/* ------------------------------------------------------------------ */

/*
 * Decode one front-coded record into @p buf, given the previous record.
 * Layout: [common_prefix_len][suffix_len][suffix bytes...]
 * Returns the new length, or negative.
 */
static int decode_front(expander *e, uint8_t *buf, int prevlen, size_t cap)
{
	int c = exp_byte(e);
	int sl = exp_byte(e);

	if (c < 0 || sl < 0)
		return SDIC_E_CORRUPT;
	if (c > prevlen || (size_t)(c + sl) > cap)
		return SDIC_E_CORRUPT;

	for (int i = 0; i < sl; i++) {
		int b = exp_byte(e);

		if (b < 0)
			return b;
		buf[c + i] = (uint8_t)b;
	}
	return c + sl;
}

static int decode_varint(expander *e, uint32_t *out)
{
	uint32_t v = 0;
	int shift = 0;

	for (;;) {
		int b = exp_byte(e);

		if (b < 0)
			return b;
		v |= (uint32_t)(b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
		if (shift > 28)
			return SDIC_E_CORRUPT;
	}
	*out = v;
	return SDIC_OK;
}

static inline int32_t unzigzag(uint32_t v)
{
	return (v & 1) ? -(int32_t)((v + 1) >> 1) : (int32_t)(v >> 1);
}

/* ------------------------------------------------------------------ */
/* Key helpers                                                         */
/* ------------------------------------------------------------------ */

static void pack_key(const uint32_t *strokes, unsigned n, uint8_t *out)
{
	for (unsigned i = 0; i < n; i++) {
		out[3 * i + 0] = (uint8_t)(strokes[i] >> 16);
		out[3 * i + 1] = (uint8_t)(strokes[i] >> 8);
		out[3 * i + 2] = (uint8_t)(strokes[i]);
	}
}

/* memcmp ordering, shorter key first on a prefix tie - matches Python sort */
static int keycmp(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
	int n = alen < blen ? alen : blen;
	int c = memcmp(a, b, (size_t)n);

	if (c)
		return c;
	return alen - blen;
}

/* Decode the first key of group @p g. */
static int group_first_key(sdic *d, uint32_t g, uint8_t *buf)
{
	expander e;

	if (g >= d->key.n_groups)
		return SDIC_E_CORRUPT;
	exp_seek(&e, &d->key, d->key.dir[g]);
	d->stat_group_decodes++;
	return decode_front(&e, buf, 0, SDIC_MAX_KEY);
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

static int stream_init(sdic_stream *s, const uint8_t *image,
		       const sdic_stream_desc *desc, size_t len)
{
	if (len) {
		if (desc->off_bits >= len || desc->off_rules >= len ||
		    desc->off_huff >= len || desc->off_dir >= len)
			return SDIC_E_TRUNCATED;
	}
	s->bits = image + desc->off_bits;
	s->rules = (const uint16_t *)(const void *)(image + desc->off_rules);
	s->hcount = (const uint16_t *)(const void *)(image + desc->off_huff);
	s->hsyms = s->hcount + 17;
	s->dir = (const uint32_t *)(const void *)(image + desc->off_dir);
	s->n_rules = desc->n_rules;
	s->n_syms_total = desc->n_syms;
	s->n_groups = desc->n_groups;
	huff_init(s);

	uint32_t sum = 0;

	for (int l = 1; l <= 16; l++)
		sum += s->hcount[l];
	if (sum != s->n_syms_total)
		return SDIC_E_CORRUPT;
	return SDIC_OK;
}

int sdic_open(sdic *d, const void *image, size_t len)
{
	const sdic_header *h = (const sdic_header *)image;
	int rc;

	memset(d, 0, sizeof(*d));
	if (len && len < sizeof(sdic_header))
		return SDIC_E_TRUNCATED;
	if (h->magic != SDIC_MAGIC)
		return SDIC_E_MAGIC;
	if (h->version != SDIC_VERSION)
		return SDIC_E_VERSION;
	if (h->max_strokes > SDIC_MAX_STROKES)
		return SDIC_E_STROKES;
	if (len && h->image_size > len)
		return SDIC_E_TRUNCATED;

	d->image = (const uint8_t *)image;
	d->hdr = h;
	d->group_shift = h->group_shift;
	d->group_size = 1u << h->group_shift;
	d->group_mask = d->group_size - 1u;

	rc = stream_init(&d->key, d->image, &h->key, len);
	if (rc) return rc;
	rc = stream_init(&d->pool, d->image, &h->pool, len);
	if (rc) return rc;
	rc = stream_init(&d->id, d->image, &h->id, len);
	if (rc) return rc;

	return SDIC_OK;
}

/* Resolve a value id into @p out. */
static int read_value(sdic *d, uint32_t id, char *out, size_t outsz)
{
	uint32_t g = id >> d->group_shift;
	uint32_t skip = id & d->group_mask;
	uint8_t buf[SDIC_MAX_VALUE];
	expander e;
	int len = 0;

	if (g >= d->pool.n_groups)
		return SDIC_E_CORRUPT;
	exp_seek(&e, &d->pool, d->pool.dir[g]);
	d->stat_group_decodes++;

	for (uint32_t i = 0; i <= skip; i++) {
		len = decode_front(&e, buf, len, sizeof(buf));
		if (len < 0)
			return len;
	}
	if ((size_t)len + 1 > outsz)
		return SDIC_E_TOOLONG;
	memcpy(out, buf, (size_t)len);
	out[len] = '\0';
	return len;
}

/* Resolve the value id stored at global entry ordinal @p ord. */
static int read_id(sdic *d, uint32_t ord, uint32_t *out)
{
	uint32_t g = ord >> d->group_shift;
	uint32_t skip = ord & d->group_mask;
	expander e;
	int32_t cur = 0;

	if (g >= d->id.n_groups)
		return SDIC_E_CORRUPT;
	exp_seek(&e, &d->id, d->id.dir[g]);
	d->stat_group_decodes++;

	for (uint32_t i = 0; i <= skip; i++) {
		uint32_t raw;
		int rc = decode_varint(&e, &raw);

		if (rc)
			return rc;
		cur += unzigzag(raw);
		if (cur < 0)
			return SDIC_E_CORRUPT;
	}
	*out = (uint32_t)cur;
	return SDIC_OK;
}

int sdic_lookup(sdic *d, const uint32_t *strokes, unsigned n,
		char *out, size_t outsz)
{
	uint8_t want[SDIC_MAX_KEY];
	uint8_t first[SDIC_MAX_KEY];
	uint8_t cur[SDIC_MAX_KEY];
	expander e;
	int wantlen = (int)(3u * n);

	d->stat_probes++;

	if (n == 0 || n > d->hdr->max_strokes)
		return SDIC_E_NOTFOUND;
	pack_key(strokes, n, want);

	/*
	 * Binary search the group directory, decoding each candidate's first
	 * key on the fly. ~11 group-first decodes per probe for a 2300-group
	 * image. We deliberately do not cache first keys: flash is the scarce
	 * resource and the decode is only a few microseconds.
	 *
	 * Invariant: lo is a group whose first key is <= want, or 0.
	 */
	uint32_t lo = 0, hi = d->key.n_groups;

	while (lo + 1 < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		int flen = group_first_key(d, mid, first);

		if (flen < 0)
			return flen;
		if (keycmp(first, flen, want, wantlen) <= 0)
			lo = mid;
		else
			hi = mid;
	}

	/* Scan group lo. */
	exp_seek(&e, &d->key, d->key.dir[lo]);
	d->stat_group_decodes++;

	uint32_t base = lo << d->group_shift;
	uint32_t count = d->hdr->n_entries - base;

	if (count > d->group_size)
		count = d->group_size;

	int len = 0;

	for (uint32_t i = 0; i < count; i++) {
		len = decode_front(&e, cur, len, SDIC_MAX_KEY);
		if (len < 0)
			return len;

		int c = keycmp(cur, len, want, wantlen);

		if (c == 0) {
			uint32_t id;
			int rc = read_id(d, base + i, &id);

			if (rc)
				return rc;
			return read_value(d, id, out, outsz);
		}
		if (c > 0)
			break;          /* sorted: gone past it */
	}
	return SDIC_E_NOTFOUND;
}
