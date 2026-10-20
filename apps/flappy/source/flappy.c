// SPDX-License-Identifier: MIT
/*
 * flappy — Flappy Bird for the badge, drawn like the menu (wbdraw.h, the
 * design's colours and fonts), as an app from the store (SOURCES.md).
 *
 * A bird falls; Space, Enter, Up or a symbol key (not the diamond) makes it
 * flap. Fly through the gaps between the pipes: each pipe passed is a
 * point. Touching a pipe or the ground ends the game. It starts gently and
 * gets harder with each point (level()). The best score is kept in
 * APP_CONFIG/best (flash), so it survives a new card.
 *
 * A full-screen game (Docs/06-design-system.md 6.9): the symbol keys are
 * controls, Esc pauses (Resume, Restart, Keys, Quit), the diamond shows the
 * keys, the logo key ends it through apprun (SIGTERM, which saves the best
 * score). The start screen shows the keys too.
 *
 * The world moves in ticks of 1/60 s, in fixed point (FP, 1/256 px): the
 * toolchain is soft-float. The panel is drawn at most 25 times a second,
 * one band of BAND_H rows at a time through wbdraw.h's clipping, as the
 * calculator does: no 1 MB frame buffer. On exit, the frame count and the
 * average time to draw one go to stderr (apprun's log).
 *
 * Built with -DFLAPPY_HOST, it takes a script from its first argument and
 * writes the screen as PPM to its second: the host test (test_flappy.py).
 * Test hook on the badge: key codes written to /tmp/flappy.keys, as
 * meshchat's FIFO.
 *
 * FLAT/NOMMU constraints (overlay-src/Makefile): no stdio, no 64-bit
 * division.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>		/* rename */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"

#ifndef FLAPPY_HOST
#include <poll.h>
#include <sys/stat.h>
#endif

/* ---- the game's numbers --------------------------------------------------- */

#define FP		256		/* fixed point: 1/256 px */
#define TICK_US		16667		/* 60 ticks a second */
#define FRAME_US	40000		/* at most 25 frames (a frame takes ~40 ms) */
#define GROUND_Y	640		/* the ground's top */
#define BIRD_X		200
#define BIRD_R		22		/* drawn */
#define HIT_R		18		/* what touches: a little forgiving */
#define READY_Y		330
#define GRAVITY		128		/* 0.5 px per tick, per tick */
#define FLAP		(-9 * FP)	/* rises about 80 px */
#define FALL_MAX	(12 * FP)
/*
 * It starts gently and gets harder with each point up to HARD points:
 * faster, narrower gaps, further apart in height. Between the two ends
 * in a straight line (level()).
 */
#define HARD		30
#define SPEED_EASY	614		/* 2.4 px per tick: 144 px/s */
#define SPEED_HARD	922		/* 3.6 px per tick: 216 px/s */
#define GAP_EASY	250
#define GAP_HARD	180
#define STEP_EASY	90		/* a gap's centre at most this far from the last */
#define STEP_HARD	180
#define PIPE_W		100
#define LIP_X		10		/* the lip sticks out this far each side */
#define LIP_H		36
#define SPACING		330		/* from one pipe to the next */
#define N_PIPES		4
#define GAP_LO		150		/* the gap's centre, highest ... */
#define GAP_HI		(GROUND_Y - 150)	/* ... and lowest */
#define CARD_T		30		/* ticks after a crash before the card */
#define STRIPE		48		/* the ground's stripes */
#define N_STARS		56

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_ENTER	28
#define KEY_SPACE	57
#define KEY_UP		103
#define KEY_DOWN	108

/* ---- state ---------------------------------------------------------------- */

enum { READY, PLAY, DEAD };
enum { P_RESUME, P_RESTART, P_KEYS, P_QUIT, P_N };

static struct {
	int mode;
	int y, v;			/* the bird's centre and speed, FP */
	int t;				/* ticks in this mode */
	int flap_t;			/* ticks since the last flap: the wing */
	struct { int x, cy, gap, scored; } p[N_PIPES];	/* x: the pipe's left, FP */
	int score, best, new_best;
	int far, near, gnd, planet;	/* how far each layer scrolled, FP */
	uint32_t seed;
	int paused, focus;		/* Esc's pop-up */
	int help, help_paused;		/* the keys; opened from the pause */
} G;

static struct { int16_t x, y; uint8_t near; uint8_t c; } stars[N_STARS];

static uint32_t rnd(void)
{
	G.seed ^= G.seed << 13;
	G.seed ^= G.seed >> 17;
	G.seed ^= G.seed << 5;
	return G.seed;
}

/* easy + (hard - easy) * score / HARD, then hard */
static int level(int easy, int hard)
{
	int s = G.score < HARD ? G.score : HARD;

	return easy + (hard - easy) * s / HARD;
}

