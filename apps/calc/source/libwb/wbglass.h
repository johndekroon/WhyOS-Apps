/* SPDX-License-Identifier: MIT */
/*
 * Glass (the canvas "Menu's met frontpaneel"): boxes of the void over what
 * is drawn, and frosted pop-ups over a blurred copy of what is under them;
 * the focus's glow; box() from a shape worked out once (box_shaped()).
 * Part of libwb: include wb.h, and call wb_init() once before drawing.
 */
#pragma once

static long tm_glass;		/* us spent in glass(), for a program's timing */

/* The focus's glow on the canvas: 18 px at 55 %, from home_assets.h's
 * 18 px at 40 % (glow_init()). */
static uint8_t glow_focus[sizeof(glow_row)];
static const struct glow GLOW_FOCUS = { glow_focus, sizeof(glow_focus) };
/* a pop-up option's: 14 px at 35 % */
static uint8_t glow_pop[sizeof(glow_row) * 14 / 18];
static const struct glow GLOW_POP = { glow_pop, sizeof(glow_pop) };

static void glow_init(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	for (unsigned i = 0; i < sizeof(glow_focus); i++)
		glow_focus[i] = (uint8_t)(glow_row[i] * 55 / 40 > 255 ? 255 : glow_row[i] * 55 / 40);
	for (unsigned i = 0; i < sizeof(glow_pop); i++)
		glow_pop[i] = (uint8_t)(glow_row[i * 18 / 14] * 35 / 40);
}


/* ------------------------------------------------------------------ */
/* Glass: boxes of the void over the scene; pop-ups over a blurred copy  */

/*
 * The canvas (2026-10-27): tiles, rows, the hint bar and the temperature
 * are the void at 85 % over the scene, unblurred, with a faint sheen from
 * the top and a light edge; on the badge they are one flat colour (see
 * glass()). Only the pop-ups (the Apps panel, dialogs, the choices) are
 * frosted: the void at 60 % over what is under them blurred
 * (18 px) and a little more saturated. For those glass_backdrop() blurs
 * the drawn picture under the pop-up into gb first: averaged into cells of
 * GLASS_DS x GLASS_DS pixels, three box blurs of 9 cells across and down
 * (sigma about 18 px), read back bilinearly.
 */
#define GLASS_DS	4
#define GB_W		(W / GLASS_DS)
#define GB_H		(H / GLASS_DS)

enum { GLASS_TILE, GLASS_HOME, GLASS_FOCUS, GLASS_BAR, GLASS_PANEL, GLASS_APPS };

/*
 * Per kind (the canvas's CSS): the void's alpha; the sheen's colour, its
 * alpha at the top and at at_pct % of the height, falling to 0 at the
 * bottom; the 1 px edge (white); the inset highlights inside the top and
 * the bottom edge (inset is the row the top one is on: inside a 2 px
 * border for the focus); and whether it is frosted.
 */
static const struct glass_style {
	uint8_t tint, s0, s1, at_pct, edge, hi, lo, inset, frosted;
	uint32_t sheen;
} glass_styles[] = {
	[GLASS_TILE]  = { 217, 18, 5, 50, 66, 51, 0, 1, 0, 0xFFFFFF },	/* rows */
	[GLASS_HOME]  = { 217, 18, 5, 45, 66, 61, 13, 1, 0, 0xFFFFFF },	/* home tiles, the pill */
	[GLASS_FOCUS] = { 217, 71, 26, 50, 0, 71, 0, 2, 0, WB_COLOR_CYAN },
	[GLASS_BAR]   = { 217, 10, 5, 50, 56, 0, 0, 1, 0, 0xFFFFFF },	/* edge: the top line */
	[GLASS_PANEL] = { 153, 20, 8, 45, 51, 66, 0, 1, 1, 0xFFFFFF },	/* pop-ups, dialogs */
	[GLASS_APPS]  = { 153, 26, 8, 40, 71, 77, 0, 1, 1, 0xFFFFFF },	/* the Apps panel */
};

/* the blurred backdrop, 8 bits a channel: 97 KB, taken at the first
 * frosted pop-up (a program drawing in bands never needs it) */
