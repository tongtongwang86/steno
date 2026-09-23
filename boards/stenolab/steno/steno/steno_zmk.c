/*
 * ZMK integration: chord accumulation, translation, HID output.
 *
 * Key positions arrive as ZMK position events, are accumulated into a chord,
 * and the chord fires when every key is released. The engine turns that into
 * (backspaces, text), which a dedicated thread types out.
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/modifiers.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

#include "steno_engine.h"
#include "steno_layout.h"
#include "steno_display.h"
#include "sdic.h"

LOG_MODULE_REGISTER(steno_zmk, CONFIG_STENO_ZMK_LOG_LEVEL);

int steno_dict_lookup(const uint32_t *strokes, unsigned n, char *out, size_t outsz);
bool steno_dict_available(void);

/* ------------------------------------------------------------------ */
/* ASCII -> HID                                                        */
/* ------------------------------------------------------------------ */

/*
 * Built from ZMK's own key definitions rather than hand-rolled HID usage
 * numbers, so the encoding and implicit-modifier bits stay correct.
 * Assumes a US layout on the host, which is also what Plover assumes.
 */
static const uint32_t ascii_key[128] = {
	['\n'] = RET,  ['\t'] = TAB,  [' '] = SPACE,

	['a'] = A, ['b'] = B, ['c'] = C, ['d'] = D, ['e'] = E, ['f'] = F,
	['g'] = G, ['h'] = H, ['i'] = I, ['j'] = J, ['k'] = K, ['l'] = L,
	['m'] = M, ['n'] = N, ['o'] = O, ['p'] = P, ['q'] = Q, ['r'] = R,
	['s'] = S, ['t'] = T, ['u'] = U, ['v'] = V, ['w'] = W, ['x'] = X,
	['y'] = Y, ['z'] = Z,

	['A'] = LS(A), ['B'] = LS(B), ['C'] = LS(C), ['D'] = LS(D),
	['E'] = LS(E), ['F'] = LS(F), ['G'] = LS(G), ['H'] = LS(H),
	['I'] = LS(I), ['J'] = LS(J), ['K'] = LS(K), ['L'] = LS(L),
	['M'] = LS(M), ['N'] = LS(N), ['O'] = LS(O), ['P'] = LS(P),
	['Q'] = LS(Q), ['R'] = LS(R), ['S'] = LS(S), ['T'] = LS(T),
	['U'] = LS(U), ['V'] = LS(V), ['W'] = LS(W), ['X'] = LS(X),
	['Y'] = LS(Y), ['Z'] = LS(Z),

	['1'] = N1, ['2'] = N2, ['3'] = N3, ['4'] = N4, ['5'] = N5,
	['6'] = N6, ['7'] = N7, ['8'] = N8, ['9'] = N9, ['0'] = N0,

	['!'] = LS(N1), ['@'] = LS(N2), ['#'] = LS(N3), ['$'] = LS(N4),
	['%'] = LS(N5), ['^'] = LS(N6), ['&'] = LS(N7), ['*'] = LS(N8),
	['('] = LS(N9), [')'] = LS(N0),

	['-'] = MINUS,     ['_'] = LS(MINUS),
	['='] = EQUAL,     ['+'] = LS(EQUAL),
	['['] = LBKT,      ['{'] = LS(LBKT),
	[']'] = RBKT,      ['}'] = LS(RBKT),
	['\\'] = BSLH,     ['|'] = LS(BSLH),
	[';'] = SEMI,      [':'] = LS(SEMI),
	['\''] = SQT,      ['"'] = LS(SQT),
	['`'] = GRAVE,     ['~'] = LS(GRAVE),
	[','] = COMMA,     ['<'] = LS(COMMA),
	['.'] = DOT,       ['>'] = LS(DOT),
	['/'] = FSLH,      ['?'] = LS(FSLH),
};

/* Inter-report spacing. Hosts drop keystrokes if reports arrive too fast. */
#define TAP_MS CONFIG_STENO_ZMK_TAP_MS

static void tap_encoded(uint32_t encoded)
{
	uint8_t mods = SELECT_MODS(encoded);
	zmk_key_t key = ZMK_HID_USAGE_ID(encoded);

	if (mods) {
		zmk_hid_register_mods(mods);
		zmk_endpoint_send_report(HID_USAGE_KEY);
	}
	zmk_hid_keyboard_press(key);
	zmk_endpoint_send_report(HID_USAGE_KEY);
	k_msleep(TAP_MS);

	zmk_hid_keyboard_release(key);
	zmk_endpoint_send_report(HID_USAGE_KEY);
	if (mods) {
		zmk_hid_unregister_mods(mods);
		zmk_endpoint_send_report(HID_USAGE_KEY);
	}
	k_msleep(TAP_MS);
}

