/*
 * sdic - compressed steno dictionary reader
 *
 * Reads an SDIC image produced by tools/mkdict.py. See docs/FORMAT.md.
 *
 * No dynamic allocation, no global state, no RTOS dependency. All flash
 * access goes through one accessor (sdic_fetch) so that pointing this at
 * external QSPI later is a one-function change.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef SDIC_H
#define SDIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest stroke sequence any entry may use. Plover's main.json tops out at
 * 11; the header carries the image's real value and open() validates it. */
#define SDIC_MAX_STROKES 16
#define SDIC_MAX_KEY     (SDIC_MAX_STROKES * 3)

/* Re-Pair expansion stack. Expanding one symbol pushes two and pops one, so
 * the stack grows by at most the maximum rule depth (measured: 9). */
#define SDIC_EXPAND_STACK 64

/* Longest translation we will return. */
#ifndef SDIC_MAX_VALUE
#define SDIC_MAX_VALUE 256
#endif

#define SDIC_MAGIC   0x31434453u   /* 'SDC1' */
#define SDIC_VERSION 1

typedef struct {
	uint32_t off_bits;
	uint32_t off_rules;
	uint32_t off_huff;
	uint32_t off_dir;
	uint16_t n_rules;
	uint16_t n_syms;
	uint32_t n_groups;
} sdic_stream_desc;

typedef struct {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint32_t image_size;
	uint32_t n_entries;
	uint32_t n_values;
	uint32_t max_strokes;
	uint32_t group_shift;
	uint32_t reserved;
	sdic_stream_desc key, pool, id;
} sdic_header;

/* Resolved stream: base pointers plus the canonical Huffman decode tables,
 * reconstructed once at open() rather than stored in the image. */
typedef struct {
	const uint8_t  *bits;
	const uint16_t *rules;
	const uint16_t *hcount;   /* [0..16] */
	const uint16_t *hsyms;
	const uint32_t *dir;
	uint32_t first_code[17];
	uint32_t first_index[17];
	uint32_t n_groups;
	uint16_t n_rules;
	uint16_t n_syms_total;
} sdic_stream;

typedef struct {
	const uint8_t *image;
	const sdic_header *hdr;
	sdic_stream key, pool, id;
	uint32_t group_size;
	uint32_t group_mask;
	uint32_t group_shift;

	/* instrumentation, useful on the diagnostics page */
	uint32_t stat_probes;
	uint32_t stat_group_decodes;
} sdic;

/* Errors */
enum {
	SDIC_OK            =  0,
	SDIC_E_MAGIC       = -1,
	SDIC_E_VERSION     = -2,
	SDIC_E_TRUNCATED   = -3,
	SDIC_E_STROKES     = -4,
	SDIC_E_CORRUPT     = -5,
	SDIC_E_NOTFOUND    = -6,
	SDIC_E_TOOLONG     = -7,
};

/**
 * Attach to an image already resident in memory-mapped flash.
 * Does not copy and does not allocate. @p len may be 0 to skip the
 * truncation check when the true size is not known.
 */
int sdic_open(sdic *d, const void *image, size_t len);

/**
 * Exact lookup of a stroke sequence.
 *
 * @param strokes  array of 23-bit masks, bit order "#STKPWHRAO*EUFRPBLGTSDZ"
 * @param n        number of strokes, 1..max_strokes
 * @param out      receives the NUL-terminated translation
 * @param outsz    size of @p out
 * @return translation length, or a negative SDIC_E_* code.
 */
int sdic_lookup(sdic *d, const uint32_t *strokes, unsigned n,
		char *out, size_t outsz);

/** Number of entries in the image. */
static inline uint32_t sdic_count(const sdic *d) { return d->hdr->n_entries; }

#ifdef __cplusplus
}
#endif

#endif /* SDIC_H */
