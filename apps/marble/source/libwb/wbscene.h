/* SPDX-License-Identifier: MIT */
/*
 * The frontpanel's figures behind the screen (the moon with its craters and
 * orbits, the wireframe head), turned with the badge so they stay in line
 * with the panel, and coloured as the canvas "Menu's met frontpaneel" does
 * (pink from the top left, cyan from the bottom right). Part of libwb:
 * include wb.h.
 *
 * scene_draw() draws rows fb_y0..fb_y1 at rotation rot (rot_read() reads
 * the panel's, kernel patch 0054). A rotation's scene is kept as runs of
 * pixels the first time it is drawn whole (about 80 KB); scene_prebuild()
 * keeps the next rotation not kept yet, for a program waiting for a key.
 */
#pragma once
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long wb_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

#include "wb_scene_data.h"
#include "wb_craters.h"

/* The figures: dimmer than the panel's white print, as if behind the
 * window's glass, so the tiles and the text stay in front. Colour and
 * opacity (design/tokens.json, "scene"). */
#define SCENE_MOON	WB_COLOR_SCENE, WB_SCENE_MOON_OPA
#define SCENE_LINE	WB_COLOR_SCENE, WB_SCENE_LINE_OPA

#define ROTATION_FILE	"/sys/class/drm/card0/device/rotation"	/* kernel patch 0054 */


static int rot;		/* how far the picture is turned, clockwise: 0, 90, 180, 270 */

static int rot_read(void)
{
#if defined(HOME_PREVIEW) || defined(WB_HOST)
	return rot;
#else
	char b[8] = { 0 };
	int fd = open(ROTATION_FILE, O_RDONLY | O_CLOEXEC), r = 0;

	if (fd >= 0) {
		if (read(fd, b, sizeof(b) - 1) > 0)
			r = atoi(b);
		close(fd);
	}
	return r == 90 || r == 180 || r == 270 ? r : 0;
#endif
}

/* Where panel point (px, py), in 1/16 px (pixel centres at +8), is in
 * the picture. */
static void unturn16(int px, int py, int *x, int *y)
{
	switch (rot) {
	case 90:  *x = py;          *y = W * 16 - px; break;
	case 180: *x = W * 16 - px; *y = H * 16 - py; break;
	case 270: *x = H * 16 - py; *y = px;          break;
	default:  *x = px;          *y = py;
	}
}

/*
 * A circle of home_scene.h ({ x, y, radius, width } in 1/16 px), turned
 * with the panel, in colour c at opacity opa: a line, or filled when the
 * width is 0. A filled one goes straight onto the void (it is drawn first).
 */
static void scene_circle(const short *s, uint32_t c, unsigned opa)
{
	int cx, cy, r = s[2], hw = s[3] / 2;
	int out = r + hw + 16, in = s[3] ? r - hw - 16 : r - 16;
	uint32_t solid = 0;
	int y0, y1;

	if (!s[3]) {		/* the disc's inside: c over the void */
		for (int sh = 0; sh <= 16; sh += 8) {
			unsigned v = (c >> sh & 255) * opa + (WB_COLOR_VOID >> sh & 255) * (255 - opa);

			solid |= (uint32_t)(v / 255) << sh;
		}
	}
	unturn16(s[0], s[1], &cx, &cy);
	y0 = (cy - out) >> 4;
	y1 = (cy + out) >> 4;
	if (y0 < fb_y0)
		y0 = fb_y0;
	if (y1 >= fb_y1)
		y1 = fb_y1 - 1;
	for (int y = y0; y <= y1; y++) {
		int dy = y * 16 + 8 - cy, dy2 = dy * dy, xo, l, rt, ea = 1, eb = 0;

		if (dy2 >= out * out)
			continue;
		xo = isqrt((unsigned)(out * out - dy2));
		l = (cx - xo - 8) >> 4;
		rt = (cx + xo - 8 + 15) >> 4;
		if (in > 0 && dy2 < in * in) {
			int m = isqrt((unsigned)(in * in - dy2));

			if (s[3]) {	/* inside the line: nothing */
				ea = ((cx - m - 8) >> 4) + 1;
				eb = ((cx + m - 8 + 15) >> 4) - 1;
			} else {	/* inside the disc: all of it */
				ea = (cx - m - 8 + 15) >> 4;
				eb = (cx + m - 8) >> 4;
				fill(ea, y, eb - ea + 1, 1, solid);
			}
		}
		if (l < 0)
			l = 0;
		if (rt >= W)
			rt = W - 1;
		for (int x = l; x <= rt; x++) {
			int dx, d;
			unsigned k;

			if (x >= ea && x <= eb) {
				x = eb;
				continue;
			}
			dx = x * 16 + 8 - cx;
			d = isqrt((unsigned)(dx * dx + dy2)) - r;
			if (s[3])
				d = (d < 0 ? -d : d) - hw;
			k = cover(d);
			if (k)
				put(x, y, c, k * opa / 255);
		}
	}
}

