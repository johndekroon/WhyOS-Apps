/* SPDX-License-Identifier: MIT */
/*
 * Files: the screens (ui.h), drawn with libwb (why2025-linux libwb) in the
 * style of the canvas "Menu's met frontpaneel" and its board Files.dc.html
 * (2026-10-27): the frontpanel's figures behind every screen, a folder as
 * one panel of glass with a column header (name, size, changed), the
 * places (SD card, USB, System) as pills right of the title, and pop-ups
 * frosted. Every band draws the whole screen, clipped to the band; a frame
 * with a pop-up is drawn whole when there is room (the frost blurs what is
 * under it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#include "wb_tokens.h"

#define W	WB_SCREEN_W
#define H	WB_SCREEN_H
#ifdef FILES_UI_HOST
#define WB_HOST		/* the rotation is rot, not the panel's */
#endif
#include "wb.h"
#include "lora_state.h"
#include "wbstatus.h"

/* The folder's panel (the board): 624 wide at 48, 148; a column header of
 * 34 px and a line; the list under it, rows of 48 with 2 between and 6
 * around; the panel's bottom 18 px over the hint bar. */
#define PANEL_X		48
#define PANEL_Y		148
#define PANEL_W		624
#define PANEL_H		499
#define COLS_H		34
#define LIST_Y		(PANEL_Y + 1 + COLS_H + 1)
#define LIST_H		(PANEL_Y + PANEL_H - 1 - LIST_Y)	/* 462 */
#define FOOT_H		40	/* a notice or the clipboard, under the list */
#define ROW		48
#define ROW_GAP		2
#define PITCH		(ROW + ROW_GAP)
#define LPAD		6
#define MARGIN		PITCH	/* the focus keeps a row of room above and below */
#define PANEL_PAD_X	20	/* the footer's text from the panel's sides */
/* the row's columns: icon, name, size (84) and changed (96), right-aligned */
#define ROW_X		(PANEL_X + 1 + LPAD)
#define ROW_W		(PANEL_W - 2 - 2 * LPAD)
#define ICON_X		(ROW_X + 2 + 12)
#define NAME_X		(ICON_X + 22 + 14)
#define DATE_R		(ROW_X + ROW_W - 2 - 22)
#define SIZE_R		(DATE_R - 96 - 14)
#define NAME_R		(SIZE_R - 84 - 14)

_Static_assert(FILES_PAGE == LIST_H / PITCH - 1, "app.h FILES_PAGE: the rows in view less one");

/* the log: lines in a panel as the folder's */
#define LOG_TOP		(PANEL_Y + 16)
#define LOG_ROW		23
_Static_assert(LOG_ROWS * LOG_ROW <= PANEL_H - 2 * 16, "app.h LOG_ROWS");

struct ui_status ui_st;

int ui_wifi_bars(void)
{
	return wifi_bars();
}

int ui_lora_on(void)
{
	return lora_on();
}

static int need_full = 1;	/* the next frame drawn whole (ui_present) */

int ui_turned(void)
{
	int t = wb_scene_turned();

	need_full |= t;
	return t;
}

/* status bar, as the menu's: WhyOS left; LoRa, Wi-Fi and the clock right */
static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
	     text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
	status_right(ui_st.hhmm, ui_st.clock_ok, ui_st.bars, ui_st.lora);
}

/*
 * The scene behind the screen. wb_scene() reads the panel's rotation: in
 * the frame's first band only, so that every band of a frame has the same.
 */
static int band_first;

static void files_scene(void)
{
	if (band_first) {
		wb_scene();
		band_first = 0;
		return;
	}
	scene_draw();
	scene_shown = 1;
	gb_ok = 0;
}

static const struct wb_icon *icon_of(enum row_icon ic)
{
	switch (ic) {
	case I_FOLDER: return &r_folder;
	case I_IMAGE: return &r_image;
	case I_GLOBE: return &r_globe;
	case I_DOC: return &r_doc;
	case I_GAMEBOY: return &r_gameboy;
	case I_TEXT: return &r_text;
	case I_FILE: return &r_file;
	case I_SDCARD: return &r_sdcard;
	case I_USBSTICK: return &r_usbstick;
	case I_DRIVE: return &r_drive;
	case I_CHIP: return &r_chip;
	case I_INFO: return &r_info;
	default: return 0;
	}
}

/* c at 40 % over the panel: a row the panel cuts off (the board) */
static uint32_t fade(uint32_t c)
{
	uint32_t o = 0;

	for (int sh = 0; sh < 24; sh += 8)
		o |= ((((c >> sh) & 255) * 102 + ((0x0C0C16 >> sh) & 255) * 153) / 255) << sh;
	return o;
}

