// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gnuboy on the WHY2025 badge: the sys/ backend for its panel and keypad
 * (built into sys/badge/ by sdapps/build.sh; see sdapps/README.md).
 *
 * Video: gnuboy draws the 160x144 screen in RGB565 into a frame of its
 * own (delegate_scaling, as its SDL2 backend does); vid_end scales it up
 * (4x: 640x576) into a full-width band of the panel and writes the lines
 * that changed since the last frame shown with one pwrite. The
 * framebuffer cannot be mmapped on this NOMMU kernel.
 *
 * Speed: every write costs the kernel a quarter turn of those lines into
 * the panel's scan-out buffer (drm/esp32p4-dsi, ~70 ms for all 720x720),
 * in a worker that shares the one CPU with the game. gnuboy's own timing
 * sleeps for what is left of a frame and lets the game slow down when
 * nothing is left; here vid_end keeps the time instead (gnuboy's
 * framelen is 0): a deadline per Game Boy frame, a frame not drawn when
 * the game is behind it (at most MAX_UNDRAWN in a row), and a sleep up to
 * the deadline, which also makes up for usleep's ticks (HZ=250).
 * panelskip frames are left out after each one drawn in any case.
 *
 * Keys: evdev events from the keypad, as gnuboy key codes; what they do is
 * bound in badge.rc. The keypad is not grabbed: the menu's runner
 * (apprun) reads it too, for the logo key. Esc pauses and the diamond key
 * shows the keys (sdapps/pause, Docs/06-design-system.md 6.9): neither reaches
 * gnuboy.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include "defs.h"
#include "fb.h"
#include "input.h"
#include "rc.h"
#include "sys.h"
#include "pause.h"

#define FRAME_US	16743		/* a Game Boy frame: 59.7 per second */
#define MAX_UNDRAWN	8		/* frames not drawn in a row, at most */

struct fb fb;

static int fbfd = -1, evfd = -1;
static int scale = 4, skip = 1, stats;
static unsigned line;			/* bytes per panel line */
static unsigned short frame[160 * 144];
static unsigned short shown[160 * 144];	/* the frame last written */
static int shown_ok;
static unsigned short *band;		/* 144 * scale panel lines */
static off_t band_off;
static long long deadline;		/* when this frame should be done, in us */
static int undrawn;			/* frames since the last one drawn */

rcvar_t vid_exports[] = {
	RCV_INT("panelscale", &scale, "scale on the panel, 1 to 4"),
	RCV_INT("panelskip", &skip, "frames not drawn after each one that is"),
	RCV_INT("panelstats", &stats, "every 5 s to stderr: frames, drawn, lines"),
	RCV_END
};

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

void vid_preinit(void)
{
}

void vid_init(void)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	unsigned w, h;

	fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fi) < 0 || vi.bits_per_pixel != 16)
		die("gnuboy: needs a 16-bit /dev/fb0\n");
	w = vi.xres;
	h = vi.yres;
	line = fi.line_length;
	if (scale < 1)
		scale = 1;
	while (scale > 1 && (160u * scale > w || 144u * scale > h))
		scale--;
	band = calloc(1, (size_t)line * 144 * scale);
	if (!band)
		die("gnuboy: no memory for the panel band\n");
	band_off = (off_t)((h - 144 * scale) / 2) * line;
	{
		/* black around it, once */
		char *z = calloc(1, line);

		for (unsigned y = 0; z && y < h; y++)
			pwrite(fbfd, z, line, (off_t)y * line);
		free(z);
	}

	fb.w = 160;
	fb.h = 144;
	fb.pelsize = 2;
	fb.pitch = 160 * 2;
	fb.indexed = 0;
	fb.ptr = (byte *)frame;
	fb.cc[0].r = 3;			/* RGB565: bits lost, shift */
	fb.cc[0].l = 11;
	fb.cc[1].r = 2;
	fb.cc[1].l = 5;
	fb.cc[2].r = 3;
	fb.cc[2].l = 0;
	fb.yuv = 0;
	fb.enabled = 1;
	fb.dirty = 0;
	fb.delegate_scaling = 1;
	{
		char zero[] = "0", *v = zero;

		rc_setvar("framelen", 1, &v);	/* vid_end keeps the time */
	}

	evfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

void vid_close(void)
{
	if (fbfd >= 0)
		close(fbfd);
	if (evfd >= 0)
		close(evfd);
	fbfd = evfd = -1;
	fb.enabled = 0;
}

void vid_settitle(char *title)
{
	(void)title;
}

void vid_setpal(int i, int r, int g, int b)
{
	(void)i; (void)r; (void)g; (void)b;
}

void vid_begin(void)
{
	fb.ptr = (byte *)frame;
}

/* the lines that changed since the last frame shown; 0 if none did */
static int draw(void)
{
	unsigned x0 = (line / 2 - 160 * scale) / 2;	/* in pixels */
	unsigned rows = scale * (line / 2);		/* band pixels per Game Boy line */
	int y0 = 0, y1 = 143;

	if (shown_ok) {
		while (y0 < 144 && !memcmp(frame + y0 * 160, shown + y0 * 160, 320))
			y0++;
		if (y0 == 144)
			return 0;
		while (!memcmp(frame + y1 * 160, shown + y1 * 160, 320))
			y1--;
	}
	for (int y = y0; y <= y1; y++) {
		const unsigned short *src = frame + y * 160;
		unsigned short *row = band + y * rows, *d = row + x0;

		for (int x = 0; x < 160; x++)
			for (int k = 0; k < scale; k++)
				*d++ = src[x];
		for (int k = 1; k < scale; k++)
			memcpy(row + k * (line / 2) + x0, row + x0, 160u * scale * 2);
	}
	pwrite(fbfd, band + y0 * rows, (size_t)line * scale * (y1 - y0 + 1),
	       band_off + (off_t)line * scale * y0);
	memcpy(shown + y0 * 160, frame + y0 * 160, 320u * (y1 - y0 + 1));
	shown_ok = 1;
	return y1 - y0 + 1;
}