/* a gap centre not too far from the last one */
static int next_gap(int last)
{
	int step = level(STEP_EASY, STEP_HARD);
	int lo = last - step < GAP_LO ? GAP_LO : last - step;
	int hi = last + step > GAP_HI ? GAP_HI : last + step;

	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ---- the best score, in APP_CONFIG ---------------------------------------- */

static int best_path(char *buf, int size)
{
	const char *d = getenv("APP_CONFIG");

	if (!d || !*d || (int)strlen(d) + 6 > size)
		return 0;
	strcpy(buf, d);
	strcat(buf, "/best");
	return 1;
}

static void load_best(void)
{
	char path[256], buf[16];
	int fd, n;

	if (!best_path(path, sizeof(path)) || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	G.best = 0;
	for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9' && G.best < 1000000; i++)
		G.best = G.best * 10 + buf[i] - '0';
}

static char *utoa(unsigned v, char *end)
{
	*--end = 0;
	do
		*--end = (char)('0' + v % 10);
	while (v /= 10);
	return end;
}

static void save_best(void)
{
	char path[256], tmp[262], buf[16], *s = utoa((unsigned)G.best, buf + sizeof(buf) - 1);
	int fd;

	if (!best_path(path, sizeof(path)))
		return;
	strcpy(tmp, path);
	strcat(tmp, ".new");
	buf[sizeof(buf) - 2] = '\n';
	buf[sizeof(buf) - 1] = 0;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, s, strlen(s)) < 0 || close(fd) < 0)
		return;
	rename(tmp, path);
}

/* ---- the game --------------------------------------------------------------- */

static void reset(void)
{
	int cy = (GAP_LO + GAP_HI) / 2;

	G.mode = READY;
	G.t = 0;
	G.y = READY_Y * FP;
	G.v = 0;
	G.score = 0;
	G.new_best = 0;
	for (int i = 0; i < N_PIPES; i++) {
		cy = next_gap(cy);
		G.p[i].x = (W + 200 + i * SPACING) * FP;
		G.p[i].cy = cy;
		G.p[i].gap = GAP_EASY;
		G.p[i].scored = 0;
	}
}

static void flap(void)
{
	if (G.mode == DEAD) {
		if (G.t >= CARD_T)
			reset();
		return;
	}
	if (G.mode == READY) {
		G.mode = PLAY;
		G.t = 0;
	}
	G.v = FLAP;
	G.flap_t = 0;
}

static void die(void)
{
	G.mode = DEAD;
	G.t = 0;
	if (G.v < 0)
		G.v = 0;
	if (G.score > G.best) {
		G.best = G.score;
		G.new_best = 1;
		save_best();
	}
}

/* does the bird touch the rectangle [x0, x1) x [y0, y1)? (px) */
static int hits(int x0, int y0, int x1, int y1)
{
	int by = G.y / FP;
	int nx = BIRD_X < x0 ? x0 : BIRD_X > x1 ? x1 : BIRD_X;
	int ny = by < y0 ? y0 : by > y1 ? y1 : by;
	int dx = BIRD_X - nx, dy = by - ny;

	return dx * dx + dy * dy < HIT_R * HIT_R;
}

static int hits_pipe(int i)
{
	int x = G.p[i].x / FP, top = G.p[i].cy - G.p[i].gap / 2, bot = G.p[i].cy + G.p[i].gap / 2;

	return hits(x, -100, x + PIPE_W, top) || hits(x - LIP_X, top - LIP_H, x + PIPE_W + LIP_X, top) ||
	       hits(x, bot, x + PIPE_W, GROUND_Y) || hits(x - LIP_X, bot, x + PIPE_W + LIP_X, bot + LIP_H);
}

/* a small sine, -64..64, for the bird's bobbing */
static int sine(int i)
{
	static const int8_t q[9] = { 0, 12, 24, 36, 45, 53, 59, 63, 64 };

	i &= 31;
	return i < 8 ? q[i] : i < 16 ? q[16 - i] : i < 24 ? -q[i - 16] : -q[32 - i];
}

static void wrap(int *v, int add, int period)
{
	*v += add;
	if (*v >= period)
		*v -= period;
}

static void step(void)
{
	int speed = level(SPEED_EASY, SPEED_HARD);

	G.t++;
	G.flap_t++;
	if (G.mode != DEAD) {
		wrap(&G.far, speed / 8, W * FP);
		wrap(&G.near, speed / 3, W * FP);
		wrap(&G.gnd, speed, STRIPE * FP);
		wrap(&G.planet, speed / 16, (W + 400) * FP);
	}
	if (G.mode == READY) {
		G.y = READY_Y * FP + sine(G.t / 3) * 10 * FP / 64;
		G.flap_t = G.t % 24 < 8 ? 0 : 20;	/* the wing beats */
		return;
	}
	G.v += GRAVITY;
	if (G.v > FALL_MAX)
		G.v = FALL_MAX;
	G.y += G.v;
	if (G.y < BIRD_R * FP) {		/* the sky has a lid */
		G.y = BIRD_R * FP;
		G.v = 0;
	}
	if (G.y >= (GROUND_Y - BIRD_R) * FP) {
		G.y = (GROUND_Y - BIRD_R) * FP;
		G.v = 0;
		if (G.mode == PLAY)
			die();
		return;
	}
	if (G.mode != PLAY)
		return;
	for (int i = 0; i < N_PIPES; i++) {
		G.p[i].x -= speed;
		if (G.p[i].x < -(PIPE_W + LIP_X) * FP) {	/* gone left: next after the last */
			int far = 0, cy = G.p[i].cy;

			for (int j = 0; j < N_PIPES; j++)
				if (G.p[j].x > far) {
					far = G.p[j].x;
					cy = G.p[j].cy;
				}
			G.p[i].x = far + SPACING * FP;
			G.p[i].cy = next_gap(cy);
			G.p[i].gap = level(GAP_EASY, GAP_HARD);
			G.p[i].scored = 0;
		}
		if (!G.p[i].scored && G.p[i].x + PIPE_W * FP / 2 < BIRD_X * FP) {
			G.p[i].scored = 1;
			G.score++;
		}
		if (hits_pipe(i)) {
			die();
			return;
		}
	}
}

