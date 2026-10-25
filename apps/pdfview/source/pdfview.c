// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * pdfview — PDF documents on the badge's panel, for Files (docs/HOME.md),
 * as imgview is for photos. MuPDF (AGPL, linked in: hence this file's
 * licence) reads the file and renders the pages.
 *
 *   pdfview FILE
 *
 * A page is shown as wide as the screen and scrolls; Enter zooms in 2x,
 * the options (square key) show the whole page instead. MuPDF records a
 * page once as a display list; the part to show is drawn from it in
 * strips of STRIP rows into an RGB565 buffer of at most CACHE_MAX bytes
 * (an A4 page at the screen's width is 1.5 MB), so scrolling within it
 * costs nothing. A page larger than that (zoomed) keeps only the part
 * around the view, drawn again when the view leaves it. MuPDF keeps fonts
 * and images in its store, at most STORE_MAX.
 *
 * Keys (Docs/06-design-system.md 6.6): Up/Down scroll, on into the next
 * or previous page; Fn+Up/Down and Space (Shift: back) a screen;
 * Left/Right the previous and next page, zoomed in they move the page
 * sideways (Fn+Left/Right: pages); Enter zoom; the square the options (go
 * to a page, whole page or the width); the diamond the help; the circle
 * and cloud a log and settings screen that say the viewer has neither;
 * Esc closes. The page each file was left at is kept in
 * APP_CONFIG/places, and the file opens there again.
 *
 * Built with -DPDFVIEW_HOST it takes keys from its arguments and writes
 * the screen as PPM (the test in sdapps/pdfview/test).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mupdf/fitz.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"
#include "keyrep.h"

#ifndef PDFVIEW_HOST
#include <poll.h>
#endif

#define VIEW_Y		WB_STATUS_BAR_H
#define VIEW_H		(H - WB_STATUS_BAR_H - WB_HINT_BAR_H)
#define CACHE_MAX	(1600 * 1024)	/* the page's pixels kept, RGB565 */
#define STRIP		128		/* rows rendered at a time (RGB: 270 KB) */
#define STORE_MAX	(1536 * 1024)	/* MuPDF's fonts and images */
#define STEP		64		/* Up and Down */
#define PAN		160		/* zoomed: Left and Right */
#define PLACES_MAX	30

#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_ENTER	28
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_SPACE	57
#define KEY_UP		103
#define KEY_PAGEUP	104
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_PAGEDOWN	109
#define KEY_FN		0x1d0

/* ---- the document ------------------------------------------------------- */

static struct {
	fz_context *ctx;
	fz_document *doc;
	fz_display_list *list;		/* the page shown */
	fz_rect bounds;			/* its size in points */
	int pages, page;		/* page counts from 0 */
	int whole, zoom;		/* show the whole page; 2x */
	float scale;			/* pixels per point */
	int pw, ph;			/* the page in pixels at scale */
	int sx, sy;			/* the view's top left on the page */
	char err[120];
	char path[600], name[256];
} D;

/* the pixels of part of the page: (cx, cy) cw x ch at D.scale */
static struct {
	uint16_t *px;
	int cx, cy, cw, ch, page;
	float scale;
} C = { .page = -1 };

static struct { int shift, fn; } K;

static inline uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void set_err(const char *what)
{
	snprintf(D.err, sizeof(D.err), "%s", what);
	fprintf(stderr, "pdfview: %s: %s\n", D.path, what);
}

/* The scale for the page as shown, and its size in pixels. */
static void measure(void)
{
	float w = D.bounds.x1 - D.bounds.x0, h = D.bounds.y1 - D.bounds.y0;

	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	D.scale = W / w;
	if (D.whole && h * D.scale > VIEW_H)
		D.scale = VIEW_H / h;
	if (D.zoom)
		D.scale *= 2;
	D.pw = (int)(w * D.scale + 0.5f);
	D.ph = (int)(h * D.scale + 0.5f);
	if (D.pw < 1)
		D.pw = 1;
	if (D.ph < 1)
		D.ph = 1;
}

/* keep the view on the page */
static void clamp_view(void)
{
	int mx = D.pw > W ? D.pw - W : 0, my = D.ph > VIEW_H ? D.ph - VIEW_H : 0;

	D.sx = D.sx < 0 ? 0 : D.sx > mx ? mx : D.sx;
	D.sy = D.sy < 0 ? 0 : D.sy > my ? my : D.sy;
}

static void drop_page(void)
{
	fz_drop_display_list(D.ctx, D.list);
	D.list = 0;
	free(C.px);
	C.px = 0;
	C.page = -1;
}

/* Page i as a display list; 0, or -1 with D.err set. */
static int load_page(int i)
{
	fz_page *page = 0;
	int rc = 0;

	drop_page();
	D.page = i;
	D.err[0] = 0;
	fz_var(page);
	fz_try(D.ctx) {
		page = fz_load_page(D.ctx, D.doc, i);
		D.bounds = fz_bound_page(D.ctx, page);
		D.list = fz_new_display_list_from_page(D.ctx, page);
	}
	fz_always(D.ctx)
		fz_drop_page(D.ctx, page);
	fz_catch(D.ctx) {
		set_err(fz_caught(D.ctx) == FZ_ERROR_SYSTEM ? "Not enough memory for this page" :
			"This page cannot be read");
		rc = -1;
	}
	measure();
	return rc;
}

/*
 * The cache: the part of the page that holds the view, drawn from the
 * display list in strips. The whole page when it fits CACHE_MAX, else as
 * wide as fits (the view at least) and as high as the rest allows,
 * centred on the view.
 */
static int render(void)
{
	int cw, ch, cx, cy;
	fz_matrix ctm;
	fz_pixmap *pix = 0;
	fz_device *dev = 0;
	int rc = 0;

	if (!D.list)
		return -1;
	if (C.px && C.page == D.page && C.scale == D.scale && D.sx >= C.cx && D.sy >= C.cy &&
	    D.sx + (D.pw < W ? D.pw : W) <= C.cx + C.cw && D.sy + (D.ph < VIEW_H ? D.ph : VIEW_H) <= C.cy + C.ch)
		return 0;
	free(C.px);
	C.px = 0;
	cw = D.pw;			/* the page as wide as it is, or only the view's width */
	ch = D.ph < 2 * VIEW_H ? D.ph : 2 * VIEW_H;
	if ((long)cw * ch * 2 > CACHE_MAX)
		cw = D.pw < W ? D.pw : W;
	ch = (int)(CACHE_MAX / (2L * cw));
	if (ch > D.ph)
		ch = D.ph;
	cx = D.sx + (D.pw < W ? D.pw : W) / 2 - cw / 2;
	cy = D.sy + (D.ph < VIEW_H ? D.ph : VIEW_H) / 2 - ch / 2;
	cx = cx < 0 ? 0 : cx + cw > D.pw ? D.pw - cw : cx;
	cy = cy < 0 ? 0 : cy + ch > D.ph ? D.ph - ch : cy;
	C.px = malloc((size_t)cw * ch * 2);
	if (!C.px) {
		set_err("Not enough memory for this page");
		return -1;
	}
	ctm = fz_pre_translate(fz_scale(D.scale, D.scale), -D.bounds.x0, -D.bounds.y0);
	fz_var(pix);
	fz_var(dev);
	fz_try(D.ctx) {
		for (int y = 0; y < ch; y += STRIP) {
			fz_irect r = { cx, cy + y, cx + cw, cy + (y + STRIP < ch ? y + STRIP : ch) };

			pix = fz_new_pixmap_with_bbox(D.ctx, fz_device_rgb(D.ctx), r, 0, 0);
			fz_clear_pixmap_with_value(D.ctx, pix, 255);
			dev = fz_new_draw_device(D.ctx, fz_identity, pix);
			fz_run_display_list(D.ctx, D.list, dev, ctm, fz_rect_from_irect(r), 0);
			fz_close_device(D.ctx, dev);
			fz_drop_device(D.ctx, dev);
			dev = 0;
			for (int j = 0; j < r.y1 - r.y0; j++) {
				const unsigned char *s = pix->samples + (size_t)j * pix->stride;
				uint16_t *o = C.px + (size_t)(y + j) * cw;

				for (int i = 0; i < cw; i++, s += 3)
					o[i] = rgb565(s[0], s[1], s[2]);
			}
			fz_drop_pixmap(D.ctx, pix);
			pix = 0;
		}
	}
	fz_always(D.ctx) {
		fz_drop_device(D.ctx, dev);
		fz_drop_pixmap(D.ctx, pix);
	}
	fz_catch(D.ctx) {
		set_err(fz_caught(D.ctx) == FZ_ERROR_SYSTEM ? "Not enough memory for this page" :
			"This page cannot be drawn");
		free(C.px);
		C.px = 0;
		rc = -1;
	}
	if (rc == 0) {
		C.cx = cx;
		C.cy = cy;
		C.cw = cw;
		C.ch = ch;
		C.page = D.page;
		C.scale = D.scale;
	}
	return rc;
}

/* The document; 0, or -1 with D.err set. */
static int open_doc(const char *path)
{
	const char *slash = strrchr(path, '/');

	snprintf(D.path, sizeof(D.path), "%s", path);
	snprintf(D.name, sizeof(D.name), "%s", slash ? slash + 1 : path);
	D.ctx = fz_new_context(0, 0, STORE_MAX);
	if (!D.ctx) {
		set_err("Not enough memory to start");
		return -1;
	}
	fz_try(D.ctx) {
		fz_register_document_handlers(D.ctx);
		D.doc = fz_open_document(D.ctx, path);
		if (fz_needs_password(D.ctx, D.doc) && !fz_authenticate_password(D.ctx, D.doc, ""))
			fz_throw(D.ctx, FZ_ERROR_ARGUMENT, "password");
		D.pages = fz_count_pages(D.ctx, D.doc);
	}
	fz_catch(D.ctx) {
		const char *m = fz_caught_message(D.ctx);

		if (D.doc && !strcmp(m, "password"))
			set_err("This PDF is locked with a password");
		else if (access(path, R_OK))	/* MuPDF says so as a system error too */
			set_err("Cannot open the file");
		else if (fz_caught(D.ctx) == FZ_ERROR_SYSTEM)
			set_err("Not enough memory for this PDF");
		else
			set_err("Not a PDF this viewer can read");
		return -1;
	}
	if (D.pages < 1) {
		set_err("This PDF has no pages");
		return -1;
	}
	return 0;
}

/* ---- where each file was left ------------------------------------------- */

static char places_path[300];

static void places_init(void)
{
	const char *d = getenv("APP_CONFIG"), *home = getenv("HOME");
	char dir[256];

	if (d && *d)
		snprintf(dir, sizeof(dir), "%s", d);
	else
		snprintf(dir, sizeof(dir), "%s/.whyos/config/pdfview", home && *home ? home : "/root");
	for (char *s = dir + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(dir, 0700);
			*s = '/';
		}
	mkdir(dir, 0700);
	snprintf(places_path, sizeof(places_path), "%s/places", dir);
}

/* the page FILE was left at, from 0; 0 when it is not there */
static int place_of(const char *path)
{
	char line[700];
	FILE *f = fopen(places_path, "r");
	int page = 0;

	while (f && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');

		if (nl)
			*nl = 0;
		if (tab && !strcmp(tab + 1, path)) {
			page = atoi(line) - 1;
			break;
		}
	}
	if (f)
		fclose(f);
	return page < 0 ? 0 : page;
}

/* "page<TAB>path" lines, the latest first, PLACES_MAX of them */
static void place_save(void)
{
	static char old[PLACES_MAX][700];
	char tmp[310], line[700];
	FILE *f = fopen(places_path, "r"), *o;
	int n = 0;

	while (f && n < PLACES_MAX - 1 && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');

		if (nl)
			*nl = 0;
		if (tab && strcmp(tab + 1, D.path))
			snprintf(old[n++], sizeof(old[0]), "%s", line);
	}
	if (f)
		fclose(f);
	snprintf(tmp, sizeof(tmp), "%s.new", places_path);
	o = fopen(tmp, "w");
	if (!o)
		return;
	fprintf(o, "%d\t%s\n", D.page + 1, D.path);
	for (int i = 0; i < n; i++)
		fprintf(o, "%s\n", old[i]);
	if (fclose(o) == 0)
		rename(tmp, places_path);
}

/* ---- drawing ------------------------------------------------------------ */

enum { SC_VIEW, SC_LOG, SC_SETTINGS };
enum { POP_NONE, POP_HELP, POP_MENU, POP_GOTO };
static int sc, pop, pf;
static char go_text[8];
static int busy;			/* "Drawing page ..." while it renders */

static void draw_help(void)
{
	static const struct help_key keys[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "Scroll, on into the next page" },
		{ "Space", "A screen down (Shift: up); Fn+\xe2\x86\x91\xe2\x86\x93 too" },
		{ "\xe2\x86\x90\xe2\x86\x92", "Previous or next page; zoomed: move" },
		{ "Enter", "Zoom in 2x, or back" },
		{ "#1", "Options: go to a page, whole page" },
		{ "Esc", "Close" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};

	help_popup("PDF viewer", "As wide as the screen; it opens where you left the file.", keys,
		   sizeof(keys) / sizeof(keys[0]));
}

static void draw_menu(void)
{
	static const struct hint hs[] = { { "Enter", "do", 0 }, { "Esc", "cancel", 1 } };
	struct menu_item it[2];
	char count[24];

	memset(it, 0, sizeof(it));
	it[0].label = "Go to page";
	snprintf(it[0].value, sizeof(it[0].value), "%d / %d", D.page + 1, D.pages);
	it[1].label = D.whole ? "Page width" : "Whole page";
	snprintf(count, sizeof(count), "%d pages", D.pages);
	menu_popup("PDF VIEWER", D.name, count, it, 0, 2, pf, -1, 0, 0, hs, 2);
}

/* the page number typed in a field, as a pop-up */
static void draw_goto(void)
{
	static const struct hint hs[] = { { "Enter", "go", 0 }, { "Esc", "cancel", 1 } };
	int h = 32 + 18 + 6 + 36 + 24 + 52 + 32, y = WB_STATUS_BAR_H + (H - WB_STATUS_BAR_H - WB_HINT_BAR_H - h) / 2;
	int x = POP_X + 32, w = POP_W - 64, tx;
	char sub[48];

	pop_frame(y, h);
	eyebrow(x, y + 32, "GO TO PAGE");
	snprintf(sub, sizeof(sub), "1 to %d", D.pages);
	text(&f_mono13, x + w - text_w(&f_mono13, sub, 0), y + 33, sub, WB_COLOR_TEXT_LOW, 0);
	text(&f_ui28b, x, y + 32 + 18 + 6 + 2, "Page number", WB_COLOR_TEXT_HI, 0);
	y += 32 + 18 + 6 + 36 + 24;
	box_focus(x, y, w, 52, WB_RADIUS_MD, &GLOW_ROW);
	tx = text(&f_mono18, x + 20, text_top(&f_mono18, y + 26), go_text, WB_COLOR_TEXT_HI, 0);
	fill(tx + 2, y + 14, 2, 24, WB_COLOR_CYAN);
	hint_bar(hs, 2);
}

static void draw_band(int y0, int y1)
{
	static const struct hint hints[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "scroll", 0 }, { "\xe2\x86\x90 \xe2\x86\x92", "page", 0 },
		{ "Enter", "zoom", 0 }, { "#1", "options", 0 }, { "Esc", "close", 1 },
	};
	static const struct hint zoomed[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "move", 0 }, { "Enter", "back", 0 },
		{ "#1", "options", 0 }, { "Esc", "close", 1 },
	};
	char right[48], name[256];

	if (sc != SC_VIEW) {
		empty_screen("PDF VIEWER", sc == SC_LOG ? "Log" : "Settings",
			     sc == SC_LOG ? "No log yet." : "This app has no settings.");
		if (pop == POP_HELP)
			draw_help();
		return;
	}
	fill(0, y0, W, y1 - y0, WB_COLOR_VOID);
	if (busy || D.err[0] || !C.px) {
		char m[80];

		if (D.err[0])
			snprintf(m, sizeof(m), "%s", D.err);
		else
			snprintf(m, sizeof(m), "Drawing page %d\xe2\x80\xa6", D.page + 1);
		text(&f_ui22, (W - text_w(&f_ui22, m, 0)) / 2, VIEW_Y + VIEW_H / 2 - 12, m,
		     D.err[0] ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	} else {
		int vw = D.pw < W ? D.pw : W, vh = D.ph < VIEW_H ? D.ph : VIEW_H;
		int ox = (W - vw) / 2, oy = VIEW_Y + (VIEW_H - vh) / 2;

		for (int y = y0 > oy ? y0 : oy; y < y1 && y < oy + vh; y++)
			memcpy(&fb[y * W + ox], &C.px[(size_t)(y - oy + D.sy - C.cy) * C.cw + (D.sx - C.cx)],
			       (size_t)vw * 2);
		if (D.ph > VIEW_H) {	/* where on the page: a thin bar on the right */
			int th = VIEW_H * VIEW_H / D.ph, ty = VIEW_Y + (VIEW_H - th) * D.sy / (D.ph - VIEW_H);

			box(W - 7, ty, 4, th < 24 ? 24 : th, 2, WB_COLOR_TEXT_LOW, 160, 0, 0, 0);
		}
	}
	/* the status bar: the file's name; the page */
	fill(0, 0, W, WB_STATUS_BAR_H, WB_COLOR_VOID);
	if (D.pages)
		snprintf(right, sizeof(right), "%d / %d%s", D.page + 1, D.pages, D.zoom ? "   2x" : "");
	else
		right[0] = 0;
	text(&f_ui18, WB_CHROME_PAD, text_top(&f_ui18, WB_STATUS_BAR_H / 2),
	     wb_fit(&f_ui18, D.name, W - 2 * WB_CHROME_PAD - 24 - text_w(&f_mono14, right, 0), name, sizeof(name)),
	     WB_COLOR_TEXT_HI, 0);
	text(&f_mono14, W - WB_CHROME_PAD - text_w(&f_mono14, right, 0), text_top(&f_mono14, WB_STATUS_BAR_H / 2),
	     right, WB_COLOR_TEXT_MID, 0);
	hint_bar(D.zoom ? zoomed : hints, D.zoom ? 4 : 5);
	if (pop == POP_HELP)
		draw_help();
	else if (pop == POP_MENU)
		draw_menu();
	else if (pop == POP_GOTO)
		draw_goto();
}