/* Distance in 1/16 px from (px, py) to the segment a-b; *end: 0 in the
 * middle, -1 before a, 1 past b. */
static int seg_dist(int px, int py, const int *a, const int *b, int *end)
{
	int vx = b[0] - a[0], vy = b[1] - a[1], wx = px - a[0], wy = py - a[1];
	int l2 = vx * vx + vy * vy, dot = wx * vx + wy * vy, cr;

	*end = dot <= 0 ? -1 : dot >= l2 ? 1 : 0;
	if (*end < 0)
		return isqrt((unsigned)(wx * wx + wy * wy));
	if (*end > 0) {
		wx = px - b[0];
		wy = py - b[1];
		return isqrt((unsigned)(wx * wx + wy * wy));
	}
	cr = wx * vy - wy * vx;
	return (cr < 0 ? -cr : cr) / isqrt((unsigned)l2);
}

/*
 * The head's polylines, turned with the panel. A pixel near a joint goes
 * to the nearer of the two segments only, so joints get no brighter dots.
 */
static void scene_lines(const short *s, uint32_t c, unsigned opa)
{
	int p[64][2];

	while (s[0]) {
		int n = s[0] > 64 ? 64 : s[0], hw = s[1] / 2, reach = hw + 24;

		for (int i = 0; i < n; i++)
			unturn16(s[2 + 2 * i], s[3 + 2 * i], &p[i][0], &p[i][1]);
		for (int i = 0; i + 1 < n; i++) {
			const int *a = p[i], *b = p[i + 1];
			int x0 = ((a[0] < b[0] ? a[0] : b[0]) - reach) >> 4;
			int x1 = ((a[0] > b[0] ? a[0] : b[0]) + reach) >> 4;
			int y0 = ((a[1] < b[1] ? a[1] : b[1]) - reach) >> 4;
			int y1 = ((a[1] > b[1] ? a[1] : b[1]) + reach) >> 4;

			if (x0 < 0)
				x0 = 0;
			if (x1 >= W)
				x1 = W - 1;
			if (y0 < fb_y0)
				y0 = fb_y0;
			if (y1 >= fb_y1)
				y1 = fb_y1 - 1;
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++) {
					int px = x * 16 + 8, py = y * 16 + 8, end, e2;
					int d = seg_dist(px, py, a, b, &end);
					unsigned k = cover(d - hw);

					if (!k)
						continue;
					/* near a joint: the lower segment wins a tie */
					if (i > 0 && seg_dist(px, py, p[i - 1], a, &e2) <= d &&
					    (px - a[0]) * (px - a[0]) + (py - a[1]) * (py - a[1]) < reach * reach)
						continue;
					if (i + 2 < n && seg_dist(px, py, b, p[i + 2], &e2) < d &&
					    (px - b[0]) * (px - b[0]) + (py - b[1]) * (py - b[1]) < reach * reach)
						continue;
					put(x, y, c, k * opa / 255);
				}
		}
		s += 2 + 2 * s[0];
	}
}

/*
 * The scene changes only when the badge turns, and drawing it (square
 * roots and divisions for every pixel near a line) takes ~175 ms. It is
 * kept for each rotation as runs, row by row: a word n | 0x8000 and one
 * pixel for n equal pixels, or n and n pixels. The void and the moon's
 * disc are long runs, so a rotation's scene is a fraction of a picture.
 * The rotation in use is kept when first drawn, the other three while the
 * home screen waits for a key (scene_prebuild), so a turn only unpacks.
 */