static uint8_t (*gb)[GB_H][GB_W];
static int gb_ok;			/* gb holds the backdrop of this frame's pop-up */
static uint32_t wb_gb_want, wb_gb_have;	/* wb_glass_panel() */
static int wb_rec_whole;	/* recording a frame drawn whole: frosted */
static int wb_rec_drawing;	/* drawing a recorded frame (wb_frame) */
static int wb_rec_panels;	/* frosted panels in the frame recorded */
static short gb_u0[W], gb_u1[W];	/* screen column -> its two cells */
static uint8_t gb_fu[W];		/* and the share of the second, in eighths */
static int scene_shown;			/* this frame has the scene under it */

/* Where pixel i lies between two cells (n cells): u0, u1 and the share of
 * u1 in eighths. */
static void glass_pos(int i, int n, short *u0, short *u1, uint8_t *f)
{
	int p8 = 2 * i + 1 - GLASS_DS;		/* (i + 0.5) / 4 - 0.5, in eighths */
	int u = p8 >> 3;

	*f = (uint8_t)(p8 - 8 * u);
	if (u < 0) {
		u = 0;
		*f = 0;
	}
	*u0 = (short)(u < n ? u : n - 1);
	*u1 = (short)(u + 1 < n ? u + 1 : n - 1);
}

/* Three box blurs of 9 cells over t[0..n), in place (edges repeat). */
static void blur9(uint8_t *t, int n, int step)
{
	uint8_t c[GB_W > GB_H ? GB_W : GB_H];

	for (int pass = 0; pass < 3; pass++) {
		unsigned sum = 0;

		for (int i = 0; i < n; i++)
			c[i] = t[i * step];
		for (int i = -4; i <= 4; i++)
			sum += c[i < 0 ? 0 : i >= n ? n - 1 : i];
		for (int i = 0; i < n; i++) {
			t[i * step] = (uint8_t)(sum * 7282u >> 16);	/* / 9 */
			sum += c[i + 5 < n ? i + 5 : n - 1];
			sum -= c[i - 4 > 0 ? i - 4 : 0];
		}
	}
}

/*
 * What is drawn now under a pop-up at x, y, w, h, blurred into gb, with
 * the CSS saturate(1.4): cells reaching 48 px past the box (the blur's
 * reach), the rest of gb as it was. A cell is the mean of 4 of its 16
 * pixels (blurred by 18 px, the rest adds nothing one can see). dim_a:
 * the void over it at that alpha first, for a backdrop not dimmed in fb.
 */
static void glass_backdrop_dim(int x, int y, int w, int h, unsigned dim_a);

static void glass_backdrop(int x, int y, int w, int h)
{
	glass_backdrop_dim(x, y, w, h, 0);
}

static void glass_backdrop_dim(int x, int y, int w, int h, unsigned dim_a)
{
	int u0 = (x - 48) / GLASS_DS, u1 = (x + w + 48 + GLASS_DS - 1) / GLASS_DS;
	int v0 = (y - 48) / GLASS_DS, v1 = (y + h + 48 + GLASS_DS - 1) / GLASS_DS;
	long t0 = wb_now_us();

	if (wb_rec)		/* it reads the frame: nothing to record */
		return;
	if (!gb && !(gb = calloc(3, sizeof(*gb)))) {
		gb_ok = 0;	/* no memory: the pop-up is unblurred glass */
		return;
	}
	u0 = u0 < 0 ? 0 : u0;
	v0 = v0 < 0 ? 0 : v0;
	u1 = u1 > GB_W ? GB_W : u1;
	v1 = v1 > GB_H ? GB_H : v1;
	for (int v = v0; v < v1; v++)
		for (int u = u0; u < u1; u++) {
			unsigned s[3] = { 0, 0, 0 }, c[3];
			int lum;

			for (int j = 0; j < GLASS_DS; j += 2)
				for (int i = 0; i < GLASS_DS; i += 2) {
					rgb565_split(fb[(v * GLASS_DS + j + (i >> 1)) * W + u * GLASS_DS + i + (j >> 1)],
						     &c[0], &c[1], &c[2]);
					s[0] += c[0];
					s[1] += c[1];
					s[2] += c[2];
				}
			for (int k = 0; k < 3; k++) {
				s[k] /= 4;
				if (dim_a)
					s[k] = div255(s[k] * (255 - dim_a) + ((WB_COLOR_VOID >> (16 - 8 * k)) & 255) * dim_a);
			}
			/* saturate(1.4): away from the grey of the same luminance */
			lum = (int)(s[0] * 213 + s[1] * 715 + s[2] * 72) / 1000;
			for (int k = 0; k < 3; k++) {
				int ch = (int)s[k];

				ch = lum + (ch - lum) * 7 / 5;
				gb[k][v][u] = (uint8_t)(ch < 0 ? 0 : ch > 255 ? 255 : ch);
			}
		}
	for (int k = 0; k < 3; k++) {
		for (int v = v0; v < v1; v++)
			blur9(&gb[k][v][u0], u1 - u0, 1);
		for (int u = u0; u < u1; u++)
			blur9(&gb[k][v0][u], v1 - v0, GB_W);
	}
	if (!gb_fu[W - 1])
		for (int i = 0; i < W; i++)
			glass_pos(i, GB_W, &gb_u0[i], &gb_u1[i], &gb_fu[i]);
	gb_ok = 1;
	tm_glass += wb_now_us() - t0;
}

