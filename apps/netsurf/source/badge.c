/*
 * libnsfb surface for the WHY2025 badge: its 720x720 RGB565 panel and its
 * keypad. Built into libnsfb by sdapps/build.sh (src/surface/badge.c);
 * NetSurf selects it with -f badge. MIT, as libnsfb.
 *
 * The panel: a shadow buffer, written to /dev/fb0 with pwrite for each
 * updated band of lines; the framebuffer cannot be mmapped on this NOMMU
 * kernel. NetSurf's pointer is drawn into the shadow as for SDL.
 *
 * The keypad (evdev) has no mouse, so it brings one along:
 *   arrows        move the pointer (faster while held)
 *   Fn + arrows   the arrow keys themselves (caret, scrolling)
 *   circle (F4)   left button: click, or hold and move to drag
 *   triangle/cross (F2/F3)   page up / page down
 *   square/diamond (F1/F6)   back / forward (NetSurf's Ctrl+Left/Right)
 *   cloud (F5)    click into the address bar
 *   Esc           the pause screen: Resume, Keys, Quit (sdapps/pause; a
 *                 full-screen app, Docs/06-design-system.md 6.9)
 * Everything else is passed on as keys. The keypad is not grabbed: the
 * menu's runner (apprun) reads it too, for the logo key.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"
#include "pause.h"

#define KEY_FN		0x1d0

struct kev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static int fbfd = -1, evfd = -1;
static int px = 360, py = 360;		/* the pointer */
static int fn, held;			/* Fn down; repeats of the held arrow */
static nsfb_event_t queue[8];
static int qhead, qlen;

static void push(enum nsfb_event_type_e type, int code)
{
	nsfb_event_t *e;

	if (qlen == (int)(sizeof(queue) / sizeof(queue[0])))
		return;
	e = &queue[(qhead + qlen++) % 8];
	memset(e, 0, sizeof(*e));
	e->type = type;
	e->value.keycode = code;
}

static void push_key(int code)
{
	push(NSFB_EVENT_KEY_DOWN, code);
	push(NSFB_EVENT_KEY_UP, code);
}

static void push_move(nsfb_t *nsfb)
{
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	if (px >= nsfb->width) px = nsfb->width - 1;
	if (py >= nsfb->height) py = nsfb->height - 1;
	push(NSFB_EVENT_MOVE_ABSOLUTE, 0);
	queue[(qhead + qlen - 1) % 8].value.vector.x = px;
	queue[(qhead + qlen - 1) % 8].value.vector.y = py;
	queue[(qhead + qlen - 1) % 8].value.vector.z = 0;
}

/* evdev code (US layout) -> libnsfb key code; 0 for none. */
static int keycode(int c)
{
	static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]",
			  row3[] = "asdfghjkl;'`", row4[] = "zxcvbnm,./";

	if (c >= 2 && c <= 13)
		return row1[c - 2];
	if (c >= 16 && c <= 27)
		return row2[c - 16];
	if (c >= 30 && c <= 41)
		return row3[c - 30];
	if (c >= 44 && c <= 53)
		return row4[c - 44];
	switch (c) {
	case 1: return NSFB_KEY_ESCAPE;
	case 14: return NSFB_KEY_BACKSPACE;
	case 15: return NSFB_KEY_TAB;
	case 28: return NSFB_KEY_RETURN;
	case 29: return NSFB_KEY_LCTRL;
	case 97: return NSFB_KEY_RCTRL;
	case 42: return NSFB_KEY_LSHIFT;
	case 54: return NSFB_KEY_RSHIFT;
	case 43: return NSFB_KEY_BACKSLASH;
	case 57: return NSFB_KEY_SPACE;
	case 111: return NSFB_KEY_DELETE;
	case 102: return NSFB_KEY_HOME;
	case 107: return NSFB_KEY_END;
	case 104: return NSFB_KEY_PAGEUP;
	case 109: return NSFB_KEY_PAGEDOWN;
	}
	return 0;
}

