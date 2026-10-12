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

	if (cp >= 0xe0 && p[0] && p[1]) {
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

/* Width in px; track16 is extra letter spacing in 1/16 px. */
static int text_w(const struct wb_font *f, const char *s, int track16)
{
	int x16 = 0;

	while (*s)
		x16 += glyph(f, next_cp(&s))->adv16 + track16;
	return (x16 - (x16 ? track16 : 0) + 8) / 16;
}

/* Draw with the line's top at y (top of the font's ascent); returns end x. */
static int text(const struct wb_font *f, int x, int y, const char *s,
		uint32_t c, int track16)
{
	int x16 = x * 16;

	while (*s) {
		const struct wb_glyph *g = glyph(f, next_cp(&s));
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