/* The board's panel: glass as a card's, a little darker (the void at
 * ~89 % instead of 85 %) */
static void files_panel(int x, int y, int w, int h)
{
	box(x, y, w, h, WB_SQUARE, WB_COLOR_VOID, 68, 0, 0, 0);
	wb_card(x, y, w, h, WB_RADIUS_LG, 0, 0);
}

/* A scroll bar inside a panel's right edge: 4 px, white 10 %, the thumb
 * cyan with a faint glow (the board). total px of which view are in
 * view, from off. */
static void panel_scrollbar(int x, int y, int h, int total, int view, int off)
{
	int th, ty;

	if (total <= view)
		return;
	th = h * view / total;
	th = th < 24 ? 24 : th;
	ty = y + (h - th) * off / (total - view);
	box(x, y, 4, h, 2, 0xFFFFFF, 26, 0, 0, 0);
	box(x - 3, ty - 3, 10, th + 6, 4, WB_COLOR_CYAN, 30, 0, 0, 0);
	box(x - 1, ty - 1, 6, th + 2, 3, WB_COLOR_CYAN, 60, 0, 0, 0);
	box(x, ty, 4, th, 2, WB_COLOR_CYAN, 255, 0, 0, 0);
}

/* ---- the folder ------------------------------------------------------------ */

enum { PL_SD, PL_USB, PL_SYSTEM };

/* the settings, or the help or log over them (as files_build) */
static int in_settings(const struct files *f)
{
	return f->sc == SC_SETTINGS || (f->under == SC_SETTINGS && (f->sc == SC_HELP || f->sc == SC_LOG));
}

/*
 * Worked out once a frame (ui_present), before the bands: the list's
 * scroll over all entries, the eyebrow, the place.
 */
static struct {
	char dir[256];		/* the folder scrolled */
	int off;		/* px of the list scrolled away */
	int view;		/* the list's height: LIST_H, less the footer */
	int total;		/* all entries' height */
	char crumb[160];
	int place, usb;		/* the pill lit; whether a USB drive is in */
} lay;

static void str_upper(char *s)
{
	for (; *s; s++)
		if (*s >= 'a' && *s <= 'z')
			*s -= 'a' - 'A';
}

/* "SD CARD › /home/john · 18 ITEMS · 12 GB FREE", the path cut at its
 * start when it is too long */
static void make_crumb(const struct files *f)
{
	const struct fsinfo *fs = disks_fs_of(&f->d, f->b.dir);
	char place[40], tail[64];
	const char *path = f->b.dir;
	int max = W - WB_CONTENT_PAD - (WB_CONTENT_PAD + 6), n = f->b.n + f->b.more;

	if (fs)
		fs_name(&f->d, fs, place, sizeof(place));
	else
		snprintf(place, sizeof(place), "System");
	str_upper(place);
	snprintf(tail, sizeof(tail), " \xc2\xb7 %d ITEM%s%s%s", n, n == 1 ? "" : "S",
		 f->l.title_r[0] ? " \xc2\xb7 " : "", f->l.title_r);
	str_upper(tail);
	for (;;) {
		snprintf(lay.crumb, sizeof(lay.crumb), "%s \xe2\x80\xba %s%s%s", place,
			 path == f->b.dir ? "" : "\xe2\x80\xa6", path, tail);
		if (text_w(&f_mono14, lay.crumb, 22) <= max || !path[0] || !path[1])
			break;
		path++;
	}
}

static void layout(const struct files *f)
{
	const struct fsinfo *fs;
	const struct diskinfo *dk;
	int foot = f->l.notice || f->l.tag[0], n = f->b.n, sel = f->b.sel, max;

	if (br_drives(&f->b) || in_settings(f))
		return;
	lay.view = LIST_H - (foot ? FOOT_H : 0);
	lay.total = n ? n * PITCH - ROW_GAP + 2 * LPAD : 0;
	max = lay.total > lay.view ? lay.total - lay.view : 0;
	if (strcmp(lay.dir, f->b.dir)) {	/* another folder: the focus in the middle */
		snprintf(lay.dir, sizeof(lay.dir), "%s", f->b.dir);
		lay.off = LPAD + sel * PITCH - (lay.view - ROW) / 2;
	}
	if (n) {		/* the focus in view, with a row of room (the board) */
		int top = LPAD + sel * PITCH, bot = top + ROW;

		if (top - MARGIN < lay.off)
			lay.off = top - MARGIN;
		if (bot + MARGIN > lay.off + lay.view)
			lay.off = bot + MARGIN - lay.view;
	}
	lay.off = lay.off > max ? max : lay.off < 0 ? 0 : lay.off;
	make_crumb(f);
	fs = disks_fs_of(&f->d, f->b.dir);
	dk = fs ? fs_disk(&f->d, fs) : 0;
	lay.place = !dk ? PL_SYSTEM : dk->usb ? PL_USB : PL_SD;
	lay.usb = lay.place == PL_USB;
	for (int i = 0; i < f->d.n_disk; i++)
		lay.usb |= f->d.disk[i].usb;
}

