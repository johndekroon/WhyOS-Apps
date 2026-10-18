/* SPDX-License-Identifier: MIT */
/*
 * Drawing for the badge's panel in the WhyBadge design's style, shared by
 * home and appterm: alpha-blended pixels, rounded boxes with the focus
 * glow, text in the pre-rendered fonts and icons of home_assets.h, key
 * caps and the hint bar. Integer-only (the toolchain is soft-float).
 *
 * Include after home_assets.h and home-assets/wb_tokens.h, with W and H
 * defined. Unused functions are fine: they are static.
 */
#pragma once
#pragma GCC diagnostic ignored "-Wunused-function"

/* ------------------------------------------------------------------ */
/* Pixels                                                              */

/*
 * fb holds rows [fb_y0, fb_y1) of the screen: the whole panel for home, one
 * band at a time for appterm (no 1 MB buffer next to the app). Drawing is
 * clipped to those rows; fb points at row 0, which may lie outside it.
 */
static uint16_t *fb;
static int fb_y0, fb_y1 = H;

static inline uint16_t pack565(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t rgb(uint32_t c)
{
	return pack565((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

/* Blend colour c (0xRRGGBB) over pixel (x, y) with alpha a (0..255). */
static void put(int x, int y, uint32_t c, unsigned a)
{
	uint16_t *p, d;
	unsigned dr, dg, db, na;

	if ((unsigned)x >= W || y < fb_y0 || y >= fb_y1 || a == 0)
		return;
	p = &fb[y * W + x];
	if (a >= 255) {
		*p = rgb(c);
		return;
	}
	d = *p;
	dr = ((d >> 11) << 3) | (d >> 13);
	dg = (((d >> 5) & 63) << 2) | ((d >> 9) & 3);
	db = ((d & 31) << 3) | ((d >> 2) & 7);
	na = 255 - a;
	*p = pack565((((c >> 16) & 255) * a + dr * na) / 255,
		     (((c >> 8) & 255) * a + dg * na) / 255,
		     ((c & 255) * a + db * na) / 255);
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
	uint16_t v = rgb(c);

	if (x < 0) { w += x; x = 0; }
	if (y < fb_y0) { h -= fb_y0 - y; y = fb_y0; }
	if (x + w > W) w = W - x;
	if (y + h > fb_y1) h = fb_y1 - y;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			fb[(y + j) * W + x + i] = v;
}

/* Darken everything (dialog backdrop). */
static void dim(uint32_t c, unsigned a)
{
	/* put()'s blend per channel value, looked up: the same result */
	uint16_t lr[32], lg[64], lb[32];
	unsigned na = 255 - a;

	for (unsigned i = 0; i < 64; i++) {
		unsigned e5 = (i & 31) << 3 | (i & 31) >> 2, e6 = i << 2 | i >> 4;

		if (i < 32) {
			lr[i] = (uint16_t)(((((c >> 16) & 255) * a + e5 * na) / 255 >> 3) << 11);
			lb[i] = (uint16_t)(((c & 255) * a + e5 * na) / 255 >> 3);
		}
		lg[i] = (uint16_t)(((((c >> 8) & 255) * a + e6 * na) / 255 >> 2) << 5);
	}
	for (int y = fb_y0 < 0 ? 0 : fb_y0; y < fb_y1 && y < H; y++)
		for (int x = 0; x < W; x++) {
			uint16_t *p = &fb[y * W + x], d = *p;

			*p = lr[d >> 11] | lg[(d >> 5) & 63] | lb[d & 31];
		}
}

/* ------------------------------------------------------------------ */
/* Rounded rectangles, in 1/16 px fixed point                          */

static int isqrt(unsigned v)
{
	unsigned r = 0, b = 1u << 30;

	while (b > v)
		b >>= 2;
	while (b) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return (int)r;
}

/* Signed distance from pixel (px, py)'s centre to the rounded rect, in
 * 1/16 px; negative inside. */
static int rr_dist(int px, int py, int x, int y, int w, int h, int r)
{
	int dx = px * 16 + 8 - (x * 16 + w * 8);
	int dy = py * 16 + 8 - (y * 16 + h * 8);

	if (dx < 0) dx = -dx;
	if (dy < 0) dy = -dy;
	dx -= w * 8 - r * 16;
	dy -= h * 8 - r * 16;
	if (dx <= 0 && dy <= 0)
		return (dx > dy ? dx : dy) - r * 16;
	if (dx <= 0)
		return dy - r * 16;
	if (dy <= 0)
		return dx - r * 16;
	return isqrt((unsigned)(dx * dx + dy * dy)) - r * 16;
}

static unsigned cover(int d)
{
	int a = 128 - d * 16;

	return a < 0 ? 0 : a > 255 ? 255 : (unsigned)a;
}

struct glow { const uint8_t *lut; int n; };
static const struct glow GLOW_TILE = { glow_tile, sizeof(glow_tile) };
static const struct glow GLOW_ROW = { glow_row, sizeof(glow_row) };

/*
 * A box like the design's: optional outer glow (box-shadow), background
 * at fill_a, and a border of bw px (0 for none) drawn over it.
 */
static void box(int x, int y, int w, int h, int r,
		uint32_t fill_c, unsigned fill_a,
		uint32_t border_c, int bw, const struct glow *g)
{
	int ext = g ? g->n / 4 + 1 : 1;
	/* Between the two corner arcs (m0..m1) a row is the same everywhere:
	 * worked out once, at the middle (exact while r >= 2 bw, as for
	 * every box the design has). */
	int m0 = x + r + 1, m1 = x + w - r - 1, uni = r >= 2 * bw && m0 < m1;

	for (int py = y - ext; py < y + h + ext; py++) {
		unsigned ga = 0, fa = 0, ba = 0;

		if (py < fb_y0 || py >= fb_y1)
			continue;
		if (uni) {
			int d = rr_dist(x + w / 2, py, x, y, w, h, r);
			unsigned ao = cover(d);

			if (d > 0 && g && d / 4 < g->n)
				ga = g->lut[d / 4];
			if (d < 8 + 16) {
				fa = fill_a ? ao * fill_a / 255 : 0;
				if (bw) {
					unsigned ai = cover(rr_dist(x + w / 2, py, x + bw, y + bw, w - 2 * bw,
								    h - 2 * bw, r > bw ? r - bw : 0));
					ba = ao > ai ? ao - ai : 0;
				}
			}
		}
		for (int px = x - ext; px < x + w + ext; px++) {
			int d;
			unsigned ao;

			if (uni && px == m0) {
				for (int q = m0; ga && q < m1; q++)
					put(q, py, WB_COLOR_CYAN, ga);
				if (fa >= 255)
					fill(m0, py, m1 - m0, 1, fill_c);
				else
					for (int q = m0; fa && q < m1; q++)
						put(q, py, fill_c, fa);
				for (int q = m0; ba && q < m1; q++)
					put(q, py, border_c, ba);
				px = m1 - 1;
				continue;
			}
			d = rr_dist(px, py, x, y, w, h, r);
			if (d > 0 && g) {
				int q = d / 4;	/* quarter pixels */
				if (q < g->n)
					put(px, py, WB_COLOR_CYAN, g->lut[q]);
			}
			if (d >= 8 + 16)
				continue;
			ao = cover(d);
			if (fill_a)
				put(px, py, fill_c, ao * fill_a / 255);
			if (bw) {
				int di = rr_dist(px, py, x + bw, y + bw, w - 2 * bw,
						 h - 2 * bw, r > bw ? r - bw : 0);
				unsigned ai = cover(di);
				if (ao > ai)
					put(px, py, border_c, ao - ai);
			}
		}
	}
}

/* The design's three box styles. */
static void box_plain(int x, int y, int w, int h, int r, uint32_t bg)
{
	box(x, y, w, h, r, bg, 255, WB_COLOR_LINE, 1, 0);
}

static void box_focus(int x, int y, int w, int h, int r, const struct glow *g)
{
	box(x, y, w, h, r, WB_COLOR_CYAN, WB_FOCUS_FILL_OPA,
	    WB_COLOR_CYAN, WB_FOCUS_BORDER_W, g);
}

/* ------------------------------------------------------------------ */
/* Text and icons                                                      */

static unsigned next_cp(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	unsigned cp = *p++;

	if (cp >= 0xf0 && p[0] && p[1] && p[2]) {	/* past U+FFFF: emoji */
		cp = ((cp & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
		p += 3;
	} else if (cp >= 0xe0 && p[0] && p[1]) {
		cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
		p += 2;
	} else if (cp >= 0xc0 && p[0]) {
		cp = ((cp & 31) << 6) | (p[0] & 63);
		p += 1;
	}
	*s = (const char *)p;
	return cp;
}

static const struct wb_glyph *glyph(const struct wb_font *f, unsigned cp)
{
	if (cp >= 0x20 && cp <= 0x7e)
		return &f->g[cp - 0x20];
	for (int i = 0x7f - 0x20; i < f->n; i++)
		if (f->g[i].cp == cp)
			return &f->g[i];
	return &f->g['?' - 0x20];
}

/*
 * Emoji (WB_EMOJI, emoji_font.h): what a font lacks from U+2000 up is looked
 * for there, and drawn in the text's colour, centred on the font's ascent. What is in neither is a hollow box, not a "?". Variation selectors,
 * the zero-width joiner and skin tones take no room: a joined sequence shows
 * as its parts.
 */
#ifdef WB_EMOJI
#include "emoji_font.h"

static const struct wb_emoji *emoji_find(unsigned cp)
{
	int lo = 0, hi = WB_EMOJI_N - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (wb_emoji[mid].cp == cp)
			return &wb_emoji[mid];
		if (wb_emoji[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

static int font_has(const struct wb_font *f, unsigned cp)
{
	if (cp >= 0x20 && cp <= 0x7e)
		return 1;
	for (int i = 0x7f - 0x20; i < f->n; i++)
		if (f->g[i].cp == cp)
			return 1;
	return 0;
}

static int zero_width(unsigned cp)
{
	return cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0x1f3fb && cp <= 0x1f3ff);
}

/* An emoji's advance in 1/16 px, or -1 when the font has cp itself. */
static int emoji_adv16(const struct wb_font *f, unsigned cp)
{
	const struct wb_emoji *e;

	if (cp < 0x2000 || font_has(f, cp))
		return -1;
	if (zero_width(cp))
		return 0;
	e = emoji_find(cp);
	return e ? e->adv16 : (WB_EMOJI_SIZE * 3 / 4 + 4) * 16;
}

static void emoji_draw(const struct wb_font *f, int x, int y, unsigned cp, uint32_t c)
{
	const struct wb_emoji *e = zero_width(cp) ? 0 : emoji_find(cp);
	int base = y + (f->ascent - WB_EMOJI_ASCENT) / 2;	/* centred on the ascent */

	if (zero_width(cp))
		return;
	if (!e) {		/* not one of ours: a hollow box */
		int w = WB_EMOJI_SIZE * 3 / 4, h = f->ascent * 3 / 4, by = y + f->ascent - h;

		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++)
				if (j == 0 || j == h - 1 || i == 0 || i == w - 1)
					put(x + 2 + i, by + j, c, 200);
		return;
	}
	{
		const uint8_t *a = wb_emoji_bits + e->off;
		int gx = x + e->x, gy = base + e->y;
		int j0 = fb_y0 - gy > 0 ? fb_y0 - gy : 0, j1 = fb_y1 - gy < e->h ? fb_y1 - gy : e->h;

		for (int j = j0; j < j1; j++)
			for (int i = 0; i < e->w; i++)
				put(gx + i, gy + j, c, a[j * e->w + i]);
	}
}
#endif

/* Width in px; track16 is extra letter spacing in 1/16 px. */
static int text_w(const struct wb_font *f, const char *s, int track16)
{
	int x16 = 0;

	while (*s) {
		unsigned cp = next_cp(&s);
#ifdef WB_EMOJI
		int e = emoji_adv16(f, cp);

		if (e >= 0) {
			x16 += e + (e ? track16 : 0);
			continue;
		}
#endif
		x16 += glyph(f, cp)->adv16 + track16;
	}
	return (x16 - (x16 ? track16 : 0) + 8) / 16;
}

/* Draw with the line's top at y (top of the font's ascent); returns end x. */
static int text(const struct wb_font *f, int x, int y, const char *s,
		uint32_t c, int track16)
{
	int x16 = x * 16;

	while (*s) {
		unsigned cp = next_cp(&s);
#ifdef WB_EMOJI
		int e = emoji_adv16(f, cp);

		if (e >= 0) {
			emoji_draw(f, (x16 + 8) / 16, y, cp, c);
			x16 += e + (e ? track16 : 0);
			continue;
		}
#endif
		const struct wb_glyph *g = glyph(f, cp);
		const uint8_t *a = f->bits + g->off;
		int gx = (x16 + 8) / 16 + g->x, gy = y + g->y;
		int j0 = fb_y0 - gy > 0 ? fb_y0 - gy : 0;	/* the rows in this band */
		int j1 = fb_y1 - gy < g->h ? fb_y1 - gy : g->h;

		for (int j = j0; j < j1; j++)
			for (int i = 0; i < g->w; i++)
				put(gx + i, gy + j, c, a[j * g->w + i]);
		x16 += g->adv16 + track16;
	}
	return (x16 + 8) / 16;
}

/* Top y that centres the font's ascent+descent box on cy, as CSS does. */
static int text_top(const struct wb_font *f, int cy)
{
	return cy - (f->ascent + f->descent) / 2;
}

static void icon(const struct wb_icon *ic, int x, int y, uint32_t c, unsigned opa)
{
	int j0 = fb_y0 - y > 0 ? fb_y0 - y : 0, j1 = fb_y1 - y < ic->h ? fb_y1 - y : ic->h;

	for (int j = j0; j < j1; j++)
		for (int i = 0; i < ic->w; i++)
			put(x + i, y + j, c, ic->a[j * ic->w + i] * opa / 255);
}

/* ------------------------------------------------------------------ */
/* Scrolling lists                                                     */

/*
 * How much of the next card a list shows below the last whole one, so a
 * longer list visibly goes on (and of the card above, once scrolled).
 */
#define LIST_PEEK	28
#define LIST_PEEK_MIN	16	/* less than this does not read as a card */

/*
 * Scroll for a list of rows: ys[i] and hs[i] are each row's offset from the
 * list's top and its height, avail the height the list has. Keeps rows
 * keep..focus in view with LIST_PEEK px of the row after focus under them.
 * *cut is where to cut the rows off, from the list's top: the bottom, or
 * higher when the first row that does not fit would show less than
 * LIST_PEEK_MIN px, so that the row above it is the one that peeks.
 */
static int list_scroll(const int *ys, const int *hs, int n, int focus, int keep,
		       int avail, int *cut)
{
	int scroll = 0;

	*cut = avail;
	if (focus >= 0 && focus < n) {
		int want = ys[focus] + hs[focus];

		if (focus + 1 < n)
			want = ys[focus + 1] + (hs[focus + 1] < LIST_PEEK ? hs[focus + 1] : LIST_PEEK);
		if (want > avail)
			scroll = want - avail;
		if (keep >= 0 && keep <= focus && scroll > ys[keep])
			scroll = ys[keep];
	}
	for (int i = 0; i < n; i++) {
		int shown = avail - (ys[i] - scroll);

		if (ys[i] + hs[i] - scroll <= avail)
			continue;
		if (shown < (hs[i] < LIST_PEEK_MIN ? hs[i] : LIST_PEEK_MIN) && i > 0 && i - 1 != focus &&
		    hs[i - 1] > LIST_PEEK && ys[i - 1] - scroll >= 0)
			*cut = ys[i - 1] - scroll + LIST_PEEK;
		break;
	}
	return scroll;
}

/*
 * Draw only rows [y0, y1) of the screen until clip_end(), on top of the
 * band being drawn. For the cut-off cards of a list.
 */
static int clip_save[2];

static void clip_rows(int y0, int y1)
{
	clip_save[0] = fb_y0;
	clip_save[1] = fb_y1;
	if (y0 > fb_y0)
		fb_y0 = y0;
	if (y1 < fb_y1)
		fb_y1 = y1;
	if (fb_y1 < fb_y0)
		fb_y1 = fb_y0;
}

static void clip_end(void)
{
	fb_y0 = clip_save[0];
	fb_y1 = clip_save[1];
}

/* ------------------------------------------------------------------ */
/* Key caps and the hint bar                                           */

struct hint { const char *keys; const char *label; int right; };

static const struct wb_icon *const keycap_syms[6] = {
	&kc_square, &kc_triangle, &kc_cross, &kc_circle, &kc_cloud, &kc_diamond,
};

static const uint32_t key_colors[6] = {
	WB_COLOR_KEY_SQUARE, WB_COLOR_KEY_TRIANGLE, WB_COLOR_KEY_CROSS,
	WB_COLOR_KEY_CIRCLE, WB_COLOR_KEY_CLOUD, WB_COLOR_KEY_DIAMOND,
};

/* "#1".."#6": one symbol key, in its colour */
static int one_sym(const char *keys)
{
	return keys[0] == '#' && keys[1] >= '1' && keys[1] <= '6' && !keys[2] ? keys[1] - '0' : 0;
}

static int keycap_w(const char *keys)
{
	return !strcmp(keys, "SYMS") ? 6 * 12 + 5 * 5 : one_sym(keys) ? 12 :
	       text_w(&f_mono13, keys, 0);
}

/* "SYMS" stands for the six symbol keys, drawn as icons. */
static int keycap(int x, int cy, const char *keys)
{
	const struct wb_font *f = &f_mono13;
	int sym = !strcmp(keys, "SYMS"), one = one_sym(keys);
	int h = 23, w = keycap_w(keys) + 18, y = cy - h / 2;

	box(x, y, w, h, 6, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	if (sym)
		for (int i = 0; i < 6; i++)
			icon(keycap_syms[i], x + 9 + i * 17, cy - 6, WB_COLOR_TEXT_HI, 255);
	else if (one)
		icon(keycap_syms[one - 1], x + 9, cy - 6, key_colors[one - 1], 255);
	else
		text(f, x + 9, text_top(f, cy), keys, WB_COLOR_TEXT_HI, 0);
	return x + w;
}

static int hint_w(const struct hint *h)
{
	return keycap_w(h->keys) + 18 + 8 + text_w(&f_ui16, h->label, 0);
}

static void hint_bar(const struct hint *hs, int n)
{
	const struct wb_font *f = &f_ui16;
	int y0 = H - WB_HINT_BAR_H, cy = y0 + WB_HINT_BAR_H / 2;
	int x = WB_CHROME_PAD;

	fill(0, y0, W, WB_HINT_BAR_H, WB_COLOR_SURFACE1);
	fill(0, y0, W, 1, WB_COLOR_LINE);
	for (int i = 0; i < n; i++) {
		int hx = hs[i].right ? W - WB_CHROME_PAD - hint_w(&hs[i]) : x;
		int ex = keycap(hx, cy, hs[i].keys);

		ex = text(f, ex + 8, text_top(f, cy), hs[i].label, WB_COLOR_TEXT_MID, 0);
		if (!hs[i].right)
			x = ex + WB_SPACE_4;
	}
}

/* Eyebrow: mono 14, uppercase, 0.1 em tracking, cyan. */
static void eyebrow(int x, int y, const char *s)
{
	text(&f_mono14, x, y, s, WB_COLOR_CYAN, 22);
}

/* ------------------------------------------------------------------ */
/* Pop-ups: help (the diamond key), menus and choices                  */

/*
 * The design's key rules (Docs/06-design-system.md 6.6): the left three
 * symbol keys are the screen's own (square: the context menu; triangle and
 * cross: frequent actions), the right three are the same everywhere:
 * circle the log, cloud the settings, diamond this help.
 */
#define KEY_SYM_MENU	59	/* F1, square */
#define KEY_SYM_MAIN	60	/* F2, triangle */
#define KEY_SYM_SECOND	61	/* F3, cross */
#define KEY_SYM_LOG	62	/* F4, circle */
#define KEY_SYM_SETTINGS 63	/* F5, cloud */
#define KEY_SYM_HELP	64	/* F6, diamond */

#define POP_X	72
#define POP_W	(W - 2 * POP_X)

/* A pop-up's frame over the dimmed screen, as the Dialog's. */
static void pop_frame(int y, int h)
{
	dim(WB_COLOR_CONSOLE, 184);	/* rgba(6,6,13,0.72) */
	box_plain(POP_X, y, POP_W, h, WB_RADIUS_LG, WB_COLOR_SURFACE2);
}

/* s cut to max_w px with an ellipsis, into buf (whole UTF-8 sequences). */
static const char *wb_fit(const struct wb_font *f, const char *s, int max_w, char *buf, int size)
{
	int n;

	if (text_w(f, s, 0) <= max_w)
		return s;
	n = (int)strlen(s);
	if (n > size - 4)
		n = size - 4;
	while (n > 0) {
		memcpy(buf, s, (size_t)n);
		memcpy(buf + n, "\xe2\x80\xa6", 4);
		if (text_w(f, buf, 0) <= max_w)
			return buf;
		n--;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
			n--;
	}
	return "";
}

/*
 * s word-wrapped to w px, lines lh apart from y; at most max lines (the
 * last one cut with an ellipsis). Draws when draw is set. Returns the
 * number of lines.
 */
static int text_wrap(const struct wb_font *f, int x, int y, int w, int lh, const char *s,
		     uint32_t c, int max, int draw)
{
	char line[160], buf[164];
	int n = 0;

	while (*s && n < max) {
		int len = 0, brk = 0;
		const char *p = s;

		while (*p == ' ')
			p++;
		s = p;
		/* the longest run of whole words that fits */
		while (*p && len < (int)sizeof(line) - 1) {
			const char *q = p;

			while (*q && *q != ' ')
				q++;
			if (q - s >= (int)sizeof(line))
				break;
			memcpy(line, s, (size_t)(q - s));
			line[q - s] = 0;
			if (text_w(f, line, 0) > w && brk)
				break;
			brk = len = (int)(q - s);
			p = q;
			while (*p == ' ')
				p++;
		}
		if (!len)	/* a single word wider than w: cut it */
			len = (int)strlen(s) < (int)sizeof(line) - 1 ? (int)strlen(s) : (int)sizeof(line) - 1;
		memcpy(line, s, (size_t)len);
		line[len] = 0;
		s += len;
		if (draw)
			text(f, x, y + n * lh, n == max - 1 && *s ? wb_fit(f, line, w - 20, buf, sizeof(buf)) : line,
			     c, 0);
		n++;
	}
	return n;
}

/* A check mark, 16 px wide, centred on cy: the current choice. */
static void tick(int x, int cy, uint32_t c)
{
	/* two strokes, 2.5 px thick: (0,8)-(5,13) and (5,13)-(16,2) */
	static const int seg[2][4] = { { 0, 8, 5, 13 }, { 5, 13, 16, 2 } };

	for (int py = cy - 8; py <= cy + 8; py++)
		for (int px = x - 2; px <= x + 18; px++) {
			int best = 1 << 30;

			for (int s = 0; s < 2; s++) {
				int ax = x * 16 + seg[s][0] * 16, ay = (cy - 8) * 16 + seg[s][1] * 16;
				int bx = x * 16 + seg[s][2] * 16, by = (cy - 8) * 16 + seg[s][3] * 16;
				int qx = px * 16 + 8, qy = py * 16 + 8, dx = bx - ax, dy = by - ay;
				int len2 = dx * dx + dy * dy, t = ((qx - ax) * dx + (qy - ay) * dy);
				int ex, ey, d2;

				t = t < 0 ? 0 : t > len2 ? len2 : t;
				ex = ax + (int)((long long)dx * t / len2) - qx;
				ey = ay + (int)((long long)dy * t / len2) - qy;
				d2 = ex * ex + ey * ey;
				if (d2 < best)
					best = d2;
			}
			put(px, py, c, cover(isqrt((unsigned)best) - 20));
		}
}

/*
 * A thin scroll bar for a list of total px of which shown px are in view,
 * from first px on: at x, from y, h high. Nothing when it all fits.
 */
static void scrollbar(int x, int y, int h, int total, int shown, int first)
{
	int th, ty;

	if (total <= shown || total <= 0)
		return;
	th = h * shown / total;
	th = th < 24 ? 24 : th;
	ty = y + (h - th) * first / (total - shown);
	box(x, y, 4, h, 2, WB_COLOR_LINE, 255, 0, 0, 0);
	box(x, ty, 4, th, 2, WB_COLOR_TEXT_LOW, 255, 0, 0, 0);
}

/*
 * The help pop-up (diamond key, 6.6 "Help"): "HELP" with the diamond, the
 * screen's name, one or two sentences about it, then every key that does
 * something here with one line each. Only an explanation: nothing in it
 * can be selected; Esc or the diamond key closes it.
 */
struct help_key { const char *keys, *what; };

#define HELP_ROW	36

static void help_popup(const char *screen, const char *about, const struct help_key *k, int n)
{
	static const struct hint hints[] = { { "Esc", "close", 1 } };
	int tw = POP_W - 64, lines = about ? text_wrap(&f_ui20, 0, 0, tw, 28, about, 0, 3, 0) : 0;
	int kw = 0, h, y, x = POP_X + 32;
	char buf[96];

	for (int i = 0; i < n; i++)
		if (keycap_w(k[i].keys) + 18 > kw)
			kw = keycap_w(k[i].keys) + 18;
	if (n > 12)
		n = 12;
	h = 32 + 18 + 6 + 36 + 16 + lines * 28 + 24 + 17 + 12 + n * HELP_ROW + 24;
	y = WB_STATUS_BAR_H + (H - WB_STATUS_BAR_H - WB_HINT_BAR_H - h) / 2;
	pop_frame(y, h);
	y += 32;
	icon(&kc_diamond, x, y + 1, WB_COLOR_KEY_DIAMOND, 255);
	eyebrow(x + kc_diamond.w + 10, y, "HELP");
	text(&f_ui28b, x, y + 18 + 6 + 2, wb_fit(&f_ui28b, screen, tw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	y += 18 + 6 + 36 + 16;
	if (about)
		text_wrap(&f_ui20, x, y, tw, 28, about, WB_COLOR_TEXT_MID, 3, 1);
	y += lines * 28 + 24;
	text(&f_mono13, x, y, "KEYS ON THIS SCREEN", WB_COLOR_TEXT_LOW, 21);
	y += 17 + 12;
	for (int i = 0; i < n; i++, y += HELP_ROW) {
		int cy = y + HELP_ROW / 2;

		keycap(x, cy, k[i].keys);
		text(&f_ui18, x + kw + 16, text_top(&f_ui18, cy),
		     wb_fit(&f_ui18, k[i].what, tw - kw - 16, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	}
	hint_bar(hints, 1);
}

/*
 * A pop-up list (6.6): the square key's context menu, or a choice from an
 * enumeration. Items may start a group under a line (sep), be
 * destructive (danger: red, last), carry a value on the right in mono
 * (a time zone's offset), a smaller word after the label (its region) and
 * a colour swatch. The current choice (cur) has a check mark. With a
 * filter (not 0), the typed text is shown in a field above the list:
 * filter_hint while it is empty.
 */
struct menu_item {
	const char *label, *sub;
	char value[16];
	int sep, danger;
	int swatch;		/* 1 a colour (sw_c), 2 a rainbow */
	uint32_t sw_c;
};

#define MENU_ROW	56
#define MENU_GAP	8
#define MENU_SEP	17	/* a line between two groups */
#define MENU_VIS	7	/* rows in view */

static void menu_swatch(int x, int cy, int kind, uint32_t c)
{
	static const uint32_t hues[6] = { 0xff0000, 0xffc800, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00c0 };

	if (kind == 1) {
		box(x, cy - 11, 22, 22, 6, c, 255, WB_COLOR_LINE, 1, 0);
		return;
	}
	for (int i = 0; i < 6; i++)
		fill(x + 1 + i * 20 / 6, cy - 10, 20 / 6 + 1, 20, hues[i]);
	box(x, cy - 11, 22, 22, 6, 0, 0, WB_COLOR_LINE, 1, 0);
}

/* items[map[i]] for i < n (map 0: all in order); focus and cur index the
 * shown list. count: "78 options", shown on the eyebrow's line. */
static void menu_popup(const char *eb, const char *title, const char *count,
		       const struct menu_item *items, const int *map, int n, int focus, int cur,
		       const char *filter, const char *filter_hint, const struct hint *hs, int nh)
{
	int ys[96], rh[96], yy = 0, list_h, h, y, x0 = POP_X + 32, w = POP_W - 64;
	int scroll, cut, top;
	char buf[96];

	if (n > 96)
		n = 96;
	for (int i = 0; i < n; i++) {
		const struct menu_item *it = &items[map ? map[i] : i];

		if (i && it->sep)
			yy += MENU_SEP;
		ys[i] = yy;
		rh[i] = MENU_ROW;
		yy += MENU_ROW + MENU_GAP;
	}
	/* A long list (or one that filters) keeps one height: as many rows
	 * as fit between the bars, MENU_VIS at most; the last one peeks when
	 * there are more. */
	{
		int head = 32 + 18 + 6 + 36 + 24 + (filter ? 52 + 16 : 0) + 32;
		int room = H - WB_STATUS_BAR_H - WB_HINT_BAR_H - 2 * 12 - head;
		int rows = (room + MENU_GAP) / (MENU_ROW + MENU_GAP);

		rows = rows > MENU_VIS ? MENU_VIS : rows;
		list_h = n ? yy - MENU_GAP : MENU_ROW;
		if (filter || n > rows)
			list_h = rows * (MENU_ROW + MENU_GAP) - MENU_GAP;
		h = head + list_h;
	}
	y = WB_STATUS_BAR_H + (H - WB_STATUS_BAR_H - WB_HINT_BAR_H - h) / 2;
	pop_frame(y, h);
	eyebrow(x0, y + 32, eb);
	if (count)
		text(&f_mono13, x0 + w - text_w(&f_mono13, count, 0), y + 33, count, WB_COLOR_TEXT_LOW, 0);
	text(&f_ui28b, x0, y + 32 + 18 + 6 + 2, wb_fit(&f_ui28b, title, w, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, 0);
	top = y + 32 + 18 + 6 + 36 + 24;
	if (filter) {
		int tx;

		box_plain(x0, top, w, 52, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		if (filter[0]) {
			tx = text(&f_mono18, x0 + 20, text_top(&f_mono18, top + 26), filter, WB_COLOR_TEXT_HI, 0);
		} else {
			text(&f_ui18, x0 + 20 + 8, text_top(&f_ui18, top + 26), filter_hint, WB_COLOR_TEXT_LOW, 0);
			tx = x0 + 20;
		}
		fill(tx + 2, top + 14, 2, 24, WB_COLOR_CYAN);
		top += 52 + 16;
	}
	if (!n)
		text(&f_ui18, x0 + 4, text_top(&f_ui18, top + MENU_ROW / 2), "Nothing matches",
		     WB_COLOR_TEXT_LOW, 0);
	scroll = list_scroll(ys, rh, n, focus, focus, list_h, &cut);
	clip_rows(top, top + cut);
	for (int i = 0; i < n; i++) {
		const struct menu_item *it = &items[map ? map[i] : i];
		int ry = top + ys[i] - scroll, cy = ry + MENU_ROW / 2, x = x0 + 20, rx = x0 + w - 20;
		uint32_t lc = it->danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI;

		if (ry + MENU_ROW <= top || ry >= top + cut)
			continue;
		if (i && it->sep)
			fill(x0 + 8, ry - MENU_SEP / 2 - MENU_GAP / 2, w - 16, 1, WB_COLOR_LINE);
		if (i == focus)
			box_focus(x0, ry, w, MENU_ROW, WB_RADIUS_MD, &GLOW_ROW);
		else
			box_plain(x0, ry, w, MENU_ROW, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		if (i == cur) {
			tick(rx - 16, cy, i == focus ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID);
			rx -= 16 + 14;
		}
		if (it->value[0]) {
			rx -= text_w(&f_mono16b, it->value, 0);
			text(&f_mono16b, rx, text_top(&f_mono16b, cy), it->value,
			     i == focus ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
			rx -= 16;
		}
		if (it->swatch) {
			menu_swatch(x, cy, it->swatch, it->sw_c);
			x += 22 + 16;
		}
		x = text(&f_ui22, x, text_top(&f_ui22, cy), wb_fit(&f_ui22, it->label, rx - x, buf, sizeof(buf)),
			 lc, 0);
		if (it->sub && x + 12 < rx)
			text(&f_ui16, x + 12, text_top(&f_ui16, cy), wb_fit(&f_ui16, it->sub, rx - x - 12, buf, sizeof(buf)),
			     WB_COLOR_TEXT_LOW, 0);
	}
	clip_end();
	if (n && yy - MENU_GAP > list_h)
		scrollbar(x0 + w + 12, top, list_h, yy - MENU_GAP, list_h, scroll);
	hint_bar(hs, nh);
}

/*
 * The log (circle key) or the settings (cloud key) of an app that has none:
 * the screen still opens, with one line saying so (6.6). Draw the status
 * bar after it.
 */
static void empty_screen(const char *eb, const char *title, const char *line)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int top = WB_STATUS_BAR_H + 20;

	fill(0, 0, W, H, WB_COLOR_VOID);
	eyebrow(WB_CONTENT_PAD, top, eb);
	text(&f_ui36b, WB_CONTENT_PAD, top + 18 + 6, title, WB_COLOR_TEXT_HI, -11);
	text(&f_ui20, WB_CONTENT_PAD, top + 18 + 6 + 44 + 24, line, WB_COLOR_TEXT_LOW, 0);
	hint_bar(hints, 1);
}

