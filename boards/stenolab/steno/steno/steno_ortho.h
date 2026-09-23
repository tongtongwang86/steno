/*
 * Orthography: English suffix joining. See steno_ortho.c.
 * SPDX-License-Identifier: MIT
 */
#ifndef STENO_ORTHO_H
#define STENO_ORTHO_H

#include <stddef.h>
#include <stdbool.h>

/**
 * Join @p suffix to @p word applying English spelling rules.
 * Always produces something: falls back to a plain concatenation.
 * @return length written, or -1 if it would not fit.
 */
int steno_add_suffix(const char *word, const char *suffix,
		     char *out, size_t outsz);

/** True if the string contains whitespace (Plover's has_word_boundary). */
bool steno_has_word_boundary(const char *s);

#endif