/* One channel of colour c over v at alpha a (0..255). */
static inline unsigned over8(unsigned v, uint32_t c, int sh, unsigned a)
{
	return div255(((c >> sh) & 255) * a + v * (255 - a));
}

/* A pixel of plain glass: x 2048 a channel, plus the dither (o5 for red
 * and blue, o6 for green), in RGB565. */
static inline uint32_t glass_565(int32_t r, int32_t g, int32_t b, unsigned o5, unsigned o6)
{
	unsigned rr = (unsigned)(r >> 3) + o5, gg = (unsigned)(g >> 3) + o6, bb = (unsigned)(b >> 3) + o5;

	return (rr & 0xF800) | (gg >> 5 & 0x7E0) | bb >> 11;
}

/* This row of the blurred backdrop between its two cell rows (v0 and v1,
 * fv eighths of the way), cell u, x8. */
#define GB_ROW(c, u)	(gb[c][v0][u] * (8 - fv) + gb[c][v1][u] * fv)

/*
 * The plain glass of one row over the backdrop, columns x0..x1 of fb row
 * q: the blurred backdrop of this row tinted per cell (v * k + m), read
 * between cells and dithered into RGB565. As glass() below, within a
 * level; a fraction of its cost a pixel.
 */
static void glass_span(uint16_t *q, int x0, int x1, const uint8_t *dth,
		       int v0, int v1, int fv, unsigned k16, const unsigned *m16)
{
	static int32_t cv[GB_W + 1][3];		/* per cell, 8.8 */
	int ua = gb_u0[x0], ub = gb_u1[x1 - 1];
	unsigned o5[4], o6[4], mr = m16[0] >> 8, mg = m16[1] >> 8, mb = m16[2] >> 8;

	/* kept below where the dither would carry into the next channel (no
	 * clamp per pixel) */
	for (int u = ua; u <= ub; u++) {
		int32_t r = (int32_t)((GB_ROW(0, u) * k16 >> 11) + mr);
		int32_t g = (int32_t)((GB_ROW(1, u) * k16 >> 11) + mg);
		int32_t b = (int32_t)((GB_ROW(2, u) * k16 >> 11) + mb);

		cv[u][0] = r > 63487 ? 63487 : r;
		cv[u][1] = g > 64575 ? 64575 : g;
		cv[u][2] = b > 63487 ? 63487 : b;
	}
	cv[ub + 1][0] = cv[ub][0];		/* no step past the last cell */
	cv[ub + 1][1] = cv[ub][1];
	cv[ub + 1][2] = cv[ub][2];
	for (int p = 0; p < 4; p++) {
		o5[p] = dth[p] * 128u;
		o6[p] = dth[p] * 64u;
	}
	/* a cell at a time: from column 2 on, cell u covers columns 4u + 2 to
	 * 4u + 5 at 1, 3, 5 and 7 eighths of the way to the next (glass_pos) */
	for (int px = x0; px < x1;) {
		int u = gb_u0[px], f = gb_fu[px], stop = px < 2 ? 2 : 4 * u + 6, st = px < 2 ? 0 : 2;
		const int32_t *a = cv[u], *n = cv[u < ub ? u + 1 : ub + 1];
		int32_t dr = n[0] - a[0], dg = n[1] - a[1], db = n[2] - a[2];
		int32_t vr = a[0] * 8 + dr * f, sr = dr * st;
		int32_t vg = a[1] * 8 + dg * f, sg = dg * st;
		int32_t vb = a[2] * 8 + db * f, sb = db * st;

		if (stop > x1)
			stop = x1;
		if (stop - px == 4 && px == 4 * u + 2) {	/* a whole cell: two words */
			uint32_t p0 = glass_565(vr, vg, vb, o5[2], o6[2]);
			uint32_t p1 = glass_565(vr + sr, vg + sg, vb + sb, o5[3], o6[3]);
			uint32_t p2 = glass_565(vr + 2 * sr, vg + 2 * sg, vb + 2 * sb, o5[0], o6[0]);
			uint32_t p3 = glass_565(vr + 3 * sr, vg + 3 * sg, vb + 3 * sb, o5[1], o6[1]);

			*(u32_alias *)&q[px] = p0 | p1 << 16;
			*(u32_alias *)&q[px + 2] = p2 | p3 << 16;
			px += 4;
			continue;
		}
		for (; px < stop; px++) {
			q[px] = (uint16_t)glass_565(vr, vg, vb, o5[px & 3], o6[px & 3]);
			vr += sr;
			vg += sg;
			vb += sb;
		}
	}
}