void vid_end(void)
{
	static long long t_stats;
	static int n_frames, n_drawn, n_lines, n_late;
	long long now = now_us();

	if (!fb.enabled || fbfd < 0)
		return;
	if (!deadline || now > deadline + 100000) {
		/* the start, after the pause, or the game itself too slow */
		n_late += deadline != 0;
		deadline = now;
	}
	deadline += FRAME_US;
	n_frames++;
	if (++undrawn > skip && (now < deadline || undrawn > MAX_UNDRAWN)) {
		int n = draw();

		undrawn = 0;
		n_drawn += n > 0;
		n_lines += n;
	}
	if (stats && now - t_stats >= 5000000) {
		if (t_stats)
			fprintf(stderr, "gnuboy: %.1f frames/s, %.1f drawn/s, %d lines a drawing, %d times 0.1 s behind\n",
				n_frames * 1e6 / (now - t_stats), n_drawn * 1e6 / (now - t_stats),
				n_drawn ? n_lines / n_drawn : 0, n_late);
		t_stats = now;
		n_frames = n_drawn = n_lines = n_late = 0;
	}
	now = now_us();
	if (now < deadline) {
		struct timespec t = { deadline / 1000000, deadline % 1000000 * 1000 };

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0);
	}
}

/* evdev code -> gnuboy key code; 0 for keys it does not use. */
static int keycode(int c)
{
	static const char row1[] = "1234567890", row2[] = "qwertyuiop",
			  row3[] = "asdfghjkl", row4[] = "zxcvbnm";

	if (c >= 2 && c <= 11)
		return row1[c - 2];
	if (c >= 16 && c <= 25)
		return row2[c - 16];
	if (c >= 30 && c <= 38)
		return row3[c - 30];
	if (c >= 44 && c <= 50)
		return row4[c - 44];
	if (c >= 59 && c <= 64)		/* the symbol keys, F1..F6 */
		return K_F1 + (c - 59);
	switch (c) {
	case 1: return K_ESC;
	case 14: return K_BS;
	case 15: return K_TAB;
	case 28: return K_ENTER;
	case 29: case 97: return K_CTRL;
	case 42: case 54: return K_SHIFT;
	case 56: case 100: return K_ALT;
	case 57: return K_SPACE;
	case 103: return K_UP;
	case 108: return K_DOWN;
	case 105: return K_LEFT;
	case 106: return K_RIGHT;
	case 110: return K_INS;
	case 111: return K_DEL;
	}
	return 0;
}

struct kev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

/*
 * Esc: the pause screen; the diamond key: the key overview. The game
 * stands still meanwhile (we do not return to its loop). Keys held when it
 * opened are let go, or the game would see them held on.
 */
static void pause_game(int help)
{
	static const int held[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_F1, K_F2, K_F3, K_F4,
				    K_ENTER, K_BS, 'x', 'o' };
	static const struct pause_key keys[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "The D-pad" },
		{ "#1", "A (Enter does too)" },
		{ "#2", "B (Backspace does too)" },
		{ "#3", "Start (X does too)" },
		{ "#4", "Select (O does too)" },
		{ "#6", "These keys" },
		{ "Esc", "Pause: resume, save, load, quit" },
		{ "Logo", "Back to the menu" },
	};
	static const char *const items[] = { "Resume", "Save state", "Load state", "Keys", "Quit" };
	struct pause_screen s = {
		.fbfd = fbfd, .evfd = evfd, .line = line, .game = band, .game_lines = 144 * scale,
		.band_off = band_off, .app = "GAME BOY", .name = "Game Boy",
		.about = "The game is paused. On the badge its buttons are these keys.",
		.keys = keys, .n_keys = sizeof(keys) / sizeof(keys[0]),
		.items = items, .n_items = 5, .keys_item = 3,
	};
	char save[] = "savestate", load[] = "loadstate", quit[] = "quit";	/* rc_command edits */
	int flags = fcntl(evfd, F_GETFL);

	for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
		event_t ev = { .type = EV_RELEASE, .code = held[i] };

		ev_postevent(&ev);
	}
	fcntl(evfd, F_SETFL, flags & ~O_NONBLOCK);
	switch (badge_pause(&s, help)) {
	case 1:
		rc_command(save);
		break;
	case 2:
		rc_command(load);
		break;
	case 4:
		rc_command(quit);
		break;
	}
	fcntl(evfd, F_SETFL, flags);
}

void ev_poll(int wait)
{
	struct pollfd p = { .fd = evfd, .events = POLLIN };
	struct kev e;

	if (evfd < 0)
		return;
	if (wait)
		poll(&p, 1, -1);
	while (read(evfd, &e, sizeof(e)) == (int)sizeof(e)) {
		event_t ev = { 0 };

		if (e.type == 1 && e.value == 1 && (e.code == 1 || e.code == 64)) {
			pause_game(e.code == 64);	/* Esc, diamond */
			continue;
		}
		if (e.type != 1 || e.value == 2 || !(ev.code = keycode(e.code)))
			continue;
		ev.type = e.value ? EV_PRESS : EV_RELEASE;
		ev_postevent(&ev);
	}
}
