/*
 * Key combos parsed from {#...} metas. See steno_combo.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef STENO_COMBO_H
#define STENO_COMBO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* HID modifier bits, usage page 0x07 report byte 0. */
#define STENO_MOD_LCTRL  (1u << 0)
#define STENO_MOD_LSHIFT (1u << 1)
#define STENO_MOD_LALT   (1u << 2)
#define STENO_MOD_LGUI   (1u << 3)
#define STENO_MOD_RCTRL  (1u << 4)
#define STENO_MOD_RSHIFT (1u << 5)
#define STENO_MOD_RALT   (1u << 6)
#define STENO_MOD_RGUI   (1u << 7)

typedef struct {
	uint8_t  mods;    /* HID modifier bits held while tapping */
	uint16_t usage;   /* HID keyboard usage ID */
} steno_key_combo;

/**
 * Parse the body of a {#...} meta into a sequence of key taps.
 * Unknown keysyms are skipped rather than guessed at.
 * @return number of taps written, never more than @p max.
 */
int steno_parse_combo(const char *s, size_t n, steno_key_combo *out,
		      unsigned max);

#endif