/* The same over what is drawn in fb (unfrosted glass): v * k + m a pixel. */
static void glass_span_fb(uint16_t *q, int x0, int x1, const uint8_t *dth,
			  unsigned k16, const unsigned *m16)
{
	for (int px = x0; px < x1; px++) {
		unsigned r, g, b, d = dth[px & 3];

		rgb565_split(q[px], &r, &g, &b);
		r = ((r * k16 + m16[0] + 32768) >> 16) + d / 2;
		g = ((g * k16 + m16[1] + 32768) >> 16) + d / 4;
		b = ((b * k16 + m16[2] + 32768) >> 16) + d / 2;
		r >>= 3;
		g >>= 2;
		b >>= 3;
		q[px] = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
	}
}

/* A pixel's cover (a) and white edge plus highlights (e) in a box of glass
 * of style st, 0..255 each. */
static void glass_ae(uint8_t *ae, int px, int py, int x, int y, int w, int h, int r,
		     const struct glass_style *st)
{
	unsigned a = cover(rr_dist(px, py, x, y, w, h, r)), e = 0;
	int b = st->inset, rb = r > b ? r - b : 0;

	if (st->edge) {
		unsigned ai = cover(rr_dist(px, py, x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0));

		e = a > ai ? div255((a - ai) * st->edge) : 0;
	}
	if (st->hi || st->lo) {
		unsigned ai = cover(rr_dist(px, py, x + b, y + b, w - 2 * b, h - 2 * b, rb));
		unsigned ah = cover(rr_dist(px, py, x + b, y + b + 1, w - 2 * b, h - 2 * b - 2, rb));

		if (ai > ah)	/* the highlight inside the top (or bottom) */
			e += div255((ai - ah) * (py - y < h / 2 ? st->hi : st->lo));
	}
	ae[0] = (uint8_t)a;
	ae[1] = (uint8_t)(e > 255 ? 255 : e);
}

/*
 * The cover and edge of a box's corners (n = r + 3 rows and columns from
 * each corner) and of the 3 columns at its sides in between: the same for
 * every box of a style with that radius that is at least 2 n across and
 * down (left and right mirror). Kept for the last few.
 */
#define GC_MAX		8
struct glass_corner {
	int n, kind;			/* n 0: unused */
	uint8_t top[GC_MAX][GC_MAX][2], bot[GC_MAX][GC_MAX][2], side[3][2];
};