/* The places right of the title, as pills: the one we are in solid cyan */
static void pills(void)
{
	static const char *const names[] = { "SD card", "USB", "System" };
	const struct wb_font *t = &f_ui16;
	int x = W - WB_CONTENT_PAD, y = 72 + 18 + 6 + 44 - 4 - 32;

	for (int i = 2; i >= 0; i--) {
		int w = text_w(t, names[i], 0) + 28, on = i == lay.place;

		if (i == PL_USB && !lay.usb)
			continue;
		x -= w;
		if (on) {
			box(x, y, w, 32, WB_SQUARE, WB_COLOR_CYAN, 255, 0, 0, 0);
		} else {
			box(x, y, w, 32, WB_SQUARE, 0xFFFFFF, 20, 0, 0, 0);
			edge_rect(x, y, w, 32, 0xFFFFFF, 46);
		}
		text(t, x + 14, text_top(t, y + 16), names[i], on ? WB_COLOR_VOID : WB_COLOR_TEXT_HI, 0);
		x -= 8;
	}
}

/* an entry of the folder: icon, name, size, when it changed */
static void entry_row(const struct row *r, int y, int on, int faded)
{
	const struct wb_font *nf = r->kind == R_FOLDER ? &f_ui18m : &f_ui18;
	const struct wb_icon *ic = icon_of(r->ic);
	uint32_t ic_c = on || r->kind == R_FOLDER ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	uint32_t hi = WB_COLOR_TEXT_HI, mid = WB_COLOR_TEXT_MID;
	int cy = y + ROW / 2;
	char buf[80];

	if (faded) {
		ic_c = fade(ic_c);
		hi = fade(hi);
		mid = fade(mid);
	}
	if (on)		/* cyan 22 %, a 2 px border, the pop-up options' glow */
		box_shaped(ROW_X, y, ROW_W, ROW, WB_SQUARE, WB_COLOR_CYAN, 56, WB_COLOR_CYAN, 2, &GLOW_POP);
	if (ic)
		icon_scaled(ic, ICON_X, cy - 11, 22, ic_c);
	text(nf, NAME_X, text_top(nf, cy), wb_fit(nf, r->label, NAME_R - NAME_X, buf, sizeof(buf)), hi, 0);
	if (r->value[0])
		text(&f_mono14, SIZE_R - text_w(&f_mono14, r->value, 0), text_top(&f_mono14, cy), r->value, mid, 0);
	if (r->date[0])
		text(&f_mono14, DATE_R - text_w(&f_mono14, r->date, 0), text_top(&f_mono14, cy), r->date, mid, 0);
}

/*
 * The board's hint bar: Enter, PgUp PgDn, Del on the left, the position
 * in mono and Esc on the right. Keys with a space are caps side by side.
 */
static int caps(int x, int cy, const char *keys)
{
	char k[16];

	for (const char *s = keys; *s; ) {
		size_t n = strcspn(s, " ");

		snprintf(k, sizeof(k), "%.*s", (int)n, s);
		x = keycap(x, cy, k);
		s += n;
		if (*s) {
			s++;
			x += 4;
		}
	}
	return x;
}

static int caps_w(const char *keys)
{
	char k[16];
	int w = 0;

	for (const char *s = keys; *s; ) {
		size_t n = strcspn(s, " ");

		snprintf(k, sizeof(k), "%.*s", (int)n, s);
		w += keycap_w(k) + 18;
		s += n;
		if (*s) {
			s++;
			w += 4;
		}
	}
	return w;
}

