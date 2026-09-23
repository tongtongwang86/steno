/* Offline renderer: drives the real steno_display.c and prints the panel.
 * SPDX-License-Identifier: MIT */
#include "stubs.h"

const struct device sim_dev;
uint8_t sim_panel[1024];
struct zmk_endpoint_instance sim_ep = { .transport = ZMK_TRANSPORT_BLE };
uint8_t sim_soc = 87;

/* pull in the real driver */
#include "../display/steno_display.c"

static void show(const char *title)
{
	memset(sim_panel, 0, sizeof(sim_panel));
	dirty = 0xFF;
	blanked = false;
	render();
	flush();

	printf("\n== %s ==\n+", title);
	for (int i = 0; i < 128; i++) putchar('-');
	printf("+\n");
	for (int y = 0; y < 64; y++) {
		putchar('|');
		for (int x = 0; x < 128; x++)
			putchar((sim_panel[(y/8)*128 + x] >> (y & 7)) & 1 ? '#' : ' ');
		printf("|\n");
	}
	printf("+");
	for (int i = 0; i < 128; i++) putchar('-');
	printf("+\n");
}

int main(void)
{
	ready = true;

	steno_display_stroke(STENO_T_L | STENO_H_L | STENO_E, "the ", 0);
	steno_display_stroke(STENO_K_L | STENO_W_L | STENO_R_L | STENO_E | STENO_U, "quick ", 0);
	steno_display_stroke(STENO_P_L | STENO_R_L | STENO_O | STENO_W_L | STENO_B_R, "brown ", 0);
	steno_display_stroke(STENO_T_L | STENO_P_L | STENO_H_L | STENO_O | STENO_F_R, "fox jumps over ", 0);

	/* a chord in progress: TKPWHRAOEU* */
	steno_display_chord(STENO_S_L | STENO_T_L | STENO_P_L | STENO_H_L |
			    STENO_A | STENO_O | STENO_STAR |
			    STENO_F_R | STENO_L_R | STENO_D_R);
	show("tape page, chord held");

	steno_display_chord(0);
	show("tape page, idle");

	struct steno_display_stats s = {
		.lookup_ns = 142000, .decompress = 1, .cache_pct = 94,
		.undo_depth = 3, .strokes = 12043,
	};
	steno_display_set_stats(&s);
	st.page = 1;
	show("diagnostics page");

	sim_ep.transport = ZMK_TRANSPORT_USB;
	sim_soc = 100;
	st.page = 0;
	show("tape page, USB + full battery");
	return 0;
}