/* Is the screen moving? Not when paused, nor once the card is up and the
 * bird lies on the ground. */
static int running(void)
{
	return !G.paused && !G.help &&
	       !(G.mode == DEAD && G.t > CARD_T && G.y >= (GROUND_Y - BIRD_R) * FP);
}

static int is_flap(int code)
{
	return code == KEY_SPACE || code == KEY_ENTER || code == KEY_UP ||
	       (code >= KEY_SYM_MENU && code <= KEY_SYM_SETTINGS);
}

/* One key press; 1 to quit. */
static int key(int code)
{
	if (G.help) {			/* only an explanation: Esc or the diamond closes it */
		if (code == KEY_ESC || code == KEY_SYM_HELP) {
			G.help = 0;
			G.paused = G.help_paused;
		}
		return 0;
	}
	if (G.paused) {
		if (code == KEY_UP || code == KEY_DOWN)
			G.focus = (G.focus + (code == KEY_UP ? P_N - 1 : 1)) % P_N;
		else if (code == KEY_ESC)
			G.paused = 0;
		else if (code == KEY_SYM_HELP || (code == KEY_ENTER && G.focus == P_KEYS))
			G.help = G.help_paused = 1;
		else if (code == KEY_ENTER) {
			if (G.focus == P_QUIT)
				return 1;
			if (G.focus == P_RESTART)
				reset();
			G.paused = 0;
		}
		return 0;
	}
	if (code == KEY_ESC) {
		G.paused = 1;
		G.focus = P_RESUME;
	} else if (code == KEY_SYM_HELP) {
		G.help = 1;
		G.help_paused = 0;
	} else if (is_flap(code)) {
		flap();
	}
	return 0;
}

/* ---- drawing ------------------------------------------------------------- */

static uint32_t mix(uint32_t a, uint32_t b, unsigned t)	/* t/255 of a over b */
{
	uint32_t r = 0;

	for (int s = 0; s < 24; s += 8)
		r |= ((((a >> s) & 255) * t + ((b >> s) & 255) * (255 - t)) / 255) << s;
	return r;
}

/* A disk: one square root per row, the ends blended. */
static void disk(int cx, int cy, int r, uint32_t c, uint32_t rim)
{
	int y0 = cy - r < fb_y0 ? fb_y0 : cy - r, y1 = cy + r > fb_y1 - 1 ? fb_y1 - 1 : cy + r;

	for (int y = y0; y <= y1; y++) {
		int dy = y - cy, hw16 = isqrt((unsigned)((r * r - dy * dy) * 256)), hw = hw16 / 16;
		unsigned a = (unsigned)(hw16 % 16) * 16;

		fill(cx - hw, y, 2 * hw, 1, c);
		fill(cx - hw, y, 2, 1, rim);
		fill(cx + hw - 2, y, 2, 1, rim);
		put(cx - hw - 1, y, rim, a);
		put(cx + hw, y, rim, a);
	}
}

/* Half a ring around the planet: the back (top) half or the front. */
static void ring(int cx, int cy, int front, uint32_t c)
{
	const int a = 104, b = 20;
	int last = -1;

	for (int dy = front ? 0 : -b; dy <= (front ? b : 0); dy++) {
		int xr = a * isqrt((unsigned)((b * b - dy * dy) * 256)) / (b * 16);
		int lo = last < 0 ? xr : last < xr ? last : xr, hi = last > xr ? last : xr;

		fill(cx - hi - 1, cy + dy, hi - lo + 3, 2, c);
		fill(cx + lo - 1, cy + dy, hi - lo + 3, 2, c);
		last = xr;
	}
}

/* The band in one colour, two pixels per store (the band is aligned). */
static void clear(uint32_t c)
{
	uint32_t v = rgb(c) * 0x10001u, *p = (uint32_t *)(fb + fb_y0 * W);

	for (int n = (fb_y1 - fb_y0) * W / 2; n > 0; n--)
		*p++ = v;
}

