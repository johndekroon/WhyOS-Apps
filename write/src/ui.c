/* SPDX-License-Identifier: MIT */
/*
 * Write: the screens (design/write/canvas), drawn with the menu's code from
 * why2025-linux (wbdraw.h, wbstatus.h, home_assets.h) and Write's own fonts
 * for the document (write_fonts.h: Latin-1, 4-bit alpha, italic slanted).
 *
 *   editor      status bar, document bar (name, edited dot, what is going
 *               on, the caret's style), the text, word count, hint bar
 *   text        rich (markdown with its styles), markdown source with line
 *               numbers, code with line numbers and highlighting
 *   find bar    under the document bar (over the text in full screen)
 *   pop-ups     Options, Style, Discard, Save as, Replace?, Language,
 *               Settings, the find menu, errors; Help and an empty log are
 *               wbdraw's help_popup and empty_screen
 *   Open        a screen of its own, as Files' lists
 *   full screen the text alone, a footer line, a hint for three seconds
 *
 * Bands: every frame works out, per band of BAND_H rows, a hash of what is
 * drawn there (the rows of text with their selection, matches and caret,
 * the bars, the counter); a band whose hash did not change is not drawn
 * again, so typing redraws a line's bands and not the screen. Pop-ups and
 * Open redraw everything.
 */
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
#include "wbstatus.h"
#include "write_fonts.h"

struct ui_status ui_st = { "14:32", 1, -1, 0, 1 };

extern int (*app_adv16)(void *ctx, int font, unsigned cp);

static const struct wf_font *const FONTS[FONT_COUNT] = {
	[FONT_UI24] = &wf_ui24, [FONT_UI24B] = &wf_ui24b, [FONT_UI36B] = &wf_ui36b,
	[FONT_UI28B] = &wf_ui28b, [FONT_UI20] = &wf_ui20, [FONT_UI20B] = &wf_ui20b,
	[FONT_MONO20] = &wf_mono20, [FONT_MONO18] = &wf_mono18, [FONT_MONO16] = &wf_mono16,
	[FONT_MONO14] = &wf_mono14,
};

#define PINK_TINT	31	/* rgba(242,94,149,0.12) */
#define SEL_A		41	/* cyan at 16 % */
#define MATCH_A		41
#define MATCH_CUR_A	82

/* ---- Write's fonts --------------------------------------------------------------- */

static const struct wf_glyph *wglyph(const struct wf_font *f, unsigned cp)
{
	int lo = 0x7f - 0x20, hi = f->n - 1;

	if (cp >= 0x20 && cp <= 0x7e)
		return &f->g[cp - 0x20];
	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (f->g[mid].cp == cp)
			return &f->g[mid];
		if (f->g[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return &f->g['?' - 0x20];
}

static int ui_adv16(void *ctx, int font, unsigned cp)
{
	(void)ctx;
	return wglyph(FONTS[font], cp)->adv16;
}

/* one glyph with its line's top at y; slant: italic, bold: struck twice */
static void wglyph_draw(const struct wf_font *f, const struct wf_glyph *g, int x16, int y, uint32_t c,
			int slant, int bold)
{
	int gx = (x16 + 8) / 16 + g->x, gy = y + g->y, stride = (g->w + 1) / 2;
	int j0 = fb_y0 - gy > 0 ? fb_y0 - gy : 0, j1 = fb_y1 - gy < g->h ? fb_y1 - gy : g->h;
	const uint8_t *bits = f->bits + g->off;

	for (int j = j0; j < j1; j++) {
		int sh = slant ? (f->ascent - (g->y + j)) / 5 : 0;
		const uint8_t *row = bits + j * stride;

		for (int i = 0; i < g->w; i++) {
			unsigned a = (unsigned)((i & 1 ? row[i >> 1] & 15 : row[i >> 1] >> 4) * 17);

			if (!a)
				continue;
			put(gx + i + sh, gy + j, c, a);
			if (bold)
				put(gx + i + sh + 1, gy + j, c, a);
		}
	}
}

static unsigned utf8_next(const char *s, int n, int *k)
{
	const unsigned char *p = (const unsigned char *)s + *k;
	unsigned cp = p[0];
	int len = 1;

	if (cp >= 0xf0 && *k + 4 <= n) {
		cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
		len = 4;
	} else if (cp >= 0xe0 && *k + 3 <= n) {
		cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
		len = 3;
	} else if (cp >= 0xc0 && *k + 2 <= n) {
		cp = ((cp & 31) << 6) | (p[1] & 63);
		len = 2;
	}
	*k += len;
	return cp;
}

/* a string in one of Write's fonts; returns the end x */
static int wtext(const struct wf_font *f, int x, int y, const char *s, uint32_t c, int slant)
{
	int x16 = x * 16, n = (int)strlen(s), k = 0;

	while (k < n) {
		const struct wf_glyph *g = wglyph(f, utf8_next(s, n, &k));

		wglyph_draw(f, g, x16, y, c, slant, 0);
		x16 += g->adv16;
	}
	return (x16 + 8) / 16;
}

static int wtext_w(const struct wf_font *f, const char *s)
{
	int x16 = 0, n = (int)strlen(s), k = 0;

	while (k < n)
		x16 += wglyph(f, utf8_next(s, n, &k))->adv16;
	return (x16 + 8) / 16;
}

static int wtop(const struct wf_font *f, int cy)
{
	return cy - (f->ascent + f->descent) / 2;
}

static void wicon(const struct wf_icon *ic, int x, int y, uint32_t c)
{
	int j0 = fb_y0 - y > 0 ? fb_y0 - y : 0, j1 = fb_y1 - y < ic->h ? fb_y1 - y : ic->h;

	for (int j = j0; j < j1; j++)
		for (int i = 0; i < ic->w; i++)
			put(x + i, y + j, c, ic->a[j * ic->w + i]);
}

/* s cut to w px in font f, with an ellipsis */
static const char *wfit(const struct wf_font *f, const char *s, int w, char *buf, int size)
{
	int n;

	if (wtext_w(f, s) <= w)
		return s;
	n = (int)strlen(s);
	if (n > size - 4)
		n = size - 4;
	while (n > 0) {
		memcpy(buf, s, (size_t)n);
		memcpy(buf + n, "\xe2\x80\xa6", 4);
		if (wtext_w(f, buf) <= w)
			return buf;
		n--;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	return "";
}

/* ---- bars ------------------------------------------------------------------------- */

static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3), text_top(n, cy), "OS",
	     WB_COLOR_CYAN, -3);
	status_right(ui_st.hhmm, ui_st.clock_ok, ui_st.bars, ui_st.lora);
}

static int keycap_any(int x, int cy, const char *keys, uint32_t c)
{
	if (one_sym(keys) || c == WB_COLOR_TEXT_HI)
		return keycap(x, cy, keys);
	{
		int w = text_w(&f_mono13, keys, 0) + 18;

		box(x, cy - 11, w, 23, 6, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK, 1, 0);
		text(&f_mono13, x + 9, text_top(&f_mono13, cy), keys, c, 0);
		return x + w;
	}
}

static int hint_wk(const struct hintk *h)
{
	return keycap_w(h->keys) + 18 + (h->label[0] ? 8 + text_w(&f_ui16, h->label, 0) : 0);
}

static void hints(const struct app *a)
{
	struct hintk h[12];
	int n = app_hints(a, h, 12), x = WB_CHROME_PAD, rx = W - WB_CHROME_PAD;
	int y0 = H - WB_HINT_BAR_H, cy = y0 + WB_HINT_BAR_H / 2;

	fill(0, y0, W, WB_HINT_BAR_H, WB_COLOR_SURFACE1);
	fill(0, y0, W, 1, WB_COLOR_LINE);
	for (int i = n - 1; i >= 0; i--) {	/* the right ones from the right: Esc, then Fn */
		int hx, ex;

		if (!h[i].right)
			continue;
		hx = rx - hint_wk(&h[i]);
		ex = keycap_any(hx, cy, h[i].keys, h[i].pink ? WB_COLOR_PINK : WB_COLOR_TEXT_HI);
		if (h[i].label[0])
			text(&f_ui16, ex + 8, text_top(&f_ui16, cy), h[i].label, WB_COLOR_TEXT_MID, 0);
		rx = hx - 20;
	}
	for (int i = 0; i < n; i++) {
		int ex;

		if (h[i].right)
			continue;
		ex = keycap(x, cy, h[i].keys);
		ex = text(&f_ui16, ex + 8, text_top(&f_ui16, cy), h[i].label, WB_COLOR_TEXT_MID, 0);
		x = ex + 20;
	}
}

static int chip(int x, int cy, int w, uint32_t bg, unsigned bga, uint32_t border)
{
	box(x, cy - 14, w, 28, 6, bg, bga, border, 1, 0);
	return x + w;
}