static const struct glass_corner *glass_corner(int x, int y, int w, int h, int r, int kind)
{
	static struct glass_corner gcs[6];
	static int next;
	struct glass_corner *g;
	int n = r + 3;

	if (n > GC_MAX || w < 2 * n || h < 2 * n)
		return NULL;
	for (int k = 0; k < 6; k++)
		if (gcs[k].n == n && gcs[k].kind == kind)
			return &gcs[k];
	g = &gcs[next];
	next = (next + 1) % 6;
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) {
			glass_ae(g->top[j][i], x + i, y + j, x, y, w, h, r, &glass_styles[kind]);
			glass_ae(g->bot[j][i], x + i, y + h - n + j, x, y, w, h, r, &glass_styles[kind]);
		}
	for (int i = 0; i < 3; i++)
		glass_ae(g->side[i], x + i, y + h / 2, x, y, w, h, r, &glass_styles[kind]);
	g->n = n;
	g->kind = kind;
	return g;
}

/*
 * A box of glass of a kind (glass_styles) over what is drawn: the void and
 * the sheen worked out in 8 bits a channel and dithered into RGB565 (a 4 x
 * 4 Bayer matrix: a few percent of white over the dark scene are less than
 * one RGB565 step, and would show as bands). A frosted kind reads gb, as
 * glass_backdrop() left it; without it (drawn in bands), what is drawn
 * under the box, with the void at 90 % instead of 60 %.
 */
