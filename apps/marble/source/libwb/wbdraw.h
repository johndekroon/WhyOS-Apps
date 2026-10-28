/* SPDX-License-Identifier: MIT */
/*
 * Drawing for the badge's panel in the WhyBadge design's style, the base
 * of libwb: alpha-blended pixels, boxes with the focus glow, text in the
 * pre-rendered fonts and icons of wb_assets.h, key caps and the hint bar.
 * Integer-only (the toolchain is soft-float).
 *
 * Programs include wb.h, which includes this after wb_assets.h and
 * wb_tokens.h, with W and H defined. Unused functions are fine: they are
 * static.
 *
 * A program may define before including: WB_SQUARE (the largest corner
 * radius: home's canvas has square corners, every radius 4), WB_DIM (the
 * colour and alpha of a pop-up's backdrop) and the hooks WB_PANEL,
 * WB_CARD and WB_HINT_GLASS (below).
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
/* 1: fb holds the whole panel even while fb_y0..fb_y1 is one band (home
 * redrawing part of its frame), so a frosted pop-up may read around it */
static int fb_whole;

/*
 * Recording a frame (wb_rec): a program drawing in bands first runs its
 * drawing once with wb_rec set, which draws nothing but hashes every call
 * into the strips of 8 rows it touches; then it draws only the bands whose
 * strips changed since the frame before (wb_rec_dirty). A list whose focus
 * moved draws two rows' bands, not the screen. What is drawn depends only
 * on the calls, in order, that touch a row, so equal hashes are equal
 * pixels. A program that writes fb itself hashes that with wb_rec_rows().
 * WB_FULL=1 in the environment draws every band (to compare in tests).
 */
#define WB_STRIP	8
#define WB_STRIPS	((H + WB_STRIP - 1) / WB_STRIP)

static int wb_rec;
static uint32_t wb_rec_h[WB_STRIPS], wb_rec_last[WB_STRIPS];
static uint32_t wb_rec_run;	/* every call so far this frame */
static int wb_rec_all = 1;	/* the next frame: every band */

static void wb_rec_rows(int y0, int y1, const void *k, size_t n)
{
	const unsigned char *p = k;
	uint32_t kh = 2166136261u;

	if (y0 < fb_y0)		/* fb_y0..fb_y1: clip_rows() while recording */
		y0 = fb_y0;
	if (y1 > fb_y1)
		y1 = fb_y1;
	if (y0 >= y1)
		return;
	for (size_t i = 0; i < n; i++)
		kh = (kh ^ p[i]) * 16777619u;
	kh ^= (uint32_t)y0 * 2654435761u ^ (uint32_t)y1 * 40503u;
	wb_rec_run = (wb_rec_run ^ kh) * 16777619u;
	for (int s = y0 / WB_STRIP; s <= (y1 - 1) / WB_STRIP; s++)
		wb_rec_h[s] = (wb_rec_h[s] ^ kh) * 16777619u + 0x9e3779b9u;
}

/* A frame's recording: start it, draw, end it; then the bands to draw. */
static void wb_rec_begin(void)
{
	memset(wb_rec_h, 0, sizeof(wb_rec_h));
	wb_rec_run = 0;
	wb_rec = 1;
	fb_y0 = 0;
	fb_y1 = H;
}

static void wb_rec_end(void)
{
	static int env = -1;

	wb_rec = 0;
	if (env < 0)
		env = getenv("WB_FULL") != 0;
	wb_rec_all |= env;
}

/* Do rows [y0, y1) differ from the frame drawn before? */
static int wb_rec_dirty(int y0, int y1)
{
	if (wb_rec_all)
		return 1;
	for (int s = y0 / WB_STRIP; s <= (y1 - 1) / WB_STRIP && s < WB_STRIPS; s++)
		if (wb_rec_h[s] != wb_rec_last[s])
			return 1;
	return 0;
}

/* The frame is on the panel: the next one is compared with it. */
static void wb_rec_done(void)
{
	memcpy(wb_rec_last, wb_rec_h, sizeof(wb_rec_last));
	wb_rec_all = 0;
}

/* Something else drew on the panel (a whole frame, a message): the next
 * frame draws every band. */
static void wb_rec_invalidate(void)
{
	wb_rec_all = 1;
}

/* a pointer as part of a call's key */
#define WB_P(p)		((int)(intptr_t)(p))