static void folder_hints(const struct files *f)
{
	const struct wb_font *t = &f_ui16;
	const struct br_ent *e = f->b.n ? &f->b.e[f->b.sel] : 0;
	int y0 = H - WB_HINT_BAR_H, cy = y0 + WB_HINT_BAR_H / 2, x = WB_CHROME_PAD, ex;
	const char *esc = f->b.dir[1] ? "up" : "close";
	struct hint h[3];
	int n = 0;
	char pos[24];

	if (e)
		h[n++] = (struct hint){ "Enter", e->dir ? "open" : "open with\xe2\x80\xa6", 0 };
	if (lay.total > lay.view)
		h[n++] = (struct hint){ "PgUp PgDn", "page", 0 };
	if (e)
		h[n++] = (struct hint){ "Del", "delete", 0 };
	hint_glass(y0);
	for (int i = 0; i < n; i++) {
		x = caps(x, cy, h[i].keys);
		x = text(t, x + 8, text_top(t, cy), h[i].label, WB_COLOR_TEXT_MID, 0) + 22;
	}
	ex = W - WB_CHROME_PAD - caps_w("Esc") - 8 - text_w(t, esc, 0);
	text(t, caps(ex, cy, "Esc") + 8, text_top(t, cy), esc, WB_COLOR_TEXT_MID, 0);
	if (e) {
		snprintf(pos, sizeof(pos), "%d / %d", f->b.sel + 1, f->b.n);
		text(&f_mono14, ex - 22 - text_w(&f_mono14, pos, 0), text_top(&f_mono14, cy), pos,
		     WB_COLOR_TEXT_MID, 0);
	}
}