static void glass(int x, int y, int w, int h, int r, int kind)
{
	static const uint8_t bayer[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
	}, round8[4][4] = {
		{ 8, 8, 8, 8 }, { 8, 8, 8, 8 }, { 8, 8, 8, 8 }, { 8, 8, 8, 8 },
	};
	const struct glass_style *st = &glass_styles[kind];
	int based = st->frosted && gb_ok, bar = kind == GLASS_BAR;
	/*
	 * Not frosted (tiles, rows, bars, the focus): one flat colour, the
	 * sheen's mean over the opaque void, rounded, the scene not through
	 * it (2026-10-27). The canvas's 85 %
	 * with a sheen falling 7 % to 2 % spans 2-4 levels of RGB565 here:
	 * rounded they showed as stripes on the panel, dithered as a grain
	 * behind the text, and the rows must look calm and flat.
	 */
	int flat = !st->frosted, mid0 = h * st->at_pct / 100;
	unsigned sa_flat = h > 0 ? (unsigned)(((st->s0 + st->s1) * mid0 + st->s1 * (h - mid0)) / (2 * h)) : 0;
	const uint8_t (*dt)[4] = flat ? round8 : bayer;
	const struct glass_corner *gc = NULL;
	long t0 = wb_now_us();

	if (r > WB_SQUARE)
		r = WB_SQUARE;
	if (x < 0 || y < 0 || x + w > W || y + h > H || w <= 0 || h <= 0)
		return;
	if (wb_rec) {
		int k[] = { 8, x, w, r, kind, based ? (int)wb_gb_want : 0 };

		wb_rec_rows(y, y + h, k, sizeof(k));
		return;
	}
	if (!bar)
		gc = glass_corner(x, y, w, h, r, kind);
	for (int j = 0; j < h; j++) {
		int py = y + j, mid = h * st->at_pct / 100;
		/* a frosted kind without its blurred backdrop (drawn in bands):
		 * the void at 90 %, or the text under it shows through */
		unsigned sa, ta = flat ? 255 : !gb_ok ? 230 : st->tint;
		unsigned k16, m16[3], kp, mp[3];	/* with the row's edge; without */
		short v0 = 0, v1 = 0;
		uint8_t fv = 0;
		int lo = 0, hi = 0;		/* this row's plain glass: [lo, hi) */
		const uint8_t (*ct)[2] = NULL;	/* this row's corners, if it has them */
		unsigned rowe = 0;		/* an edge or highlight along the whole row */

		if (py < fb_y0 || py >= fb_y1)
			continue;
		/* the sheen: s0 at the top, s1 at mid, 0 at the bottom */
		sa = flat ? sa_flat : j < mid ? st->s0 - (st->s0 - st->s1) * (unsigned)j / (unsigned)mid :
		     st->s1 * (unsigned)(h - j) / (unsigned)(h - mid);
		if (based)
			glass_pos(py, GB_H, &v0, &v1, &fv);
		/* the void and then this row's sheen over v, as one v * k + m in
		 * 16.16 (within a level of the two blends) */
		k16 = (unsigned)(((255u - ta) * (255 - sa) * 65536ull + 32512) / 65025);
		for (int c = 0; c < 3; c++) {
			unsigned t = (WB_COLOR_VOID >> (16 - 8 * c)) & 255, sv = (st->sheen >> (16 - 8 * c)) & 255;

			m16[c] = (unsigned)(((t * ta * (255 - sa) + sv * sa * 255u) * 65536ull + 32512) / 65025);
		}
		if (bar) {
			lo = 0;
			hi = w;
			rowe = j == 0 ? st->edge : 0;
		} else if (gc) {
			int n = gc->n;

			ct = j < n ? gc->top[j] : j >= h - n ? gc->bot[j - (h - n)] : NULL;
			lo = ct ? n : 3;
			hi = w - lo;
			if (ct)		/* the edge or highlight along the top or bottom */
				rowe = ct[n - 1][1];
		}
		kp = k16;
		mp[0] = m16[0];
		mp[1] = m16[1];
		mp[2] = m16[2];
		if (rowe) {
			k16 = (unsigned)((uint64_t)k16 * (255 - rowe) / 255);
			for (int c = 0; c < 3; c++)
				m16[c] = (unsigned)((uint64_t)m16[c] * (255 - rowe) / 255) + rowe * 65536u;
		}
		if (lo < hi) {
			if (based)
				glass_span(&fb[py * W], x + lo, x + hi, dt[py & 3], v0, v1, fv, k16, m16);
			else
				glass_span_fb(&fb[py * W], x + lo, x + hi, dt[py & 3], k16, m16);
		}
		for (int i = 0; i < w; i++) {
			int px = x + i, dth = dt[py & 3][px & 3];
			unsigned a, e, rgb[3], base[3];
			uint16_t *q = &fb[py * W + px];
			uint8_t own[2];
			const uint8_t *ae = own;

			if (i == lo && lo < hi) {
				i = hi - 1;
				continue;
			}
			if (gc) {
				int ci = i < w / 2 ? i : w - 1 - i;

				ae = ct ? ct[ci] : gc->side[ci];
			} else {
				glass_ae(own, px, py, x, y, w, h, r, st);
			}
			a = ae[0];
			e = ae[1];
			if (!a)
				continue;
			rgb565_split(*q, &rgb[0], &rgb[1], &rgb[2]);
			if (based) {	/* the blurred backdrop, bilinearly */
				unsigned fu = gb_fu[px], u0 = (unsigned)gb_u0[px], u1 = (unsigned)gb_u1[px];

				for (int c = 0; c < 3; c++)
					base[c] = (GB_ROW(c, u0) * (8 - fu) + GB_ROW(c, u1) * fu + 32) >> 6;
			} else {
				base[0] = rgb[0];
				base[1] = rgb[1];
				base[2] = rgb[2];
			}
			for (int c = 0; c < 3; c++) {
				int sh = 16 - 8 * c;
				unsigned v = (base[c] * kp + mp[c] + 32768) >> 16;	/* the glass */

				if (e)
					v = over8(v > 255 ? 255 : v, 0xFFFFFF, sh, e);
				rgb[c] = div255(v * a + rgb[c] * (255 - a));
			}
			rgb[0] = (rgb[0] + dth / 2) >> 3;
			rgb[1] = (rgb[1] + dth / 4) >> 2;
			rgb[2] = (rgb[2] + dth / 2) >> 3;
			*q = (uint16_t)((rgb[0] > 31 ? 31 : rgb[0]) << 11 | (rgb[1] > 63 ? 63 : rgb[1]) << 5 |
					(rgb[2] > 31 ? 31 : rgb[2]));
		}
	}
	tm_glass += wb_now_us() - t0;
}