static inline uint16_t pack565(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t rgb(uint32_t c)
{
	return pack565((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

/* x / 255 for x up to 65534 (two 8-bit values multiplied), without a
 * division: the badge's divide takes tens of cycles a time. */
static inline unsigned div255(unsigned x)
{
	return (x + 1 + (x >> 8)) >> 8;
}

/* Blend colour c (0xRRGGBB) over pixel (x, y) with alpha a (0..255). */
static void put(int x, int y, uint32_t c, unsigned a)
{
	uint16_t *p, d;
	unsigned dr, dg, db, na;

	if ((unsigned)x >= W || y < fb_y0 || y >= fb_y1 || a == 0)
		return;
	if (wb_rec) {
		int k[] = { 1, x, (int)c, (int)a };

		wb_rec_rows(y, y + 1, k, sizeof(k));
		return;
	}
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
	*p = pack565(div255(((c >> 16) & 255) * a + dr * na),
		     div255(((c >> 8) & 255) * a + dg * na),
		     div255((c & 255) * a + db * na));
}

static void fill(int x, int y, int w, int h, uint32_t c)
{
	uint16_t v = rgb(c);

	if (wb_rec) {
		int k[] = { 2, x, w, (int)c };

		wb_rec_rows(y, y + h, k, sizeof(k));
		return;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < fb_y0) { h -= fb_y0 - y; y = fb_y0; }
	if (x + w > W) w = W - x;
	if (y + h > fb_y1) h = fb_y1 - y;
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			fb[(y + j) * W + x + i] = v;
}

/*
 * put() along row y from x0 to x1 (not included): the same blend, looked
 * up per channel value from tables made once for the colour and alpha (a
 * large box at an alpha costs a table look-up a pixel, not three
 * multiplications and divisions).
 */
static void put_span(int x0, int x1, int y, uint32_t c, unsigned a)
{
	static uint16_t lr[32], lg[64], lb[32];
	static uint32_t lc;
	static unsigned la;	/* 0: no tables yet */
	uint16_t *p;

	if (a == 0 || y < fb_y0 || y >= fb_y1)
		return;
	if (wb_rec) {
		int k[] = { 3, x0, x1, (int)c, (int)a };

		wb_rec_rows(y, y + 1, k, sizeof(k));
		return;
	}
	x0 = x0 < 0 ? 0 : x0;
	x1 = x1 > W ? W : x1;
	if (x0 >= x1)
		return;
	if (a >= 255) {
		fill(x0, y, x1 - x0, 1, c);
		return;
	}
	if (a != la || c != lc) {
		unsigned na = 255 - a;

		for (unsigned i = 0; i < 64; i++) {
			unsigned e5 = (i & 31) << 3 | (i & 31) >> 2, e6 = i << 2 | i >> 4;

			if (i < 32) {
				lr[i] = pack565(div255(((c >> 16) & 255) * a + e5 * na), 0, 0);
				lb[i] = pack565(0, 0, div255((c & 255) * a + e5 * na));
			}
			lg[i] = pack565(0, div255(((c >> 8) & 255) * a + e6 * na), 0);
		}
		la = a;
		lc = c;
	}
	for (p = &fb[y * W + x0]; x0 < x1; x0++, p++) {
		uint16_t d = *p;

		*p = lr[d >> 11] | lg[(d >> 5) & 63] | lb[d & 31];
	}
}

/* Darken everything (dialog backdrop). */
static void dim(uint32_t c, unsigned a)
{
	/* put()'s blend per channel value, looked up: the same result */
	uint16_t lr[32], lg[64], lb[32];
	unsigned na = 255 - a;

	if (wb_rec) {
		int k[] = { 4, (int)c, (int)a };

		wb_rec_rows(0, H, k, sizeof(k));
		return;
	}
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
#ifdef WB_SQUARE
	if (r > WB_SQUARE)
		r = WB_SQUARE;
#endif
	/* Between the two corner arcs (m0..m1) a row is the same everywhere:
	 * worked out once, at the middle (exact while r >= 2 bw, as for
	 * every box the design has). */
	int m0 = x + r + 1, m1 = x + w - r - 1, uni = r >= 2 * bw && m0 < m1;

	if (wb_rec) {
		int k[] = { 5, x, y, w, h, r, (int)fill_c, (int)fill_a, (int)border_c, bw, WB_P(g) };

		wb_rec_rows(y - ext, y + h + ext, k, sizeof(k));
		return;
	}
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
				put_span(m0, m1, py, WB_COLOR_CYAN, ga);
				put_span(m0, m1, py, fill_c, fa);
				put_span(m0, m1, py, border_c, ba);
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

/* A pop-up's panel and a card (a row, an option): plain boxes, or what the
 * program puts in their place (WB_PANEL, WB_CARD return 1): home draws
 * glass over its scene. */
static void wb_panel(int x, int y, int w, int h, int r)
{
#ifdef WB_PANEL
	if (WB_PANEL(x, y, w, h, r))
		return;
#endif
	box_plain(x, y, w, h, r, WB_COLOR_SURFACE2);
}

static void wb_card(int x, int y, int w, int h, int r, int on, const struct glow *g)
{
#ifdef WB_CARD
	if (WB_CARD(x, y, w, h, r, on, g))
		return;
#endif
	if (on)
		box_focus(x, y, w, h, r, g);
	else
		box_plain(x, y, w, h, r, WB_COLOR_SURFACE1);
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

	if (wb_rec) {		/* the rows its glyphs take; then where it ends */
		int k[] = { 6, WB_P(f), x, y, (int)c, track16 }, y0 = y, y1 = y + 1;
		const char *t = s;

		while (*t) {
			unsigned cp = next_cp(&t);
			const struct wb_glyph *g;
#ifdef WB_EMOJI
			int e = emoji_adv16(f, cp);

			if (e >= 0) {
				y0 = y - WB_EMOJI_SIZE < y0 ? y - WB_EMOJI_SIZE : y0;
				y1 = y + f->ascent + WB_EMOJI_SIZE > y1 ? y + f->ascent + WB_EMOJI_SIZE : y1;
				x16 += e + (e ? track16 : 0);
				continue;
			}
#endif
			g = glyph(f, cp);
			y0 = y + g->y < y0 ? y + g->y : y0;
			y1 = y + g->y + g->h > y1 ? y + g->y + g->h : y1;
			x16 += g->adv16 + track16;
		}
		wb_rec_rows(y0, y1, k, sizeof(k));
		wb_rec_rows(y0, y1, s, strlen(s));
		return (x16 + 8) / 16;
	}
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

	if (wb_rec) {
		int k[] = { 7, WB_P(ic), x, (int)c, (int)opa };

		wb_rec_rows(y, y + ic->h, k, sizeof(k));
		return;
	}
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

#ifdef WB_HINT_GLASS	/* home: glass over its scene */
	WB_HINT_GLASS(y0);
#else
	fill(0, y0, W, WB_HINT_BAR_H, WB_COLOR_SURFACE1);
	fill(0, y0, W, 1, WB_COLOR_LINE);
#endif
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

#ifndef POP_X
#define POP_X	72
#endif
#define POP_W	(W - 2 * POP_X)
#ifndef WB_DIM
#define WB_DIM	WB_COLOR_CONSOLE, 184	/* rgba(6,6,13,0.72) */
#endif

/* A pop-up's frame over the dimmed screen, as the Dialog's. */
static void pop_frame(int y, int h)
{
	dim(WB_DIM);
	wb_panel(POP_X, y, POP_W, h, WB_RADIUS_LG);
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

static void panel_hints(int x0, int w, int top, const struct hint *hs, int nh);	/* wbui.h */

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
	h = 32 + 18 + 6 + 36 + 16 + lines * 28 + 24 + 17 + 12 + n * HELP_ROW + 14 + 1 + 10 + 23 + 16;
	y = (H - h) / 2;
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
	/* its key inside the panel, as every pop-up has */
	panel_hints(x, tw, y + 14, hints, 1);
}

/*
 * A pop-up list (6.6): the square key's context menu, or a choice from an
 * enumeration. Items may start a group under a line (sep), be
 * destructive (danger: red, last), carry a value on the right in mono
 * (a time zone's offset), a smaller word after the label (its region) and
 * a colour swatch. Drawn by menu_popup() (wbui.h): the present choice
 * (cur) has its box ticked; cur -1 is a choice whose present one is not in
 * view, -2 a menu (no boxes). With a filter (not 0), the typed text is
 * shown in a field above the list: filter_hint while it is empty.
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