/* Esc: the pause screen over the page; Quit asks NetSurf to end. */
static void pause_browser(nsfb_t *nsfb)
{
	static const struct pause_key keys[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "Move the pointer (faster when held)" },
		{ "#4", "Click; hold and move to drag" },
		{ "Fn", "+ arrows: the arrow keys (caret, scrolling)" },
		{ "#2", "Page up" },
		{ "#3", "Page down" },
		{ "#1", "Back" },
		{ "#6", "Forward" },
		{ "#5", "Type an address" },
		{ "Esc", "Pause: resume, keys, quit" },
		{ "Logo", "Back to the menu" },
	};
	static const char *const items[] = { "Resume", "Keys", "Quit" };
	struct pause_screen s = {
		.fbfd = fbfd, .evfd = evfd, .line = (unsigned)(long)nsfb->surface_priv,
		.game = (const unsigned short *)nsfb->ptr, .game_lines = nsfb->height, .band_off = 0,
		.app = "NETSURF", .name = "NetSurf",
		.about = "The keypad brings its own pointer: the arrows move it, the circle clicks.",
		.keys = keys, .n_keys = sizeof(keys) / sizeof(keys[0]),
		.items = items, .n_items = 3, .keys_item = 1,
	};
	int flags = fcntl(evfd, F_GETFL);

	if ((long)nsfb->surface_priv != nsfb->linelen)
		s.game = 0;	/* a panel with other lines: just black under it */
	fcntl(evfd, F_SETFL, flags & ~O_NONBLOCK);
	if (badge_pause(&s, 0) == 2) {
		push(NSFB_EVENT_CONTROL, 0);
		queue[(qhead + qlen - 1) % 8].value.controlcode = NSFB_CONTROL_QUIT;
	}
	fcntl(evfd, F_SETFL, flags);
}

static void translate(nsfb_t *nsfb, const struct kev *e)
{
	int down = e->value != 0, code = e->code;

	if (e->type != 1)
		return;
	if (code == 1) {		/* Esc */
		if (e->value == 1)
			pause_browser(nsfb);
		return;
	}
	if (code == KEY_FN) {
		fn = down;
		return;
	}
	if (code == 103 || code == 108 || code == 105 || code == 106) {
		static const int arrows[4] = { NSFB_KEY_UP, NSFB_KEY_DOWN,
					       NSFB_KEY_LEFT, NSFB_KEY_RIGHT };
		int i = code == 103 ? 0 : code == 108 ? 1 : code == 105 ? 2 : 3, step;

		if (fn) {
			if (e->value != 2)
				push(down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, arrows[i]);
			return;
		}
		if (!down) {
			held = 0;
			return;
		}
		held = e->value == 2 ? held + 1 : 0;
		step = held < 4 ? 6 : held < 12 ? 16 : 36;
		if (i == 0) py -= step;
		if (i == 1) py += step;
		if (i == 2) px -= step;
		if (i == 3) px += step;
		push_move(nsfb);
		return;
	}
	if (code >= 59 && code <= 64) {		/* the symbol keys */
		if (e->value == 2)
			return;
		switch (code - 59) {
		case 3:				/* circle: left button */
			push(down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, NSFB_KEY_MOUSE_1);
			return;
		case 1:
			if (down)
				push_key(NSFB_KEY_PAGEUP);
			return;
		case 2:
			if (down)
				push_key(NSFB_KEY_PAGEDOWN);
			return;
		case 0:
		case 5:
			if (!down)
				return;
			push(NSFB_EVENT_KEY_DOWN, NSFB_KEY_LCTRL);
			push_key(code == 59 ? NSFB_KEY_LEFT : NSFB_KEY_RIGHT);
			push(NSFB_EVENT_KEY_UP, NSFB_KEY_LCTRL);
			return;
		case 4:				/* cloud: the address bar */
			if (!down)
				return;
			px = nsfb->width / 2;
			py = 15;
			push_move(nsfb);
			push_key(NSFB_KEY_MOUSE_1);
			return;
		}
	}
	code = keycode(code);
	if (!code || (e->value == 2 && (code == NSFB_KEY_LSHIFT || code == NSFB_KEY_RSHIFT ||
					 code == NSFB_KEY_LCTRL || code == NSFB_KEY_RCTRL)))
		return;			/* held keys repeat, held modifiers do not */
	push(down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP, code);
}

static int badge_defaults(nsfb_t *nsfb)
{
	nsfb->width = 720;
	nsfb->height = 720;
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);
	return 0;
}