static uint16_t *scene_rle[4];
static uint32_t scene_at[4][H];	/* where each row starts in scene_rle[] */
static long kept_bytes;		/* the scenes and tiles kept, for the timing line */

/* fb's rows y0..y1 as runs into out from word n on, or only counted when
 * out is 0; the words used up to there */
static int scene_encode_rows(uint16_t *out, uint32_t *at, int y0, int y1, int n)
{
	for (int y = y0; y < y1; y++) {
		const uint16_t *row = fb + y * W;

		if (out)
			at[y] = (uint32_t)n;
		for (int x = 0; x < W;) {
			int run = 1, lit = 0;

			while (x + run < W && row[x + run] == row[x])
				run++;
			if (run >= 3) {
				if (out) {
					out[n] = (uint16_t)(0x8000 | run);
					out[n + 1] = row[x];
				}
				n += 2;
				x += run;
				continue;
			}
			while (x + lit < W && !(x + lit + 2 < W && row[x + lit] == row[x + lit + 1] &&
						 row[x + lit] == row[x + lit + 2]))
				lit++;
			if (out) {
				out[n] = (uint16_t)lit;
				memcpy(&out[n + 1], &row[x], (size_t)lit * 2);
			}
			n += 1 + lit;
			x += lit;
		}
	}
	return n;
}

static int scene_encode(uint16_t *out, uint32_t *at)
{
	return scene_encode_rows(out, at, 0, H, 0);
}

static inline void rgb565_split(uint16_t d, unsigned *r, unsigned *g, unsigned *b)
{
	*r = ((d >> 11) << 3) | (d >> 13);
	*g = (((d >> 5) & 63) << 2) | ((d >> 9) & 3);
	*b = ((d & 31) << 3) | ((d >> 2) & 7);
}

/*
 * The scene's colour (the canvas: a layer over the figures with CSS
 * mix-blend-mode: color): radial gradients of pink from the top left
 * (95 % at 19 %, 8 %, gone at half the distance to the farthest corner)
 * over cyan from the bottom right (90 % at 82 %, 86 %, gone at 62 %).
 * A pixel takes the layer's hue and saturation at its own luminance, by
 * the layer's alpha. The layer per cell of TINT_CELL px (its steps are
 * well under a level): its colour and alpha, worked out once.
 */
#define TINT_CELL	16
#define TINT_N		(W / TINT_CELL)

static uint8_t tint_c[TINT_N][TINT_N][3], tint_a[TINT_N][TINT_N];
static int tint_ready;
/* the two long runs, the void and the moon's disc, tinted per cell */
static uint16_t tint_run_c[2];
static uint8_t tint_run_rgb[2][TINT_N][TINT_N][3];
static void tint_px(unsigned *b, int u, int v);

