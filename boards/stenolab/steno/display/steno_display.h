/*
 * Steno OLED display - public API
 *
 * A direct-framebuffer status/tape display for a 128x64 SSD1306, built on
 * Zephyr's display API only. It deliberately does NOT use CONFIG_ZMK_DISPLAY,
 * because that symbol does `select LVGL` (non-overridable), which costs
 * ~60-100KB of flash. This module costs roughly 4KB flash and 2.5KB RAM.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>

/*
 * 23-key steno mask. The bit order is "#STKPWHRAO*EUFRPBLGTSDZ" with bit 0
 * = '#', which is the same packing used for dictionary keys - so the value
 * you hand the display is the same uint32_t you hand the lookup, with no
 * translation layer in between.
 */
#define STENO_NUM   BIT(0)
#define STENO_S_L   BIT(1)
#define STENO_T_L   BIT(2)
#define STENO_K_L   BIT(3)
#define STENO_P_L   BIT(4)
#define STENO_W_L   BIT(5)
#define STENO_H_L   BIT(6)
#define STENO_R_L   BIT(7)
#define STENO_A     BIT(8)
#define STENO_O     BIT(9)
#define STENO_STAR  BIT(10)
#define STENO_E     BIT(11)
#define STENO_U     BIT(12)
#define STENO_F_R   BIT(13)
#define STENO_R_R   BIT(14)
#define STENO_P_R   BIT(15)
#define STENO_B_R   BIT(16)
#define STENO_L_R   BIT(17)
#define STENO_G_R   BIT(18)
#define STENO_T_R   BIT(19)
#define STENO_S_R   BIT(20)
#define STENO_D_R   BIT(21)
#define STENO_Z_R   BIT(22)

#define STENO_KEY_MASK 0x7FFFFFU

/** Engine diagnostics, rendered on the stats page. */
struct steno_display_stats {
	uint32_t lookup_ns;    /**< duration of the last dictionary lookup */
	uint32_t strokes;      /**< total strokes this session */
	uint16_t decompress;   /**< block decompressions for the last stroke */
	uint8_t  cache_pct;    /**< block-cache hit rate, 0-100 */
	uint8_t  undo_depth;   /**< strokes currently held for retroactive edit */
};

/**
 * Report the chord currently being held.
 *
 * Call this whenever the set of held steno keys changes, while the chord is
 * still accumulating. Cheap: it only flags a redraw, which is coalesced.
 */
void steno_display_chord(uint32_t keys);

/**
 * Report a committed stroke and the text it produced.
 *
 * @param keys the chord that fired
 * @param out  the translation, or NULL if the stroke produced no output.
 *             Copied immediately; the caller keeps ownership.
 * @param backspaces how many characters this stroke retracted, for
 *             retroactive corrections. Shown as "<<n" on the tape.
 */
void steno_display_stroke(uint32_t keys, const char *out, uint8_t backspaces);

/** Update the diagnostics page. Safe to call from the lookup hot path. */
void steno_display_set_stats(const struct steno_display_stats *stats);

/** Cycle to the next page (tape -> stats -> tape). Bind this to a key. */
void steno_display_next_page(void);

/** Force a full repaint, e.g. after waking from idle. */
void steno_display_refresh(void);
