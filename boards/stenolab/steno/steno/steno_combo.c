/*
 * Key combos: {#Return}, {#control(c)}, {#alt_l(Tab)}, {#shift(a b)}.
 *
 * main.json uses exactly one of these ({#BackSpace}), so this is not here
 * for the main dictionary - it is here so a user dictionary can bind Enter,
 * Tab, arrows and shortcuts to strokes, which is what makes the keyboard
 * usable for real work.
 *
 * Plover's syntax is X11 keysym names, optionally nested inside modifier
 * calls, separated by spaces. The table below covers the keysyms a
 * keyboard actually needs; an unknown name yields no key rather than a
 * wrong one.
 *
 * SPDX-License-Identifier: MIT
 */

#include "steno_combo.h"
#include <string.h>

/* HID keyboard usage IDs (usage page 0x07). */
#define HID_A        0x04
#define HID_1        0x1E
#define HID_ENTER    0x28
#define HID_ESC      0x29
#define HID_BSPC     0x2A
#define HID_TAB      0x2B
#define HID_SPACE    0x2C
#define HID_F1       0x3A
#define HID_HOME     0x4A
#define HID_RIGHT    0x4F

struct keysym {
	const char *name;
	uint16_t    usage;
};

/* Names are compared case-insensitively, so "Return" and "return" both work. */
static const struct keysym KEYSYMS[] = {
	{"return",     HID_ENTER}, {"enter",   HID_ENTER},
	{"tab",        HID_TAB},   {"space",   HID_SPACE},
	{"backspace",  HID_BSPC},  {"delete",  0x4C},
	{"escape",     HID_ESC},   {"esc",     HID_ESC},
	{"insert",     0x49},
	{"home",       0x4A},      {"page_up",   0x4B},
	{"end",        0x4D},      {"page_down", 0x4E},
	{"right",      0x4F},      {"left",      0x50},
	{"down",       0x51},      {"up",        0x52},
	{"minus",      0x2D},      {"equal",     0x2E},
	{"bracketleft", 0x2F},     {"bracketright", 0x30},
	{"backslash",  0x31},      {"semicolon", 0x33},
	{"apostrophe", 0x34},      {"grave",     0x35},
	{"comma",      0x36},      {"period",    0x37},
	{"slash",      0x38},      {"capslock",  0x39},
	{"print",      0x46},      {"scroll_lock", 0x47}, {"pause", 0x48},
};
#define N_KEYSYMS (sizeof(KEYSYMS) / sizeof(KEYSYMS[0]))

struct modname {
	const char *name;
	uint8_t     bit;
};

static const struct modname MODS[] = {
	{"shift",   STENO_MOD_LSHIFT}, {"shift_l", STENO_MOD_LSHIFT},
	{"shift_r", STENO_MOD_RSHIFT},
	{"control", STENO_MOD_LCTRL},  {"ctrl",    STENO_MOD_LCTRL},
	{"control_l", STENO_MOD_LCTRL}, {"control_r", STENO_MOD_RCTRL},
	{"alt",     STENO_MOD_LALT},   {"alt_l",   STENO_MOD_LALT},
	{"alt_r",   STENO_MOD_RALT},   {"option",  STENO_MOD_LALT},
	{"super",   STENO_MOD_LGUI},   {"super_l", STENO_MOD_LGUI},
	{"super_r", STENO_MOD_RGUI},   {"meta",    STENO_MOD_LGUI},
	{"command", STENO_MOD_LGUI},   {"windows", STENO_MOD_LGUI},
};
#define N_MODS (sizeof(MODS) / sizeof(MODS[0]))

static char lc(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool name_eq(const char *tok, size_t n, const char *name)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (!name[i] || lc(tok[i]) != name[i])
			return false;
	}
	return name[i] == '\0';
}

/* Resolve one token to a HID usage, or 0 if unknown. */
static uint16_t lookup_key(const char *tok, size_t n)
{
	if (n == 0)
		return 0;

	if (n == 1) {
		char c = lc(tok[0]);

		if (c >= 'a' && c <= 'z')
			return (uint16_t)(HID_A + (c - 'a'));
		if (c >= '1' && c <= '9')
			return (uint16_t)(HID_1 + (c - '1'));
		if (c == '0')
			return 0x27;
	}
	/* F1..F12 */
	if ((tok[0] == 'f' || tok[0] == 'F') && n >= 2 && n <= 3) {
		unsigned v = 0;

		for (size_t i = 1; i < n; i++) {
			if (tok[i] < '0' || tok[i] > '9')
				return 0;
			v = v * 10 + (unsigned)(tok[i] - '0');
		}
		if (v >= 1 && v <= 12)
			return (uint16_t)(HID_F1 + v - 1);
	}
	for (size_t i = 0; i < N_KEYSYMS; i++)
		if (name_eq(tok, n, KEYSYMS[i].name))
			return KEYSYMS[i].usage;
	return 0;
}

static uint8_t lookup_mod(const char *tok, size_t n)
{
	for (size_t i = 0; i < N_MODS; i++)
		if (name_eq(tok, n, MODS[i].name))
			return MODS[i].bit;
	return 0;
}

int steno_parse_combo(const char *s, size_t n, steno_key_combo *out,
		      unsigned max)
{
	unsigned count = 0;
	uint8_t mod_stack[8];
	unsigned depth = 0;
	uint8_t active = 0;
	size_t i = 0;

	while (i < n) {
		if (s[i] == ' ' || s[i] == '\t') {
			i++;
			continue;
		}
		if (s[i] == ')') {
			/* close the innermost modifier group */
			if (depth > 0) {
				depth--;
				active &= (uint8_t)~mod_stack[depth];
			}
			i++;
			continue;
		}

		size_t start = i;

		while (i < n && s[i] != ' ' && s[i] != '(' && s[i] != ')')
			i++;

		size_t len = i - start;

		if (i < n && s[i] == '(') {
			/* a modifier holding whatever follows */
			uint8_t m = lookup_mod(s + start, len);

			if (depth < sizeof(mod_stack)) {
				mod_stack[depth++] = m;
				active |= m;
			}
			i++;
			continue;
		}

		uint16_t usage = lookup_key(s + start, len);

		if (usage && count < max) {
			out[count].mods = active;
			out[count].usage = usage;
			count++;
		}
	}
	return (int)count;
}