static void sky(void)
{
	static uint32_t ring_c;
	int px = W + 200 - G.planet / FP, py = 200;

	if (!ring_c)
		ring_c = mix(WB_COLOR_PINK, WB_COLOR_VOID, 150);
	clear(WB_COLOR_VOID);
	for (int i = 0; i < N_STARS; i++) {
		int off = (stars[i].near ? G.near : G.far) / FP, x = stars[i].x - off, s = stars[i].near ? 3 : 2;

		if (x < 0)
			x += W;
		fill(x, stars[i].y, s, s, stars[i].c == 0 ? WB_COLOR_TEXT_LOW :
		     stars[i].c == 1 ? WB_COLOR_TEXT_MID : stars[i].c == 2 ? WB_COLOR_CYAN : WB_COLOR_PINK);
	}
	if (px > -150 && px < W + 150 && fb_y0 < py + 80 && fb_y1 > py - 80) {
		ring(px, py, 0, ring_c);
		disk(px, py, 62, WB_COLOR_SURFACE2, WB_COLOR_PINK);
		ring(px, py, 1, ring_c);
	}
}

static void draw_pipe(int x, int cy, int gap)
{
	static uint32_t shine;
	int top = cy - gap / 2, bot = cy + gap / 2;

	if (!shine)
		shine = mix(WB_COLOR_CYAN, WB_COLOR_SURFACE2, 60);
	/* the bodies are plain rectangles (box() per pixel of their edges cost
	 * most of a frame); the lips keep their round corners */
	if (fb_y0 < top) {
		fill(x, 0, PIPE_W, top - LIP_H, WB_COLOR_SURFACE2);
		fill(x, 0, 3, top - LIP_H, WB_COLOR_CYAN);
		fill(x + PIPE_W - 3, 0, 3, top - LIP_H, WB_COLOR_CYAN);
		fill(x + 14, 0, 10, top - LIP_H, shine);
		box(x - LIP_X, top - LIP_H, PIPE_W + 2 * LIP_X, LIP_H, 8, WB_COLOR_SURFACE2, 255,
		    WB_COLOR_CYAN, 3, 0);
		fill(x - LIP_X + 14, top - LIP_H + 6, 10, LIP_H - 12, shine);
	}
	if (fb_y1 > bot) {
		fill(x, bot + LIP_H, PIPE_W, GROUND_Y - bot - LIP_H, WB_COLOR_SURFACE2);
		fill(x, bot + LIP_H, 3, GROUND_Y - bot - LIP_H, WB_COLOR_CYAN);
		fill(x + PIPE_W - 3, bot + LIP_H, 3, GROUND_Y - bot - LIP_H, WB_COLOR_CYAN);
		fill(x + 14, bot + LIP_H, 10, GROUND_Y - bot - LIP_H, shine);
		box(x - LIP_X, bot, PIPE_W + 2 * LIP_X, LIP_H, 8, WB_COLOR_SURFACE2, 255, WB_COLOR_CYAN, 3, 0);
		fill(x - LIP_X + 14, bot + 6, 10, LIP_H - 12, shine);
	}
}

static void ground(void)
{
	if (fb_y1 <= GROUND_Y)
		return;
	fill(0, GROUND_Y, W, H - GROUND_Y, WB_COLOR_SURFACE1);
	for (int y = GROUND_Y + 4; y < H; y++) {	/* stripes, leaning right */
		int sh = G.gnd / FP + (y - GROUND_Y) / 2;

		if (y < fb_y0 || y >= fb_y1)
			continue;
		for (int x = -STRIPE - sh % STRIPE; x < W; x += STRIPE)
			fill(x, y, STRIPE / 2, 1, WB_COLOR_SURFACE2);
	}
	fill(0, GROUND_Y, W, 4, WB_COLOR_PINK);
}

/* ---- sprites: drawn once with wbdraw.h, blended in every frame -------- */

/*
 * Blend premultiplied colours c with alpha a (w x h) in at (x, y). The
 * scale by 256 - a and a shift stand in for put()'s three divisions.
 */
static void blit(const uint16_t *c, const uint8_t *a, int w, int h, int x, int y)
{
	for (int j = 0; j < h; j++) {
		uint16_t *row = fb + (y + j) * W;

		if (y + j < fb_y0 || y + j >= fb_y1)
			continue;
		for (int i = 0; i < w; i++) {
			unsigned k = a[j * w + i], d, s, n, r, g, b;

			if (!k || (unsigned)(x + i) >= W)
				continue;
			s = c[j * w + i];
			if (k == 255) {
				row[x + i] = (uint16_t)s;
				continue;
			}
			d = row[x + i];
			n = 256 - k;
			r = ((d >> 11) * n >> 8) + (s >> 11);
			g = (((d >> 5) & 63) * n >> 8) + ((s >> 5) & 63);
			b = ((d & 31) * n >> 8) + (s & 31);
			row[x + i] = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
		}
	}
}

#define BIRD_SW		64		/* the bird's sprite */
#define BIRD_SH		54
#define BIRD_OX		28		/* the bird's centre in it */
#define BIRD_OY		27

/* wing up, middle, down; and fallen */
static struct { uint16_t c[BIRD_SW * BIRD_SH]; uint8_t a[BIRD_SW * BIRD_SH]; } birds[4];

