/*
 * Steno OLED display - direct framebuffer driver for SSD1306 128x64.
 *
 * Deliberately does not use CONFIG_ZMK_DISPLAY / LVGL. See steno_display.h.
 *
 * Framebuffer layout matches the SSD1306's native tiling as reported by the
 * Zephyr driver: SCREEN_INFO_MONO_VTILED with SCREEN_INFO_MONO_MSB_FIRST
 * *not* set, i.e. one byte covers 8 vertically-stacked pixels and bit 0 is
 * the topmost. fb[page * 128 + x] holds the column at x for that page.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdio.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/events/battery_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#include <zmk/endpoints.h>
#include <zmk/endpoints_types.h>
#include <zmk/events/endpoint_changed.h>

#include "steno_display.h"
#include "font5x7.h"

LOG_MODULE_REGISTER(steno_display, CONFIG_STENO_DISPLAY_LOG_LEVEL);

#define FB_W     128
#define FB_H     64
#define FB_PAGES (FB_H / 8)
#define FB_SIZE  (FB_W * FB_PAGES)

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint8_t fb[FB_SIZE];
static uint8_t dirty;          /* one bit per 8px page */
static bool    ready;
static bool    blanked;

/* ------------------------------------------------------------------ */
/* Shared state, written by the engine / ZMK events, read by the thread */
/* ------------------------------------------------------------------ */

#define TAPE_CHARS 63          /* 3 lines x 21 columns */

static struct {
	uint32_t held;                     /* chord in progress */
	uint32_t last;                     /* last committed chord */
	char     tape[TAPE_CHARS + 1];     /* rolling output text */
	uint8_t  tape_len;
	uint8_t  page;
	struct steno_display_stats stats;
} st;

static K_MUTEX_DEFINE(st_lock);
static K_SEM_DEFINE(wake, 0, 1);

static inline void poke(void)
{
	k_sem_give(&wake);
}

/* ------------------------------------------------------------------ */
/* Framebuffer primitives                                              */
/* ------------------------------------------------------------------ */

static inline void px(int x, int y, bool on)
{
	if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) {
		return;
	}
	uint8_t *b = &fb[(y >> 3) * FB_W + x];
	uint8_t m = BIT(y & 7);

	if (on) {
		*b |= m;
	} else {
		*b &= ~m;
	}
	dirty |= BIT(y >> 3);
}

static void fill(int x, int y, int w, int h, bool on)
{
	for (int j = y; j < y + h; j++) {
		for (int i = x; i < x + w; i++) {
			px(i, j, on);
		}
	}
}

static void hline(int x, int y, int w)
{
	for (int i = x; i < x + w; i++) {
		px(i, y, true);
	}
}

/*
 * Draw one glyph.
 *
 * This only ever touches pixels the glyph itself sets - it never writes
 * the background. That matters: an earlier version wrote `false` for every
 * off-pixel, which silently erased whatever the glyph was sitting on (box
 * borders, highlight blocks). Callers that want a background fill it first
 * and pass inv=true, which clears the glyph out of it instead.
 */
static void glyph(int x, int y, char c, bool inv)
{
	if (c < FONT5X7_FIRST || c > FONT5X7_LAST) {
		c = '?';
	}
	const uint8_t *g = font5x7[(uint8_t)c - FONT5X7_FIRST];

	for (int col = 0; col < FONT5X7_WIDTH; col++) {
		for (int row = 0; row < FONT5X7_HEIGHT; row++) {
			if ((g[col] >> row) & 1) {
				px(x + col, y + row, !inv);
			}
		}
	}
}

static int text(int x, int y, const char *s, bool inv)
{
	if (inv) {
		int w = (int)strlen(s) * FONT5X7_ADVANCE;

		fill(x - 1, y - 1, w + 1, FONT5X7_HEIGHT + 2, true);
	}
	while (*s && x < FB_W) {
		glyph(x, y, *s++, inv);
		x += FONT5X7_ADVANCE;
	}
	return x;
}

static void clear(void)
{
	memset(fb, 0, sizeof(fb));
	dirty = 0xFF;
}

/*
 * Push dirty pages. The SSD1306 driver requires y % 8 == 0, height % 8 == 0
 * and pitch == width, so we emit one display_write per run of dirty pages
 * rather than one per page - fewer I2C transactions for the common case
 * where the whole tape area changed at once.
 */
