// SPDX-License-Identifier: MIT
/*
 * The pause screen of a full-screen app on the badge (Docs/06-design-system.md
 * 6.9): such an app may use the symbol keys as controls, and gets Esc as
 * "pause", not as "quit". The pause is a pop-up over the app in the menu's
 * style (libwb, the canvas "Menu's met frontpaneel"): the app's last picture
 * dimmed, a frosted panel over it with the choices as rows, Resume (the
 * default), the app's own choices, a key overview, Quit under a line, and
 * the keys inside the panel at its bottom. Used by gnuboy and NetSurf
 * (sdapps/build.sh copies it in).
 *
 * A file of its own: libwb's shadow buffer is called fb, as are names in
 * the apps. The panel is frosted (the picture under it blurred) only with
 * the whole picture in one buffer: a panel's worth of memory (1 MB) taken
 * while the pause is open and given back after. Without that memory it is
 * drawn in bands of 40 rows, as appterm does, with unblurred glass. Each
 * frame is recorded first and only the bands that changed are drawn
 * (libwb's wb_frame and wb_bands): a focus that moves draws its two rows,
 * the panel's blur kept. PAUSE_UI_TIMING=1 in the environment: a line per
 * frame on stderr (the app's log).
 */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#include "wb_tokens.h"

#define W	WB_SCREEN_W
#define H	WB_SCREEN_H
#include "wb.h"

#include "pause.h"

#define KEY_ESC		1
#define KEY_ENTER	28
#define KEY_UP		103
#define KEY_DOWN	108
#define BAND_H		40
#define ITEMS_MAX	8

static uint16_t pband[W * BAND_H];
static uint16_t *whole;			/* the whole picture, while the pause is open */

struct kev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static const struct hint hints[] = {
	{ "\xe2\x86\x91\xe2\x86\x93", "select", 0 }, { "Enter", "choose", 0 }, { "Esc", "resume", 1 },
};

static int game_row(const struct pause_screen *s, int y)
{
	int y0 = (int)(s->band_off / (off_t)s->line);

	return s->game && y >= y0 && y < y0 + s->game_lines ? y - y0 : -1;
}

/*
 * The key overview (the diamond key, or Keys): as wbdraw.h's help_popup,
 * "HELP" with the diamond, the app's name and sentence, every key with a
 * line, but its own key (Esc) inside the panel at the bottom, as the
 * canvas's pop-ups have them, not in a hint bar over the game.
 */
#define KEYS_ROW	36