/* B, I, U and Rich/MD on the document bar's right, right to left */
static void docbar_right(struct app *a, const struct docbar *d, int cy)
{
	int x = W - WB_CHROME_PAD;

	if (d->lang) {	/* code: the language and the indent */
		int iw = text_w(&f_mono13, d->indent, 0) + 20, lw = 14 + 6 + text_w(&f_ui16, d->lang, 0) + 20;

		x -= iw;
		chip(x, cy, iw, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE);
		text(&f_mono13, x + 10, text_top(&f_mono13, cy), d->indent, WB_COLOR_TEXT_MID, 0);
		x -= 6 + lw;
		chip(x, cy, lw, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE);
		wicon(&wi_code14, x + 10, cy - 7, WB_COLOR_TEXT_MID);
		text(&f_ui16, x + 10 + 14 + 6, text_top(&f_ui16, cy), d->lang, WB_COLOR_TEXT_HI, 0);
		return;
	}
	{
		int rw = text_w(&f_mono13, "Rich", 0) + 16, mw = text_w(&f_mono13, "MD", 0) + 16;

		x -= rw + mw;
		box(x, cy - 13, rw + mw, 26, 6, 0, 0, WB_COLOR_LINE, 1, 0);
		fill(d->md_view ? x + rw : x + 1, cy - 12, d->md_view ? mw - 1 : rw - 1, 24, WB_COLOR_SURFACE2);
		text(&f_mono13, x + 8, text_top(&f_mono13, cy), "Rich", d->md_view ? WB_COLOR_TEXT_LOW : WB_COLOR_TEXT_HI, 0);
		text(&f_mono13, x + rw + 8, text_top(&f_mono13, cy), "MD", d->md_view ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_LOW, 0);
		x -= 4;
	}
	for (int i = 2; i >= 0; i--) {
		int on = d->style & (i == 0 ? ST_B : i == 1 ? ST_I : ST_U);
		uint32_t c = on ? WB_COLOR_PINK : WB_COLOR_TEXT_MID;

		x -= 28 + 6;
		if (on)
			chip(x, cy, 28, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK);
		else
			chip(x, cy, 28, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE);
		if (i == 0)
			text(&f_ui18b, x + 14 - text_w(&f_ui18b, "B", 0) / 2, text_top(&f_ui18b, cy), "B", c, 0);
		else if (i == 1)
			wtext(&wf_ui20, x + 12 - wtext_w(&wf_ui20, "I") / 2, wtop(&wf_ui20, cy), "I", c, 1);
		else {
			int tw = text_w(&f_ui16, "U", 0);

			text(&f_ui16, x + 14 - tw / 2, text_top(&f_ui16, cy), "U", c, 0);
			fill(x + 14 - tw / 2, cy + 8, tw, 1, c);
		}
	}
	{
		int pw = text_w(&f_ui16, d->para, 0) + 20;

		x -= 6 + pw;
		chip(x, cy, pw, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE);
		text(&f_ui16, x + 10, text_top(&f_ui16, cy), d->para, WB_COLOR_TEXT_HI, 0);
	}
	(void)a;
}

