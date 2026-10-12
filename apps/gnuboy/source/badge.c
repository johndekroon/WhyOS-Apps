// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gnuboy on the WHY2025 badge: the sys/ backend for its panel and keypad
 * (built into sys/badge/ by sdapps/build.sh; see sdapps/README.md).
 *
 * Video: gnuboy draws the 160x144 screen in RGB565 into a frame of its
 * own (delegate_scaling, as its SDL2 backend does); vid_end scales it up
 * (4x: 640x576) into a full-width band of the panel and writes that with
 * one pwrite. The framebuffer cannot be mmapped on this NOMMU kernel.
 * Writing ~0.8 MB 60 times a second is more than the badge does, so only
 * every (panelskip + 1)th frame is written; the emulation itself runs at
 * full speed.
 *
 * Keys: evdev events from the keypad, as gnuboy key codes; what they do is
 * bound in badge.rc. The keypad is not grabbed: the menu's runner
 * (apprun) reads it too, for the logo key.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include "defs.h"
#include "fb.h"
#include "input.h"
#include "rc.h"
#include "sys.h"

struct fb fb;

static int fbfd = -1, evfd = -1;
static int scale = 4, skip = 1, frames;
static unsigned line;			/* bytes per panel line */
static unsigned short frame[160 * 144];
static unsigned short *band;		/* 144 * scale panel lines */
static off_t band_off;

rcvar_t vid_exports[] = {
	RCV_INT("panelscale", &scale, "scale on the panel, 1 to 4"),
	RCV_INT("panelskip", &skip, "frames not drawn after each one that is"),
	RCV_END
};

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

void vid_end(void)
{
	unsigned x0 = (line / 2 - 160 * scale) / 2;	/* in pixels */
	unsigned short *row = band;

	if (!fb.enabled || fbfd < 0 || frames++ % (skip + 1))
		return;
	for (int y = 0; y < 144; y++) {
		const unsigned short *src = frame + y * 160;
		unsigned short *d = row + x0;

		for (int x = 0; x < 160; x++)
			for (int k = 0; k < scale; k++)
				*d++ = src[x];
		for (int k = 1; k < scale; k++)
			memcpy(row + k * (line / 2) + x0, row + x0, 160u * scale * 2);
		row += scale * (line / 2);
	}
	pwrite(fbfd, band, (size_t)line * 144 * scale, band_off);
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

		if (e.type != 1 || e.value == 2 || !(ev.code = keycode(e.code)))
			continue;
		ev.type = e.value ? EV_PRESS : EV_RELEASE;
		ev_postevent(&ev);
	}
}