static void bird_shape(int x, int y, int wing, int dead)
{
	box(x - BIRD_R, y - BIRD_R, 2 * BIRD_R, 2 * BIRD_R, BIRD_R, WB_COLOR_KEY_CROSS, 255, 0, 0, 0);
	box(x - 14, y + 2, 28, 16, 8, WB_COLOR_YELLOW, 255, 0, 0, 0);
	box(x - 26, y - 5 + wing, 24, 14, 7, WB_COLOR_PINK, 255, 0, 0, 0);
	box(x + 3, y - 17, 17, 17, 8, WB_COLOR_TEXT_HI, 255, 0, 0, 0);
	if (dead) {				/* an x for an eye */
		for (int i = 0; i < 7; i++) {
			fill(x + 8 + i, y - 12 + i, 2, 2, WB_COLOR_CONSOLE);
			fill(x + 14 - i, y - 12 + i, 2, 2, WB_COLOR_CONSOLE);
		}
	} else {
		box(x + 12, y - 12, 6, 8, 3, WB_COLOR_CONSOLE, 255, 0, 0, 0);
	}
	box(x + 14, y - 2, 20, 11, 5, WB_COLOR_KEY_TRIANGLE, 255, 0, 0, 0);
	box(x + 14, y + 6, 16, 8, 4, 0xD9722E, 255, 0, 0, 0);
}

/*
 * Draws each pose on black and on white in the band (before the first
 * frame): the difference gives the alpha, the black one the premultiplied
 * colour.
 */
static void make_birds(void)
{
	static const int wings[4] = { -10, -3, 3, 3 };
	static uint16_t on_black[BIRD_SW * BIRD_SH];

	fb_y0 = 0;
	fb_y1 = BIRD_SH;
	for (int k = 0; k < 4; k++) {
		for (int pass = 0; pass < 2; pass++) {
			fill(0, 0, BIRD_SW, BIRD_SH, pass ? 0xFFFFFF : 0);
			bird_shape(BIRD_OX, BIRD_OY, wings[k], k == 3);
			for (int j = 0; !pass && j < BIRD_SH; j++)
				memcpy(on_black + j * BIRD_SW, fb + j * W, BIRD_SW * 2);
		}
		for (int j = 0; j < BIRD_SH; j++)
			for (int i = 0; i < BIRD_SW; i++) {
				unsigned b = on_black[j * BIRD_SW + i], w = fb[j * W + i];
				int a = 255 - ((int)((w >> 5) & 63) - (int)((b >> 5) & 63)) * 255 / 63;

				birds[k].c[j * BIRD_SW + i] = (uint16_t)b;
				birds[k].a[j * BIRD_SW + i] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
			}
	}
}

static void bird(void)
{
	int k = G.mode == DEAD ? 3 : G.flap_t < 5 ? 0 : G.flap_t < 10 ? 1 : 2;

	blit(birds[k].c, birds[k].a, BIRD_SW, BIRD_SH, BIRD_X - BIRD_OX, G.y / FP - BIRD_OY);
}

/* Text at twice a font's size, for the score. */
static int big_w(const struct wb_font *f, const char *s)
{
	return 2 * text_w(f, s, 0);
}

/* the glyph's coverage at (i, j), 0 outside it */
static unsigned cov(const struct wb_glyph *g, const uint8_t *a, int i, int j)
{
	return i < 0 || j < 0 || i >= g->w || j >= g->h ? 0 : a[j * g->w + i];
}

/* A line of f_ui36b at twice its size, as an alpha mask; made again only
 * when its text changes. Pixel (1, 1) of the mask is the line's top left. */
#define BIG_W		300
#define BIG_H		96
struct big { char s[12]; int w; uint8_t a[BIG_W * BIG_H]; };
static struct big big_score, big_title, big_card;

/* Scaled bilinearly: an output pixel lies a quarter pixel from its source
 * pixel's centre, towards the neighbour that weighs 1/4. */
static void big_make(struct big *m, const char *s)
{
	const struct wb_font *f = &f_ui36b;
	int x16 = 16;

	if (m->w && !strcmp(m->s, s))
		return;
	strncpy(m->s, s, sizeof(m->s) - 1);
	m->w = big_w(f, s) + 4 > BIG_W ? BIG_W : big_w(f, s) + 4;
	memset(m->a, 0, sizeof(m->a));
	while (*s) {
		const struct wb_glyph *g = glyph(f, next_cp(&s));
		const uint8_t *a = f->bits + g->off;
		int gx = (x16 + 8) / 16 + 2 * g->x, gy = 1 + 2 * g->y;

		for (int j = -1; j <= 2 * g->h; j++) {
			int sj = j >> 1, nj = j & 1 ? sj + 1 : sj - 1;

			for (int i = -1; i <= 2 * g->w; i++) {
				int si = i >> 1, ni = i & 1 ? si + 1 : si - 1, X = gx + i, Y = gy + j;
				unsigned v = (9 * cov(g, a, si, sj) + 3 * cov(g, a, ni, sj) +
					      3 * cov(g, a, si, nj) + cov(g, a, ni, nj)) / 16;

				if (X >= 0 && X < BIG_W && Y >= 0 && Y < BIG_H && v > m->a[Y * BIG_W + X])
					m->a[Y * BIG_W + X] = (uint8_t)v;
			}
		}
		x16 += 2 * g->adv16;
	}
}

