/*
 * steno_engine - stroke segmentation and output formatting
 *
 * Pure C, no RTOS and no ZMK dependency, so it can be replayed against
 * reference traces on a host. Takes stroke masks in, produces the exact
 * (backspaces, text) action a host should receive.
 *
 * Algorithms follow Plover's translator and formatter; see docs/ENGINE.md
 * for the correspondence and for what is deliberately not implemented yet.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef STENO_ENGINE_H
#define STENO_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest stroke sequence a dictionary entry may use. */
#ifndef STENO_MAX_KEY
#define STENO_MAX_KEY 12
#endif

/* Retained translations. Anything older can no longer be re-segmented. */
#ifndef STENO_MAX_SEGMENTS
#define STENO_MAX_SEGMENTS 16
#endif

/* Longest single translation. */
#ifndef STENO_MAX_ENGLISH
#define STENO_MAX_ENGLISH 96
#endif

/* Rendered output window. Must exceed the longest run of text the retained
 * segments can produce, or a re-segmentation could reach past the buffer. */
#ifndef STENO_RENDER_BUF
#define STENO_RENDER_BUF 512
#endif

/* Steno key bits, order "#STKPWHRAO*EUFRPBLGTSDZ" - Plover's KEYS exactly. */
#define STENO_NUM   (1u << 0)
#define STENO_S_L   (1u << 1)
#define STENO_T_L   (1u << 2)
#define STENO_K_L   (1u << 3)
#define STENO_P_L   (1u << 4)
#define STENO_W_L   (1u << 5)
#define STENO_H_L   (1u << 6)
#define STENO_R_L   (1u << 7)
#define STENO_A     (1u << 8)
#define STENO_O     (1u << 9)
#define STENO_STAR  (1u << 10)
#define STENO_E     (1u << 11)
#define STENO_U     (1u << 12)
#define STENO_F_R   (1u << 13)
#define STENO_R_R   (1u << 14)
#define STENO_P_R   (1u << 15)
#define STENO_B_R   (1u << 16)
#define STENO_L_R   (1u << 17)
#define STENO_G_R   (1u << 18)
#define STENO_T_R   (1u << 19)
#define STENO_S_R   (1u << 20)
#define STENO_D_R   (1u << 21)
#define STENO_Z_R   (1u << 22)
#define STENO_KEY_MASK 0x7FFFFFu

/**
 * Dictionary lookup callback.
 * @return translation length, or negative when the sequence is not present.
 */
typedef int (*steno_lookup_fn)(void *ctx, const uint32_t *strokes, unsigned n,
			       char *out, size_t outsz);

/** What the host should be sent for one stroke. */
typedef struct {
	uint16_t    backspaces;
	const char *text;       /* NUL-terminated, owned by the engine */
	uint16_t    text_len;
} steno_action;

/* Formatter state carried across the window head. */
typedef struct {
	bool    space_next;
	uint8_t case_next;
	bool    last_glue;
} steno_fmt_state;

typedef struct {
	uint8_t  n_strokes;
	bool     untranslated;
	uint32_t strokes[STENO_MAX_KEY];
	char     english[STENO_MAX_ENGLISH];
	uint16_t render_off;    /* where this segment's text begins */
} steno_segment;

typedef struct {
	steno_lookup_fn lookup;
	void           *ctx;
	unsigned        max_key;

	steno_segment seg[STENO_MAX_SEGMENTS];
	uint8_t       n_seg;

	char            render[STENO_RENDER_BUF];
	uint16_t        render_len;
	steno_fmt_state head;      /* state at the start of the retained window */

	char     out[STENO_RENDER_BUF];   /* text of the last action */

	/* stats, for the diagnostics page */
	uint32_t stat_strokes;
	uint32_t stat_lookups;
	uint32_t stat_untranslated;
} steno_engine;

/**
 * @param max_key longest stroke sequence in the dictionary (image header's
 *                max_strokes). Clamped to STENO_MAX_KEY.
 */
void steno_engine_init(steno_engine *e, steno_lookup_fn lookup, void *ctx,
		       unsigned max_key);

/** Feed one chord. Returns what to send to the host. */
steno_action steno_engine_stroke(steno_engine *e, uint32_t stroke);

/** Drop all history. Emits nothing. */
void steno_engine_reset(steno_engine *e);

/** Render a stroke mask as steno (e.g. "TKPW-G"), for untranslated output. */
int steno_format_stroke(uint32_t stroke, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* STENO_ENGINE_H */