static int badge_initialise(nsfb_t *nsfb)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;

	fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fi) < 0 || vi.bits_per_pixel != 16)
		return -1;
	if (nsfb->width > (int)vi.xres)
		nsfb->width = vi.xres;
	if (nsfb->height > (int)vi.yres)
		nsfb->height = vi.yres;
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);
	nsfb->linelen = nsfb->width * 2;
	nsfb->ptr = calloc(1, (size_t)nsfb->linelen * nsfb->height);
	if (!nsfb->ptr)
		return -1;
	nsfb->surface_priv = (void *)(long)fi.line_length;
	evfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	return 0;
}

static int badge_finalise(nsfb_t *nsfb)
{
	free(nsfb->ptr);
	nsfb->ptr = NULL;
	if (fbfd >= 0)
		close(fbfd);
	if (evfd >= 0)
		close(evfd);
	fbfd = evfd = -1;
	return 0;
}

static int badge_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
	if (nsfb->ptr)			/* fixed once running */
		return -1;
	if (width > 0)
		nsfb->width = width;
	if (height > 0)
		nsfb->height = height;
	if (format != NSFB_FMT_ANY && format != NSFB_FMT_RGB565)
		return -1;
	nsfb->format = NSFB_FMT_RGB565;
	select_plotters(nsfb);
	return 0;
}

/* Lines y0..y1 of the shadow to the panel. */
static void blit(nsfb_t *nsfb, int y0, int y1)
{
	long line = (long)nsfb->surface_priv;

	if (y0 < 0)
		y0 = 0;
	if (y1 > nsfb->height)
		y1 = nsfb->height;
	if (fbfd < 0 || y1 <= y0)
		return;
	if (line == nsfb->linelen) {
		pwrite(fbfd, nsfb->ptr + y0 * nsfb->linelen,
		       (size_t)(y1 - y0) * nsfb->linelen, (off_t)y0 * line);
		return;
	}
	for (int y = y0; y < y1; y++)
		pwrite(fbfd, nsfb->ptr + y * nsfb->linelen, nsfb->linelen, (off_t)y * line);
}

static bool badge_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
	struct pollfd p = { .fd = evfd, .events = POLLIN };
	struct kev e;

	while (!qlen) {
		if (evfd < 0 || poll(&p, 1, timeout) <= 0)
			return false;
		while (read(evfd, &e, sizeof(e)) == (int)sizeof(e))
			translate(nsfb, &e);
		timeout = 0;
		if (!qlen)
			return false;
	}
	*event = queue[qhead];
	qhead = (qhead + 1) % 8;
	qlen--;
	return true;
}

static int badge_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	struct nsfb_cursor_s *cursor = nsfb->cursor;

	if (cursor && cursor->plotted && nsfb_plot_bbox_intersect(box, &cursor->loc))
		nsfb_cursor_clear(nsfb, cursor);
	return 0;
}

static int badge_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
	nsfb_bbox_t redraw, shifted;

	if (!cursor || !cursor->plotted)
		return true;
	shifted = cursor->loc;
	shifted.x0 -= cursor->hotspot_x;
	shifted.y0 -= cursor->hotspot_y;
	shifted.x1 -= cursor->hotspot_x;
	shifted.y1 -= cursor->hotspot_y;
	nsfb_plot_add_rect(&cursor->savloc, &shifted, &redraw);
	nsfb_cursor_clear(nsfb, cursor);
	nsfb_cursor_plot(nsfb, cursor);
	blit(nsfb, redraw.y0, redraw.y1);
	return true;
}

static int badge_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	struct nsfb_cursor_s *cursor = nsfb->cursor;

	if (cursor && !cursor->plotted)
		nsfb_cursor_plot(nsfb, cursor);
	blit(nsfb, box->y0, box->y1);
	return 0;
}

const nsfb_surface_rtns_t badge_rtns = {
	.defaults = badge_defaults,
	.initialise = badge_initialise,
	.finalise = badge_finalise,
	.input = badge_input,
	.claim = badge_claim,
	.update = badge_update,
	.cursor = badge_cursor,
	.geometry = badge_geometry,
};

NSFB_SURFACE_DEF(badge, NSFB_SURFACE_LINUX, &badge_rtns)