/* the mask in colour c, its line's top left at (x, y) */
static void big_draw(const struct big *m, int x, int y, uint32_t c)
{
	unsigned v = rgb(c), cr = v >> 11, cg = (v >> 5) & 63, cb = v & 31;

	x--;
	y--;
	for (int j = 0; j < BIG_H; j++) {
		uint16_t *row = fb + (y + j) * W;

		if (y + j < fb_y0 || y + j >= fb_y1)
			continue;
		for (int i = 0; i < m->w; i++) {
			unsigned k = m->a[j * BIG_W + i], d, n;

			if (!k || (unsigned)(x + i) >= W)
				continue;
			d = row[x + i];
			n = 256 - k;
			row[x + i] = (uint16_t)((((d >> 11) * n + cr * k) >> 8) << 11 |
						((((d >> 5) & 63) * n + cg * k) >> 8) << 5 |
						(((d & 31) * n + cb * k) >> 8));
		}
	}
}

static void centred(const struct wb_font *f, int y, const char *s, uint32_t c)
{
	text(f, (W - text_w(f, s, 0)) / 2, y, s, c, 0);
}

static void score(int y)
{
	char buf[12];
	int x;

	big_make(&big_score, utoa((unsigned)G.score, buf + sizeof(buf)));
	x = (W - big_score.w + 4) / 2;
	big_draw(&big_score, x + 3, y + 3, WB_COLOR_CONSOLE);
	big_draw(&big_score, x, y, WB_COLOR_TEXT_HI);
}

static void ready_screen(void)
{
	const struct wb_font *f = &f_ui22;
	int cy = 470, w, x;
	char buf[24], *n;

	text(&f_mono14, (W - text_w(&f_mono14, "GAMES", 22)) / 2, 96, "GAMES", WB_COLOR_CYAN, 22);
	big_make(&big_title, "Flappy");
	big_draw(&big_title, (W - big_title.w + 4) / 2, 120, WB_COLOR_TEXT_HI);
	/* "[Space] or [▲] to flap" */
	w = keycap_w("Space") + 18 + 10 + text_w(f, "or", 0) + 10 + keycap_w("#2") + 18 + 10 +
	    text_w(f, "to flap", 0);
	x = (W - w) / 2;
	x = keycap(x, cy, "Space") + 10;
	x = text(f, x, text_top(f, cy), "or", WB_COLOR_TEXT_MID, 0) + 10;
	x = keycap(x, cy, "#2") + 10;
	text(f, x, text_top(f, cy), "to flap", WB_COLOR_TEXT_MID, 0);
	if (G.best) {
		n = utoa((unsigned)G.best, buf + sizeof(buf));
		memcpy(n - 5, "Best ", 5);
		centred(&f_mono16b, 512, n - 5, WB_COLOR_TEXT_LOW);
	}
}

static void dead_card(void)
{
	const int cw = 400, ch = 250, x = (W - cw) / 2, y = 180;
	char buf[24], *n;

	box_plain(x, y, cw, ch, WB_RADIUS_LG, WB_COLOR_SURFACE1);
	text(&f_mono14, (W - text_w(&f_mono14, "GAME OVER", 22)) / 2, y + 30, "GAME OVER",
	     WB_COLOR_PINK, 22);
	big_make(&big_card, utoa((unsigned)G.score, buf + sizeof(buf)));
	big_draw(&big_card, (W - big_card.w + 4) / 2, y + 62, WB_COLOR_TEXT_HI);
	if (G.new_best) {
		centred(&f_ui22, y + 170, "New best!", WB_COLOR_YELLOW);
	} else {
		n = utoa((unsigned)G.best, buf + sizeof(buf));
		memcpy(n - 5, "Best ", 5);
		centred(&f_ui22, y + 170, n - 5, WB_COLOR_TEXT_MID);
	}
}

static void draw_help(void)
{
	static const struct help_key keys[] = {
		{ "Space", "Flap (Enter and \xe2\x86\x91 too)" },
		{ "SYMS", "Flap: any symbol key but the diamond" },
		{ "Esc", "Pause: resume, restart or quit" },
		{ "#6", "These keys (the game waits)" },
		{ "Logo", "Back to the menu" },
	};

	help_popup("Flappy", "Fly through the gaps: each pipe you pass is a point. "
		   "A pipe or the ground ends the game.", keys, sizeof(keys) / sizeof(keys[0]));
}