static void tint_init(void)
{
	/* centres and the radius where the alpha reaches 0, in px */
	static const int g[2][3] = { { 137, 58, 441 }, { 590, 619, 530 } };
	static const uint32_t gc[2] = { WB_COLOR_PINK, WB_COLOR_CYAN };
	static const int ga[2] = { 242, 230 };	/* 0.95, 0.90 */

	for (int v = 0; v < TINT_N; v++)
		for (int u = 0; u < TINT_N; u++) {
			int px = u * TINT_CELL + TINT_CELL / 2, py = v * TINT_CELL + TINT_CELL / 2;
			unsigned a[2], ao, c[3] = { 0, 0, 0 };

			for (int k = 0; k < 2; k++) {
				int dx = px - g[k][0], dy = py - g[k][1];
				int d = isqrt((unsigned)(dx * dx + dy * dy));

				a[k] = d >= g[k][2] ? 0 : (unsigned)ga[k] * (unsigned)(g[k][2] - d) / (unsigned)g[k][2];
			}
			/* pink over cyan */
			ao = a[0] + div255(a[1] * (255 - a[0]));
			for (int ch = 0; ch < 3 && ao; ch++) {
				unsigned pk = (gc[0] >> (16 - 8 * ch)) & 255, cy = (gc[1] >> (16 - 8 * ch)) & 255;

				c[ch] = (pk * a[0] + div255(cy * a[1] * (255 - a[0]))) / ao;
			}
			tint_a[v][u] = (uint8_t)ao;
			tint_c[v][u][0] = (uint8_t)c[0];
			tint_c[v][u][1] = (uint8_t)c[1];
			tint_c[v][u][2] = (uint8_t)c[2];
		}
	tint_run_c[0] = rgb(WB_COLOR_VOID);
	tint_run_c[1] = rgb(((((WB_COLOR_SCENE >> 16) & 255) * WB_SCENE_MOON_OPA +
			      ((WB_COLOR_VOID >> 16) & 255) * (255 - WB_SCENE_MOON_OPA)) / 255) << 16 |
			    ((((WB_COLOR_SCENE >> 8) & 255) * WB_SCENE_MOON_OPA +
			      ((WB_COLOR_VOID >> 8) & 255) * (255 - WB_SCENE_MOON_OPA)) / 255) << 8 |
			    (((WB_COLOR_SCENE & 255) * WB_SCENE_MOON_OPA +
			      (WB_COLOR_VOID & 255) * (255 - WB_SCENE_MOON_OPA)) / 255));
	for (int k = 0; k < 2; k++)
		for (int v = 0; v < TINT_N; v++)
			for (int u = 0; u < TINT_N; u++) {
				unsigned b[3];

				rgb565_split(tint_run_c[k], &b[0], &b[1], &b[2]);
				tint_px(b, u, v);
				tint_run_rgb[k][v][u][0] = (uint8_t)b[0];
				tint_run_rgb[k][v][u][1] = (uint8_t)b[1];
				tint_run_rgb[k][v][u][2] = (uint8_t)b[2];
			}
	tint_ready = 1;
}

/* Colour b (8 bits a channel) under the layer of cell (u, v): the blend
 * mode "color" (SetLum of the layer's colour to b's luminance, clipped),
 * by the layer's alpha. */
static void tint_px(unsigned *b, int u, int v)
{
	unsigned a = tint_a[v][u];
	int l, lc, c[3], n, x;

	if (!a)
		return;
	l = (int)(b[0] * 77 + b[1] * 151 + b[2] * 28) >> 8;
	lc = (int)(tint_c[v][u][0] * 77 + tint_c[v][u][1] * 151 + tint_c[v][u][2] * 28) >> 8;
	for (int k = 0; k < 3; k++)
		c[k] = tint_c[v][u][k] + l - lc;
	n = c[0] < c[1] ? c[0] : c[1];
	n = n < c[2] ? n : c[2];
	x = c[0] > c[1] ? c[0] : c[1];
	x = x > c[2] ? x : c[2];
	for (int k = 0; k < 3; k++) {
		if (n < 0)
			c[k] = l + (c[k] - l) * l / (l - n);
		if (x > 255)
			c[k] = l + (c[k] - l) * (255 - l) / (x - l);
		c[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k];
		b[k] = div255((unsigned)c[k] * a + b[k] * (255 - a));
	}
}