static void overview(const struct pause_screen *s)
{
	static const struct hint close[] = { { "Esc", "close", 1 } };
	const struct pause_key *k = s->keys;
	int tw = POP_W - 64, n = s->n_keys < 12 ? s->n_keys : 12;
	int lines = s->about ? text_wrap(&f_ui20, 0, 0, tw, 28, s->about, 0, 3, 0) : 0;
	int kw = 0, h, y, x = POP_X + 32;
	char buf[96];

	for (int i = 0; i < n; i++)
		if (keycap_w(k[i].keys) + 18 > kw)
			kw = keycap_w(k[i].keys) + 18;
	h = 32 + 18 + 6 + 36 + 16 + lines * 28 + 24 + 17 + 12 + n * KEYS_ROW + 14 + 1 + 10 + 23 + 16;
	y = (H - h) / 2;
	dim(WB_DIM);
	wb_panel(POP_X, y, POP_W, h, WB_RADIUS_LG);
	y += 32;
	icon(&kc_diamond, x, y + 1, WB_COLOR_KEY_DIAMOND, 255);
	eyebrow(x + kc_diamond.w + 10, y, "HELP");
	text(&f_ui28b, x, y + 18 + 6 + 2, wb_fit(&f_ui28b, s->name, tw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	y += 18 + 6 + 36 + 16;
	if (s->about)
		text_wrap(&f_ui20, x, y, tw, 28, s->about, WB_COLOR_TEXT_MID, 3, 1);
	y += lines * 28 + 24;
	text(&f_mono13, x, y, "KEYS", WB_COLOR_TEXT_MID, 21);
	y += 17 + 12;
	for (int i = 0; i < n; i++, y += KEYS_ROW) {
		int cy = y + KEYS_ROW / 2;

		keycap(x, cy, k[i].keys);
		text(&f_ui18, x + kw + 16, text_top(&f_ui18, cy),
		     wb_fit(&f_ui18, k[i].what, tw - kw - 16, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	}
	panel_hints(x, tw, y + 14, close, 1);
}

struct frame { const struct pause_screen *s; int focus, help, plain; };

/* The pop-up over the app's picture, in fb's rows: it only draws (libwb
 * runs it once more to record the frame). */
static void paint(void *arg)
{
	const struct frame *f = arg;
	const struct pause_screen *s = f->s;
	struct menu_item items[ITEMS_MAX];
	int n = s->n_items < ITEMS_MAX ? s->n_items : ITEMS_MAX, focus = f->focus, help = f->help;

	memset(items, 0, sizeof(items));
	for (int i = 0; i < n; i++) {
		items[i].label = s->items[i];
		items[i].sep = i == n - 1;
	}
	if (wb_rec) {		/* the app's picture stands still while the pause is open */
		int k[] = { 0x7061, WB_P(s->game), (int)s->band_off, s->game_lines, f->plain };

		wb_rec_rows(0, H, k, sizeof(k));
	}
	for (int y = fb_y0; y < fb_y1 && !wb_rec; y++) {
		int g = f->plain ? game_row(s, y) : -1;

		if (g >= 0)
			memcpy(fb + y * W, s->game + g * W, W * 2);
		else
			memset(fb + y * W, 0, W * 2);
	}
	/*
	 * Glass (libwb draws it over the scene; here over the game): frosted
	 * with the whole picture in fb, where the panel can blur what is
	 * under it; in bands unblurred.
	 */
	scene_shown = 1;
	gb_ok = 0;
	if (help)
		overview(s);
	else
		menu_popup(s->app, "Paused", 0, items, 0, n, focus, -2, 0, 0, hints, 3);
	scene_shown = 0;
}

static const struct pause_screen *out_s;

/* a band (or the whole picture) to the panel; not when its lines are not
 * the picture's (then the panel is left as the app had it) */
static void out(const uint16_t *b, int y0, int y1)
{
	if (out_s->line == W * 2)
		pwrite(out_s->fbfd, b, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
}

/* PAUSE_UI_TIMING=1: a line per frame on stderr (the app's log) */
static void timing(long t0, int n)
{
	static int on = -1;
	char line[96];

	if (on < 0)
		on = getenv("PAUSE_UI_TIMING") != 0;
	if (on) {
		int len = snprintf(line, sizeof(line), "pause: frame %ld us (%s, %d bands), glass %ld\n",
				   wb_now_us() - t0, whole ? "whole" : "bands", n, tm_glass);

		if (write(2, line, (size_t)len) < 0)
			on = 0;
	}
	tm_scene = tm_glass = 0;
}

static void draw(const struct pause_screen *s, int focus, int help)
{
	struct frame f = { s, focus, help, s->line == W * 2 };
	long t0 = wb_now_us();

	out_s = s;
	timing(t0, whole ? wb_frame(paint, &f, whole, BAND_H, out) : wb_bands(paint, &f, pband, BAND_H, out));
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
	/* an anonymous map: NetSurf's malloc ends the program when it fails */
	void *m = mmap(0, (size_t)W * H * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	whole = m == MAP_FAILED ? 0 : (uint16_t *)m;
	wb_init();
	wb_rec_invalidate();	/* the app drew the panel since the last pause */
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
	if (whole)
		munmap(whole, (size_t)W * H * 2);
	whole = 0;
	return what;
}