static uint16_t band[W * 40];
#ifdef PDFVIEW_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void redraw(void)
{
	for (int y = 0; y < H; y += 40) {
		int y1 = y + 40 > H ? H : y + 40;

		/* fb points at row 0, which lies outside band: as an address only */
		fb = (uint16_t *)((uintptr_t)band - (uintptr_t)y * W * sizeof(uint16_t));
		fb_y0 = y;
		fb_y1 = y1;
		draw_band(y, y1);
#ifdef PDFVIEW_HOST
		memcpy(screen + y * W, band, (size_t)(y1 - y) * W * 2);
#else
		if (pwrite(fb_fd, band, (size_t)(y1 - y) * W * 2, (off_t)y * W * 2) < 0)
			return;
#endif
	}
}

/* Page i, from its top (or its bottom), drawn: "Drawing page" first. */
static void show(int i, int at_end)
{
	if (i < 0 || i >= D.pages)
		return;
	busy = 1;
	if (i != D.page || !D.list) {
		D.page = i;
		redraw();
		load_page(i);
	}
	D.sx = D.pw > W ? (D.pw - W) / 2 : 0;
	D.sy = at_end ? D.ph : 0;
	clamp_view();
	render();
	busy = 0;
	redraw();
}

/* the view moved: drawn again from the cache, or rendered first */
static void moved(void)
{
	clamp_view();
	if (render() < 0)
		C.px = 0;
	redraw();
}