static void draw_band(void)
{
	static const struct hint ready_hints[] = { { "Space", "flap", 0 }, { "Esc", "pause", 1 } };
	static const struct hint dead_hints[] = { { "Space", "again", 0 }, { "Esc", "pause", 1 } };
	static const struct hint pause_hints[] = { { "Enter", "choose", 0 }, { "Esc", "resume", 1 } };
	static const struct menu_item items[P_N] = {
		{ .label = "Resume" }, { .label = "Restart" }, { .label = "Keys" },
		{ .label = "Quit", .sep = 1 },
	};

	sky();
	for (int i = 0; i < N_PIPES; i++) {
		int x = G.p[i].x / FP;

		if (x < W + LIP_X && x > -(PIPE_W + LIP_X))
			draw_pipe(x, G.p[i].cy, G.p[i].gap);
	}
	ground();
	bird();
	if (G.mode == READY) {
		ready_screen();
		hint_bar(ready_hints, 2);
	} else if (G.mode == DEAD && G.t >= CARD_T) {
		dead_card();
		hint_bar(dead_hints, 2);
	} else {
		score(40);
	}
	if (G.help)
		draw_help();
	else if (G.paused)
		menu_popup("FLAPPY", "Paused", 0, items, 0, P_N, G.focus, -1, 0, 0, pause_hints, 2);
}

static void init_stars(void)
{
	for (int i = 0; i < N_STARS; i++) {
		stars[i].x = (int16_t)(rnd() % W);
		stars[i].y = (int16_t)(rnd() % (GROUND_Y - 20));
		stars[i].near = i % 3 == 0;
		stars[i].c = (uint8_t)(i % 7 == 0 ? 2 + (i / 7) % 2 : i % 2);
	}
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

#define BAND_H	20	/* measured on the badge: 41 ms a frame; 40 rows 54, 8 rows 54 */
static uint32_t band32[W * BIRD_SH / 2];	/* BIRD_SH rows: make_birds draws here */
#define band	((uint16_t *)band32)
#ifdef FLAPPY_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void draw(void)
{
	for (int y0 = 0; y0 < H; y0 += BAND_H) {
		int y1 = y0 + BAND_H < H ? y0 + BAND_H : H;

		fb = band - (long)y0 * W;
		fb_y0 = y0;
		fb_y1 = y1;
		draw_band();
#ifdef FLAPPY_HOST
		memcpy(screen + y0 * W, band, (size_t)(y1 - y0) * W * 2);
#else
		pwrite(fb_fd, band, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
	}
}

/* ---- the host test ------------------------------------------------------ */

#ifdef FLAPPY_HOST
static void save_ppm(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	static uint8_t rgb_[W * H * 3];
	static const char hdr[] = "P6\n720 720\n255\n";

	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];

		rgb_[i * 3] = (uint8_t)((p >> 11) << 3);
		rgb_[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) << 2);
		rgb_[i * 3 + 2] = (uint8_t)((p & 31) << 3);
	}
	if (fd < 0 || write(fd, hdr, sizeof(hdr) - 1) < 0 || write(fd, rgb_, sizeof(rgb_)) < 0)
		_exit(1);
	close(fd);
}

/* Flaps when below the next gap's centre and falling. */
static void autopilot(void)
{
	int target = (GAP_LO + GAP_HI) / 2, best = 1 << 30;

	for (int i = 0; i < N_PIPES; i++) {
		int right = G.p[i].x + (PIPE_W + LIP_X) * FP;

		if (right > (BIRD_X - HIT_R) * FP && G.p[i].x < best) {
			best = G.p[i].x;
			target = G.p[i].cy;
		}
	}
	if (G.mode != DEAD && G.y > (target + 12) * FP && G.v >= 0)
		flap();
}

/*
 * flappy-host SCRIPT [OUT.ppm]: SCRIPT is words: f (a flap: Space), kN (key
 * code N), tN (N ticks), aN (N ticks of autopilot). Prints the state; the
 * screen goes to OUT.ppm. The seed is fixed.
 */
int main(int argc, char **argv)
{
	const char *s = argc > 1 ? argv[1] : "";
	char out[256] = "", num[12];

	G.seed = 12345;
	init_stars();
	fb = band;
	make_birds();
	load_best();
	reset();
	while (*s) {
		int n = 0;
		char c = *s++;

		while (*s >= '0' && *s <= '9')
			n = n * 10 + *s++ - '0';
		while (*s == ' ')
			s++;
		if (c == 'f')
			key(KEY_SPACE);
		else if (c == 'k' && key(n)) {
			write(1, "closed\n", 7);
			return 0;
		} else if (c == 't' || c == 'a') {
			for (int i = 0; i < n; i++) {
				if (c == 'a' && running())
					autopilot();
				if (running())
					step();
			}
		}
	}
	draw();
	if (argc > 2)
		save_ppm(argv[2]);
	strcat(out, G.mode == READY ? "mode ready" : G.mode == PLAY ? "mode play" : "mode dead");
	strcat(out, G.help ? " help" : G.paused ? " paused" : "");
	strcat(out, "\nscore ");
	strcat(out, utoa((unsigned)G.score, num + sizeof(num)));
	strcat(out, "\nbest ");
	strcat(out, utoa((unsigned)G.best, num + sizeof(num)));
	strcat(out, "\ny ");
	strcat(out, utoa((unsigned)(G.y / FP), num + sizeof(num)));
	strcat(out, "\nfocus ");
	strcat(out, utoa((unsigned)G.focus, num + sizeof(num)));
	strcat(out, "\n");
	write(1, out, strlen(out));
	return 0;
}

