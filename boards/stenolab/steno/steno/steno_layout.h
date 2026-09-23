/*
 * Physical key position -> steno key mapping for this board.
 *
 * Positions are kscan/matrix-transform order, i.e. the order of
 * input-gpios in the devicetree: 0..26.
 *
 * Derived from the board's original keymap, whose QWERTY letters follow
 * Plover's standard keyboard-mode layout:
 *
 *     N1 N2 N3 N4                          number bar (all '#')
 *     W  E  R  T   U  I  O  P  [           T- P- H- *   -F -P -L -T -D
 *     A  S  D  F   J  K  L  ;  '           S- K- W- R-  -R -B -G -S -Z
 *              C V   N M   <mo 1>          A- O-  -E -U  (layer toggle)
 *
 * That accounts for all 23 steno keys exactly once, with position 26
 * left over as the control-layer toggle.
 *
 * If a key turns out to be wrong, fix it here - one line, then rebuild.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef STENO_LAYOUT_H
#define STENO_LAYOUT_H

#include "steno_engine.h"

#define STENO_NONE 0u   /* not a steno key */

#define STENO_POSITIONS 27

static const uint32_t steno_layout[STENO_POSITIONS] = {
	/*  0 */ STENO_NUM,    /* N1  number bar */
	/*  1 */ STENO_NUM,    /* N2  number bar */
	/*  2 */ STENO_NUM,    /* N3  number bar */
	/*  3 */ STENO_NUM,    /* N4  number bar */

	/*  4 */ STENO_T_L,    /* W */
	/*  5 */ STENO_P_L,    /* E */
	/*  6 */ STENO_H_L,    /* R */
	/*  7 */ STENO_STAR,   /* T */
	/*  8 */ STENO_F_R,    /* U */
	/*  9 */ STENO_P_R,    /* I */
	/* 10 */ STENO_L_R,    /* O */
	/* 11 */ STENO_T_R,    /* P */
	/* 12 */ STENO_D_R,    /* [ */

	/* 13 */ STENO_S_L,    /* A */
	/* 14 */ STENO_K_L,    /* S */
	/* 15 */ STENO_W_L,    /* D */
	/* 16 */ STENO_R_L,    /* F */
	/* 17 */ STENO_R_R,    /* J */
	/* 18 */ STENO_B_R,    /* K */
	/* 19 */ STENO_G_R,    /* L */
	/* 20 */ STENO_S_R,    /* ; */
	/* 21 */ STENO_Z_R,    /* ' */

	/* 22 */ STENO_A,      /* C */
	/* 23 */ STENO_O,      /* V */
	/* 24 */ STENO_E,      /* N */
	/* 25 */ STENO_U,      /* M */

	/* 26 */ STENO_NONE,   /* &mo 1 - control layer, never a steno key */
};

#endif /* STENO_LAYOUT_H */
