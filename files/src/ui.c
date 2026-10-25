/* SPDX-License-Identifier: MIT */
/*
 * Files: the screens (ui.h), drawn as the menu drew Files (why2025-linux
 * home.c: draw_list, draw_row, dialog, draw_input, the log and help), with
 * its drawing code. Every band draws the whole screen, clipped to the band.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W	WB_SCREEN_W
#define H	WB_SCREEN_H
#include "wbdraw.h"
#include "lora_state.h"
#include "wbstatus.h"

#define LOG_TOP		(WB_STATUS_BAR_H + 20 + 18 + 6 + 44 + 24)
#define LOG_ROW		24
_Static_assert(LOG_ROWS == (H - WB_HINT_BAR_H - 8 - LOG_TOP) / LOG_ROW, "app.h LOG_ROWS");

#define DLG_X	72
#define DLG_W	(W - 2 * DLG_X)

struct ui_status ui_st;

int ui_wifi_bars(void)
{
	return wifi_bars();
}

int ui_lora_on(void)
{
	return lora_on();
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

static int focusable(const struct row *r)
{
	return r->kind == R_FOLDER || r->kind == R_FILE || r->kind == R_DRIVE || r->kind == R_TOGGLE;
}

/* files and folders 56 px (compact), drives 104 */
static int row_h(const struct row *r)
{
	return r->kind == R_GROUP ? 17 : r->kind == R_DRIVE ? 104 :
	       r->kind == R_FOLDER || r->kind == R_FILE ? 56 : 68;
}

