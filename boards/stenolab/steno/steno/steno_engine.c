/*
 * steno_engine - stroke segmentation and output formatting.
 * SPDX-License-Identifier: MIT
 */

#include "steno_engine.h"
#include <string.h>

/* Plover's SUFFIX_KEYS for English Stenotype, in its order. */
static const uint32_t SUFFIX_KEYS[] = {
	STENO_Z_R, STENO_D_R, STENO_S_R, STENO_G_R,
};
#define N_SUFFIX_KEYS (sizeof(SUFFIX_KEYS) / sizeof(SUFFIX_KEYS[0]))

/* ------------------------------------------------------------------ */
/* Stroke formatting (for untranslated output)                         */
/* ------------------------------------------------------------------ */

static const char KEY_CHARS[23] = {
	'#', 'S', 'T', 'K', 'P', 'W', 'H', 'R', 'A', 'O', '*',
	'E', 'U', 'F', 'R', 'P', 'B', 'L', 'G', 'T', 'S', 'D', 'Z'
};
/* Keys that make a hyphen unnecessary, Plover's IMPLICIT_HYPHEN_KEYS. */
#define IMPLICIT_HYPHEN (STENO_A | STENO_O | STENO_STAR | STENO_E | STENO_U)
#define LEFT_KEYS  (STENO_S_L | STENO_T_L | STENO_K_L | STENO_P_L | \
		    STENO_W_L | STENO_H_L | STENO_R_L)
#define RIGHT_KEYS (STENO_F_R | STENO_R_R | STENO_P_R | STENO_B_R | \
		    STENO_L_R | STENO_G_R | STENO_T_R | STENO_S_R | \
		    STENO_D_R | STENO_Z_R)

int steno_format_stroke(uint32_t stroke, char *out, size_t outsz)
{
	size_t n = 0;

	stroke &= STENO_KEY_MASK;
	bool need_hyphen = !(stroke & IMPLICIT_HYPHEN) && (stroke & RIGHT_KEYS);

	for (int i = 0; i < 23; i++) {
		if (need_hyphen && i == 13) {          /* before -F */
			if (n + 1 >= outsz)
				return -1;
			out[n++] = '-';
		}
		if (stroke & (1u << i)) {
			if (n + 1 >= outsz)
				return -1;
			out[n++] = KEY_CHARS[i];
		}
	}
	if (n >= outsz)
		return -1;
	out[n] = '\0';
	return (int)n;
}

/* ------------------------------------------------------------------ */
/* Formatter                                                           */
/* ------------------------------------------------------------------ */

enum { CASE_NONE = 0, CASE_CAP, CASE_LOWER, CASE_UPPER };

typedef struct {
	char    *buf;
	uint16_t len;
	uint16_t cap;
	bool     space_next;   /* next word wants a leading space */
	uint8_t  case_next;
	bool     last_glue;
} fmt;