static void flush(void)
{
	if (!dirty || blanked) {
		return;
	}

	for (int p = 0; p < FB_PAGES;) {
		if (!(dirty & BIT(p))) {
			p++;
			continue;
		}
		int q = p;

		while (q < FB_PAGES && (dirty & BIT(q))) {
			q++;
		}

		const struct display_buffer_descriptor d = {
			.buf_size = (uint32_t)(q - p) * FB_W,
			.width    = FB_W,
			.height   = (uint16_t)((q - p) * 8),
			.pitch    = FB_W,
		};

		int err = display_write(disp, 0, p * 8, &d, &fb[p * FB_W]);

		if (err) {
			LOG_WRN("display_write(page %d..%d) failed: %d", p, q - 1, err);
		}
		p = q;
	}
	dirty = 0;
}

/* ------------------------------------------------------------------ */
/* Steno key layout                                                    */
/* ------------------------------------------------------------------ */

/*
 * Steno-tape style layout: every key's letter is always drawn, and a pressed
 * key is shown as an inverted block. This is both more legible at 128x64 and
 * more compact than outlined cells - a 7px glyph plus a 1px frame does not
 * fit in an 8px row, and the frame is wasted contrast anyway.
 *
 *   NNNNNNNNNNNNNNNNNNNN      number bar
 *   S T P H * F P L T D
 *     K W R * R B G S Z
 *       A O   E U
 *
 * S and * are single tall keys spanning both consonant rows.
 */
#define HL_W    9    /* highlight block width  */
#define HL_H    9    /* highlight block height */
#define CELL_DX 12
#define CELL_X0 4
#define GLYPH_DX 2   /* centres a 5px glyph in a 9px block */

#define NUMBAR_Y 9
#define ROW0_Y  13
#define ROW1_Y  22
#define ROW2_Y  31
#define TALL_Y  17   /* glyph baseline for a key spanning ROW0..ROW1 */
#define CHORD_BOTTOM 38

#define ROW_TALL 3   /* spans ROW0 and ROW1 */

struct cell {
	uint32_t bit;
	char     label;
	uint8_t  col;
	uint8_t  row;
};

static const struct cell cells[] = {
	{STENO_S_L,  'S', 0, ROW_TALL},
	{STENO_T_L,  'T', 1, 0},
	{STENO_P_L,  'P', 2, 0},
	{STENO_H_L,  'H', 3, 0},
	{STENO_STAR, '*', 4, ROW_TALL},
	{STENO_F_R,  'F', 5, 0},
	{STENO_P_R,  'P', 6, 0},
	{STENO_L_R,  'L', 7, 0},
	{STENO_T_R,  'T', 8, 0},
	{STENO_D_R,  'D', 9, 0},

	{STENO_K_L,  'K', 1, 1},
	{STENO_W_L,  'W', 2, 1},
	{STENO_R_L,  'R', 3, 1},
	{STENO_R_R,  'R', 5, 1},
	{STENO_B_R,  'B', 6, 1},
	{STENO_G_R,  'G', 7, 1},
	{STENO_S_R,  'S', 8, 1},
	{STENO_Z_R,  'Z', 9, 1},

	{STENO_A,    'A', 2, 2},
	{STENO_O,    'O', 3, 2},
	{STENO_E,    'E', 5, 2},
	{STENO_U,    'U', 6, 2},
};