/* Up or down by dy px; past the page's end into the next or previous one. */
static void scroll(int dy)
{
	int my = D.ph > VIEW_H ? D.ph - VIEW_H : 0;

	if (dy > 0 && D.sy >= my) {
		if (D.page + 1 < D.pages)
			show(D.page + 1, 0);
		return;
	}
	if (dy < 0 && D.sy <= 0) {
		if (D.page > 0)
			show(D.page - 1, 1);
		return;
	}
	D.sy += dy;
	moved();
}

/* a new scale (zoom, whole page): the view keeps its middle */
static void rescale(void)
{
	float old = D.scale;
	int mx = D.sx + (D.pw < W ? D.pw : W) / 2, my = D.sy + (D.ph < VIEW_H ? D.ph : VIEW_H) / 2;

	measure();
	D.sx = (int)(mx * D.scale / old) - W / 2;
	D.sy = (int)(my * D.scale / old) - VIEW_H / 2;
	clamp_view();
	busy = 1;
	redraw();
	render();
	busy = 0;
	redraw();
}

/* One key; 1 to close. */
static int key(int code)
{
	if (pop == POP_HELP) {		/* only an explanation: Esc or the diamond closes it */
		if (code == KEY_ESC || code == KEY_SYM_HELP)
			pop = POP_NONE;
		redraw();
		return 0;
	}
	if (pop == POP_MENU) {
		if (code == KEY_UP || code == KEY_DOWN)
			pf = code == KEY_UP ? 0 : 1;
		else if (code == KEY_ESC)
			pop = POP_NONE;
		else if (code == KEY_ENTER && pf == 0) {
			pop = POP_GOTO;
			go_text[0] = 0;
		} else if (code == KEY_ENTER) {
			pop = POP_NONE;
			D.whole = !D.whole;
			D.zoom = 0;
			rescale();
			return 0;
		}
		redraw();
		return 0;
	}
	if (pop == POP_GOTO) {
		int len = (int)strlen(go_text);

		if (code >= 2 && code <= 11 && len < 6) {	/* 1..9, 0 */
			go_text[len] = (char)(code == 11 ? '0' : '0' + code - 1);
			go_text[len + 1] = 0;
		} else if (code == KEY_BACKSPACE && len) {
			go_text[len - 1] = 0;
		} else if (code == KEY_ESC) {
			pop = POP_NONE;
		} else if (code == KEY_ENTER) {
			int n = atoi(go_text);

			pop = POP_NONE;
			if (n >= 1 && n <= D.pages) {
				show(n - 1, 0);
				return 0;
			}
		}
		redraw();
		return 0;
	}
	if (code == KEY_SYM_HELP || code == KEY_SYM_LOG || code == KEY_SYM_SETTINGS) {
		if (code == KEY_SYM_HELP)
			pop = POP_HELP;
		else
			sc = code == KEY_SYM_LOG ? SC_LOG : SC_SETTINGS;
		redraw();
		return 0;
	}
	if (sc != SC_VIEW) {		/* the log or settings screen: Esc back */
		if (code == KEY_ESC) {
			sc = SC_VIEW;
			redraw();
		}
		return 0;
	}
	if (code == KEY_ESC)
		return 1;
	if (!D.pages)
		return 0;
	switch (code) {
	case KEY_ENTER:
		D.zoom = !D.zoom;
		rescale();
		break;
	case KEY_SYM_MENU:
		pop = POP_MENU;
		pf = 0;
		redraw();
		break;
	case KEY_UP:
	case KEY_DOWN:
		if (K.fn)
			scroll(code == KEY_UP ? -(VIEW_H - STEP) : VIEW_H - STEP);
		else
			scroll(code == KEY_UP ? -STEP : STEP);
		break;
	case KEY_PAGEUP:
	case KEY_PAGEDOWN:
		scroll(code == KEY_PAGEUP ? -(VIEW_H - STEP) : VIEW_H - STEP);
		break;
	case KEY_SPACE:
		scroll(K.shift ? -(VIEW_H - STEP) : VIEW_H - STEP);
		break;
	case KEY_LEFT:
	case KEY_RIGHT:
		if (D.pw > W && !K.fn) {	/* zoomed: sideways */
			D.sx += code == KEY_LEFT ? -PAN : PAN;
			moved();
		} else {
			show(D.page + (code == KEY_LEFT ? -1 : 1), 0);
		}
		break;
	}
	return 0;
}