/*
 * box() (wbdraw.h) with its shape worked out once: per pixel the glow's
 * alpha, the cover (for the fill) and the border's alpha, as box() finds
 * them, kept for a quarter of the box with its glow (the rest mirrors
 * it). Drawing it then only blends. box() works out the distance to the
 * rounded rectangle per pixel, with a square root in the corners: ~15 ms
 * for the focus of a tile with its glow, ~2 ms for a 52 px badge.
 */
struct shape {
	int w, h, r, bw;
	const struct glow *g;
	int ext, qw, qh;	/* the glow's reach; the quarter's size */
	uint8_t *m;		/* qw x qh x 3: glow, cover, border */
	uint16_t *in;		/* per row of the quarter: where glow and border end */
};

#define SHAPES_MAX	6
static struct shape shapes[SHAPES_MAX];

static const struct shape *shape_get(int w, int h, int r, int bw, const struct glow *g)
{
	struct shape *sh = 0;

	for (int i = 0; i < SHAPES_MAX; i++) {
		struct shape *t = &shapes[i];

		if (t->m && t->w == w && t->h == h && t->r == r && t->bw == bw && t->g == g)
			return t;
		if (!t->m && !sh)
			sh = t;
	}
	if (!sh) {			/* all taken: the first goes */
		sh = &shapes[0];
		kept_bytes -= (long)sh->qw * sh->qh * 3 + sh->qh * 2;
		free(sh->m);
		free(sh->in);
		sh->m = 0;
	}
	sh->ext = g ? g->n / 4 + 1 : 1;
	sh->qw = (w + 2 * sh->ext + 1) / 2;
	sh->qh = (h + 2 * sh->ext + 1) / 2;
	sh->m = malloc((size_t)sh->qw * sh->qh * 3);
	sh->in = malloc((size_t)sh->qh * 2);
	if (!sh->m || !sh->in) {
		free(sh->m);
		free(sh->in);
		sh->m = 0;
		return 0;
	}
	sh->w = w;
	sh->h = h;
	sh->r = r;
	sh->bw = bw;
	sh->g = g;
	kept_bytes += (long)sh->qw * sh->qh * 3 + sh->qh * 2;
	for (int j = 0; j < sh->qh; j++) {
		int py = j - sh->ext;		/* the box at 0, 0 */

		sh->in[j] = (uint16_t)sh->qw;
		for (int i = 0; i < sh->qw; i++) {
			int px = i - sh->ext, d = rr_dist(px, py, 0, 0, w, h, r);
			uint8_t *q = &sh->m[(j * sh->qw + i) * 3];
			unsigned ao = d < 8 + 16 ? cover(d) : 0, ba = 0;

			q[0] = d > 0 && g && d / 4 < g->n ? g->lut[d / 4] : 0;
			if (bw && d < 8 + 16) {
				unsigned ai = cover(rr_dist(px, py, bw, bw, w - 2 * bw, h - 2 * bw,
							    r > bw ? r - bw : 0));

				ba = ao > ai ? ao - ai : 0;
			}
			q[1] = (uint8_t)ao;
			q[2] = (uint8_t)ba;
		}
		/* from the outside in: past the last glow or border, nothing */
		for (int i = sh->qw - 1; i >= 0; i--) {
			const uint8_t *q = &sh->m[(j * sh->qw + i) * 3];

			if (q[0] || q[2])
				break;
			sh->in[j] = (uint16_t)i;
		}
	}
	return sh;
}

