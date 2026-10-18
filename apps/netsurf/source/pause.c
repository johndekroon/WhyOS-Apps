// SPDX-License-Identifier: MIT
/*
 * The pause screen of a full-screen app on the badge (Docs/06-design-system.md
 * 6.9): such an app may use the symbol keys as controls, and gets Esc as
 * "pause", not as "quit". The pause is a pop-up over the app in the menu's
 * style (wbdraw.h): Resume (the default), the app's own choices, a key
 * overview, Quit. Used by gnuboy and NetSurf (sdapps/build.sh copies it in).
 *
 * A file of its own: wbdraw.h's shadow buffer is called fb, as are names
 * in the apps. Drawn in bands of 40 rows, as appterm does: the app's last
 * picture under the dimmed backdrop.
 */
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W	WB_SCREEN_W
#define H	WB_SCREEN_H
#include "wbdraw.h"

#include "pause.h"

#define KEY_ESC		1
#define KEY_ENTER	28
#define KEY_UP		103
#define KEY_DOWN	108
#define BAND_H		40
#define ITEMS_MAX	8

static uint16_t pband[W * BAND_H];

struct kev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static const struct hint hints[] = { { "Enter", "choose", 0 }, { "Esc", "close", 1 } };

static int game_row(const struct pause_screen *s, int y)
{
	int y0 = (int)(s->band_off / (off_t)s->line);

	return s->game && y >= y0 && y < y0 + s->game_lines ? y - y0 : -1;
}

static void draw(const struct pause_screen *s, int focus, int help)
{
	struct menu_item items[ITEMS_MAX];
	int n = s->n_items < ITEMS_MAX ? s->n_items : ITEMS_MAX, plain = s->line == W * 2;

	memset(items, 0, sizeof(items));
	for (int i = 0; i < n; i++) {
		items[i].label = s->items[i];
		items[i].sep = i == n - 1;
	}
	for (int y0 = 0; y0 < H; y0 += BAND_H) {
		int y1 = y0 + BAND_H < H ? y0 + BAND_H : H;

		fb = pband - (long)y0 * W;
		fb_y0 = y0;
		fb_y1 = y1;
		for (int y = y0; y < y1; y++) {
			int g = plain ? game_row(s, y) : -1;

			if (g >= 0)
				memcpy(fb + y * W, s->game + g * W, W * 2);
			else
				memset(fb + y * W, 0, W * 2);
		}
		if (help)
			help_popup(s->name, s->about, (const struct help_key *)s->keys, s->n_keys);
		else
			menu_popup(s->app, "Paused", 0, items, 0, n, focus, -1, 0, 0, hints, 2);
		if (plain)
			pwrite(s->fbfd, pband, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
	}
}

/* The app as it was, with black around it. */
static void restore(const struct pause_screen *s)
{
	memset(pband, 0, sizeof(pband));
	for (int y = 0; y < H; y++)
		if (game_row(s, y) < 0)
			pwrite(s->fbfd, pband, s->line, (off_t)y * s->line);
	if (s->game)
		pwrite(s->fbfd, s->game, (size_t)s->line * (size_t)s->game_lines, s->band_off);
}

int badge_pause(const struct pause_screen *s, int help)
{
	struct pollfd p = { .fd = s->evfd, .events = POLLIN };
	int n = s->n_items < ITEMS_MAX ? s->n_items : ITEMS_MAX, focus = 0, what = -1, only_help = help;

	draw(s, focus, help);
	while (what < 0) {
		struct kev e;

		if (poll(&p, 1, -1) < 0)
			continue;
		if (read(s->evfd, &e, sizeof(e)) != (int)sizeof(e) || e.type != 1 || e.value != 1)
			continue;
		if (help) {		/* only an explanation: Esc or the diamond closes it */
			if (e.code == KEY_ESC || e.code == KEY_SYM_HELP) {
				help = 0;
				if (only_help)
					what = 0;
			}
		} else if (e.code == KEY_ESC) {
			what = 0;
		} else if (e.code == KEY_SYM_HELP) {
			help = 1;
		} else if (e.code == KEY_UP || e.code == KEY_DOWN) {
			focus = (focus + (e.code == KEY_UP ? n - 1 : 1)) % n;
		} else if (e.code == KEY_ENTER) {
			if (focus == s->keys_item)
				help = 1;
			else
				what = focus;
		}
		if (what < 0)
			draw(s, focus, help);
	}
	restore(s);
	return what;
}