static void start(const char *path)
{
	places_init();
	if (open_doc(path) == 0) {
		int p = place_of(D.path);

		D.page = -1;
		show(p < D.pages ? p : 0, 0);
	} else {
		redraw();
	}
}

#ifdef PDFVIEW_HOST
/*
 * pdfview-host FILE OUT.ppm [KEYS]: the screen after KEYS (one character a
 * key: u d l r arrows, U D Fn+Up/Down, e Enter, x Esc, s Space, S
 * Shift+Space, m the square, h the diamond, b Backspace, 0-9 digits);
 * prints the page, the view and the error.
 */
int main(int argc, char **argv)
{
	FILE *o;
	const char *k = argc > 3 ? argv[3] : "";
	int closed = 0;

	if (argc < 3)
		return 2;
	start(argv[1]);
	for (; *k && !closed; k++) {
		static const char ch[] = "udlrUDexsSmhb";
		static const int code[] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_ENTER,
					    KEY_ESC, KEY_SPACE, KEY_SPACE, KEY_SYM_MENU, KEY_SYM_HELP, KEY_BACKSPACE };
		const char *p = strchr(ch, *k);

		K.fn = *k == 'U' || *k == 'D';
		K.shift = *k == 'S';
		if (*k >= '0' && *k <= '9')
			closed = key(*k == '0' ? 11 : 1 + *k - '0');
		else if (p)
			closed = key(code[p - ch]);
		K.fn = K.shift = 0;
	}
	if (D.pages)
		place_save();
	o = fopen(argv[2], "wb");
	fprintf(o, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];
		unsigned char c[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2),
				       (unsigned char)((p & 31) << 3) };

		fwrite(c, 3, 1, o);
	}
	fclose(o);
	printf("page %d of %d\nsize %d %d\nview %d %d\nzoom %d\nwhole %d\npop %d\nclosed %d\ncache %d %d %d %d\n"
	       "error %s\n", D.page + 1, D.pages, D.pw, D.ph, D.sx, D.sy, D.zoom, D.whole, pop, closed,
	       C.cx, C.cy, C.cw, C.ch, D.err);
	return 0;
}
#else

/* uClibc has no getentropy, which MuPDF asks for its random numbers */
int getentropy(void *buf, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC), r;

	if (fd < 0)
		return -1;
	r = (int)read(fd, buf, n);
	close(fd);
	return r == (int)n ? 0 : -1;
}

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t stop;

static void on_signal(int s)
{
	stop = s;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int ev;

	if (argc < 2)
		return 2;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;	/* no SA_RESTART: read() returns, the page is kept */
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev < 0)
		return 1;
	start(argv[1]);
	while (!stop) {
		struct ev e;

		if (read(ev, &e, sizeof(e)) != sizeof(e))
			break;
		if (e.type != 1)
			continue;
		if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT) {
			K.shift = e.value != 0;
			continue;
		}
		if (e.code == KEY_FN) {
			K.fn = e.value != 0;
			continue;
		}
		if (!key_acts(e.code, e.value))
			continue;
		if (e.value == 2 && key_stale(e.sec, e.usec))
			continue;	/* fell behind a held key: skip what waited */
		if (key(e.code))
			break;
	}
	if (D.pages)
		place_save();
	return 0;
}
#endif