static void draw_chord(uint32_t keys)
{
	const int bar_w = CELL_DX * 9 + HL_W;

	fill(0, NUMBAR_Y, FB_W, CHORD_BOTTOM - NUMBAR_Y, false);

	/* number bar: a solid slab when engaged, a hairline when not */
	if (keys & STENO_NUM) {
		fill(CELL_X0, NUMBAR_Y, bar_w, 3, true);
	} else {
		hline(CELL_X0, NUMBAR_Y + 1, bar_w);
	}

	for (size_t i = 0; i < ARRAY_SIZE(cells); i++) {
		const struct cell *c = &cells[i];
		int x = CELL_X0 + c->col * CELL_DX;
		int gy, hy, hh;

		switch (c->row) {
		case 0:        gy = ROW0_Y; hy = ROW0_Y - 1; hh = HL_H;     break;
		case 1:        gy = ROW1_Y; hy = ROW1_Y - 1; hh = HL_H;     break;
		case 2:        gy = ROW2_Y; hy = ROW2_Y - 1; hh = HL_H;     break;
		case ROW_TALL: gy = TALL_Y; hy = ROW0_Y - 1; hh = HL_H * 2; break;
		default:       continue;
		}

		bool on = (keys & c->bit) != 0;

		if (on) {
			fill(x, hy, HL_W, hh, true);
		}
		glyph(x + GLYPH_DX, gy, c->label, on);
	}
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

static void draw_status_bar(void)
{
	char buf[24];
	int n = 0;

	fill(0, 0, FB_W, 8, false);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
	n += snprintf(buf + n, sizeof(buf) - n, "%3u%% ", zmk_battery_state_of_charge());
#endif

	struct zmk_endpoint_instance ep = zmk_endpoint_get_selected();

	switch (ep.transport) {
	case ZMK_TRANSPORT_USB:
		n += snprintf(buf + n, sizeof(buf) - n, "USB");
		break;
#if IS_ENABLED(CONFIG_ZMK_BLE)
	case ZMK_TRANSPORT_BLE:
		n += snprintf(buf + n, sizeof(buf) - n, "BT%d%s",
			      zmk_ble_active_profile_index(),
			      zmk_ble_active_profile_is_connected() ? "" : "?");
		break;
#endif
	default:
		n += snprintf(buf + n, sizeof(buf) - n, "---");
		break;
	}

	text(0, 0, buf, false);

	/* page indicator, right-aligned */
	const char *tag = st.page == 0 ? "TAPE" : "DIAG";

	text(FB_W - 4 * FONT5X7_ADVANCE, 0, tag, false);
	hline(0, 8, FB_W);
}

#define TAPE_COLS  21
#define TAPE_LINES 3
#define TAPE_Y     41

static void draw_tape_page(void)
{
	draw_chord(st.held ? st.held : st.last);

	hline(0, CHORD_BOTTOM + 1, FB_W);
	fill(0, TAPE_Y, FB_W, FB_H - TAPE_Y, false);

	/*
	 * Greedy word wrap, keeping the last TAPE_LINES produced. Breaking
	 * mid-word on a 21-column line is unreadable at speed, which is
	 * exactly when you are looking at this.
	 */
	char buf[TAPE_LINES + 1][TAPE_COLS + 1];
	int n = 0;
	const char *p = st.tape;

	while (*p) {
		int len = 0;

		while (p[len] && len < TAPE_COLS) {
			len++;
		}

		int brk = len;

		if (p[len]) {
			int s = len;

			while (s > 0 && p[s] != ' ') {
				s--;
			}
			if (s > 0) {
				brk = s + 1;   /* keep the space on this line */
			}
		}
		if (brk <= 0) {
			brk = 1;
		}

		memcpy(buf[n % (TAPE_LINES + 1)], p, brk);
		buf[n % (TAPE_LINES + 1)][brk] = '\0';
		n++;
		p += brk;
	}

	int show = MIN(n, TAPE_LINES);

	for (int i = 0; i < show; i++) {
		text(0, TAPE_Y + i * 8,
		     buf[(n - show + i) % (TAPE_LINES + 1)], false);
	}
}

static void draw_diag_page(void)
{
	char buf[24];

	fill(0, 10, FB_W, 54, false);

	snprintf(buf, sizeof(buf), "LOOKUP  %lu us",
		 (unsigned long)(st.stats.lookup_ns / 1000U));
	text(0, 12, buf, false);

	snprintf(buf, sizeof(buf), "DECOMP  %u blk", st.stats.decompress);
	text(0, 21, buf, false);

	snprintf(buf, sizeof(buf), "CACHE   %u%%", st.stats.cache_pct);
	text(0, 30, buf, false);

	snprintf(buf, sizeof(buf), "UNDO    %u", st.stats.undo_depth);
	text(0, 39, buf, false);

	snprintf(buf, sizeof(buf), "STROKES %lu", (unsigned long)st.stats.strokes);
	text(0, 48, buf, false);
}

static void render(void)
{
	draw_status_bar();

	if (st.page == 0) {
		draw_tape_page();
	} else {
		draw_diag_page();
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void steno_display_chord(uint32_t keys)
{
	k_mutex_lock(&st_lock, K_FOREVER);
	st.held = keys & STENO_KEY_MASK;
	k_mutex_unlock(&st_lock);
	poke();
}

static void tape_append(const char *s, uint8_t backspaces)
{
	if (backspaces) {
		if (backspaces >= st.tape_len) {
			st.tape_len = 0;
		} else {
			st.tape_len -= backspaces;
		}
		st.tape[st.tape_len] = '\0';
	}
	if (!s || !*s) {
		return;
	}

	size_t add = strlen(s);

	if (add > TAPE_CHARS) {
		s += add - TAPE_CHARS;
		add = TAPE_CHARS;
	}
	if (st.tape_len + add > TAPE_CHARS) {
		/* scroll: drop from the front */
		size_t drop = st.tape_len + add - TAPE_CHARS;

		memmove(st.tape, st.tape + drop, st.tape_len - drop);
		st.tape_len -= drop;
	}
	memcpy(st.tape + st.tape_len, s, add);
	st.tape_len += add;
	st.tape[st.tape_len] = '\0';
}

void steno_display_stroke(uint32_t keys, const char *out, uint8_t backspaces)
{
	k_mutex_lock(&st_lock, K_FOREVER);
	st.last = keys & STENO_KEY_MASK;
	st.held = 0;
	tape_append(out, backspaces);
	st.stats.strokes++;
	k_mutex_unlock(&st_lock);
	poke();
}

void steno_display_set_stats(const struct steno_display_stats *stats)
{
	k_mutex_lock(&st_lock, K_FOREVER);
	uint32_t keep = st.stats.strokes;

	st.stats = *stats;
	if (!stats->strokes) {
		st.stats.strokes = keep;
	}
	k_mutex_unlock(&st_lock);

	if (st.page == 1) {
		poke();
	}
}

void steno_display_next_page(void)
{
	k_mutex_lock(&st_lock, K_FOREVER);
	st.page = (st.page + 1) % 2;
	k_mutex_unlock(&st_lock);
	clear();
	poke();
}

void steno_display_refresh(void)
{
	clear();
	poke();
}

/* ------------------------------------------------------------------ */
/* ZMK events                                                          */
/* ------------------------------------------------------------------ */

static int steno_display_listener(const zmk_event_t *eh)
{
	const struct zmk_activity_state_changed *act = as_zmk_activity_state_changed(eh);

	if (act) {
		bool want_blank = act->state != ZMK_ACTIVITY_ACTIVE;

		if (want_blank != blanked) {
			blanked = want_blank;
			if (ready) {
				if (blanked) {
					display_blanking_on(disp);
				} else {
					display_blanking_off(disp);
					clear();
				}
			}
		}
	}

	/*
	 * Any subscribed event is a reason to repaint the status bar.
	 *
	 * Always return BUBBLE. ZMK's dispatcher aborts the remaining
	 * listeners on any return value that is not 0/1/2, so returning
	 * -ENOTSUP here - which some in-tree listeners do - would risk
	 * starving listeners linked after this one.
	 */
	poke();
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(steno_display, steno_display_listener);
ZMK_SUBSCRIPTION(steno_display, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(steno_display, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_SUBSCRIPTION(steno_display, zmk_battery_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(steno_display, zmk_ble_active_profile_changed);
#endif

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

static void steno_display_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	if (!device_is_ready(disp)) {
		LOG_ERR("display not ready, steno display disabled");
		return;
	}

	/*
	 * MONO01 = bit set means lit, which is what we assume throughout.
	 * The driver defaults to this unless the DT node has `inversion-on`,
	 * but ask explicitly so a DT change cannot silently invert the UI.
	 */
	int err = display_set_pixel_format(disp, PIXEL_FORMAT_MONO01);

	if (err && err != -ENOSYS) {
		LOG_WRN("could not set MONO01 (%d); display may be inverted", err);
	}

	struct display_capabilities cap;

	display_get_capabilities(disp, &cap);
	if (!(cap.screen_info & SCREEN_INFO_MONO_VTILED)) {
		LOG_ERR("panel is not vertically tiled; framebuffer layout is wrong");
		return;
	}
	if (cap.screen_info & SCREEN_INFO_MONO_MSB_FIRST) {
		LOG_ERR("panel is MSB-first; framebuffer layout is wrong");
		return;
	}
	if (cap.x_resolution != FB_W || cap.y_resolution != FB_H) {
		LOG_ERR("panel is %ux%u, driver is built for %ux%u",
			cap.x_resolution, cap.y_resolution, FB_W, FB_H);
		return;
	}

	display_blanking_off(disp);
	ready = true;

	clear();
	flush();

	for (;;) {
		k_sem_take(&wake, K_FOREVER);

		/*
		 * Coalesce. A chord builds over several milliseconds and a
		 * full-screen I2C write is ~25ms at 400kHz, so repainting on
		 * every key transition would fall behind a fast writer.
		 */
		k_msleep(CONFIG_STENO_DISPLAY_COALESCE_MS);
		k_sem_reset(&wake);

		if (blanked) {
			continue;
		}

		k_mutex_lock(&st_lock, K_FOREVER);
		render();
		k_mutex_unlock(&st_lock);

		flush();
	}
}

K_THREAD_DEFINE(steno_display_tid, CONFIG_STENO_DISPLAY_STACK_SIZE,
		steno_display_thread, NULL, NULL, NULL,
		CONFIG_STENO_DISPLAY_THREAD_PRIORITY, 0, 0);