static void draw_folder(const struct files *f)
{
	const struct list *l = &f->l;
	int bottom = LIST_Y + lay.view;
	char buf[96];

	files_scene();
	status_bar();
	wb_header(lay.crumb, "Files", 0);
	pills();
	files_panel(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
	/* the columns' names, a line under them */
	{
		const struct wb_font *m = &f_mono13;
		int cy = PANEL_Y + 1 + COLS_H / 2, ty = text_top(m, cy);

		text(m, NAME_X, ty, "NAME", WB_COLOR_PLACEHOLDER, 12);
		text(m, SIZE_R - text_w(m, "SIZE", 12), ty, "SIZE", WB_COLOR_PLACEHOLDER, 12);
		text(m, DATE_R - text_w(m, "CHANGED", 12), ty, "CHANGED", WB_COLOR_PLACEHOLDER, 12);
		for (int x = PANEL_X + 1; x < PANEL_X + PANEL_W - 1; x++)
			put(x, LIST_Y - 1, 0xFFFFFF, 26);
	}
	/* the rows, cut off by the list: those it cuts at 40 % */
	clip_rows(LIST_Y, bottom);
	for (int k = 0; k < l->n; k++) {
		const struct row *r = &l->rows[k];
		int y, seen;

		if (r->kind == R_INFO) {	/* "Empty folder" */
			int cy = LIST_Y + LPAD + ROW / 2;

			icon_scaled(&r_info, ICON_X, cy - 11, 22, WB_COLOR_TEXT_LOW);
			text(&f_ui18, NAME_X, text_top(&f_ui18, cy), r->label, WB_COLOR_TEXT_MID, 0);
			continue;
		}
		y = LIST_Y + LPAD + r->idx * PITCH - lay.off;
		if (y + ROW <= LIST_Y || y >= bottom)
			continue;
		seen = (y + ROW < bottom ? y + ROW : bottom) - (y > LIST_Y ? y : LIST_Y);
		entry_row(r, y, k == l->focus, seen < ROW);
	}
	clip_end();
	panel_scrollbar(PANEL_X + PANEL_W - 1 - 6 - 4, LIST_Y + 6, lay.view - 12, lay.total, lay.view, lay.off);
	/* under the list: a notice (red: something failed), the clipboard */
	if (lay.view < LIST_H) {
		int fy = bottom, cy = fy + FOOT_H / 2, tx = PANEL_X + PANEL_PAD_X, tw = 0;

		for (int x = PANEL_X + 1; x < PANEL_X + PANEL_W - 1; x++)
			put(x, fy, 0xFFFFFF, 26);
		if (l->tag[0]) {
			const char *tg = wb_fit(&f_ui16, l->tag, PANEL_W / 2, buf, sizeof(buf));
			int x;

			tw = text_w(&f_ui16, tg, 0) + 24;
			x = PANEL_X + PANEL_W - PANEL_PAD_X - tw;
			box(x, cy - 14, tw, 28, WB_SQUARE, WB_COLOR_PINK, 36, 0, 0, 0);
			edge_rect(x, cy - 14, tw, 28, WB_COLOR_PINK, 140);
			text(&f_ui16, x + 12, text_top(&f_ui16, cy), tg, WB_COLOR_PINK, 0);
			tw += 16;
		}
		if (l->notice)
			text(&f_ui16, tx, text_top(&f_ui16, cy),
			     wb_fit(&f_ui16, l->notice, PANEL_W - 2 * PANEL_PAD_X - tw, buf, sizeof(buf)),
			     l->notice_bad ? WB_COLOR_ERROR : WB_COLOR_OK, 0);
	}
	folder_hints(f);
}

/* ---- Drives and the settings: rows on the scene ---------------------------- */

/* drives 104 px (name, path, usage), the rest 68; a group's name 17 */
static int row_h(const struct row *r)
{
	return r->kind == R_GROUP ? 17 : r->kind == R_DRIVE ? 104 : 68;
}

static int focusable(const struct row *r)
{
	return r->kind == R_FOLDER || r->kind == R_FILE || r->kind == R_DRIVE || r->kind == R_TOGGLE;
}

static void drive_row(const struct files *f, const struct row *r, int y, int on)
{
	const struct fsinfo *fs = &f->d.fs[r->idx];
	const struct wb_icon *ic = icon_of(r->ic);
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, top = y + 18 + 16;
	int rx = x + w - 18, lx = x + 20 + 26 + 16, tx = x + 24, tw = w - 48, ty = y + 104 - 18 - 8;
	int pct = fs->size ? (int)(fs->used * 100 / fs->size) : 0;
	int full = pct >= 90 && strcmp(fs->type, "squashfs");
	uint32_t ic_c = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	char v[48], buf[80];

	wb_card(x, y, w, 104, WB_RADIUS_MD, on, &GLOW_FOCUS);
	if (ic)
		icon(ic, x + 20, top - 13, ic_c, 255);
	icon(&r_chevron, rx - 20, top - 10, ic_c, 255);
	rx -= 20 + 16;
	if (!fs->mnt[0])
		snprintf(v, sizeof(v), "Not mounted");
	else if (!strcmp(fs->type, "squashfs"))
		snprintf(v, sizeof(v), "Read-only");
	else
		strcat(human(v, fs->size > fs->used ? fs->size - fs->used : 0), " free");
	rx -= text_w(&f_mono14, v, 0);
	text(&f_mono14, rx, text_top(&f_mono14, top), v, full ? WB_COLOR_WARN : on ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	rx -= 16;
	text(&f_ui22, lx, text_top(&f_ui22, top), wb_fit(&f_ui22, r->label, rx - lx, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, 0);
	snprintf(v, sizeof(v), "%s  %s", fs->mnt[0] ? fs->mnt : fs->dev, fs->type);
	text(&f_mono13, lx, top + 16, v, WB_COLOR_PLACEHOLDER, 0);
	/* the usage, as Settings > Storage's */
	box(tx, ty, tw, 8, 4, 0xFFFFFF, 46, 0, 0, 0);
	if (fs->mnt[0] && pct)
		box(tx, ty, tw * pct / 100 < 8 ? 8 : tw * pct / 100, 8, 4,
		    full ? WB_COLOR_WARN : on ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER, 255, 0, 0, 0);
}

static void draw_row(const struct files *f, const struct row *r, int y, int on)
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD;

	switch (r->kind) {
	case R_GROUP:
		text(&f_mono13, x + 6, y, r->label, WB_COLOR_PLACEHOLDER, 21);
		break;
	case R_DRIVE:
		drive_row(f, r, y, on);
		break;
	case R_TOGGLE:		/* the settings: the line under it is its second line */
		wb_row(x, y, w, 68, icon_of(r->ic), r->label,
		       r + 1 < f->l.rows + f->l.n && r[1].kind == R_INFO ? r[1].label : 0, 0,
		       WB_ROW_SWITCH, r->on, on);
		break;
	default:
		wb_row(x, y, w, 68, icon_of(r->ic), r->label, 0, r->value, WB_ROW_NONE, 0, on);
		break;
	}
}

static void draw_list(const struct files *f)
{
	const struct list *l = &f->l;
	int settings = in_settings(f);
	int ys[LIST_MAX], hs[LIST_MAX], y = 0, top = WB_ROWS_TOP, bottom = H - WB_HINT_BAR_H - 8;
	int focus = l->focus, scroll, cut, show, n = 0;
	struct hint h[4];
	char buf[96];

	files_scene();
	status_bar();
	wb_header("FILES \xe2\x80\xba", settings ? "Settings" : l->title ? l->title : "", 0);
	if (l->notice) {
		text(&f_ui18, WB_CONTENT_PAD + 6, top, wb_fit(&f_ui18, l->notice, W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     l->notice_bad ? WB_COLOR_ERROR : WB_COLOR_OK, 0);
		top += 24 + 12;
	}
	/* lay out, then scroll so the focused row is fully visible; the
	 * settings' info row is the switch's second line */
	for (int i = 0; i < l->n; i++) {
		if (settings && l->rows[i].kind == R_INFO) {
			ys[i] = y;
			hs[i] = 0;
			continue;
		}
		if (i && l->rows[i].kind == R_GROUP)
			y += 24 - 10;		/* group gap replaces row gap */
		ys[i] = y;
		hs[i] = row_h(&l->rows[i]);
		y += hs[i] + 10;
	}
	/* on the last row that takes the focus, the rows under it show too */
	show = focus < 0 ? focus : l->n - 1;
	for (int i = focus + 1; focus >= 0 && i < l->n; i++)
		if (focusable(&l->rows[i]))
			show = focus;
	scroll = list_scroll(ys, hs, l->n, show, focus, bottom - top, &cut);
	/* cards cut off at either end show only their part inside the list;
	 * the focus's glow reaches past them */
	clip_rows(scroll ? top : 0, l->n && y - 10 - scroll > bottom - top ? top + cut : H);
	for (int i = 0; i < l->n; i++) {
		int ry = top + ys[i] - scroll;

		if (!hs[i] || (ry + hs[i] <= top && scroll))
			continue;
		if (ry >= top + cut)
			break;
		draw_row(f, &l->rows[i], ry, i == focus);
	}
	clip_end();
	scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, y - 10, bottom - top, scroll);
	/* the hint bar (6.8): the symbol keys first, Esc on the right */
	if (settings) {
		h[n++] = (struct hint){ "Enter", "switch", 0 };
		h[n++] = (struct hint){ "Esc", "back", 1 };
	} else {
		h[n++] = (struct hint){ "#2", "home", 0 };
		if (focus >= 0)
			h[n++] = (struct hint){ "Enter", "open", 0 };
		h[n++] = (struct hint){ "Esc", "close", 1 };
	}
	hint_bar(h, n);
}

/* ---- pop-ups ---------------------------------------------------------------- */

/* Rename and New folder: the name, the extension in grey (Rename) */
static void draw_input(const struct files *f)
{
	static const struct hint hints[] = { { "Enter", "save", 0 }, { "Esc", "cancel", 1 } };
	const struct input *in = &f->in;
	const char *dot = in->rename ? strrchr(in->buf, '.') : 0;
	int ext = dot && dot != in->buf ? (int)(dot - in->buf) : in->len;
	int fx = DLG_X + 32, fw = DLG_W - 64, fy, tx, max = fw - 40, from = 0, cx, e;
	char shown[sizeof(in->buf)], c;

	fy = dialog("FILES", in->title, 56 + (in->err ? 34 : 0), 0, 0, 0, 0, -1);
	/* the field: the void, its border cyan with the options' glow */
	box(fx, fy, fw, 56, WB_SQUARE, WB_COLOR_VOID, 115, 0, 0, 0);
	box_shaped(fx, fy, fw, 56, WB_SQUARE, WB_COLOR_CYAN, 20, WB_COLOR_CYAN, 2, &GLOW_POP);
	memcpy(shown, in->buf, sizeof(shown));
	/* scroll so the cursor shows: drop characters off the front */
	for (;;) {
		c = shown[in->cur];
		shown[in->cur] = 0;
		cx = text_w(&f_mono18, shown + from, 0);
		shown[in->cur] = c;
		if (cx <= max || from >= in->cur)
			break;
		from++;
	}
	e = ext > from ? ext : from;
	c = shown[e];
	shown[e] = 0;
	tx = text(&f_mono18, fx + 20, text_top(&f_mono18, fy + 28), shown + from, WB_COLOR_TEXT_HI, 0);
	shown[e] = c;
	text(&f_mono18, tx, text_top(&f_mono18, fy + 28), shown + e, WB_COLOR_PLACEHOLDER, 0);
	fill(fx + 20 + cx + 1, fy + 16, 2, 24, WB_COLOR_CYAN);
	if (in->err)
		text(&f_ui18, fx, fy + 56 + 10, in->err, WB_COLOR_ERROR, 0);
	dlg_hints(hints, 2);
}

/* the square key's options, and Open with: a menu, no choice boxes */
static void draw_pick(const struct files *f)
{
	static const struct hint hints[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
	static struct menu_item it[PICK_MAX];
	const struct pick *p = &f->p;
	char count[16];

	for (int i = 0; i < p->n; i++) {
		memset(&it[i], 0, sizeof(it[i]));
		it[i].label = p->label[i];
		it[i].sub = p->sub[i];
		it[i].sep = p->sep[i];
		it[i].danger = p->danger[i];
	}
	snprintf(count, sizeof(count), "%d app%s", p->n, p->n == 1 ? "" : "s");
	menu_popup("FILES", p->title, p->openwith ? count : 0, it, 0, p->n, p->focus, -2, 0, 0, hints, 2);
}

static void draw_confirm(const struct files *f)
{
	static const char *const labels[2] = { "Cancel", "Delete" };
	static const struct wb_icon *const ics[2] = { &r_chevron, &r_trash };
	static const struct hint hints[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "select", 0 }, { "Enter", "confirm", 0 }, { "Esc", "cancel", 1 },
	};
	char t[96];

	snprintf(t, sizeof(t), "Delete %s?", f->b.n ? f->b.e[f->b.sel].name : "");
	dialog("FILES", t, 0, labels, ics, 2, f->cfocus, 1);
	dlg_hints(hints, 3);
}

/* ---- the log ---------------------------------------------------------------- */

static void draw_log(const struct files *f)
{
	static const struct hint hints[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "scroll", 0 }, { "Esc", "back", 1 },
	};
	char buf[120];

	files_scene();
	status_bar();
	wb_header("FILES \xe2\x80\xba", "Copy, move and delete", 0);
	if (!f->log_n) {
		text(&f_ui20, WB_CONTENT_PAD + 6, WB_ROWS_TOP, "No log yet.", WB_COLOR_TEXT_MID, 0);
		hint_bar(hints + 1, 1);
		return;
	}
	files_panel(PANEL_X, PANEL_Y, PANEL_W, PANEL_H);
	for (int i = 0; i < LOG_ROWS && f->log_top + i < f->log_n; i++)
		text(&f_mono14, PANEL_X + 20, LOG_TOP + i * LOG_ROW,
		     wb_fit(&f_mono14, f->log_line[f->log_top + i], PANEL_W - 40 - 10, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
	panel_scrollbar(PANEL_X + PANEL_W - 1 - 6 - 4, PANEL_Y + 7, PANEL_H - 14, f->log_n, LOG_ROWS, f->log_top);
	if (f->log_n > LOG_ROWS)
		hint_bar(hints, 2);
	else
		hint_bar(hints + 1, 1);
}

/* the diamond key: what this screen is for and its keys */
static void draw_help(const struct files *f)
{
	struct help_key k[12];
	int n = 0, drives = br_drives(&f->b);
	const char *about, *title = drives ? "Drives" : "Files";

	if (f->under == SC_LOG) {
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Scroll" };
		title = "Copy, move and delete";
		about = "The end of the log, newest at the bottom.";
	} else if (f->under == SC_SETTINGS) {
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Choose a row" };
		k[n++] = (struct help_key){ "Enter", "Switch it" };
		title = "Files settings";
		about = "How Files shows folders.";
	} else {
		about = drives ? "All storage: the SD card, USB drives and the badge's own. "
				 "Enter mounts a drive when needed and opens it." :
				 "Every file on the badge, as it is.";
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Choose a row; hold to scroll" };
		k[n++] = (struct help_key){ "Enter", drives ? "Open" : "Open a folder, or a file with its app" };
		if (!drives) {
			k[n++] = (struct help_key){ "PgUp PgDn", "A page up or down; \xe2\x86\x90 \xe2\x86\x92 too" };
			k[n++] = (struct help_key){ "Del", "Delete the file or folder" };
			k[n++] = (struct help_key){ "#1", "Options: open with, rename, copy\xe2\x80\xa6" };
		}
		k[n++] = (struct help_key){ "#2", "Go to your home folder, /root" };
		if (!drives)
			k[n++] = (struct help_key){ "#3", "Go to Drives" };
	}
	k[n++] = (struct help_key){ "#4", "Log of copy, move and delete" };
	k[n++] = (struct help_key){ "#5", "Settings" };
	/* the folder's keys fill the pop-up: this key goes there */
	if (n < 9)
		k[n++] = (struct help_key){ "#6", "This help" };
	k[n++] = (struct help_key){ "Esc", f->under != SC_LIST ? "Back" :
					  drives || !f->b.dir[1] ? "Close Files" : "Up a folder" };
	k[n++] = (struct help_key){ "Logo", "Home screen" };
	help_popup(title, about, k, n);
}

/* ---- frames ----------------------------------------------------------------- */

static int popup(const struct files *f)
{
	return f->sc == SC_PICK || f->sc == SC_INPUT || f->sc == SC_CONFIRM || f->sc == SC_HELP;
}

static void draw(const struct files *f)
{
	enum screen under = f->sc == SC_HELP ? f->under : f->sc;

	if (under == SC_LOG)
		draw_log(f);
	else if (br_drives(&f->b) || under == SC_SETTINGS)
		draw_list(f);
	else
		draw_folder(f);
	switch (f->sc) {
	case SC_PICK:
		draw_pick(f);
		break;
	case SC_INPUT:
		draw_input(f);
		break;
	case SC_CONFIRM:
		draw_confirm(f);
		break;
	case SC_HELP:
		draw_help(f);
		break;
	default:
		break;
	}
}

static uint16_t band[W * BAND_H];
static uint16_t *whole;		/* a whole frame, while a pop-up is open */
static long tm_out;		/* us spent writing bands out, for timing() */

static void bands(void (*paint)(const void *), const void *arg,
		  void (*out)(const uint16_t *band, int y0, int y1))
{
	band_first = 1;
	for (int y0 = 0; y0 < H; y0 += BAND_H) {
		int y1 = y0 + BAND_H < H ? y0 + BAND_H : H;

		/* row 0, outside the band (UBSan: not as an index) */
		fb = (uint16_t *)((uintptr_t)band - (uintptr_t)y0 * W * sizeof(*band));
		fb_y0 = y0;
		fb_y1 = y1;
		paint(arg);
		out(band, y0, y1);
	}
}

static void (*out_now)(const uint16_t *band, int y0, int y1);

static void paint_rec(void *f)
{
	draw(f);
}

static void out_timed(const uint16_t *b, int y0, int y1)
{
	long t = wb_now_us();

	out_now(b, y0, y1);
	tm_out += wb_now_us() - t;
}

/* FILES_UI_TIMING=1: a line per frame on stderr (the app's log) */
static void timing(long t0, int whole_frame)
{
	static int on = -1;
	char line[96];

	if (on < 0)
		on = getenv("FILES_UI_TIMING") != 0;
	if (on) {
		int n = snprintf(line, sizeof(line), "files: frame %ld us (%s), scene %ld, glass %ld, out %ld\n",
				 wb_now_us() - t0, whole_frame ? "whole" : "bands", tm_scene, tm_glass, tm_out);

		if (write(2, line, (size_t)n) < 0)
			on = 0;
	}
	tm_scene = tm_glass = tm_out = 0;
}

void ui_present(const struct files *f, void (*out)(const uint16_t *band, int y0, int y1))
{
	long t0 = wb_now_us();

	out_now = out;
	wb_init();
	layout(f);
	/* a pop-up blurs what is under it: the frame drawn whole (1 MB, only
	 * while the pop-up is open), else in bands, where libwb leaves the
	 * pop-up's glass unblurred */
	if (popup(f) && !whole) {
		whole = malloc((size_t)W * H * sizeof(*whole));
		need_full = 1;		/* nothing in it yet */
	}
#ifdef FILES_UI_HOST
	if (whole && getenv("FILES_BANDS_ONLY")) {	/* the tests: as if there were no room */
		free(whole);
		whole = 0;
	}
#endif
	else if (!popup(f) && whole) {
		free(whole);
		whole = 0;
	}
	if (need_full)
		wb_rec_invalidate();
	if (whole) {
		/* the pop-up's blur kept while only its focus moves */
		band_first = 1;
		wb_frame(paint_rec, (void *)f, whole, BAND_H, out_timed);
		need_full = 0;
		timing(t0, 1);
		return;
	}
	band_first = 1;
	/* recorded, then only the bands that changed: a focus that moves
	 * draws its two rows and the hint bar (libwb wb_bands) */
	wb_bands(paint_rec, (void *)f, band, BAND_H, out_timed);
	need_full = 0;
	timing(t0, 0);
}

static const char *msg_eb, *msg_text;

/* while a copy, move or delete runs: a card on the scene */
static void paint_message(const void *unused)
{
	int w = text_w(&f_ui28b, msg_text, 0), cw = W - 2 * POP_X, ch = 32 + 18 + 6 + 36 + 32, y = (H - ch) / 2;

	(void)unused;
	files_scene();
	status_bar();
	wb_card(POP_X, y, cw, ch, WB_RADIUS_LG, 0, 0);
	eyebrow((W - text_w(&f_mono14, msg_eb, 22)) / 2, y + 32, msg_eb);
	text(&f_ui28b, (W - w) / 2, y + 32 + 18 + 6 + 2, msg_text, WB_COLOR_TEXT_HI, 0);
}

void ui_message(const char *eyebrow_s, const char *msg, void (*out)(const uint16_t *band, int y0, int y1))
{
	msg_eb = eyebrow_s;
	msg_text = msg;
	need_full = 1;
	wb_rec_invalidate();
	wb_init();
	bands(paint_message, 0, out);
}
