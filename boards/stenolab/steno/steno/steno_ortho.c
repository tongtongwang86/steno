/*
 * Orthography: joining a suffix to a word the way English spelling wants.
 *
 * This is Plover's ORTHOGRAPHY_RULES, hand-compiled. Each of its 38 regexes
 * becomes a small predicate; they are tried in order and the first match
 * wins, which is exactly Plover's behaviour on its no-dictionary path.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO
 *
 * Plover's _add_suffix first generates candidates from the rules, keeps only
 * those present in american_english_words.txt, and picks the most frequent.
 * That word list is 338,882 words / 4.3 MB - six times the whole dictionary
 * budget - so it cannot ship. When no candidate is in the list Plover falls
 * back to "apply the rules in order, take the first", and that fallback is
 * what this implements. Divergence is therefore confined to words where the
 * list would have preferred a different plausible spelling.
 *
 * Several rules below look like a plain concatenation. They are not
 * redundant: they exist to match before the silent-e and
 * consonant-doubling rules at the end, which would otherwise fire.
 *
 * SPDX-License-Identifier: MIT
 */

#include "steno_ortho.h"
#include <string.h>

/*
 * All matching is case insensitive: Plover compiles ORTHOGRAPHY_RULES with
 * re.I, so "Ron" + "ing" doubles to "Ronning" exactly as "ron" would. The
 * replacement text keeps the original characters, so capitalisation
 * survives.
 */