static void docbar(struct app *a)
{
	struct docbar d;
	int cy = WB_STATUS_BAR_H + 22, x = WB_CHROME_PAD, maxw;
	char buf[160];

	app_docbar(a, &d);
	fill(0, WB_STATUS_BAR_H + 43, W, 1, WB_COLOR_SURFACE2);
	maxw = a->find_open ? W - 2 * WB_CHROME_PAD : 240;
	x = text(&f_ui18b, x, text_top(&f_ui18b, cy), wb_fit(&f_ui18b, d.name, maxw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	if (d.edited) {
		box(x + 10, cy - 3, 7, 7, 3, WB_COLOR_YELLOW, 255, 0, 0, 0);
		x += 17;
	}
	if (d.info[0] && !a->find_open) {
		x += 10;
		if (d.info_kind == 2) {
			int w = 8 + 12 + 6 + text_w(&f_mono13, d.info, 0) + 8;

			box(x, cy - 11, w, 22, 11, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK, 1, 0);
			wicon(&wi_copy, x + 8, cy - 6, WB_COLOR_PINK);
			text(&f_mono13, x + 8 + 12 + 6, text_top(&f_mono13, cy), d.info, WB_COLOR_PINK, 0);
		} else {
			uint32_t c = d.info_kind == 1 ? WB_COLOR_CYAN : d.info_kind == 3 ? WB_COLOR_ERROR : WB_COLOR_TEXT_LOW;

			text(&f_mono13, x, text_top(&f_mono13, cy), d.info, c, 0);
		}
	}
	if (!a->find_open)
		docbar_right(a, &d, cy);
}

/* ---- the text -------------------------------------------------------------------- */

static const uint32_t SYN_COLORS[] = {
	[C_TEXT] = WB_COLOR_TEXT_HI, [C_KEYWORD] = WB_COLOR_CYAN, [C_STRING] = WB_COLOR_YELLOW,
	[C_NUMBER] = WB_COLOR_PINK, [C_COMMENT] = WB_COLOR_TEXT_LOW, [C_FUNC] = WB_COLOR_OK,
	[C_PUNCT] = WB_COLOR_TEXT_MID,
};

/* the rows on screen this frame */
struct frow { int line, state, sy; struct row r; int first, last; };

#define FROWS_MAX	160
static struct frow FR[FROWS_MAX];
static int n_fr;
static int TX, TY, TW, TH;	/* the text area */

static unsigned char *cls;	/* per byte of the line being drawn: syntax or markdown class */
static int cls_cap;

static int ensure_cls(int n)
{
	if (n + 1 > cls_cap) {
		unsigned char *c = realloc(cls, (size_t)n + 64);

		if (!c)
			return -1;
		cls = c;
		cls_cap = n + 64;
	}
	return 0;
}

static void frame_rows(struct app *a)
{
	static struct row rows[ROWS_MAX];
	int line = lay_line_at(&a->lay, a->scroll);

	n_fr = 0;
	for (; line < a->e.t.n && a->lay.ys[line] < a->scroll + TH && n_fr < FROWS_MAX; line++) {
		int h, nr = lay_rows(&a->lay, &a->e, line, lay_state_in(&a->e, line), rows, ROWS_MAX, &h);

		for (int i = 0; i < nr && n_fr < FROWS_MAX; i++) {
			int sy = TY + a->lay.ys[line] - a->scroll + rows[i].y;

			if (sy + rows[i].h <= TY - 40 || sy >= TY + TH + 40)
				continue;
			FR[n_fr++] = (struct frow){ line, lay_state_in(&a->e, line), sy, rows[i], i == 0, i == nr - 1 };
		}
	}
}

/* the selection on line, in its visible offsets: k0..k1, and past its end (nl) */
static int sel_on(struct app *a, int line, int n, int *k0, int *k1, int *nl)
{
	struct pos s0, s1;

	if (!a->e.sel)
		return 0;
	ed_sel_range(&a->e, &s0, &s1);
	if (line < s0.line || line > s1.line)
		return 0;
	*k0 = line == s0.line ? (a->e.mode == M_RICH ? md_vis(&a->e.m, s0.col) : s0.col) : 0;
	*k1 = line == s1.line ? (a->e.mode == M_RICH ? md_vis(&a->e.m, s1.col) : s1.col) : n;
	*nl = line < s1.line;
	return 1;
}

/* the colours and styles of each byte in the markdown source view */
static void source_classes(struct app *a, int line, int state, const char *s, int n)
{
	struct mdline *m = &a->e.m2;

	memset(cls, 0, (size_t)n);
	if (md_parse(m, s, n, state & 1))
		return;
	if (m->kind == B_CODE || m->kind == B_FENCE) {
		memset(cls, m->kind == B_FENCE ? 1 : 0, (size_t)n);
		return;
	}
	memset(cls, 1, (size_t)n);	/* markers: text-low */
	for (int i = 0; i < m->prefix && i < n; i++)
		cls[i] = 2;		/* the block marker: cyan */
	for (int i = 0; i < m->n; i++) {
		int b = m->src[i], st = m->st[i] & 7;

		if (m->kind >= B_H1 && m->kind <= B_H3)
			st |= ST_B;
		cls[b] = (unsigned char)(8 | st << 4);
		if (m->w[i] == 2 && b + 1 < n)
			cls[b + 1] = (unsigned char)(8 | st << 4);
	}
	(void)line;
}

static void caret_draw(int x, int y, int h)
{
	fill(x - 1, y, 2, h, WB_COLOR_CYAN);
	for (int j = y; j < y + h; j++) {
		put(x - 3, j, WB_COLOR_CYAN, 40);
		put(x - 2, j, WB_COLOR_CYAN, 80);
		put(x + 1, j, WB_COLOR_CYAN, 80);
		put(x + 2, j, WB_COLOR_CYAN, 40);
	}
}

static void row_draw(struct app *a, const struct frow *fr)
{
	const char *s;
	const unsigned char *st;
	enum blk kind;
	struct lstyle ls;
	const struct wf_font *base;
	int n, x16, k, top, cur_line = fr->line == a->e.caret.line, sk0 = 0, sk1 = 0, snl = 0, have_sel;
	int caret_k = -1, ty, lh = fr->r.h;
	uint32_t tc = WB_COLOR_TEXT_HI;

	s = lay_text(&a->lay, &a->e, fr->line, fr->state, &n, &kind, &st);
	/* the caret and the selection in this line's offsets, while e->m is this line */
	if (cur_line)
		caret_k = a->e.mode == M_RICH ? md_vis(&a->e.m, a->e.caret.col) : a->e.caret.col;
	have_sel = sel_on(a, fr->line, n, &sk0, &sk1, &snl);
	lay_style(&a->lay, kind, fr->state & 1, &ls);
	base = FONTS[ls.font];
	ty = fr->sy + (lh - (base->ascent + base->descent)) / 2;
	top = fr->sy;
	if (ensure_cls(n))
		return;
	/* copied: lay_x and the source classes parse again */
	{
		static char *vs;
		static unsigned char *vst;
		static int vcap;

		if (n + 1 > vcap) {
			char *v = realloc(vs, (size_t)n + 64);
			unsigned char *t = realloc(vst, (size_t)n + 64);

			if (v)
				vs = v;
			if (t)
				vst = t;
			if (!v || !t)
				return;
			vcap = n + 64;
		}
		memcpy(vs, s, (size_t)n);
		if (st)
			memcpy(vst, st, (size_t)n);
		s = vs;
		st = st ? vst : 0;
	}
	if (a->e.mode == M_CODE)
		syn_line(a->lang, s, n, fr->state, cls);
	else if (a->e.mode == M_SOURCE)
		source_classes(a, fr->line, fr->state, s, n);

	/* backgrounds */
	if (a->e.mode == M_RICH && (kind == B_CODE || kind == B_FENCE))
		fill(TX - 4, top, TW + 4, lh, WB_COLOR_SURFACE1);
	if (a->e.mode != M_RICH && cur_line && !a->full)
		box(TX - 8, top, TW + 8, lh, 6, WB_COLOR_CYAN, 13, 0, 0, 0);
	if (have_sel && !a->find_open) {	/* in the find bar the match is the selection */
		int a0 = sk0 > fr->r.k0 ? sk0 : fr->r.k0, a1 = sk1 < fr->r.k1 ? sk1 : fr->r.k1;

		if (a1 > a0 || (snl && fr->last && sk1 >= fr->r.k1)) {
			int x0 = TX + lay_x(&a->lay, &a->e, fr->line, &fr->r, a0);
			int x1 = TX + lay_x(&a->lay, &a->e, fr->line, &fr->r, a1) + (snl && fr->last && a1 == fr->r.k1 ? 8 : 0);

			box(x0, ty - 1, x1 - x0, base->ascent + base->descent + 2, 3, WB_COLOR_CYAN, SEL_A,
			    WB_COLOR_CYAN, 0, 0);
		}
	}
	if (a->find_open || a->f.cur >= 0) {
		for (int i = 0; i < a->f.n; i++) {
			const struct match *m = &a->f.m[i];
			int a0, a1, x0, x1;

			if (m->line != fr->line)
				continue;
			a0 = m->k0 > fr->r.k0 ? m->k0 : fr->r.k0;
			a1 = m->k1 < fr->r.k1 ? m->k1 : fr->r.k1;
			if (a1 <= a0 || (!a->find_open && i != a->f.cur))
				continue;
			x0 = TX + lay_x(&a->lay, &a->e, fr->line, &fr->r, a0);
			x1 = TX + lay_x(&a->lay, &a->e, fr->line, &fr->r, a1);
			if (i == a->f.cur && a->find_open)
				box(x0, ty - 1, x1 - x0, base->ascent + base->descent + 2, 3, WB_COLOR_YELLOW, MATCH_CUR_A,
				    WB_COLOR_YELLOW, 2, 0);
			else if (a->find_open)
				box(x0, ty - 1, x1 - x0, base->ascent + base->descent + 2, 3, WB_COLOR_YELLOW, MATCH_A, 0, 0, 0);
		}
	}
	/* list markers, a quote's line, line numbers */
	if (a->e.mode == M_RICH && fr->first) {
		if (kind == B_BULLET) {
			wtext(base, fr->r.x + TX - 32 + 9 - wtext_w(base, "\xe2\x80\xa2") / 2, ty, "\xe2\x80\xa2",
			      WB_COLOR_TEXT_MID, 0);
		} else if (kind == B_NUMBER) {
			char num[16];
			const struct wf_font *mf = a->small ? &wf_mono18 : &wf_mono20;

			snprintf(num, sizeof(num), "%d.", a->e.t.l[fr->line].s[0] ? atoi(a->e.t.l[fr->line].s) : 0);
			wtext(mf, TX, wtop(mf, top + lh / 2), num, WB_COLOR_TEXT_MID, 0);
		}
	}
	if (a->e.mode == M_RICH && kind == B_QUOTE)
		fill(TX + 6, top + 2, 3, lh - 4, WB_COLOR_LINE);
	if (fr->first && ((a->e.mode == M_SOURCE && a->lineno && !a->full) || a->e.mode == M_CODE) && !a->full) {
		char num[12];
		const struct wb_font *nf = a->e.mode == M_CODE ? &f_mono13 : &f_mono14;
		int right = a->e.mode == M_CODE ? 16 + 32 : 24 + 28;

		snprintf(num, sizeof(num), "%d", fr->line + 1);
		text(nf, right - text_w(nf, num, 0), text_top(nf, top + lh / 2), num,
		     cur_line ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_LOW, 0);
	}
	/* the text */
	if (a->e.mode == M_RICH && kind == B_QUOTE)
		tc = WB_COLOR_TEXT_MID;
	if (a->e.mode == M_RICH && kind == B_FENCE)
		tc = WB_COLOR_TEXT_LOW;
	x16 = (TX + fr->r.x) * 16;
	for (k = fr->r.k0; k < fr->r.k1; ) {
		int k0 = k, style = st ? st[k] & 7 : 0, bold = 0, slant = 0;
		unsigned cp = utf8_next(s, n, &k);
		const struct wf_font *f = FONTS[lay_font_of(&ls, style)];
		const struct wf_glyph *g;
		uint32_t c = tc;
		int adv;

		if (a->e.mode == M_CODE) {
			c = SYN_COLORS[cls[k0] < 7 ? cls[k0] : 0];
			f = base;
		} else if (a->e.mode == M_SOURCE) {
			unsigned v = cls[k0];

			f = base;
			if (v == 1)
				c = WB_COLOR_TEXT_LOW;
			else if (v == 2)
				c = WB_COLOR_CYAN;
			else if (v & 8) {
				style = (int)(v >> 4) & 7;
				bold = (style & ST_B) != 0;
			}
		} else if (ls.mono) {
			bold = (style & ST_B) != 0;
		}
		slant = (style & ST_I) || (a->e.mode == M_RICH && kind == B_QUOTE);
		g = wglyph(f, cp == '\t' ? ' ' : cp);
		adv = cp == '\t' ? 4 * g->adv16 : g->adv16;
		if (cp != ' ' && cp != '\t')
			wglyph_draw(f, g, x16, ty + (base->ascent - f->ascent), c, slant, bold);
		if (style & ST_U)
			fill((x16 + 8) / 16, ty + base->ascent + 4, (adv + 8) / 16, 2, c);
		x16 += adv;
	}
	/* the caret: hidden under a pop-up, and while the find bar types */
	if (caret_k >= 0 && ui_st.blink && a->ov == OV_NONE && a->screen == SC_EDIT &&
	    caret_k >= fr->r.k0 && (caret_k < fr->r.k1 || fr->last) && !(a->find_open && !a->e.sel)) {
		int cx = TX + lay_x(&a->lay, &a->e, fr->line, &fr->r, caret_k), ch = base->size + 4;

		caret_draw(cx, fr->sy + (lh - ch) / 2, ch);
	}
}

/* the new, empty document: a placeholder and what markdown does (design New) */
static void new_doc_tips(struct app *a)
{
	static const char *const md[] = { "# ", "- ", "## ", "1. ", "### ", "> ", "**bold**", "*italic*",
					  "<u>text</u>", "```" };
	static const char *const what[] = { "Heading 1", "Bulleted list", "Heading 2", "Numbered list",
					    "Heading 3", "Quote", "Bold", "Italic", "Underline", "Code block" };
	int y = TY + 32 + 36, colw = (TW - 24) / 2;

	wtext(&wf_ui24, TX + 4, wtop(&wf_ui24, TY + 16), "Start typing\xe2\x80\xa6", WB_COLOR_TEXT_LOW, 0);
	text(&f_mono13, TX, y, "MARKDOWN WORKS WHILE YOU TYPE", WB_COLOR_TEXT_LOW, 21);
	y += 17 + 10;
	for (int i = 0; i < 10; i++) {
		int x = TX + (i % 2) * (colw + 24), ry = y + (i / 2) * 41, cy = ry + 20;
		const struct wf_font *lf = i == 0 || i == 2 || i == 4 || i == 6 ? &wf_ui20b : &wf_ui20;

		fill(x, ry, colw, 1, WB_COLOR_SURFACE2);
		wtext(&wf_mono16, x, wtop(&wf_mono16, cy), md[i], WB_COLOR_CYAN, 0);
		if (i == 9)
			wtext(&wf_mono16, x + 116 + 14, wtop(&wf_mono16, cy), what[i], WB_COLOR_TEXT_MID, 0);
		else
			wtext(lf, x + 116 + 14, wtop(lf, cy), what[i], WB_COLOR_TEXT_MID, i == 7);
		if (i == 8)
			fill(x + 116 + 14, cy + 9, wtext_w(lf, what[i]), 1, WB_COLOR_TEXT_MID);
	}
	(void)a;
}

static int empty_new(struct app *a)
{
	return !a->path[0] && text_size(&a->e.t) == 0 && a->e.mode == M_RICH;
}

static void text_area(struct app *a)
{
	clip_rows(TY - (a->full ? 36 : 14), a->full ? H - 40 : H - WB_HINT_BAR_H);
	for (int i = 0; i < n_fr; i++)
		if (FR[i].sy < fb_y1 && FR[i].sy + FR[i].r.h > fb_y0)
			row_draw(a, &FR[i]);
	if (empty_new(a))
		new_doc_tips(a);
	clip_end();
}

static void scroll_bar(struct app *a)
{
	int x = a->full ? W - 56 - 3 : W - 48 - 4, w = a->full ? 3 : 4;
	int y = TY, h = (a->full ? H - 40 - 8 : H - WB_HINT_BAR_H) - TY, total = a->lay.total;
	int th, tt;

	box(x, y, w, h, w / 2, a->full ? WB_COLOR_SURFACE1 : WB_COLOR_SURFACE2, 255, 0, 0, 0);
	if (total <= TH) {
		th = h;
		tt = y;
	} else {
		th = h * TH / total;
		th = th < 24 ? 24 : th;
		tt = y + (int)((long long)(h - th) * a->scroll / (total - TH > 0 ? total - TH : 1));
		if (tt + th > y + h)
			tt = y + h - th;
	}
	box(x, tt, w, th, w / 2, a->full ? WB_COLOR_LINE : WB_COLOR_TEXT_LOW, 255, 0, 0, 0);
	if (a->find_open && total > 0)
		for (int i = 0; i < a->f.n; i++) {
			int my = y + (int)((long long)h * a->lay.ys[a->f.m[i].line] / total);

			fill(x - 3, my, 10, 3, WB_COLOR_YELLOW);
		}
}

static void word_count(struct app *a)
{
	struct docbar d;
	int w, x, y = H - 64 - 21;

	app_docbar(a, &d);
	w = text_w(&f_mono13, d.count, 0) + 12;
	x = W - 58 - w;
	box(x, y, w, 21, 6, WB_COLOR_VOID, 255, 0, 0, 0);
	text(&f_mono13, x + 6, text_top(&f_mono13, y + 10), d.count, WB_COLOR_TEXT_LOW, 0);
}

/* ---- the find bar ------------------------------------------------------------------ */

static int find_y(struct app *a)
{
	return a->full ? 16 : WB_STATUS_BAR_H + 44 + 10;
}

static void field(int x, int y, int w, int focus)
{
	if (focus)
		box(x, y, w, 44, 10, WB_COLOR_CYAN, 20, WB_COLOR_CYAN, 2, &GLOW_ROW);
	else
		box(x, y, w, 44, 10, WB_COLOR_VOID, 255, WB_COLOR_LINE, 1, 0);
}

static void toggle_box(int x, int cy, int on, const char *label, int underline)
{
	uint32_t c = on ? WB_COLOR_PINK : WB_COLOR_TEXT_MID;
	int tw = text_w(&f_mono14, label, 0);

	if (on)
		box(x, cy - 18, 36, 36, 8, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK, 1, 0);
	else
		box(x, cy - 18, 36, 36, 8, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	text(&f_mono14, x + 18 - tw / 2, text_top(&f_mono14, cy), label, c, 0);
	if (underline)
		fill(x + 18 - tw / 2 - 2, cy + 8, tw + 4, 2, c);
}

static void find_bar(struct app *a)
{
	int x = WB_CHROME_PAD, y = find_y(a), w = W - 2 * WB_CHROME_PAD;
	int h = 12 + 44 + 8 + 44 + 12 + (a->replaced ? 30 : 0), fx = x + 12 + 72 + 10;
	int fw = w - 12 - 72 - 10 - 10 - 36 - 12;
	char cnt[32], buf[160];

	box(x, y, w, h, 14, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
	for (int r = 0; r < 2; r++) {
		int ry = y + 12 + r * 52, cy = ry + 22, focus = a->find_focus == r && a->ov == OV_NONE;
		const char *v = r ? a->f.repl : a->f.q;
		int tx = fx + 12;

		text(&f_ui16, x + 12, text_top(&f_ui16, cy), r ? "Replace" : "Find", WB_COLOR_TEXT_MID, 0);
		field(fx, ry, fw, focus);
		if (!v[0] && r) {
			text(&f_ui16, tx, text_top(&f_ui16, cy), "Replace with\xe2\x80\xa6", WB_COLOR_TEXT_LOW, 0);
		} else {
			tx = wtext(&wf_mono18, tx, wtop(&wf_mono18, cy), wfit(&wf_mono18, v, fw - 120, buf, sizeof(buf)),
				   WB_COLOR_TEXT_HI, 0);
		}
		if (focus)
			fill(v[0] || !r ? tx : fx + 12, cy - 11, 2, 22, WB_COLOR_CYAN);
		if (!r && a->f.q[0]) {
			find_count(&a->f, cnt, sizeof(cnt));
			text(&f_mono14, fx + fw - 12 - text_w(&f_mono14, cnt, 0), text_top(&f_mono14, cy), cnt,
			     a->f.n ? WB_COLOR_YELLOW : WB_COLOR_TEXT_LOW, 0);
		}
		toggle_box(x + w - 12 - 36, cy, r ? a->f.whole_word : a->f.match_case, r ? "ab" : "Aa", r);
	}
	if (a->replaced) {
		int cy = y + 12 + 104 + 8 + 11, cw;
		char rep[32], undo[64];

		snprintf(rep, sizeof(rep), "Replaced %d", a->replaced);
		snprintf(undo, sizeof(undo), "Ctrl+Z undoes all %d at once", a->replaced);
		cw = text_w(&f_ui16, rep, 0) + 20;
		box(fx, cy - 11, cw, 22, 11, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK, 1, 0);
		text(&f_ui16, fx + 10, text_top(&f_ui16, cy), rep, WB_COLOR_PINK, 0);
		text(&f_ui16, fx + cw + 10, text_top(&f_ui16, cy), undo, WB_COLOR_TEXT_LOW, 0);
	}
}

/* ---- full screen ------------------------------------------------------------------- */

static void full_footer(struct app *a)
{
	struct docbar d;
	char right[64];
	int cy = H - 20, x, pct;

	app_docbar(a, &d);
	x = wtext(&wf_mono14, 56, wtop(&wf_mono14, cy), d.name, WB_COLOR_TEXT_LOW, 0);
	if (d.edited)
		box(x + 8, cy - 3, 6, 6, 3, WB_COLOR_YELLOW, 255, 0, 0, 0);
	pct = a->lay.total > TH ? (int)((long long)a->scroll * 100 / (a->lay.total - TH)) : 100;
	if (pct > 100)
		pct = 100;
	snprintf(right, sizeof(right), "%s \xc2\xb7 %d%%", d.count, pct);
	wtext(&wf_mono14, W - 56 - wtext_w(&wf_mono14, right), wtop(&wf_mono14, cy), right, WB_COLOR_TEXT_LOW, 0);
}

static void full_hint(void)
{
	const char *t1 = "leave full screen", *t2 = "keys";
	int w = 16 + keycap_w("Esc") + 18 + 10 + text_w(&f_ui16, t1, 0) + 10 + 6 + 10 + 12 + 18 + 10 +
		text_w(&f_ui16, t2, 0) + 16;
	int x = (W - w) / 2, y = H - 56 - 44, cy = y + 22;

	box(x, y, w, 44, 14, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	x = keycap(x + 16, cy, "Esc");
	x = text(&f_ui16, x + 10, text_top(&f_ui16, cy), t1, WB_COLOR_TEXT_MID, 0);
	x = text(&f_ui16, x + 10, text_top(&f_ui16, cy), "\xe2\x80\xa2", WB_COLOR_LINE, 0);
	x = keycap(x + 10, cy, "#6");
	text(&f_ui16, x + 10, text_top(&f_ui16, cy), t2, WB_COLOR_TEXT_MID, 0);
}

/* ---- pop-ups ------------------------------------------------------------------------- */

#define DX	96
#define DW	(W - 2 * DX)

static void dialog_box(int y, int h)
{
	dim(WB_COLOR_CONSOLE, 184);
	box(DX, y, DW, h, WB_RADIUS_LG, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
}

static int centered(int h)
{
	return (H - WB_HINT_BAR_H - h) / 2;
}

static void row_box(int x, int y, int w, int h, int focus)
{
	if (focus)
		box_focus(x, y, w, h, WB_RADIUS_MD, &GLOW_ROW);
	else
		box(x, y, w, h, WB_RADIUS_MD, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
}

static void switch_draw(int x, int cy, int on)
{
	int y = cy - 14;

	box(x, y, 48, 28, 14, on ? WB_COLOR_PINK : WB_COLOR_LINE, 255, 0, 0, 0);
	box(on ? x + 48 - 3 - 22 : x + 3, y + 3, 22, 22, 11, on ? WB_COLOR_VOID : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
}

static void head(int x, int y, const char *eb, uint32_t ebc, const char *title, const char *sub, int tw)
{
	char buf[160];

	text(&f_mono14, x, y, eb, ebc, 22);
	wtext(&wf_ui24b, x, y + 18 + 4, wfit(&wf_ui24b, title, tw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	if (sub)
		text(&f_mono14, x, y + 18 + 4 + 32 + 4, wb_fit(&f_mono14, sub, tw, buf, sizeof(buf)), WB_COLOR_TEXT_LOW, 0);
}

static void menu_rows_draw(struct app *a, int x, int y, int w)
{
	for (int i = 0; i < a->n_rows; i++) {
		const struct menu_row *r = &a->rows[i];
		int f = i == a->focus, cy, rx = x + w - 16;

		if (r->sep) {
			fill(x + 8, y + 6 - 6 + 6, w - 16, 1, WB_COLOR_LINE);
			y += 13 - 6 + 6;
			continue;
		}
		row_box(x, y, w, 56, f);
		cy = y + 28;
		text(&f_ui20, x + 16, text_top(&f_ui20, cy), r->label, WB_COLOR_TEXT_HI, 0);
		if (r->chev) {
			wicon(&wi_chev, rx - 18, cy - 9, WB_COLOR_TEXT_MID);
			rx -= 18 + 12;
		}
		if (r->toggle) {
			switch_draw(rx - 48, cy, r->on);
			rx -= 48 + 12;
		}
		if (r->value[0])
			text(&f_mono14, rx - text_w(&f_mono14, r->value, 0), text_top(&f_mono14, cy), r->value,
			     f ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
		y += 56 + 6;
	}
}

static int menu_rows_h(const struct app *a)
{
	int h = 0;

	for (int i = 0; i < a->n_rows; i++)
		h += a->rows[i].sep ? 13 : 56 + 6;
	return h - 6;
}

static void options_draw(struct app *a, const char *eb, const char *title, const char *sub)
{
	int hh = 18 + 4 + 32 + (sub ? 4 + 18 : 0), h = 22 + hh + 14 + menu_rows_h(a) + 20;
	int y = centered(h);

	dialog_box(y, h);
	head(DX + 20 + 8, y + 22, eb, WB_COLOR_CYAN, title, sub, DW - 56);
	menu_rows_draw(a, DX + 20, y + 22 + hh + 14, DW - 40);
}

static void style_draw(struct app *a)
{
	static const char *const marks[3] = { "Bold", "Italic", "Underline" };
	static const char *const keys[3] = { "Ctrl+B", "Ctrl+I", "Ctrl+U" };
	static const char *const paras[8] = { "Body", "Heading 1", "Heading 2", "Heading 3",
		"\xe2\x80\xa2  Bulleted list", "1.  Numbered list", "Quote", "Code block" };
	static const char *const pkeys[8] = { "Ctrl+0", "Ctrl+1", "Ctrl+2", "Ctrl+3", "Ctrl+Shift+8",
		"Ctrl+Shift+7", "", "" };
	static const enum blk kinds[8] = { B_BODY, B_H1, B_H2, B_H3, B_BULLET, B_NUMBER, B_QUOTE, B_CODE };
	int x = DX + 20, w = DW - 40, y = 16, h = 20 + 52 + 14 + 18 + 8 + 56 + 14 + 25 + 8 * 46 + 7 * 4 + 20;
	int style, cur = 0, tw = (w - 16) / 3;
	enum blk b;
	char word[64], title[80];
	enum mode m = a->e.mode;

	b = ed_block_at(&a->e);
	if (b == B_FENCE)
		b = B_CODE;
	a->e.mode = M_RICH;
	style = ed_style_at(&a->e);
	a->e.mode = m;
	for (int i = 0; i < 8; i++)
		if (kinds[i] == (b == B_BLANK ? B_BODY : b))
			cur = i;
	ed_word_at(&a->e, word, sizeof(word));
	snprintf(title, sizeof(title), "\xe2\x80\x9c%s\xe2\x80\x9d", word[0] ? word : "\xe2\x80\xa6");
	dialog_box(y, h);
	y += 20;
	wicon(&wi_triangle, x + 8, y + 2, WB_COLOR_KEY_TRIANGLE);
	text(&f_mono14, x + 8 + 12 + 8, y, "STYLE", WB_COLOR_CYAN, 22);
	{
		char buf[100];

		wtext(&wf_ui24b, x + 8, y + 20, wfit(&wf_ui24b, title, w - 16, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	}
	y += 52 + 14;
	text(&f_mono13, x + 8, y, "TEXT", WB_COLOR_TEXT_LOW, 21);
	y += 18 + 8;
	for (int i = 0; i < 3; i++) {
		int on = style & (i == 0 ? ST_B : i == 1 ? ST_I : ST_U), tx = x + i * (tw + 8), cy = y + 28;
		uint32_t c = on ? WB_COLOR_PINK : WB_COLOR_TEXT_HI;
		const struct wf_font *lf = i == 0 ? &wf_ui20b : &wf_ui20;

		if (a->focus == i)
			box_focus(tx, y, tw, 56, WB_RADIUS_MD, &GLOW_ROW);
		else if (on)
			box(tx, y, tw, 56, WB_RADIUS_MD, WB_COLOR_PINK, PINK_TINT, WB_COLOR_PINK, 1, 0);
		else
			box(tx, y, tw, 56, WB_RADIUS_MD, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
		/* the shortcut small under the label (design/write/README.md §7) */
		wtext(lf, tx + 14, wtop(lf, cy - 8), marks[i], c, i == 1);
		if (i == 2)
			fill(tx + 14, cy + 1, wtext_w(lf, marks[i]), 2, c);
		text(&f_mono13, tx + 14, text_top(&f_mono13, cy + 14), keys[i], on ? WB_COLOR_PINK : WB_COLOR_TEXT_MID, 0);
	}
	y += 56 + 14;
	text(&f_mono13, x + 8, y, "PARAGRAPH", WB_COLOR_TEXT_LOW, 21);
	y += 17 + 8;
	for (int i = 0; i < 8; i++, y += 46 + 4) {
		int f = a->focus == 3 + i, cy = y + 23, lx = x + 16 + 20 + 14;
		const struct wf_font *lf = i == 1 ? &wf_ui28b : i == 2 ? &wf_ui24b : i == 3 ? &wf_ui20b :
					   i == 7 ? &wf_mono16 : &wf_ui20;

		row_box(x, y, w, 46, f);
		if (i == cur)
			wicon(&wi_check, x + 16, cy - 10, WB_COLOR_CYAN);
		wtext(lf, lx, wtop(lf, cy), paras[i], i == 6 ? WB_COLOR_TEXT_MID : WB_COLOR_TEXT_HI, i == 6);
		if (pkeys[i][0])
			text(&f_mono14, x + w - 16 - text_w(&f_mono14, pkeys[i], 0), text_top(&f_mono14, cy), pkeys[i],
			     f ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
	}
}

/* Discard changes? and Replace? */
static void ask_draw(struct app *a, const char *eb, uint32_t ebc, const char *title, const char *desc,
		     const char *const *buttons, int nb, int danger)
{
	int x = DX + 22, w = DW - 44, lines, h, y;
	char dbuf[300];

	snprintf(dbuf, sizeof(dbuf), "%s", desc);
	lines = text_wrap(&f_ui18, 0, 0, w - 16, 26, dbuf, 0, 3, 0);
	h = 26 + 18 + 6 + 36 + 6 + lines * 26 + 18 + nb * 56 + (nb - 1) * 8 + (danger >= 0 ? 13 : 0) + 22;
	y = centered(h);
	dialog_box(y, h);
	y += 26;
	text(&f_mono14, x + 8, y, eb, ebc, 22);
	y += 18 + 6;
	wtext(&wf_ui28b, x + 8, y, title, WB_COLOR_TEXT_HI, 0);
	y += 36 + 6;
	text_wrap(&f_ui18, x + 8, y, w - 16, 26, dbuf, WB_COLOR_TEXT_MID, 3, 1);
	y += lines * 26 + 18;
	for (int i = 0; i < nb; i++) {
		int cy, tx = x + 16;

		if (i == danger) {
			fill(x + 8, y - 8 + 6, w - 16, 1, WB_COLOR_LINE);
			y += 13;
		}
		row_box(x, y, w, 56, a->focus == i);
		cy = y + 28;
		if (i == danger) {
			wicon(&wi_trash, tx, cy - 10, WB_COLOR_ERROR);
			tx += 20 + 12;
		}
		text(&f_ui20, tx, text_top(&f_ui20, cy), buttons[i], i == danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
		y += 56 + 8;
	}
}

static void saveas_draw(struct app *a)
{
	int x = 72 + 24, w = W - 144 - 48, h = 26 + 18 + 4 + 36 + 20 + 20 + 8 + 56 + 8 + 44 + 20 + 56 + 24, y;
	int cy, ok = !a->sa_err[0], bad = a->sa_err[0] != 0;
	uint32_t c = bad ? WB_COLOR_ERROR : WB_COLOR_CYAN;
	char buf[160];

	y = centered(h);
	dim(WB_COLOR_CONSOLE, 184);
	box(72, y, W - 144, h, WB_RADIUS_LG, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	y += 26;
	text(&f_mono14, x + 4, y, "WRITE", WB_COLOR_CYAN, 22);
	wtext(&wf_ui28b, x + 4, y + 18 + 4, "Save as", WB_COLOR_TEXT_HI, 0);
	y += 18 + 4 + 36 + 20;
	text(&f_ui16, x + 4, y, "Name", WB_COLOR_TEXT_MID, 0);
	y += 20 + 8;
	cy = y + 28;
	if (a->sa_focus == 0)
		box(x, y, w, 56, WB_RADIUS_MD, c, 20, c, 2, &GLOW_ROW);
	else
		box(x, y, w, 56, WB_RADIUS_MD, WB_COLOR_VOID, 255, WB_COLOR_LINE, 1, 0);
	{
		char before[NAME_MAX_W + 1];
		int tx;

		snprintf(before, sizeof(before), "%.*s", a->sa_caret, a->sa_name);
		tx = wtext(&wf_mono20, x + 16, wtop(&wf_mono20, cy), before, WB_COLOR_TEXT_HI, 0);
		if (a->sa_focus == 0)
			fill(tx, cy - 12, 2, 24, c);
		tx = wtext(&wf_mono20, tx + 2, wtop(&wf_mono20, cy), a->sa_name + a->sa_caret, WB_COLOR_TEXT_HI, 0);
		wtext(&wf_mono20, tx, wtop(&wf_mono20, cy), a->sa_ext, WB_COLOR_TEXT_LOW, 0);
	}
	y += 56 + 8;
	snprintf(buf, sizeof(buf), "%s", ok ? (!strcmp(a->sa_ext, ".md") ? "Saved as markdown. \xe2\x86\x93 to choose another folder." :
				       "\xe2\x86\x93 to choose another folder.") : a->sa_err);
	text_wrap(&f_ui16, x + 4, y, w - 8, 22, buf, bad ? WB_COLOR_ERROR : WB_COLOR_TEXT_LOW, 2, 1);
	y += 44 + 20;
	cy = y + 28;
	row_box(x, y, w, 56, a->sa_focus == 1);
	wicon(&wi_folder, x + 16, cy - 11, WB_COLOR_TEXT_MID);
	text(&f_ui20, x + 16 + 22 + 14, text_top(&f_ui20, cy), "Folder", WB_COLOR_TEXT_HI, 0);
	wicon(&wi_chev, x + w - 16 - 18, cy - 9, WB_COLOR_TEXT_LOW);
	{
		char pb[96];
		const char *p = wb_fit(&f_mono14, a->sa_dir, w - 200, pb, sizeof(pb));

		text(&f_mono14, x + w - 16 - 18 - 14 - text_w(&f_mono14, p, 0), text_top(&f_mono14, cy), p,
		     WB_COLOR_TEXT_MID, 0);
	}
}

static void language_draw(struct app *a)
{
	int x = DX + 20, w = DW - 40, y = 24, h = H - 72 - 24, ly, first = 0, vis;
	char cnt[24];

	dialog_box(y, h);
	text(&f_mono14, x + 8, y + 22, "OPTIONS", WB_COLOR_CYAN, 22);
	wtext(&wf_ui24b, x + 8, y + 22 + 18 + 4, "Language", WB_COLOR_TEXT_HI, 0);
	snprintf(cnt, sizeof(cnt), "%d options", L_COUNT);
	text(&f_mono13, x + w - 8 - text_w(&f_mono13, cnt, 0), y + 22 + 18 + 4 + 10, cnt, WB_COLOR_TEXT_LOW, 0);
	ly = y + 22 + 18 + 4 + 32 + 14;
	vis = (y + h - ly) / 50;
	if (a->focus >= vis)
		first = a->focus - vis + 1;
	clip_rows(ly, y + h - 2);
	for (int i = first; i < L_COUNT; i++) {
		int ry = ly + (i - first) * 50, cy = ry + 23, f = i == a->focus;

		if (ry > y + h)
			break;
		row_box(x, ry, w - 14, 46, f);
		if (i == a->lang)
			wicon(&wi_check, x + 16, cy - 10, WB_COLOR_CYAN);
		text(&f_ui20, x + 16 + 20 + 14, text_top(&f_ui20, cy), lang_name((enum lang)i), WB_COLOR_TEXT_HI, 0);
		text(&f_mono14, x + w - 14 - 16 - text_w(&f_mono14, lang_exts((enum lang)i), 0), text_top(&f_mono14, cy),
		     lang_exts((enum lang)i), f ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
	}
	clip_end();
	scrollbar(x + w - 4, ly, y + h - ly - 20, L_COUNT * 50, vis * 50, first * 50);
}

static void log_draw(struct app *a)
{
	int y = WB_STATUS_BAR_H + 20;

	if (!a->n_log) {
		empty_screen("WRITE", "Log", "No log yet.");
		status_bar();
		return;
	}
	fill(0, 0, W, H, WB_COLOR_VOID);
	eyebrow(WB_CONTENT_PAD, y, "WRITE");
	text(&f_ui36b, WB_CONTENT_PAD, y + 18 + 6, "Log", WB_COLOR_TEXT_HI, -11);
	y += 18 + 6 + 44 + 20;
	{
		int rows = (H - WB_HINT_BAR_H - 16 - y) / 26, first = a->n_log > rows ? a->n_log - rows : 0;
		char buf[140];

		for (int i = first; i < a->n_log; i++, y += 26)
			text(&f_mono14, WB_CONTENT_PAD, y, wb_fit(&f_mono14, a->log[i], W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
			     strstr(a->log[i], "error") ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	}
	status_bar();
}

/* a key cap of the help: mono 13, a thicker bottom edge (design Help) */
static int help_cap(int x, int cy, const char *k)
{
	int sym = one_sym(k), w = (sym ? 12 : text_w(&f_mono13, k, 0)) + 16;

	box(x, cy - 12, w, 24, 6, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
	fill(x + 3, cy + 11, w - 6, 1, WB_COLOR_LINE);
	if (sym)
		icon(keycap_syms[sym - 1], x + 8, cy - 6, key_colors[sym - 1], 255);
	else
		text(&f_mono13, x + 8, text_top(&f_mono13, cy), k, WB_COLOR_TEXT_HI, 0);
	return x + w;
}

struct help_row { const char *caps[3]; const char *what; };

static void help_draw(struct app *a)
{
	static const struct help_row rich[] = {
		{ { "Fn+arrows" }, "Select. Ctrl+A: select all" },
		{ { "Ctrl+C", "X", "V" }, "Copy, cut, paste" },
		{ { "Ctrl+S" }, "Save. A new document asks for a name" },
		{ { "Ctrl+F", "H" }, "Find. Ctrl+H: find and replace" },
		{ { "Ctrl+B", "I", "U" }, "Bold, italic, underline" },
		{ { "#1" }, "Options: save, open, find, full screen" },
		{ { "#2" }, "Style: headings, lists, bold, italic, underline" },
		{ { "#3" }, "Switch between rich text and markdown" },
		{ { "#4" }, "Log: saves and errors" },
		{ { "#5" }, "Settings: text size, line numbers" },
		{ { "Esc" }, "Close. Asks first if there are unsaved changes" },
		{ { "Logo" }, "Back to the menu. Write saves first" },
	};
	static const struct help_row code[] = {
		{ { "Fn+arrows" }, "Select. Ctrl+A: select all" },
		{ { "Ctrl+C", "X", "V" }, "Copy, cut, paste" },
		{ { "Ctrl+S" }, "Save. A new file asks for a name" },
		{ { "Ctrl+F", "H" }, "Find. Ctrl+H: find and replace" },
		{ { "Tab" }, "Indent. Enter keeps the indent" },
		{ { "#1" }, "Options: save, open, find, language" },
		{ { "#2" }, "Run it (Python, shell) or preview it (HTML)" },
		{ { "#4" }, "Log: saves and errors" },
		{ { "#5" }, "Settings: text size, line numbers" },
		{ { "Ctrl+Z" }, "Undo. Ctrl+Shift+Z: redo" },
		{ { "Esc" }, "Close. Asks first if there are unsaved changes" },
		{ { "Logo" }, "Back to the menu. Write saves first" },
	};
	static const struct hint esc[] = { { "Esc", "close", 1 } };
	const struct help_row *k = a->e.mode == M_CODE ? code : rich;
	const char *about = a->e.mode == M_CODE ? "Edit text and code. Files stay as they are: Write adds nothing." :
			    "Write notes and documents. Files are saved as markdown (.md), so any other editor can open them.";
	int x = 48 + 30, w = W - 96 - 60, y = 16, lines = text_wrap(&f_ui18, 0, 0, w, 25, about, 0, 2, 0);
	int h = 20 + 18 + 6 + 36 + 6 + lines * 25 + 16 + 25 + 12 * 32 + 20;
	char buf[100];

	dim(WB_COLOR_CONSOLE, 184);
	box(48, y, W - 96, h, WB_RADIUS_LG, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	y += 20;
	icon(&kc_diamond, x, y + 2, WB_COLOR_KEY_DIAMOND, 255);
	text(&f_mono14, x + 12 + 8, y, "HELP", WB_COLOR_CYAN, 22);
	y += 18 + 6;
	wtext(&wf_ui28b, x, y, "Write", WB_COLOR_TEXT_HI, 0);
	y += 36 + 6;
	text_wrap(&f_ui18, x, y, w, 25, about, WB_COLOR_TEXT_MID, 2, 1);
	y += lines * 25 + 16;
	text(&f_mono13, x, y, "KEYS ON THIS SCREEN", WB_COLOR_TEXT_LOW, 21);
	y += 17 + 8;
	for (int i = 0; i < 12; i++, y += 32) {
		int cx = x, cy = y + 16;

		fill(x, y, w, 1, WB_COLOR_LINE);
		for (int c = 0; c < 3 && k[i].caps[c]; c++)
			cx = help_cap(cx, cy, k[i].caps[c]) + 4;
		text(&f_ui18, x + 168 + 16, text_top(&f_ui18, cy), wb_fit(&f_ui18, k[i].what, w - 184, buf, sizeof(buf)),
		     WB_COLOR_TEXT_HI, 0);
	}
	hint_bar(esc, 1);
}

/* ---- Open ------------------------------------------------------------------------------- */

static const char *drive_label(const char *p, const char *home)
{
	if (!strcmp(p, home))
		return "Home";
	if (!strcmp(p, "/mnt/sd"))
		return "SD card";
	if (!strcmp(p, "/media/sd"))
		return "SD card (FAT)";
	if (!strcmp(p, "/data"))
		return "Badge storage";
	if (!strcmp(p, "/tmp"))
		return "Memory";
	if (!strcmp(p, "/"))
		return "System";
	return p;
}

static void open_draw(struct app *a)
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD - 16, y = WB_STATUS_BAR_H + 20, ly, vis, first = 0;
	char buf[160];
	time_t now = time(0);

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	text(&f_mono14, x, y, "WRITE", WB_COLOR_CYAN, 22);
	wtext(&wf_ui36b, x, y + 18 + 4, a->op_folders ? "Choose a folder" : a->op_drives ? "Drives" : "Open",
	      WB_COLOR_TEXT_HI, 0);
	if (!a->op_folders && !a->op_drives)
		text(&f_ui16, x + w + 16 - text_w(&f_ui16, "text and code", 0), y + 18 + 4 + 16, "text and code",
		     WB_COLOR_TEXT_LOW, 0);
	text(&f_mono14, x, y + 18 + 4 + 44 + 4, wb_fit(&f_mono14, a->op_drives ? "" : a->op_dir, w, buf, sizeof(buf)),
	     WB_COLOR_TEXT_LOW, 0);
	ly = y + 18 + 4 + 44 + 4 + 20 + 20;
	if (a->op_filter[0]) {
		box(x, ly, w, 44, 10, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
		fill(text(&f_mono18, x + 16, text_top(&f_mono18, ly + 22), a->op_filter, WB_COLOR_TEXT_HI, 0) + 2, ly + 11,
		     2, 22, WB_COLOR_CYAN);
		ly += 44 + 12;
	}
	vis = (H - WB_HINT_BAR_H - 8 - ly) / 64;
	if (a->op_focus >= vis)
		first = a->op_focus - vis + 1;
	if (!a->op_n)
		text(&f_ui18, x + 4, ly + 16, a->op_err[0] ? a->op_err : "Nothing matches", WB_COLOR_TEXT_LOW, 0);
	clip_rows(ly - 20, H - WB_HINT_BAR_H);
	for (int i = first; i < a->op_n; i++) {
		const struct open_row *r = &a->op[i];
		int ry = ly + (i - first) * 64, cy = ry + 28, f = i == a->op_focus, rx = x + w - 16;
		uint32_t ink = f ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
		char val[32] = "";
		const char *name = a->op_drives ? drive_label(r->name, a->home) : r->name;

		if (ry > H)
			break;
		row_box(x, ry, w, 56, f);
		wicon(r->isnew ? &wi_plus : r->here ? &wi_check : a->op_drives ? &wi_drive : r->dir ? &wi_folder :
		      lang_of_path(r->name) == L_MARKDOWN || lang_of_path(r->name) == L_PLAIN ? &wi_doc : &wi_code,
		      x + 16, cy - 11, ink);
		if (r->dir) {
			wicon(&wi_chev, rx - 18, cy - 9, f ? WB_COLOR_CYAN : WB_COLOR_TEXT_LOW);
			rx -= 18 + 14;
		}
		if (r->isnew)
			snprintf(val, sizeof(val), "Ctrl+N");
		else if (a->op_drives)
			snprintf(val, sizeof(val), "%s", r->name);
		else if (r->dir)
			snprintf(val, sizeof(val), "%d item%s", r->items, r->items == 1 ? "" : "s");
		else if (!r->here)
			when_text(r->mtime, now, val, sizeof(val));
		if (val[0]) {
			rx -= text_w(&f_mono14, val, 0);
			text(&f_mono14, rx, text_top(&f_mono14, cy), val, ink, 0);
			rx -= 14;
		}
		wtext(&wf_ui20, x + 16 + 22 + 14, wtop(&wf_ui20, cy),
		      wfit(&wf_ui20, r->here ? "This folder" : name, rx - x - 16 - 22 - 14, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	}
	clip_end();
	scrollbar(x + w + 12, ly, H - WB_HINT_BAR_H - 8 - ly, a->op_n * 64, vis * 64, first * 64);
	hints(a);
}

/* ---- a frame ------------------------------------------------------------------------- */

static void error_draw(struct app *a)
{
	static const char *const ok[] = { "OK" };

	ask_draw(a, "WRITE", WB_COLOR_ERROR, a->err_title, a->err_text, ok, 1, -1);
}

static void draw(struct app *a)
{
	if (a->screen == SC_OPEN) {
		open_draw(a);
		return;
	}
	if (a->ov == OV_LOG) {
		log_draw(a);
		hints(a);
		return;
	}
	fill(0, fb_y0, W, fb_y1 - fb_y0, WB_COLOR_VOID);
	if (!a->full) {
		status_bar();
		docbar(a);
	}
	text_area(a);
	if (a->find_open)
		find_bar(a);
	scroll_bar(a);
	if (a->full)
		full_footer(a);
	else
		word_count(a);
	switch (a->ov) {
	case OV_OPTIONS: {
		char sub[160], when[32] = "not saved yet";

		if (a->saved_at) {
			struct tm tm;

			localtime_r(&a->saved_at, &tm);
			snprintf(when, sizeof(when), "saved %02d:%02d", tm.tm_hour, tm.tm_min);
		}
		snprintf(sub, sizeof(sub), "%s \xc2\xb7 %s", a->dir, when);
		options_draw(a, "OPTIONS", a->name, sub);
		break;
	}
	case OV_FIND_MENU:
		options_draw(a, "FIND", "Find and replace", 0);
		break;
	case OV_SETTINGS:
		options_draw(a, "SETTINGS", "Write", 0);
		break;
	case OV_STYLE:
		style_draw(a);
		break;
	case OV_DISCARD: {
		static const char *const close_b[] = { "Keep editing", "Save and close", "Discard changes" };
		static const char *const go_b[] = { "Keep editing", "Save first", "Discard changes" };
		char desc[200];

		snprintf(desc, sizeof(desc), "%s has changes since the last save.", a->name);
		ask_draw(a, "UNSAVED CHANGES", WB_COLOR_YELLOW, "Discard changes?", desc,
			 a->after == AF_CLOSE ? close_b : go_b, 3, 2);
		break;
	}
	case OV_REPLACE_ASK: {
		static const char *const b[] = { "Cancel", "Replace" };
		char title[160], desc[200];

		snprintf(title, sizeof(title), "Replace %s%s?", a->sa_name, a->sa_ext);
		snprintf(desc, sizeof(desc), "The file in %s is overwritten with this document.", a->sa_dir);
		ask_draw(a, "SAVE AS", WB_COLOR_CYAN, title, desc, b, 2, 1);
		break;
	}
	case OV_SAVEAS:
		saveas_draw(a);
		break;
	case OV_LANGUAGE:
		language_draw(a);
		break;
	case OV_ERROR:
		error_draw(a);
		break;
	case OV_HELP:
		help_draw(a);
		return;		/* help_popup drew its own hint bar */
	default:
		break;
	}
	if (a->full && a->hint_until && a->ov == OV_NONE)
		full_hint();
	if (!a->full || a->ov != OV_NONE)
		hints(a);
}

/* ---- bands ---------------------------------------------------------------------------- */

#define BANDS	((H + BAND_H - 1) / BAND_H)

static uint16_t band[W * BAND_H];
static unsigned sig_last[BANDS], sig_now[BANDS];

static unsigned mix(unsigned h, unsigned v)
{
	return (h ^ v) * 16777619u;
}

static unsigned hash_str(unsigned h, const char *s, int n)
{
	for (int i = 0; i < n; i++)
		h = mix(h, (unsigned char)s[i]);
	return h;
}

static void sig_add(int y0, int y1, unsigned h)
{
	if (y0 < 0)
		y0 = 0;
	for (int b = y0 / BAND_H; b < BANDS && b * BAND_H < y1; b++)
		sig_now[b] = mix(sig_now[b], h);
}

/* what each band shows, as a hash: see the comment at the top */
static void signatures(struct app *a)
{
	struct docbar d;
	struct hintk hs[12];
	unsigned g = 2166136261u, h;
	int nh;

	g = mix(g, (unsigned)a->scroll);
	g = mix(g, (unsigned)a->e.mode | (unsigned)a->full << 2 | (unsigned)a->small << 3 | (unsigned)a->lineno << 4 |
		       (unsigned)a->find_open << 5 | (unsigned)a->find_focus << 6 | (unsigned)a->replaced << 8);
	g = mix(g, (unsigned)a->lay.total);
	g = mix(g, (unsigned)a->lang);
	g = mix(g, (unsigned)empty_new(a));
	g = hash_str(g, a->name, (int)strlen(a->name));
	for (int b = 0; b < BANDS; b++)
		sig_now[b] = g;
	if (a->ov != OV_NONE || a->screen != SC_EDIT) {
		for (int b = 0; b < BANDS; b++)
			sig_now[b] = mix(sig_now[b], (unsigned)rand());	/* pop-ups: every frame anew */
		return;
	}
	h = hash_str(2166136261u, ui_st.hhmm, 5);
	h = mix(h, (unsigned)ui_st.bars ^ (unsigned)ui_st.lora << 4);
	sig_add(0, WB_STATUS_BAR_H, h);
	app_docbar(a, &d);
	h = hash_str(2166136261u, d.info, (int)strlen(d.info));
	h = mix(h, (unsigned)d.edited | (unsigned)d.style << 1 | (unsigned)d.md_view << 4 | (unsigned)d.info_kind << 6);
	h = hash_str(h, d.para ? d.para : "", d.para ? (int)strlen(d.para) : 0);
	h = hash_str(h, d.indent, (int)strlen(d.indent));
	sig_add(WB_STATUS_BAR_H, WB_STATUS_BAR_H + 44, h);
	h = hash_str(2166136261u, d.count, (int)strlen(d.count));
	sig_add(H - 64 - 21, H - 64, h);
	if (a->full) {
		h = mix(h, (unsigned)d.edited ^ (unsigned)(a->hint_until != 0) << 1);
		sig_add(H - 140, H, h);
	}
	if (a->find_open) {
		h = hash_str(2166136261u, a->f.q, (int)strlen(a->f.q));
		h = hash_str(h, a->f.repl, (int)strlen(a->f.repl));
		h = mix(h, (unsigned)a->f.n ^ (unsigned)a->f.cur << 14 ^ (unsigned)a->f.match_case << 28 ^
			       (unsigned)a->f.whole_word << 29);
		sig_add(find_y(a), find_y(a) + 160, h);
	}
	/* the scroll bar: its thumb and marks follow the scroll and the matches */
	sig_add(TY, H - WB_HINT_BAR_H, mix(mix((unsigned)a->scroll, (unsigned)a->lay.total), (unsigned)a->f.n));
	nh = app_hints(a, hs, 12);
	h = 2166136261u;
	for (int i = 0; i < nh; i++) {
		h = hash_str(h, hs[i].keys, (int)strlen(hs[i].keys));
		h = hash_str(h, hs[i].label, (int)strlen(hs[i].label));
	}
	sig_add(H - WB_HINT_BAR_H, H, h);
	/* the rows of text: their text, selection, matches and caret */
	{
		struct pos s0, s1;

		ed_sel_range(&a->e, &s0, &s1);
		for (int i = 0; i < n_fr; i++) {
			const struct frow *fr = &FR[i];
			const struct tline *l = &a->e.t.l[fr->line];

			h = hash_str(2166136261u, l->s, l->len);
			h = mix(h, (unsigned)fr->r.k0 ^ (unsigned)fr->r.k1 << 16);
			h = mix(h, (unsigned)fr->sy ^ (unsigned)fr->state << 20);
			if (a->e.sel && fr->line >= s0.line && fr->line <= s1.line)
				h = mix(mix(h, (unsigned)s0.col ^ (unsigned)s0.line << 16), (unsigned)s1.col ^ (unsigned)s1.line << 16);
			if (fr->line == a->e.caret.line)
				h = mix(mix(h, (unsigned)a->e.caret.col), (unsigned)ui_st.blink | 0x100);
			if (a->find_open)
				for (int m = 0; m < a->f.n; m++)
					if (a->f.m[m].line == fr->line)
						h = mix(h, (unsigned)a->f.m[m].k0 ^ (unsigned)a->f.m[m].k1 << 15 ^
							       (unsigned)(m == a->f.cur) << 30);
			if (a->e.mode == M_RICH && fr->first && l->len && l->s[0] >= '0' && l->s[0] <= '9')
				h = mix(h, (unsigned)fr->line);
			sig_add(fr->sy - 4, fr->sy + fr->r.h + 4, h);
		}
	}
}

int ui_present(struct app *a, int force, void (*out)(const uint16_t *band, int y0, int y1))
{
	int sent = 0;

	app_follow(a);
	app_geometry(a, &TX, &TY, &TW, &TH);
	frame_rows(a);
	signatures(a);
	if (a->redraw)
		force = 1;
	a->redraw = 0;
	for (int b = 0; b < BANDS; b++) {
		int b0 = b * BAND_H, b1 = b0 + BAND_H < H ? b0 + BAND_H : H;

		if (!force && sig_now[b] == sig_last[b])
			continue;
		sig_last[b] = sig_now[b];
		fb = (uint16_t *)((uintptr_t)band - (uintptr_t)b0 * W * sizeof(*band));	/* row 0, outside band */
		fb_y0 = b0;
		fb_y1 = b1;
		draw(a);
		out(band, b0, b1);
		sent++;
	}
	return sent;
}

void ui_init(void)
{
	app_adv16 = ui_adv16;
}

int ui_wifi_bars(void)
{
	return wifi_bars();
}

int ui_lora_on(void)
{
	return lora_on();
}