/* box(), from its shape: the same pixels. */
static void box_shaped(int x, int y, int w, int h, int r, uint32_t fill_c, unsigned fill_a,
		       uint32_t border_c, int bw, const struct glow *g)
{
	const struct shape *sh;
	int tw, th;

	glow_init();
	if (r > WB_SQUARE)
		r = WB_SQUARE;
	sh = shape_get(w, h, r, bw, g);

	if (wb_rec && sh) {
		int k[] = { 9, x, y, w, h, r, (int)fill_c, (int)fill_a, (int)border_c, bw, WB_P(g) };

		wb_rec_rows(y - sh->ext, y + h + sh->ext, k, sizeof(k));
		return;
	}
	if (!sh) {
		box(x, y, w, h, r, fill_c, fill_a, border_c, bw, g);
		return;
	}
	tw = w + 2 * sh->ext;
	th = h + 2 * sh->ext;
	for (int jj = 0; jj < th; jj++) {
		int py = y - sh->ext + jj, j = jj < sh->qh ? jj : th - 1 - jj, in = sh->in[j];
		const uint8_t *row = &sh->m[j * sh->qw * 3];

		if (py < fb_y0 || py >= fb_y1)
			continue;
		for (int ii = 0; ii < tw; ii++) {
			int i = ii < sh->qw ? ii : tw - 1 - ii;
			const uint8_t *q = &row[i * 3];

			if (i >= in && !fill_a) {	/* the inside: nothing to draw */
				ii = tw - 1 - in;
				continue;
			}
			if (q[0])
				put(x - sh->ext + ii, py, WB_COLOR_CYAN, q[0]);
			if (fill_a && q[1])
				put(x - sh->ext + ii, py, fill_c, q[1] * fill_a / 255);
			if (q[2])
				put(x - sh->ext + ii, py, border_c, q[2]);
		}
	}
}


/* A pop-up's panel and a card (wbdraw.h) as glass over the scene: 1, or 0
 * to leave them plain. The panel is frosted when fb holds the whole
 * picture; drawn in bands, fb has only the band, so it is plain glass. */
/*
 * The blur under a frosted panel is kept between frames drawn with
 * wb_frame(): recorded, a panel's backdrop is everything drawn before it
 * (wb_gb_want: the calls so far and the panel's box); drawn, the blur in gb
 * is used again while it is of the same backdrop (wb_gb_have), as when only
 * the pop-up's focus moved, and the panel's bands are drawn alone.
 */
static int wb_glass_panel(int x, int y, int w, int h, int r)
{
	if (!scene_shown)
		return 0;
	if (wb_rec) {
		if (wb_rec_whole) {	/* frosted when drawn: of this backdrop */
			int k[] = { 13, x, y, w, h };

			wb_rec_rows(y, y + h, k, sizeof(k));
			wb_gb_want = wb_rec_run;
			wb_rec_panels++;
			gb_ok = 1;
		} else {
			gb_ok = 0;
		}
	} else if (wb_rec_drawing && gb && wb_gb_have == wb_gb_want) {
		gb_ok = 1;		/* the same backdrop's blur, kept */
	} else if (fb_whole || (fb_y0 == 0 && fb_y1 == H)) {
		glass_backdrop(x, y, w, h);
		wb_gb_have = wb_rec_drawing ? wb_gb_want : 0;
	} else {
		gb_ok = 0;
	}
	glass(x, y, w, h, r, GLASS_PANEL);
	return 1;
}

static int wb_glass_card(int x, int y, int w, int h, int r, int on, const struct glow *g)
{
	glow_init();
	if (!scene_shown)
		return 0;
	(void)g;
	glass(x, y, w, h, r, on ? GLASS_FOCUS : GLASS_TILE);
	if (on)		/* the focus: its border and glow over the glass */
		box_shaped(x, y, w, h, r, WB_COLOR_CYAN, 0, WB_COLOR_CYAN, WB_FOCUS_BORDER_W, &GLOW_FOCUS);
	return 1;
}

/* The hint bar (wbdraw.h) as glass over the scene; plain elsewhere. */
static void hint_glass(int y0)
{
	if (scene_shown) {
		glass(0, y0, W, WB_HINT_BAR_H, 0, GLASS_BAR);
	} else {
		fill(0, y0, W, WB_HINT_BAR_H, WB_COLOR_SURFACE1);
		fill(0, y0, W, 1, WB_COLOR_LINE);
	}
}


/* dim() (the void at a) but for the box x0..x1, y0..y1 (a panel goes
 * there) */
static void dim_outside(unsigned a, int x0, int y0, int x1, int y1)
{
	uint16_t lr[32], lg[64], lb[32];
	unsigned na = 255 - a;
	uint32_t c = WB_COLOR_VOID;

	if (wb_rec) {
		int k[] = { 10, (int)a, x0, y0, x1, y1 };

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

			if (x == x0 && y >= y0 && y < y1) {
				x = x1 - 1;
				continue;
			}
			*p = lr[d >> 11] | lg[(d >> 5) & 63] | lb[d & 31];
		}
}