static void toggle(int x, int cy, int on)
{
	int y = cy - 16;

	box(x, y, 56, 32, 16, on ? WB_COLOR_PINK : WB_COLOR_LINE, 255, 0, 0, 0);
	box(on ? x + 56 - 3 - 26 : x + 3, y + 3, 26, 26, 13,
	    on ? WB_COLOR_VOID : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
}

static void draw_row(const struct files *f, const struct row *r, int y, int on)
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, h = row_h(r);
	int top = r->kind == R_DRIVE ? y + 18 + 16 : y + h / 2;
	const struct wb_icon *ic = icon_of(r->ic);
	int rx = x + w - 20, lx = x + 20 + (ic ? 26 + 16 : 0);
	char buf[80];

	if (r->kind == R_GROUP) {
		text(&f_mono13, x, y, r->label, WB_COLOR_TEXT_LOW, 21);
		return;
	}
	if (on)
		box_focus(x, y, w, h, WB_RADIUS_MD, &GLOW_ROW);
	else
		box_plain(x, y, w, h, WB_RADIUS_MD, WB_COLOR_SURFACE1);
	if (ic)
		icon(ic, x + 20, top - 13, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);

	/* right-hand side first, so the label knows how much room it has */
	switch (r->kind) {
	case R_FOLDER:
		icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 20 + 16;
		/* fall through */
	case R_INFO:
	case R_FILE:
		if (r->value[0]) {
			rx -= text_w(&f_ui18, r->value, 0);
			text(&f_ui18, rx, text_top(&f_ui18, top), r->value, WB_COLOR_TEXT_MID, 0);
			rx -= 16;
		}
		break;
	case R_TOGGLE:
		toggle(rx - 56, top, r->on);
		rx -= 56 + 16;
		break;
	case R_DRIVE: {		/* name, path and file system, free space, usage */
		const struct fsinfo *fs = &f->d.fs[r->idx];
		int tx = x + 24, tw = w - 48, ty = y + 104 - 18 - 8;
		int pct = fs->size ? (int)(fs->used * 100 / fs->size) : 0;
		int full = pct >= 90 && strcmp(fs->type, "squashfs");
		char v[48];

		icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 20 + 16;
		if (!fs->mnt[0])
			snprintf(v, sizeof(v), "Not mounted");
		else if (!strcmp(fs->type, "squashfs"))
			snprintf(v, sizeof(v), "Read-only");
		else
			strcat(human(v, fs->size > fs->used ? fs->size - fs->used : 0), " free");
		rx -= text_w(&f_ui18, v, 0);
		text(&f_ui18, rx, text_top(&f_ui18, top), v, full ? WB_COLOR_WARN : WB_COLOR_TEXT_MID, 0);
		rx -= 16;
		snprintf(v, sizeof(v), "%s  %s", fs->mnt[0] ? fs->mnt : fs->dev, fs->type);
		text(&f_mono13, lx, top + 16, v, WB_COLOR_TEXT_LOW, 0);
		box(tx, ty, tw, 8, 4, WB_COLOR_LINE, 255, 0, 0, 0);
		if (fs->mnt[0] && pct)
			box(tx, ty, tw * pct / 100 < 8 ? 8 : tw * pct / 100, 8, 4,
			    full ? WB_COLOR_WARN : on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
		break;
	}
	default:
		break;
	}
	text(&f_ui22, lx, text_top(&f_ui22, top), wb_fit(&f_ui22, r->label, rx - lx, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, 0);
}

/* the hint bar (6.8): only what differs per screen */
static void list_hints(const struct files *f)
{
	const struct list *l = &f->l;
	const struct row *r = l->focus >= 0 ? &l->rows[l->focus] : 0;
	int drives = br_drives(&f->b);
	struct hint h[6];
	int n = 0;

	if (f->sc == SC_SETTINGS) {
		h[n++] = (struct hint){ "Enter", "switch", 0 };
		h[n++] = (struct hint){ "Esc", "back", 1 };
		hint_bar(h, n);
		return;
	}
	if (r)
		h[n++] = (struct hint){ "Enter", "open", 0 };
	if (!drives)
		h[n++] = (struct hint){ "#1", "options", 0 };
	h[n++] = (struct hint){ "#2", "home", 0 };
	if (!drives)
		h[n++] = (struct hint){ "#3", "drives", 0 };
	h[n++] = (struct hint){ "Esc", !drives && f->b.dir[1] ? "up" : "close", 1 };
	hint_bar(h, n);
}

static void draw_list(const struct files *f)
{
	const struct list *l = &f->l;
	int ys[LIST_MAX], hs[LIST_MAX], y = 0, top, bottom = H - WB_HINT_BAR_H - 8, scroll, cut, show;
	int focus = l->focus;
	char buf[96];

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	top = WB_STATUS_BAR_H + 20;
	eyebrow(WB_CONTENT_PAD, top, "FILES");
	{
		int rw = l->title_r[0] ? text_w(&f_ui20, l->title_r, 0) : 0;

		if (rw)
			text(&f_ui20, W - WB_CONTENT_PAD - rw, top + 18 + 6 + 12, l->title_r, WB_COLOR_TEXT_MID, 0);
		text(&f_ui36b, WB_CONTENT_PAD, top + 18 + 6,
		     wb_fit(&f_ui36b, l->title ? l->title : "", W - 2 * WB_CONTENT_PAD - (rw ? rw + 24 : 0),
			    buf, sizeof(buf)),
		     WB_COLOR_TEXT_HI, -11);
	}
	top += 18 + 6 + 44;
	if (l->path[0]) {	/* the full path in mono, the clipboard after it */
		int tw = l->tag[0] ? text_w(&f_ui16, l->tag, 0) + 24 : 0;
		int x = text(&f_mono14, WB_CONTENT_PAD, top + 6,
			     wb_fit(&f_mono14, l->path, W - 2 * WB_CONTENT_PAD - (tw ? tw + 16 : 0), buf, sizeof(buf)),
			     WB_COLOR_TEXT_LOW, 0);

		if (tw) {
			box(x + 16, top, tw, 28, 14, WB_COLOR_PINK, 36, WB_COLOR_PINK, 1, 0);
			text(&f_ui16, x + 16 + 12, text_top(&f_ui16, top + 14), l->tag, WB_COLOR_PINK, 0);
		}
		top += 28;
	}
	if (l->notice) {
		text(&f_ui18, WB_CONTENT_PAD, top + 8, wb_fit(&f_ui18, l->notice, W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     l->notice_bad ? WB_COLOR_ERROR : WB_COLOR_OK, 0);
		top += 8 + 24;
	}
	top += 24;

	/* lay out, then scroll so the focused row is fully visible */
	for (int i = 0; i < l->n; i++) {
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
	/* cards cut off at either end show only their part inside the list */
	clip_rows(scroll ? top : 0, l->n && y - 10 - scroll > bottom - top ? top + cut : H);
	for (int i = 0; i < l->n; i++) {
		int ry = top + ys[i] - scroll;

		if (ry + hs[i] <= top && scroll)
			continue;
		if (ry >= top + cut)
			break;
		draw_row(f, &l->rows[i], ry, i == focus);
	}
	clip_end();
	/* a narrow scroll bar right of a list that does not fit */
	if (l->sb_total)
		scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, l->sb_total + 10, bottom - top,
			  l->sb_first + scroll);
	else
		scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, y, bottom - top, scroll);
	list_hints(f);
}

/* the design's dialog over what was drawn: backdrop, eyebrow, title, room
 * for a body of body_h, then option rows; the body's y */
static int dialog(const char *eb, const char *title, int body_h, const char *const *labels,
		  const struct wb_icon *const *ics, int n, int focus, int danger)
{
	int gap = n && body_h ? 24 : 0;
	int h = 32 + 18 + 6 + 36 + 24 + body_h + gap + (n ? n * 68 + (n - 1) * 12 : 0) + 32;
	int y = (H - h) / 2, body = y + 32 + 18 + 6 + 36 + 24, by = body + body_h + gap;
	char tbuf[96];

	dim(WB_COLOR_CONSOLE, 184);	/* rgba(6,6,13,0.72) */
	box_plain(DLG_X, y, DLG_W, h, WB_RADIUS_LG, WB_COLOR_SURFACE2);
	eyebrow(DLG_X + 32, y + 32, eb);
	text(&f_ui28b, DLG_X + 32, y + 32 + 18 + 6 + 2, wb_fit(&f_ui28b, title, DLG_W - 64, tbuf, sizeof(tbuf)),
	     WB_COLOR_TEXT_HI, 0);
	for (int i = 0; i < n; i++, by += 68 + 12) {
		int on = i == focus;

		if (on)
			box_focus(DLG_X + 32, by, DLG_W - 64, 68, WB_RADIUS_MD, &GLOW_ROW);
		else
			box_plain(DLG_X + 32, by, DLG_W - 64, 68, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		/* the destructive choice in red (6.5: Dialog) */
		icon(ics[i], DLG_X + 32 + 20, by + (68 - 26) / 2,
		     i == danger ? WB_COLOR_ERROR : on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		text(&f_ui22, DLG_X + 32 + 20 + 26 + 16, text_top(&f_ui22, by + 34), labels[i],
		     i == danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
	}
	return body;
}

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
	box_focus(fx, fy, fw, 56, WB_RADIUS_MD, &GLOW_ROW);
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
	text(&f_mono18, tx, text_top(&f_mono18, fy + 28), shown + e, WB_COLOR_TEXT_LOW, 0);
	fill(fx + 20 + cx + 1, fy + 16, 2, 24, WB_COLOR_CYAN);
	if (in->err)
		text(&f_ui18, fx, fy + 56 + 10, in->err, WB_COLOR_ERROR, 0);
	hint_bar(hints, 2);
}

static void draw_pick(const struct files *f)
{
	static const struct hint hints[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
	static struct menu_item it[PICK_MAX];
	const struct pick *p = &f->p;

	for (int i = 0; i < p->n; i++) {
		memset(&it[i], 0, sizeof(it[i]));
		it[i].label = p->label[i];
		it[i].sub = p->sub[i];
		it[i].sep = p->sep[i];
		it[i].danger = p->danger[i];
	}
	menu_popup("FILES", p->title, 0, it, 0, p->n, p->focus, -1, 0, 0, hints, 2);
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
	hint_bar(hints, 3);
}

static void draw_log(const struct files *f)
{
	static const struct hint hints[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "scroll", 0 }, { "Esc", "back", 1 },
	};
	char buf[120];

	if (!f->log_n) {
		empty_screen("FILES", "Copy, move and delete", "No log yet.");
		status_bar();
		return;
	}
	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	eyebrow(WB_CONTENT_PAD, WB_STATUS_BAR_H + 20, "FILES");
	text(&f_ui36b, WB_CONTENT_PAD, WB_STATUS_BAR_H + 20 + 18 + 6, "Copy, move and delete", WB_COLOR_TEXT_HI, -11);
	for (int i = 0; i < LOG_ROWS && f->log_top + i < f->log_n; i++)
		text(&f_mono14, WB_CONTENT_PAD, LOG_TOP + i * LOG_ROW,
		     wb_fit(&f_mono14, f->log_line[f->log_top + i], W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
	scrollbar(W - WB_CONTENT_PAD + 18, LOG_TOP, LOG_ROWS * LOG_ROW, f->log_n, LOG_ROWS, f->log_top);
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
	const char *about, *title = f->l.title ? f->l.title : "Files";

	if (f->under == SC_LOG) {
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Scroll" };
		title = "Copy, move and delete";
		about = "The end of the log, newest at the bottom.";
	} else if (f->under == SC_SETTINGS) {
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Choose a row" };
		k[n++] = (struct help_key){ "Enter", "Switch it" };
		about = "How Files shows folders.";
	} else {
		about = drives ? "All storage: the SD card, USB drives and the badge's own. "
				 "Enter mounts a drive when needed and opens it." :
				 "Every file on the badge, as it is. Enter opens a folder, or a file "
				 "with the app that opens it.";
		k[n++] = (struct help_key){ "\xe2\x86\x91\xe2\x86\x93", "Choose a row; hold to scroll" };
		k[n++] = (struct help_key){ "Enter", "Open" };
		if (!drives) {
			k[n++] = (struct help_key){ "\xe2\x86\x90\xe2\x86\x92", "A page up or down" };
			k[n++] = (struct help_key){ "#1", "Options: open with, rename, copy, delete\xe2\x80\xa6" };
		}
		k[n++] = (struct help_key){ "#2", "Go to your home folder, /root" };
		if (!drives)
			k[n++] = (struct help_key){ "#3", "Go to Drives" };
	}
	k[n++] = (struct help_key){ "#4", "Log of copy, move and delete" };
	k[n++] = (struct help_key){ "#5", "Settings" };
	k[n++] = (struct help_key){ "#6", "This help" };
	k[n++] = (struct help_key){ "Esc", f->under != SC_LIST ? "Back" :
					  drives || !f->b.dir[1] ? "Close Files" : "Up a folder" };
	k[n++] = (struct help_key){ "Logo", "Home screen" };
	help_popup(title, about, k, n);
}

static void draw(const struct files *f)
{
	enum screen under = f->sc == SC_HELP ? f->under : f->sc;

	if (under == SC_LOG)
		draw_log(f);
	else
		draw_list(f);
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

static void bands(void (*paint)(const void *), const void *arg,
		  void (*out)(const uint16_t *band, int y0, int y1))
{
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

static void paint_files(const void *f)
{
	draw(f);
}

void ui_present(const struct files *f, void (*out)(const uint16_t *band, int y0, int y1))
{
	bands(paint_files, f, out);
}

static const char *msg_eb, *msg_text;

static void paint_message(const void *unused)
{
	int w = text_w(&f_ui28b, msg_text, 0);

	(void)unused;
	fill(0, 0, W, H, WB_COLOR_VOID);
	eyebrow((W - text_w(&f_mono14, msg_eb, 22)) / 2, H / 2 - 40, msg_eb);
	text(&f_ui28b, (W - w) / 2, H / 2 - 12, msg_text, WB_COLOR_TEXT_HI, 0);
}

void ui_message(const char *eyebrow_s, const char *msg, void (*out)(const uint16_t *band, int y0, int y1))
{
	msg_eb = eyebrow_s;
	msg_text = msg;
	bands(paint_message, 0, out);
}