#else

/* ---- the badge ---------------------------------------------------------- */

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t stop_sig;

static void on_signal(int s)
{
	stop_sig = s;
}

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/*
 * Test hook, as meshchat's /tmp/meshchat.keys (docs/HOME.md): decimal key
 * codes written to this FIFO, separated by spaces or newlines, count as
 * key presses. Made when the game starts, root only, gone when it ends.
 */
#define KEYS_FIFO	"/tmp/flappy.keys"
static char fifo_buf[128];
static int fifo_len;

/* The complete codes in fifo_buf, one by one; 1 to quit. */
static int fifo_keys(void)
{
	int i = 0, done = 0;

	for (;;) {
		int start, v = 0, digits = 0;

		while (i < fifo_len && (fifo_buf[i] == ' ' || fifo_buf[i] == '\n'))
			i++;
		start = i;
		while (i < fifo_len && fifo_buf[i] >= '0' && fifo_buf[i] <= '9')
			v = v * 10 + fifo_buf[i++] - '0', digits++;
		if (i == fifo_len) {		/* the rest may still come */
			i = start;
			break;
		}
		i++;
		if (digits && !done)
			done = key(v);
	}
	memmove(fifo_buf, fifo_buf + i, (size_t)(fifo_len - i));
	fifo_len -= i;
	return done;
}

/* draw_s seconds and draw_us microseconds drawing: 32-bit sums, as there
 * is no 64-bit division */
static void log_stats(int frames, unsigned draw_s, unsigned draw_us)
{
	char line[96] = "flappy: ", num[12];
	int tenths = frames ? (int)((draw_s * 10000 + draw_us / 100) / (unsigned)frames) : 0;

	strcat(line, utoa((unsigned)frames, num + sizeof(num)));
	strcat(line, " frames, ");
	strcat(line, utoa((unsigned)(tenths / 10), num + sizeof(num)));
	strcat(line, ".");
	strcat(line, utoa((unsigned)(tenths % 10), num + sizeof(num)));
	strcat(line, " ms to draw one; best ");
	strcat(line, utoa((unsigned)G.best, num + sizeof(num)));
	strcat(line, "\n");
	write(2, line, strlen(line));
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p[2];
	int ev, fifo = -1, nfds = 1, quit = 0, dirty = 1, was_running = 0, frames = 0;
	long long tick_due = 0, last_frame = 0;
	unsigned draw_s = 0, draw_us = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev < 0)
		return 1;
	unlink(KEYS_FIFO);
	if (mkfifo(KEYS_FIFO, 0600) == 0)
		fifo = open(KEYS_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	p[0].fd = ev;
	p[0].events = POLLIN;
	if (fifo >= 0) {
		p[1].fd = fifo;
		p[1].events = POLLIN;
		nfds = 2;
	}
	G.seed = (uint32_t)time(0) ^ (uint32_t)getpid() << 16;
	if (!G.seed)
		G.seed = 1;
	init_stars();
	fb = band;
	make_birds();
	load_best();
	reset();
	while (!stop_sig && !quit) {
		long long now = now_us();
		int run = running(), timeout = -1, n = 0;

		if (run && !was_running)
			tick_due = now;
		was_running = run;
		if (run) {
			while (tick_due <= now && n++ < 8) {
				step();
				tick_due += TICK_US;
			}
			if (tick_due <= now)		/* far behind: let it go */
				tick_due = now + TICK_US;
		}
		if (dirty || (run && now - last_frame >= FRAME_US - 2000)) {
			draw();
			last_frame = now;
			draw_us += (unsigned)(now_us() - now);
			if (draw_us >= 1000000) {
				draw_s++;
				draw_us -= 1000000;
			}
			frames++;
			dirty = 0;
		}
		if (running()) {
			timeout = (int)(last_frame + FRAME_US - now_us()) / 1000;
			if (timeout < 0)
				timeout = 0;
		}
		if (poll(p, (nfds_t)nfds, timeout) <= 0)
			continue;
		if (p[0].revents & POLLIN) {
			struct ev evs[16];
			int got = (int)read(ev, evs, sizeof(evs)) / (int)sizeof(evs[0]);

			for (int q = 0; q < got && !quit; q++)
				if (evs[q].type == 1 && evs[q].value == 1) {	/* presses, not repeats */
					quit = key(evs[q].code);
					dirty = 1;
				}
		}
		if (nfds > 1 && (p[1].revents & POLLIN)) {
			int r = (int)read(fifo, fifo_buf + fifo_len, sizeof(fifo_buf) - (size_t)fifo_len);

			if (r > 0) {
				fifo_len += r;
				quit |= fifo_keys();
				dirty = 1;
			}
			if (fifo_len == (int)sizeof(fifo_buf))
				fifo_len = 0;
		}
	}
	if (fifo >= 0)
		unlink(KEYS_FIFO);
	if (G.score > G.best) {		/* the logo key in the middle of a game */
		G.best = G.score;
		save_best();
	}
	log_stats(frames, draw_s, draw_us);
	return 0;
}
#endif