static void type_out(uint16_t backspaces, const char *text)
{
	for (uint16_t i = 0; i < backspaces; i++)
		tap_encoded(BSPC);

	for (const char *p = text; *p; p++) {
		uint8_t c = (uint8_t)*p;
		uint32_t k = (c < 128) ? ascii_key[c] : 0;

		if (k)
			tap_encoded(k);
		else
			LOG_WRN("no keycode for 0x%02x", c);
	}
}

/* ------------------------------------------------------------------ */
/* Chord accumulation                                                  */
/* ------------------------------------------------------------------ */

static steno_engine engine;
static uint32_t held;      /* keys physically down right now */
static uint32_t accum;     /* union of everything pressed this chord */

static K_MUTEX_DEFINE(chord_lock);

/* Fired chords waiting to be typed. */
K_MSGQ_DEFINE(chord_q, sizeof(uint32_t), 8, 4);

static int lookup_cb(void *ctx, const uint32_t *strokes, unsigned n,
		     char *out, size_t outsz)
{
	ARG_UNUSED(ctx);
	return steno_dict_lookup(strokes, n, out, outsz);
}

static int steno_listener(const zmk_event_t *eh)
{
	const struct zmk_position_state_changed *ev =
		as_zmk_position_state_changed(eh);

	if (!ev)
		return ZMK_EV_EVENT_BUBBLE;

	/*
	 * The control layer reuses the same physical keys for media and BLE
	 * bindings, so only accumulate while the base layer is on top.
	 */
	if (zmk_keymap_highest_layer_active() != 0)
		return ZMK_EV_EVENT_BUBBLE;

	if (ev->position >= STENO_POSITIONS)
		return ZMK_EV_EVENT_BUBBLE;

	uint32_t bit = steno_layout[ev->position];

	if (bit == STENO_NONE)
		return ZMK_EV_EVENT_BUBBLE;

	k_mutex_lock(&chord_lock, K_FOREVER);
	if (ev->state) {
		held |= bit;
		accum |= bit;
	} else {
		held &= ~bit;
		if (held == 0 && accum != 0) {
			uint32_t chord = accum;

			accum = 0;
			k_mutex_unlock(&chord_lock);
			steno_display_chord(0);
			if (k_msgq_put(&chord_q, &chord, K_NO_WAIT) != 0)
				LOG_WRN("chord queue full, stroke dropped");
			return ZMK_EV_EVENT_BUBBLE;
		}
	}
	uint32_t shown = accum;

	k_mutex_unlock(&chord_lock);
	steno_display_chord(shown);

	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(steno_zmk, steno_listener);
ZMK_SUBSCRIPTION(steno_zmk, zmk_position_state_changed);

/* ------------------------------------------------------------------ */
/* Translation thread                                                  */
/* ------------------------------------------------------------------ */

static void steno_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	/* The dictionary opens in SYS_INIT at APPLICATION 90; wait for it. */
	k_msleep(200);

	if (!steno_dict_available()) {
		LOG_ERR("no dictionary - steno translation disabled");
		return;
	}
	steno_engine_init(&engine, lookup_cb, NULL, SDIC_MAX_STROKES);
	LOG_INF("steno engine ready");

	for (;;) {
		uint32_t chord;

		k_msgq_get(&chord_q, &chord, K_FOREVER);

		uint32_t t0 = k_cycle_get_32();
		steno_action act = steno_engine_stroke(&engine, chord);
		uint32_t dt = k_cyc_to_ns_floor32(k_cycle_get_32() - t0);

		LOG_DBG("chord %06x -> bs=%u '%s'", chord, act.backspaces,
			act.text);

		steno_display_stroke(chord, act.text, (uint8_t)act.backspaces);

		struct steno_display_stats st = {
			.lookup_ns = dt,
			.strokes = engine.stat_strokes,
			.decompress = 0,
			.cache_pct = 0,
			.undo_depth = engine.n_seg,
		};
		steno_display_set_stats(&st);

		type_out(act.backspaces, act.text);
	}
}

K_THREAD_DEFINE(steno_tid, CONFIG_STENO_ZMK_STACK_SIZE, steno_thread,
		NULL, NULL, NULL, CONFIG_STENO_ZMK_THREAD_PRIORITY, 0, 0);