static const uint8_t tint_bayer[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static inline uint16_t tint_565(const unsigned *b, unsigned d)
{
	unsigned r = (b[0] + d / 2) >> 3, g = (b[1] + d / 4) >> 2, bl = (b[2] + d / 2) >> 3;

	return (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (bl > 31 ? 31 : bl));
}

typedef uint32_t __attribute__((may_alias)) u32_alias;

/* m pixels from column x of row d with the pattern q (q[x & 3]), two to a
 * store (rows start on a word: 1440 bytes). */
static inline void fill4(uint16_t *d, int x, int m, const uint16_t *q)
{
	uint32_t w[2] = { q[0] | (uint32_t)q[1] << 16, q[2] | (uint32_t)q[3] << 16 };
	int i = 0;

	if (m > 0 && (x & 1)) {
		d[x] = q[x & 3];
		i = 1;
	}
	for (; i + 1 < m; i += 2)
		*(u32_alias *)&d[x + i] = w[((x + i) >> 1) & 1];
	if (i < m)
		d[x + i] = q[(x + i) & 3];
}

/* n pixels of colour c from column x of row y, tinted: a cell at a time,
 * with the 4 dithered values of each. */
static void tint_run(uint16_t *d, int x, int y, int n, uint16_t c)
{
	const uint8_t *dth = tint_bayer[y & 3];
	int v = y / TINT_CELL, kc = c == tint_run_c[0] ? 0 : c == tint_run_c[1] ? 1 : -1;

	while (n > 0) {
		int u = x / TINT_CELL, m = (u + 1) * TINT_CELL - x;
		uint16_t q[4];
		unsigned b[3];

		m = m > n ? n : m;
		if (!tint_a[v][u]) {
			q[0] = q[1] = q[2] = q[3] = c;
			fill4(d, x, m, q);
		} else {
			if (kc >= 0) {
				b[0] = tint_run_rgb[kc][v][u][0];
				b[1] = tint_run_rgb[kc][v][u][1];
				b[2] = tint_run_rgb[kc][v][u][2];
			} else {
				rgb565_split(c, &b[0], &b[1], &b[2]);
				tint_px(b, u, v);
			}
			for (int k = 0; k < 4; k++)
				q[k] = tint_565(b, dth[k]);
			fill4(d, x, m, q);
		}
		x += m;
		n -= m;
	}
}

/* n pixels from p at column x of row y, each tinted. */
static void tint_lit(uint16_t *d, int x, int y, int n, const uint16_t *p)
{
	const uint8_t *dth = tint_bayer[y & 3];
	int v = y / TINT_CELL;

	for (int i = 0; i < n; i++) {
		int u = (x + i) / TINT_CELL;
		unsigned b[3];

		if (!tint_a[v][u]) {
			d[x + i] = p[i];
			continue;
		}
		rgb565_split(p[i], &b[0], &b[1], &b[2]);
		tint_px(b, u, v);
		d[x + i] = tint_565(b, dth[(x + i) & 3]);
	}
}

/* The scene's rows fb_y0..fb_y1 as drawn into fb, tinted in place (when
 * no rotation's runs are kept). */
static void tint_rows(void)
{
	static uint16_t row[W];

	if (!tint_ready)
		tint_init();
	for (int y = fb_y0 < 0 ? 0 : fb_y0; y < fb_y1 && y < H; y++) {
		memcpy(row, fb + y * W, sizeof(row));
		tint_lit(fb + y * W, 0, y, W, row);
	}
}

/* A rotation's kept runs: their single pixels tinted once, here (they
 * stay where they are); runs of one colour are tinted as they are
 * unpacked, a cell at a time (scene_decode). */
static void scene_pretint(int k)
{
	static uint16_t lit[W];

	if (!tint_ready)
		tint_init();
	for (int y = 0; y < H; y++) {
		uint16_t *p = scene_rle[k] + scene_at[k][y];

		for (int x = 0; x < W;) {
			int n = *p & 0x7fff;

			if (*p++ & 0x8000) {
				p++;
			} else {
				memcpy(lit, p, (size_t)n * 2);
				tint_lit(p - x, x, y, n, lit);
				p += n;
			}
			x += n;
		}
	}
}

static void scene_decode(int k)
{
	if (!tint_ready)
		tint_init();
	for (int y = fb_y0 < 0 ? 0 : fb_y0; y < fb_y1 && y < H; y++) {
		const uint16_t *p = scene_rle[k] + scene_at[k][y];
		uint16_t *d = fb + y * W;

		for (int x = 0; x < W;) {
			int n = *p & 0x7fff;

			if (*p++ & 0x8000) {
				tint_run(d, x, y, n, *p);
				p++;
			} else {
				memcpy(d + x, p, (size_t)n * 2);	/* tinted when kept */
				p += n;
			}
			x += n;
		}
	}
}

/* The moon's craters (home_craters.h): holes in its disc where the void
 * shows, turned with the moon. */
static void scene_crater_holes(void)
{
	const uint8_t *p = scene_craters;

	for (int k = 0; k < SCENE_CRATER_RUNS; k++) {
		int py = p[0] << 8 | p[1], px = p[2] << 8 | p[3], n = p[4];

		p += 5;
		for (int i = 0; i < n; i++) {
			unsigned a = (i & 1 ? p[i / 2] & 15 : p[i / 2] >> 4) * 17;
			int x, y;

			unturn16((px + i) * 16 + 8, py * 16 + 8, &x, &y);
			put(x >> 4, y >> 4, WB_COLOR_VOID, a);
		}
		p += (n + 1) / 2;
	}
}

/* The figures, untinted, as far as fb_y0..fb_y1 reach. */
static void scene_raw(void)
{
	fill(0, 0, W, H, WB_COLOR_VOID);
	scene_circle(scene_moon, SCENE_MOON);
	scene_crater_holes();
	for (unsigned i = 0; i < sizeof(scene_orbits) / sizeof(scene_orbits[0]); i++)
		scene_circle(scene_orbits[i], SCENE_LINE);
	scene_lines(scene_head, SCENE_LINE);
}

/*
 * Keep rotation k's runs without a whole picture (a program that draws in
 * bands): the scene drawn SCENE_BAND rows at a time into a buffer of its
 * own, counted, then drawn again and kept (about twice the time of one
 * drawing, once a rotation). 1 if kept.
 */
#define SCENE_BAND	40

static int scene_keep_banded(int k)
{
	uint16_t *buf = malloc((size_t)W * SCENE_BAND * 2), *was_fb = fb;
	int y0 = fb_y0, y1 = fb_y1, was = rot, n = 0, ok = 0;

	if (!buf)
		return 0;
	rot = k * 90;
	for (int pass = 0; pass < 2; pass++) {
		int m = 0;

		for (int y = 0; y < H; y += SCENE_BAND) {
			fb = buf - y * W;
			fb_y0 = y;
			fb_y1 = y + SCENE_BAND < H ? y + SCENE_BAND : H;
			scene_raw();
			m = scene_encode_rows(pass ? scene_rle[k] : 0, scene_at[k], y, fb_y1, m);
		}
		if (pass) {
			ok = 1;
		} else if (!(scene_rle[k] = malloc((size_t)m * 2))) {
			break;
		}
		n = m;
	}
	fb = was_fb;
	fb_y0 = y0;
	fb_y1 = y1;
	rot = was;
	free(buf);
	if (!ok)
		return 0;
	kept_bytes += (long)n * 2;
	scene_pretint(k);
	return 1;
}

/* Draw the scene at rotation rot, and keep it when the whole picture is drawn
 * (or, drawing in bands, keep it first). */
static long tm_scene;		/* us spent in scene_draw(), for a program's timing */

static void scene_draw_(void);

static void scene_draw(void)
{
	long t0 = wb_now_us();

	if (wb_rec) {
		int k[] = { 11, rot };

		wb_rec_rows(0, H, k, sizeof(k));
		return;
	}
	scene_draw_();
	tm_scene += wb_now_us() - t0;
}

static void scene_draw_(void)
{
	int k = rot / 90, n;

	if (!scene_rle[k] && (fb_y0 > 0 || fb_y1 < H))
		scene_keep_banded(k);
	if (scene_rle[k]) {
		scene_decode(k);
		return;
	}
	scene_raw();
	if (fb_y0 > 0 || fb_y1 < H) {
		tint_rows();
		return;
	}
	n = scene_encode(0, 0);
	if ((scene_rle[k] = malloc((size_t)n * 2))) {
		scene_encode(scene_rle[k], scene_at[k]);
		kept_bytes += (long)n * 2;
		scene_pretint(k);
		scene_decode(k);	/* tinted */
	} else {
		tint_rows();
	}
}

/*
 * While the home screen waits: draw and keep one rotation's scene not
 * kept yet (into fb, which the caller then draws again). 1 if it did.
 */
static int scene_prebuild(void)
{
	int was = rot;

	for (int k = 0; k < 4; k++) {
		if (scene_rle[k])
			continue;
		rot = k * 90;
		scene_draw();
		rot = was;
		return 1;	/* fb holds this scene now */
	}
	return 0;
}