static void put(fmt *f, char c)
{
	if (f->len + 1 < f->cap)
		f->buf[f->len++] = c;
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char dn(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Emit one run of literal text with its attachment flags resolved. */
static void emit_text(fmt *f, const char *s, size_t n,
		      bool attach_before, bool attach_after, bool glue)
{
	if (n == 0) {
		/* A bare {} or {^} still suppresses the pending space. */
		if (attach_before)
			f->space_next = false;
		if (attach_after)
			f->space_next = false;
		return;
	}

	bool space = f->space_next && !attach_before;

	if (glue && f->last_glue)
		space = false;

	if (space)
		put(f, ' ');

	for (size_t i = 0; i < n; i++) {
		char c = s[i];

		if (i == 0) {
			switch (f->case_next) {
			case CASE_CAP:   c = up(c); break;
			case CASE_LOWER: c = dn(c); break;
			case CASE_UPPER: c = up(c); break;
			default: break;
			}
		} else if (f->case_next == CASE_UPPER) {
			c = up(c);
		}
		put(f, c);
	}
	f->case_next = CASE_NONE;
	f->space_next = !attach_after;
	f->last_glue = glue;
}

/*
 * Apply one meta command. Returns true if the atom produced text.
 *
 * Covers the metas that actually occur in main.json. Unrecognised metas
 * (key combos {#...}, engine commands {PLOVER:...}, retro transforms) are
 * swallowed: the right behaviour for a keyboard is to do nothing rather
 * than type the literal braces.
 */
static void apply_meta(fmt *f, const char *m, size_t n)
{
	bool attach_before = false, attach_after = false, glue = false;

	if (n == 0) {                       /* {} - cancel the pending space */
		f->space_next = false;
		return;
	}

	/* Commands and key combos: no output, no state change. */
	if (m[0] == '#' || m[0] == ':')
		return;
	if (n >= 7 && memcmp(m, "PLOVER:", 7) == 0)
		return;
	if (m[0] == '*')                    /* retro transforms, not yet */
		return;

	/* Case controls. */
	if (n == 2 && m[0] == '-' && m[1] == '|') { f->case_next = CASE_CAP;   return; }
	if (n == 1 && m[0] == '>')           { f->case_next = CASE_LOWER; return; }
	if (n == 1 && m[0] == '<')           { f->case_next = CASE_UPPER; return; }

	/* Carry capitalisation across an attach. */
	if (n == 1 && m[0] == '~') return;

	/* Glue: {&x} joins to adjacent glued atoms. */
	if (m[0] == '&') {
		emit_text(f, m + 1, n - 1, false, false, true);
		return;
	}

	/* Attachment markers. */
	const char *t = m;
	size_t tn = n;

	if (tn && t[0] == '^') { attach_before = true; t++; tn--; }
	if (tn && t[tn - 1] == '^') { attach_after = true; tn--; }

	/* Sentence punctuation attaches and capitalises what follows. */
	if (tn == 1 && (t[0] == '.' || t[0] == '!' || t[0] == '?')) {
		emit_text(f, t, tn, true, false, false);
		f->case_next = CASE_CAP;
		return;
	}
	if (tn == 1 && (t[0] == ',' || t[0] == ';' || t[0] == ':')) {
		emit_text(f, t, tn, true, false, false);
		return;
	}

	emit_text(f, t, tn, attach_before, attach_after, glue);
}

/* Split a translation into atoms and apply each. */
static void render_english(fmt *f, const char *s)
{
	size_t i = 0, n = strlen(s);

	/* A translation that is only digits is glued to neighbouring digits. */
	bool all_digits = n > 0;

	for (size_t k = 0; k < n; k++) {
		if (s[k] < '0' || s[k] > '9') {
			all_digits = false;
			break;
		}
	}
	if (all_digits) {
		emit_text(f, s, n, false, false, true);
		return;
	}

	while (i < n) {
		if (s[i] == '{') {
			size_t j = i + 1;
			size_t depth = 1;

			while (j < n && depth) {
				if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
				if (s[j] == '{') depth++;
				else if (s[j] == '}') depth--;
				if (depth) j++;
			}
			apply_meta(f, s + i + 1, j - i - 1);
			i = (j < n) ? j + 1 : n;
		} else {
			size_t j = i;

			while (j < n && s[j] != '{')
				j++;
			/* Plover strips surrounding spaces from text atoms. */
			size_t a = i, b = j;

			while (a < b && s[a] == ' ') a++;
			while (b > a && s[b - 1] == ' ') b--;
			if (b > a)
				emit_text(f, s + a, b - a, false, false, false);
			i = j;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Engine                                                              */
/* ------------------------------------------------------------------ */

void steno_engine_init(steno_engine *e, steno_lookup_fn lookup, void *ctx,
		       unsigned max_key)
{
	memset(e, 0, sizeof(*e));
	e->lookup = lookup;
	e->ctx = ctx;
	e->max_key = max_key ? max_key : STENO_MAX_KEY;
	if (e->max_key > STENO_MAX_KEY)
		e->max_key = STENO_MAX_KEY;
	steno_engine_reset(e);
}

void steno_engine_reset(steno_engine *e)
{
	e->n_seg = 0;
	e->render_len = 0;
	e->render[0] = '\0';
	/* Plover emits a leading space for the first word typed. */
	e->head.space_next = true;
	e->head.case_next = CASE_NONE;
	e->head.last_glue = false;
}

static int try_lookup(steno_engine *e, const uint32_t *strokes, unsigned n,
		      char *out, size_t outsz)
{
	e->stat_lookups++;
	return e->lookup(e->ctx, strokes, n, out, outsz);
}

/*
 * Plover's _find_longest_match: consume as many trailing segments as the
 * dictionary's longest key allows, longest candidate first.
 *
 * On success fills @p out, sets *first_seg to the index of the first
 * replaced segment, and returns the stroke count of the match.
 */
static int find_longest(steno_engine *e, uint32_t stroke, unsigned min_len,
			char *out, size_t outsz, uint8_t *first_seg,
			uint32_t *strokes_out)
{
	/* How far back can we reach without exceeding max_key? */
	unsigned num = 1;
	int first = e->n_seg;

	while (first > 0) {
		unsigned add = e->seg[first - 1].n_strokes;

		if (num + add > e->max_key)
			break;
		num += add;
		first--;
	}

	/* Longest first: start at the earliest reachable segment. */
	for (int i = first; i <= e->n_seg; i++) {
		uint32_t buf[STENO_MAX_KEY];
		unsigned k = 0;

		for (int j = i; j < e->n_seg; j++) {
			for (unsigned s = 0; s < e->seg[j].n_strokes; s++) {
				if (k >= STENO_MAX_KEY)
					goto next;
				buf[k++] = e->seg[j].strokes[s];
			}
		}
		if (k >= STENO_MAX_KEY)
			continue;
		buf[k++] = stroke;

		if (k < min_len || k > e->max_key)
			continue;

		if (try_lookup(e, buf, k, out, outsz) >= 0) {
			*first_seg = (uint8_t)i;
			memcpy(strokes_out, buf, k * sizeof(uint32_t));
			return (int)k;
		}
next:
		continue;
	}
	return -1;
}

/*
 * Implicit suffix folding: if the stroke carries -S/-D/-G/-Z and removing
 * that key yields a match, the suffix's own translation is appended.
 * Plover: _lookup_with_suffix.
 */
static int find_with_suffix(steno_engine *e, uint32_t stroke,
			    char *out, size_t outsz, uint8_t *first_seg,
			    uint32_t *strokes_out, uint8_t *n_strokes_out)
{
	for (unsigned s = 0; s < N_SUFFIX_KEYS; s++) {
		uint32_t key = SUFFIX_KEYS[s];

		if (!(stroke & key))
			continue;

		char suffix_text[STENO_MAX_ENGLISH];
		uint32_t just_suffix = key;

		if (try_lookup(e, &just_suffix, 1, suffix_text,
			       sizeof(suffix_text)) < 0)
			continue;

		char main_text[STENO_MAX_ENGLISH];
		int k = find_longest(e, stroke & ~key, 1, main_text,
				     sizeof(main_text), first_seg, strokes_out);

		if (k < 0)
			continue;

		/* Plover joins as "main SPACE suffix". */
		size_t a = strlen(main_text), b = strlen(suffix_text);

		if (a + 1 + b + 1 > outsz)
			continue;
		memcpy(out, main_text, a);
		out[a] = ' ';
		memcpy(out + a + 1, suffix_text, b);
		out[a + 1 + b] = '\0';

		/* The real stroke, with the suffix key, is what we consumed. */
		strokes_out[k - 1] = stroke;
		*n_strokes_out = (uint8_t)k;
		return 0;
	}
	return -1;
}

/*
 * Re-render every retained segment.
 *
 * Records where each segment's text starts and the formatter state that
 * applied just before it. That state matters: whether a segment gets a
 * leading space depends on whether the previous one ended attached, so
 * retiring a segment would silently change how the next one renders unless
 * we carry the state forward.
 */
static void render_all(steno_engine *e, char *buf, uint16_t cap, uint16_t *len,
		       uint16_t *starts, steno_fmt_state *state_at)
{
	fmt f = {
		.buf = buf, .len = 0, .cap = cap,
		.space_next = e->head.space_next,
		.case_next  = e->head.case_next,
		.last_glue  = e->head.last_glue,
	};

	for (uint8_t i = 0; i < e->n_seg; i++) {
		starts[i] = f.len;
		state_at[i].space_next = f.space_next;
		state_at[i].case_next = f.case_next;
		state_at[i].last_glue = f.last_glue;
		if (e->seg[i].untranslated) {
			char raw[32];

			if (steno_format_stroke(e->seg[i].strokes[0], raw,
						sizeof(raw)) > 0)
				emit_text(&f, raw, strlen(raw), false, false,
					  false);
		} else {
			render_english(&f, e->seg[i].english);
		}
	}
	buf[f.len] = '\0';
	*len = f.len;
}

steno_action steno_engine_stroke(steno_engine *e, uint32_t stroke)
{
	steno_action act = { 0, e->out, 0 };
	char english[STENO_MAX_ENGLISH];
	uint32_t strokes[STENO_MAX_KEY];
	uint8_t first_seg = e->n_seg;
	uint8_t n_strokes = 1;
	bool untranslated = false;

	stroke &= STENO_KEY_MASK;
	e->stat_strokes++;

	/* 1. longest multi-stroke match (>= 2 strokes) */
	int k = find_longest(e, stroke, 2, english, sizeof(english),
			     &first_seg, strokes);

	if (k > 0) {
		n_strokes = (uint8_t)k;
	} else {
		/* 2. single-stroke match */
		uint32_t one = stroke;

		if (try_lookup(e, &one, 1, english, sizeof(english)) >= 0) {
			first_seg = e->n_seg;
			n_strokes = 1;
			strokes[0] = stroke;
		} else if (find_with_suffix(e, stroke, english, sizeof(english),
					    &first_seg, strokes,
					    &n_strokes) == 0) {
			/* 3. implicit suffix */
		} else {
			/* 4. untranslated */
			first_seg = e->n_seg;
			n_strokes = 1;
			strokes[0] = stroke;
			english[0] = '\0';
			untranslated = true;
			e->stat_untranslated++;
		}
	}

	/* Replace segments [first_seg .. n_seg) with the new one. */
	if (first_seg > STENO_MAX_SEGMENTS - 1)
		first_seg = STENO_MAX_SEGMENTS - 1;
	e->n_seg = first_seg;

	steno_segment *sg = &e->seg[e->n_seg];

	memset(sg, 0, sizeof(*sg));
	sg->n_strokes = n_strokes > STENO_MAX_KEY ? STENO_MAX_KEY : n_strokes;
	memcpy(sg->strokes, strokes, sg->n_strokes * sizeof(uint32_t));
	sg->untranslated = untranslated;
	if (!untranslated) {
		size_t n = strlen(english);

		if (n >= STENO_MAX_ENGLISH)
			n = STENO_MAX_ENGLISH - 1;
		memcpy(sg->english, english, n);
		sg->english[n] = '\0';
	}
	e->n_seg++;

	/* Re-render and diff against what we last emitted. */
	char newbuf[STENO_RENDER_BUF];
	uint16_t newlen = 0;
	uint16_t starts[STENO_MAX_SEGMENTS];
	steno_fmt_state state_at[STENO_MAX_SEGMENTS];

	render_all(e, newbuf, sizeof(newbuf), &newlen, starts, state_at);

	uint16_t common = 0;

	while (common < newlen && common < e->render_len &&
	       newbuf[common] == e->render[common])
		common++;

	act.backspaces = (uint16_t)(e->render_len - common);
	act.text_len = (uint16_t)(newlen - common);
	memcpy(e->out, newbuf + common, act.text_len);
	e->out[act.text_len] = '\0';
	act.text = e->out;

	memcpy(e->render, newbuf, newlen);
	e->render_len = newlen;
	e->render[newlen] = '\0';

	/*
	 * Retire the oldest segments, trimming the same bytes off the render
	 * so the next diff still lines up at offset 0, and adopting the
	 * formatter state that applied at the new head.
	 *
	 * Bounded by the render buffer as well as the segment count: a run of
	 * long translations would otherwise overflow the buffer and silently
	 * corrupt every subsequent diff. Never retire below the history the
	 * longest dictionary key could still need for re-segmentation.
	 */
	uint8_t keep_min = (uint8_t)(e->max_key + 1);

	if (keep_min > STENO_MAX_SEGMENTS - 1)
		keep_min = STENO_MAX_SEGMENTS - 1;

	while (e->n_seg > keep_min &&
	       (e->n_seg >= STENO_MAX_SEGMENTS ||
		e->render_len > STENO_RENDER_BUF / 2)) {
		uint16_t drop = starts[1];

		memmove(e->render, e->render + drop, e->render_len - drop);
		e->render_len -= drop;
		e->render[e->render_len] = '\0';
		memmove(&e->seg[0], &e->seg[1],
			(e->n_seg - 1) * sizeof(steno_segment));
		e->n_seg--;
		e->head = state_at[1];
		for (uint8_t i = 0; i + 1 < STENO_MAX_SEGMENTS; i++) {
			starts[i] = (uint16_t)(starts[i + 1] - drop);
			state_at[i] = state_at[i + 1];
		}
	}

	return act;
}