static char lc(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool is_vowel(char c)
{
	c = lc(c);
	return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

static bool in_set(char c, const char *set)
{
	return c && strchr(set, lc(c)) != NULL;
}

/* word ends with `tail`, ignoring case */
static bool ends(const char *w, size_t wl, const char *tail)
{
	size_t tl = strlen(tail);

	if (wl < tl)
		return false;
	for (size_t i = 0; i < tl; i++)
		if (lc(w[wl - tl + i]) != lc(tail[i]))
			return false;
	return true;
}

static bool eq(const char *s, const char *t) { return strcmp(s, t) == 0; }

/* suffix == one of a NULL-terminated list */
static bool eq_any(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (eq(s, *list))
			return true;
	return false;
}

/* suffix begins with `pre`; if so *rest points past it */
static bool starts(const char *s, const char *pre, const char **rest)
{
	size_t pl = strlen(pre);

	if (strncmp(s, pre, pl) != 0)
		return false;
	*rest = s + pl;
	return true;
}

/* suffix == pre + one of list; *rest is the tail */
static bool starts_any(const char *s, const char *pre,
		       const char *const *list, const char **rest)
{
	const char *r;

	if (!starts(s, pre, &r))
		return false;
	if (!eq_any(r, list))
		return false;
	*rest = r;
	return true;
}

/* char at wl-n from the end, or 0 */
static char at(const char *w, size_t wl, size_t n)
{
	return (wl >= n) ? w[wl - n] : 0;
}

/* out = word[0..keep) + a + b ; returns length or -1 */
static int build(char *out, size_t outsz, const char *w, size_t keep,
		 const char *a, const char *b)
{
	size_t la = a ? strlen(a) : 0;
	size_t lb = b ? strlen(b) : 0;

	if (keep + la + lb + 1 > outsz)
		return -1;
	memcpy(out, w, keep);
	if (la) memcpy(out + keep, a, la);
	if (lb) memcpy(out + keep + la, b, lb);
	out[keep + la + lb] = '\0';
	return (int)(keep + la + lb);
}

#define CONS "bcdfghjklmnpqrstvwxz"

static const char *const LY[]      = {"ly", NULL};
static const char *const RY_ARY[]  = {"ry", "ary", NULL};
static const char *const Y_ILY[]   = {"y", "ily", NULL};
static const char *const Y_IES[]   = {"y", "ies", NULL};
static const char *const N_NS[]    = {"n", "ns", NULL};
static const char *const IAL[]     = {"ial", "ially", NULL};
static const char *const IFY[]     = {"y", "ying", "ied", "ies",
				      "ication", "ications", NULL};
static const char *const ICAL[]    = {"ical", "ically", NULL};
static const char *const AL_ALLY[] = {"al", "ally", NULL};
static const char *const ICA[]     = {"l", "lly", "lity", NULL};
static const char *const T_TS[]    = {"t", "ts", NULL};
static const char *const TIV[]     = {"e", "ity", "ities", NULL};
static const char *const IZ[]      = {"e", "es", "ing", "ed", "er", "ers",
				      "ation", "ations", "able", "ability",
				      NULL};
static const char *const IZ_AL[]   = {"e", "ed", "es", "ing", "er", "ers",
				      "ation", "ations", "m", "ms", "able",
				      "ability", "abilities", NULL};
static const char *const IZ_AR[]   = {"e", "ed", "es", "ing", "er", "ers",
				      "ation", "ations", "m", "ms", NULL};
static const char *const OLOG[]    = {"y", "ist", "ists", "ical", "ically",
				      NULL};

int steno_add_suffix(const char *w, const char *suf, char *out, size_t outsz)
{
	size_t wl = strlen(w);
	const char *g;

	if (wl == 0 || suf[0] == '\0')
		return build(out, outsz, w, wl, suf, NULL);

	/* 0: ^(.*[aeiou]c) ^ ly$ -> \1ally */
	if (eq_any(suf, LY) && lc(at(w, wl, 1)) == 'c' && is_vowel(at(w, wl, 2)))
		return build(out, outsz, w, wl, "ally", NULL);

	/* 1: ^(.+[aeioubmnp])le ^ ly$ -> \1ly */
	if (eq_any(suf, LY) && ends(w, wl, "le") && wl >= 3 &&
	    in_set(at(w, wl, 3), "aeioubmnp"))
		return build(out, outsz, w, wl - 2, "ly", NULL);

	/* 2: ^(.*t)e ^ (ry|ary)$ -> \1ory */
	if (eq_any(suf, RY_ARY) && ends(w, wl, "te"))
		return build(out, outsz, w, wl - 1, "ory", NULL);

	/* 3: ^(.+)m ^ tor(y|ily)$ -> \1mator\2 */
	if (lc(at(w, wl, 1)) == 'm' && wl >= 2 &&
	    starts_any(suf, "tor", Y_ILY, &g))
		return build(out, outsz, w, wl, "ator", g);

	/* 4: ^(.+)se ^ ar(y|ies)$ -> \1sor\2 */
	if (ends(w, wl, "se") && wl >= 3 && starts_any(suf, "ar", Y_IES, &g))
		return build(out, outsz, w, wl - 1, "or", g);

	/* 5: ^(.*[naeiou])te? ^ cy$ -> \1cy */
	if (eq(suf, "cy")) {
		if (ends(w, wl, "te") && in_set(at(w, wl, 3), "naeiou"))
			return build(out, outsz, w, wl - 2, "cy", NULL);
		if (lc(at(w, wl, 1)) == 't' && in_set(at(w, wl, 2), "naeiou"))
			return build(out, outsz, w, wl - 1, "cy", NULL);
	}

	/* 6: ^(.*(?:s|sh|x|z|zh)) ^ s$ -> \1es */
	if (eq(suf, "s") &&
	    (ends(w, wl, "s") || ends(w, wl, "sh") || ends(w, wl, "x") ||
	     ends(w, wl, "z") || ends(w, wl, "zh")))
		return build(out, outsz, w, wl, "es", NULL);

	/* 7: ^(.*(?:oa|ea|i|ee|oo|au|ou|l|n|(?<![gin]a)r|t)ch) ^ s$ -> \1es */
	if (eq(suf, "s") && ends(w, wl, "ch")) {
		char p = lc(at(w, wl, 3));
		bool ok = false;

		if (in_set(p, "ilnt"))
			ok = true;
		else if (p == 'a' && in_set(at(w, wl, 4), "oe"))
			ok = true;                       /* oach / each */
		else if (p == 'e' && in_set(at(w, wl, 4), "e"))
			ok = true;                       /* eech */
		else if (p == 'o' && in_set(at(w, wl, 4), "o"))
			ok = true;                       /* ooch */
		else if (p == 'u' && in_set(at(w, wl, 4), "ao"))
			ok = true;                       /* auch / ouch */
		else if (p == 'r')
			/* not when preceded by [gin]a: monarchs, not monarches */
			ok = !(lc(at(w, wl, 4)) == 'a' && in_set(at(w, wl, 5), "gin"));
		if (ok)
			return build(out, outsz, w, wl, "es", NULL);
	}

	/* 8: ^(.+[bcdfghjklmnpqrstvwxz])y ^ s$ -> \1ies */
	if (eq(suf, "s") && lc(at(w, wl, 1)) == 'y' && wl >= 2 &&
	    in_set(at(w, wl, 2), CONS))
		return build(out, outsz, w, wl - 1, "ies", NULL);

	/* 9: ^(.+)ie ^ ing$ -> \1ying */
	if (eq(suf, "ing") && ends(w, wl, "ie") && wl >= 3)
		return build(out, outsz, w, wl - 2, "ying", NULL);

	/* 10: ^(.+[cdfghlmnpr])y ^ ist$ -> \1ist */
	if (eq(suf, "ist") && lc(at(w, wl, 1)) == 'y' && wl >= 2 &&
	    in_set(at(w, wl, 2), "cdfghlmnpr"))
		return build(out, outsz, w, wl - 1, "ist", NULL);

	/* 11: ^(.+[bcdfghjklmnpqrstvwxz])y ^ ([a-hj-xz].*)$ -> \1i\2 */
	if (lc(at(w, wl, 1)) == 'y' && wl >= 2 && in_set(at(w, wl, 2), CONS) &&
	    suf[0] >= 'a' && suf[0] <= 'z' && suf[0] != 'i' && suf[0] != 'y')
		return build(out, outsz, w, wl - 1, "i", suf);

	/* 12: ^(.+)te ^ en$ -> \1tten */
	if (eq(suf, "en") && ends(w, wl, "te") && wl >= 3)
		return build(out, outsz, w, wl - 2, "tten", NULL);

	/* 13: ^(.+[ae]) ^ e(n|ns)$ -> \1\2 */
	if (in_set(at(w, wl, 1), "ae") && wl >= 2 &&
	    starts_any(suf, "e", N_NS, &g))
		return build(out, outsz, w, wl, g, NULL);

	/* 14: ^(.+)y ^ (ial|ially)$ -> \1\2 */
	if (eq_any(suf, IAL) && lc(at(w, wl, 1)) == 'y' && wl >= 2)
		return build(out, outsz, w, wl - 1, suf, NULL);

	/* 15: ^(.+i) ^ if(y|ying|ied|ies|ication|ications)$ -> \1f\2 */
	if (lc(at(w, wl, 1)) == 'i' && wl >= 2 && starts_any(suf, "if", IFY, &g))
		return build(out, outsz, w, wl, "f", g);

	/* 16: ^(.*[^aeiou])y ^ if(...)$ -> \1if\2 */
	if (lc(at(w, wl, 1)) == 'y' && wl >= 2 && !is_vowel(at(w, wl, 2)) &&
	    starts_any(suf, "if", IFY, &g))
		return build(out, outsz, w, wl - 1, "if", g);

	/* 17: ^(.+)ic ^ (ical|ically)$ -> \1\2 */
	if (eq_any(suf, ICAL) && ends(w, wl, "ic") && wl >= 3)
		return build(out, outsz, w, wl - 2, suf, NULL);

	/* 18: ^(.+)ology ^ ic(al|ally)$ -> \1ologic\2 */
	if (ends(w, wl, "ology") && wl >= 6 &&
	    starts_any(suf, "ic", AL_ALLY, &g))
		return build(out, outsz, w, wl - 1, "ic", g);

	/* 19: ^(.*)ry ^ ica(l|lly|lity)$ -> \1rica\2 */
	if (ends(w, wl, "ry") && starts_any(suf, "ica", ICA, &g))
		return build(out, outsz, w, wl - 1, "ica", g);

	/* 20: ^(.*[l]) ^ is(t|ts)$ -> \1is\2 */
	if (lc(at(w, wl, 1)) == 'l' && starts_any(suf, "is", T_TS, &g))
		return build(out, outsz, w, wl, suf, NULL);

	/* 21: ^(.*)ry ^ ity$ -> \1rity */
	if (eq(suf, "ity") && ends(w, wl, "ry"))
		return build(out, outsz, w, wl - 1, "ity", NULL);

	/* 22: ^(.*)l ^ ity$ -> \1lity */
	if (eq(suf, "ity") && lc(at(w, wl, 1)) == 'l')
		return build(out, outsz, w, wl, "ity", NULL);

	/* 23: ^(.+)rm ^ tiv(e|ity|ities)$ -> \1rmativ\2 */
	if (ends(w, wl, "rm") && wl >= 3 && starts_any(suf, "tiv", TIV, &g))
		return build(out, outsz, w, wl, "ativ", g);

	/* 24: ^(.+)e ^ tiv(e|ity|ities)$ -> \1ativ\2 */
	if (lc(at(w, wl, 1)) == 'e' && wl >= 2 && starts_any(suf, "tiv", TIV, &g))
		return build(out, outsz, w, wl - 1, "ativ", g);

	/* 25/26: ^(.+)y ^ i[zs](...)$ -> \1i[zs]\2 */
	if (lc(at(w, wl, 1)) == 'y' && wl >= 2) {
		if (starts_any(suf, "iz", IZ, &g))
			return build(out, outsz, w, wl - 1, "iz", g);
		if (starts_any(suf, "is", IZ, &g))
			return build(out, outsz, w, wl - 1, "is", g);
	}

	/* 27/28: ^(.+)al ^ i[zs](...)$ -> \1ali[zs]\2  (plain join) */
	if (ends(w, wl, "al") && wl >= 3 &&
	    (starts_any(suf, "iz", IZ_AL, &g) || starts_any(suf, "is", IZ_AL, &g)))
		return build(out, outsz, w, wl, suf, NULL);

	/* 29/30: ^(.+)ar ^ i[zs](...)$ -> \1ari[zs]\2  (plain join) */
	if (ends(w, wl, "ar") && wl >= 3 &&
	    (starts_any(suf, "iz", IZ_AR, &g) || starts_any(suf, "is", IZ_AR, &g)))
		return build(out, outsz, w, wl, suf, NULL);

	/* 31/32: ^(.*[lmnty]) ^ i[zs](...)$ -> \1i[zs]\2  (plain join) */
	if (in_set(at(w, wl, 1), "lmnty") &&
	    (starts_any(suf, "iz", IZ_AL, &g) || starts_any(suf, "is", IZ_AL, &g)))
		return build(out, outsz, w, wl, suf, NULL);

	/* 33: ^(.+)al ^ olog(y|ist|ists|ical|ically)$ -> \1olog\2 */
	if (ends(w, wl, "al") && wl >= 3 && starts_any(suf, "olog", OLOG, &g))
		return build(out, outsz, w, wl - 2, "olog", g);

	/* 34: ^(.+)(ar|er|or) ^ ish$ -> \1\2ish  (plain join) */
	if (eq(suf, "ish") && wl >= 3 && lc(at(w, wl, 1)) == 'r' &&
	    in_set(at(w, wl, 2), "aeo"))
		return build(out, outsz, w, wl, "ish", NULL);

	/* 35: ^(.+e)e ^ (e.+)$ -> \1\2 */
	if (ends(w, wl, "ee") && wl >= 3 && suf[0] == 'e' && suf[1])
		return build(out, outsz, w, wl - 1, suf, NULL);

	/* 36: ^(.+[bcdfghjklmnpqrstuvwxz])e ^ ([aeiouy].*)$ -> \1\2
	 * the silent-e drop: pune + ing -> puning */
	if (lc(at(w, wl, 1)) == 'e' && wl >= 2 &&
	    in_set(at(w, wl, 2), "bcdfghjklmnpqrstuvwxz") &&
	    (is_vowel(suf[0]) || suf[0] == 'y'))
		return build(out, outsz, w, wl - 1, suf, NULL);

	/* 37: consonant doubling: flub + ing -> flubbing */
	if ((is_vowel(suf[0]) || suf[0] == 'y') && wl >= 3) {
		char last = at(w, wl, 1);
		char v    = at(w, wl, 2);
		char pre  = at(w, wl, 3);

		if (in_set(last, "bcdfgklmnprtvz") && is_vowel(v) &&
		    (in_set(pre, "bcdfghjklmnprstvwxyz") ||
		     (pre == 'u' && lc(at(w, wl, 4)) == 'q'))) {
			char dbl[2] = { last, '\0' };

			return build(out, outsz, w, wl, dbl, suf);
		}
	}

	/* no rule matched */
	return build(out, outsz, w, wl, suf, NULL);
}

bool steno_has_word_boundary(const char *s)
{
	for (; *s; s++)
		if (*s == ' ' || *s == '\t' || *s == '\n')
			return true;
	return false;
}
