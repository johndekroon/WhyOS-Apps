// SPDX-License-Identifier: MIT
/*
 * marble — Marble Orbit: tilt the badge to roll a marble into the ring, in
 * eighteen levels of six worlds (levels.h), drawn like the menu (wbdraw.h,
 * the design's fonts) as a store app (SOURCES.md).
 *
 * A level is one screen of 18 x 18 tiles of 40 px; the top row is the HUD.
 * The marble rolls with the accelerometer (the BMI270 through IIO, as sleepd
 * reads it; ROTATION=locked leaves it to the app and keeps the screen
 * upright), or with the arrows. Each surface has its own friction; holes,
 * lasers and the black hole cost one of three lives, after which the level
 * starts again. Stars, lives left and time under par add to a level's
 * score; the best of each level, summed, is the high score. Kept in
 * APP_CONFIG (flash): scores and settings.
 *
 * A full-screen game (Docs/06-design-system.md 6.9): Esc pauses (Resume,
 * Restart level, Levels, Keys, Quit), the diamond shows the keys, the
 * cloud the settings, the circle the results; the logo key ends it through
 * apprun.
 *
 * Ticks of 1/60 s in fixed point (FP, 1/256 px): the toolchain is
 * soft-float. The screen is drawn in bands of one tile row, and only the
 * rows that changed (the marble's, moving things', the HUD's): a band is
 * built from tiles rendered once per level into a cache, then the things
 * on them. On exit the frame count, the time to draw and to read the
 * sensor go to stderr (apprun's log).
 *
 * Built with -DMARBLE_HOST, it takes a script from its first argument and
 * writes the screen as PPM to its second: the host test (test_marble.py).
 * Test hook on the badge: key codes written to /tmp/marble.keys.
 *
 * FLAT/NOMMU constraints (why2025-linux overlay-src/Makefile): no stdio, no 64-bit
 * division.
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>		/* rename */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"
#include "levels.h"

#ifndef MARBLE_HOST
#include <poll.h>
#include <sys/stat.h>
#endif

/* ---- the game's numbers --------------------------------------------------- */

#define FP		256		/* fixed point: 1/256 px */
#define TICK_US		16667		/* 60 ticks a second */
#define FRAME_US	33333		/* at most 30 frames */
#define T		40		/* a tile, px */
#define N		18		/* tiles across and down */
#define R		14		/* the marble's radius, px */
#define VMAX		(10 * FP)	/* px per tick, each way */
#define STEP_MAX	(4 * FP)	/* moved at most this far between collisions */
#define VJUMP		640		/* 2.5 px a tick up a ramp: it jumps */
#define SLOPE		22		/* a ramp's pull back, FP per tick per tick */
#define WATER_PUSH	10
#define BELT_PUSH	24
#define ARROW_TILT	1600		/* an arrow held: 0.4 g */
#define DEADZONE	40		/* sensor counts */
#define BUMP_R		14
#define MAGNET_R	130		/* px */
#define MAGNET_A	40		/* FP per tick per tick, at the centre */
#define HOLE_R		300		/* the black hole's reach */
#define HOLE_A		45
#define BLOCK_V		384		/* 1.5 px a tick */
#define READY_T		90		/* ticks of the level's card */
#define FALL_T		36
#define OVER_T		150
#define LIVES		3
#define ANIM_T		6		/* animations at 10 a second */

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_ENTER	28
#define KEY_SPACE	57
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108

/* ---- worlds --------------------------------------------------------------- */

struct world {
	const char *name;
	uint32_t floor, grain, alt, wall, wall_hi, wall_lo, hole, rim, accent;
	uint32_t ball, ball_rim, ball_hi, shadow;
	int friction;			/* the floor's, per 1024 a tick */
};

static const struct world worlds[6] = {
	{ "WORKSHOP", 0xC4955E, 0xAF8250, 0xCFA36C, 0x6E4428, 0x9A643C, 0x45281A, 0x1B110A, 0x0E0805,
	  0xFFC857, 0xB8BEC8, 0x5E6672, 0xF2F4F8, 0x3A2412, 12 },
	{ "DESERT", 0xE7C38A, 0xD6AC6C, 0xEDCC97, 0xB47A45, 0xD3985F, 0x82532B, 0x2A1A10, 0x150C06,
	  0xFF9A3D, 0x3D8BFF, 0x1F4FA8, 0xC9DEFF, 0x6E4E26, 12 },
	{ "ICE", 0xE6EFF7, 0xD0DFEC, 0xF2F7FB, 0x86B2D4, 0xC4E1F4, 0x587FA0, 0x10263A, 0x06121C,
	  0x61F2FF, 0xF25E95, 0xA8325F, 0xFFC9DC, 0x7890A8, 12 },
	{ "TEMPLE", 0x5F7A4B, 0x4E693C, 0x688454, 0x8E8471, 0xB0A690, 0x5D5646, 0x0E140D, 0x050805,
	  0xE8C14A, 0xFFE14D, 0xB08D22, 0xFFF6C4, 0x1E2A16, 12 },
	{ "FACTORY", 0x4B5059, 0x5E646E, 0x464B53, 0x2C3037, 0x454B55, 0x191C21, 0x060708, 0x000000,
	  0xFFC400, 0xFF9A3D, 0xA85612, 0xFFDDB8, 0x15171B, 12 },
	{ "NEON ORBIT", WB_COLOR_VOID, 0x17172C, 0x0D0D1A, WB_COLOR_SURFACE2, WB_COLOR_CYAN,
	  WB_COLOR_SURFACE1, 0x000000, WB_COLOR_PINK, WB_COLOR_CYAN, WB_COLOR_TEXT_HI, 0x7C7C99,
	  0xFFFFFF, 0x000000, 5 },
};

/* the elements' own colours, the same in every world */
#define C_SAND		0xD7A35E
#define C_SAND_DOT	0xBC8743
#define C_ICE		0xBFE4F8
#define C_ICE_HI	0xF0FAFF
#define C_CRACK		0x5F8EB4
#define C_SNOW		0xFAFCFF
#define C_SNOW_LO	0xC2D4E6
#define C_WATER		0x2E7DA6
#define C_WATER_HI	0x7FC4E8
#define C_BELT		0x24272C
#define C_BELT_HI	0xFFC400
#define C_RAMP		0x9A6738
#define C_RAMP_HI	0xD49A5E
#define C_GOLD		0xE8C14A
#define C_GOLD_LO	0x9C7F22
#define C_GATE		0x4A3A22
#define C_GATE_HI	0x8A7246
#define C_BUMPER	0xFF4D5E
#define C_MAGNET	0x3D8BFF
#define C_LASER		0xFF3B3B
#define C_STAR		WB_COLOR_YELLOW

/* ---- tiles ------------------------------------------------------------------ */

enum { FLOOR, WALL, HOLE, SAND, ICE, CRUMBLE, SNOW, WATER, BELT, RAMP, BUTTON, GATE, OPEN, PORTAL,
       LASER, CHECK, GOAL };
enum { UP, RIGHT, DOWN, LEFT };		/* d of water, belts and ramps */
static const int dx4[4] = { 0, 1, 0, -1 }, dy4[4] = { -1, 0, 1, 0 };

struct tile {
	uint8_t k, d;	/* PORTAL: pair * 2 + the second end; LASER: phase, + 2 when it runs up and down */
	uint8_t f;	/* CRUMBLE: rolled on; HOLE: broken ice; BUTTON, CHECK: on */
};

#define MAX_OBJ		24

static struct {
	struct tile t[N][N];
	int li;				/* the level */
	int stars, n_stars;		/* taken, in the level */
	struct { int x, y, got; } star[4];
	struct { int x, y, hit; } bump[MAX_OBJ];
	int n_bump;
	struct { int x, y; } magnet[8];
	int n_magnet;
	int hole_x, hole_y, has_hole;	/* the black hole, px */
	struct { int x, y, v, vert; } block[8];	/* top left, FP */
	int n_block;
	int portal[3][2][2];		/* pair, end: tile x, y */
	int gx, gy;			/* the goal's centre, px */
	int sx, sy;			/* where the marble comes back, px */
	int anim_rows;			/* rows that move with the animation */
	int gravity, laser;		/* the level's (levels.h) */
} L;

static struct {
	int x, y, vx, vy;		/* FP */
	int air, air_n;			/* ticks of the jump left, and its length */
	int portal_lock;
	int fall_x, fall_y;		/* where it falls, px */
} B;

enum { TITLE, READY, PLAY, FALL, WON, OVER };
enum { POP_NONE, POP_PAUSE, POP_HELP, POP_SETTINGS, POP_SENS, POP_NEUTRAL, POP_RESULTS };
enum { P_RESUME, P_RESTART, P_LEVELS, P_KEYS, P_QUIT, P_N };

static struct {
	int mode, t;			/* ticks in this mode */
	int time;			/* ticks played in this level */
	int lives;
	int pop, pf, help_back;		/* the pop-up, its focus, where help goes back to */
	int sel;			/* the title's level */
	int score, new_best;		/* the last level finished */
	int anim;
	uint32_t dirty;			/* rows to draw */
	int tilt_x, tilt_y;		/* sensor counts, 4096 a g, after the neutral */
	int raw_x, raw_y;		/* filtered, before it */
	int zero_x, zero_y, zero_n, zsum_x, zsum_y;
	int arrows;			/* held: 1 up 2 right 4 down 8 left */
	int no_sensor;
	/* settings */
	int control;			/* 0 tilt, 1 arrows */
	int sens;			/* 0 low, 1 normal, 2 high */
	int neutral;			/* 0 as held at the level's start, 1 flat */
	/* scores, kept */
	int best[N_LEVELS], best_t[N_LEVELS], best_s[N_LEVELS];
	int unlocked;
} G;

static const int sens_pct[3] = { 70, 100, 140 };
static const char *const sens_names[3] = { "Low", "Normal", "High" };
static const char *const neutral_names[2] = { "As held at the start", "Flat" };

static int world_of(int li)
{
	return li / 3;
}

static const struct world *WD(void)
{
	return &worlds[world_of(L.li)];
}

/* ---- small helpers ------------------------------------------------------- */

static char *utoa(unsigned v, char *end)
{
	*--end = 0;
	do
		*--end = (char)('0' + v % 10);
	while (v /= 10);
	return end;
}

static char *cat(char *d, const char *s)
{
	while (*d)
		d++;
	while ((*d = *s++))
		d++;
	return d;
}

static char *cat_u(char *d, unsigned v)
{
	char b[12];

	return cat(d, utoa(v, b + sizeof(b)));
}

/* "1:05.3" for a time in ticks */
static char *cat_time(char *d, int ticks)
{
	int tenths = ticks / 6, s = tenths / 10;

	cat_u(d, (unsigned)(s / 60));
	cat(d, s % 60 < 10 ? ":0" : ":");
	cat_u(d, (unsigned)(s % 60));
	cat(d, ".");
	return cat_u(d, (unsigned)(tenths % 10));
}

static uint32_t seed = 1;

static uint32_t rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

/* a small sine, -64..64, a period of 32 */
static int sine(int i)
{
	static const int8_t q[9] = { 0, 12, 24, 36, 45, 53, 59, 63, 64 };

	i &= 31;
	return i < 8 ? q[i] : i < 16 ? q[16 - i] : i < 24 ? -q[i - 16] : -q[32 - i];
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned t)	/* t/255 of a over b */
{
	uint32_t r = 0;

	for (int s = 0; s < 24; s += 8)
		r |= ((((a >> s) & 255) * t + ((b >> s) & 255) * (255 - t)) / 255) << s;
	return r;
}

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void mark(int y0, int y1)	/* rows of px y0..y1 to draw */
{
	y0 = y0 < 0 ? 0 : y0 / T;
	y1 = y1 >= H ? N - 1 : y1 / T;
	for (int r = y0; r <= y1; r++)
		G.dirty |= 1u << r;
}

static void mark_all(void)
{
	G.dirty = (1u << N) - 1;
}

/* ---- scores and settings, in APP_CONFIG --------------------------------- */

static int conf_path(char *buf, int size, const char *name)
{
	const char *d = getenv("APP_CONFIG");

	if (!d || !*d || (int)(strlen(d) + strlen(name)) + 2 > size)
		return 0;
	strcpy(buf, d);
	strcat(buf, "/");
	strcat(buf, name);
	return 1;
}

static int read_conf(const char *name, char *buf, int size)
{
	char path[256];
	int fd, n;

	if (!conf_path(path, sizeof(path), name) || (fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	n = (int)read(fd, buf, (size_t)size - 1);
	close(fd);
	buf[n < 0 ? 0 : n] = 0;
	return n > 0;
}

static void write_conf(const char *name, const char *s)
{
	char path[256], tmp[262];
	int fd;

	if (!conf_path(path, sizeof(path), name))
		return;
	strcpy(tmp, path);
	strcat(tmp, ".new");
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, s, strlen(s)) < 0 || close(fd) < 0)
		return;
	rename(tmp, path);
}

/* the line starting "key " or "key=": what follows, or 0 */
static const char *conf_line(const char *buf, const char *key)
{
	int n = (int)strlen(key);

	for (const char *p = buf; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : 0)
		if (!strncmp(p, key, (size_t)n) && (p[n] == ' ' || p[n] == '='))
			return p + n + 1;
	return 0;
}

/* a number at *p, moving past it and the spaces after; def if none */
static int conf_num(const char **p, int def)
{
	int v = 0, neg = 0;

	if (!*p)
		return def;
	if (**p == '-')
		neg = 1, (*p)++;
	if (**p < '0' || **p > '9')
		return def;
	while (**p >= '0' && **p <= '9' && v < 100000000)
		v = v * 10 + *(*p)++ - '0';
	while (**p == ' ')
		(*p)++;
	return neg ? -v : v;
}

static int conf_int(const char *buf, const char *key, int def)
{
	const char *p = conf_line(buf, key);

	return conf_num(&p, def);
}

static void load_conf(void)
{
	static char buf[1024];
	char key[8] = "L";

	G.sens = 1;
	if (read_conf("settings", buf, sizeof(buf))) {
		G.control = conf_int(buf, "control", 0) == 1;
		G.sens = conf_int(buf, "sensitivity", 1);
		G.sens = G.sens < 0 || G.sens > 2 ? 1 : G.sens;
		G.neutral = conf_int(buf, "neutral", 0) == 1;
	}
	if (!read_conf("scores", buf, sizeof(buf)))
		return;
	G.unlocked = conf_int(buf, "unlocked", 0);
	G.unlocked = G.unlocked < 0 ? 0 : G.unlocked >= N_LEVELS ? N_LEVELS - 1 : G.unlocked;
	for (int i = 0; i < N_LEVELS; i++) {
		const char *p;

		key[1] = 0;
		cat_u(key, (unsigned)i);
		if (!(p = conf_line(buf, key)))
			continue;
		G.best[i] = conf_num(&p, 0);
		G.best_t[i] = conf_num(&p, 0);
		G.best_s[i] = clamp(conf_num(&p, 0), 0, 3);
	}
}

static void save_settings(void)
{
	char b[96] = "control=";

	cat(cat_u(cat(cat_u(cat(cat_u(b, (unsigned)G.control), "\nsensitivity="), (unsigned)G.sens),
		  "\nneutral="), (unsigned)G.neutral), "\n");
	write_conf("settings", b);
}

static void save_scores(void)
{
	static char b[1024];

	b[0] = 0;
	cat(cat_u(cat(b, "# Marble Orbit: level score ticks stars\nunlocked "), (unsigned)G.unlocked), "\n");
	for (int i = 0; i < N_LEVELS; i++) {
		if (!G.best[i])
			continue;
		cat_u(cat(b, "L"), (unsigned)i);
		cat_u(cat(b, " "), (unsigned)G.best[i]);
		cat_u(cat(b, " "), (unsigned)G.best_t[i]);
		cat(cat_u(cat(b, " "), (unsigned)G.best_s[i]), "\n");
	}
	write_conf("scores", b);
}

static int total_score(void)
{
	int s = 0;

	for (int i = 0; i < N_LEVELS; i++)
		s += G.best[i];
	return s;
}

/* ---- a level --------------------------------------------------------------- */

static const char *level_map(int li)
{
#ifdef MARBLE_HOST
	const char *m = getenv("MARBLE_MAP");	/* the test's own map, for level 0 */

	if (li == 0 && m && strlen(m) == N * N)
		return m;
#endif
	return levels[li].map;
}

static int laser_on(const struct tile *t)
{
	int p = L.laser;

	return p && (G.time / p) % 2 == (t->d & 1);
}

static struct tile *at(int tx, int ty)
{
	static struct tile wall = { WALL, 0, 0 };

	return tx < 0 || ty < 0 || tx >= N || ty >= N ? &wall : &L.t[ty][tx];
}

static void load_level(int li)
{
	const char *m = level_map(li);
	static const char dirs[] = "URDL", belts[] = "urdl", ramps[] = "^>v<", portals[] = "TtYyZz";

	memset(&L, 0, sizeof(L));
	L.li = li;
	L.gravity = levels[li].gravity;
	L.laser = levels[li].laser;
#ifdef MARBLE_HOST
	if (li == 0 && getenv("MARBLE_MAP")) {	/* and its own numbers */
		L.gravity = getenv("MARBLE_GRAVITY") ? atoi(getenv("MARBLE_GRAVITY")) : 100;
		L.laser = getenv("MARBLE_LASER") ? atoi(getenv("MARBLE_LASER")) : 0;
	}
#endif
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			char c = m[y * N + x];
			struct tile *t = &L.t[y][x];
			const char *p;
			int px = x * T + T / 2, py = y * T + T / 2;

			t->k = FLOOR;
			if (c == '#')
				t->k = WALL;
			else if (c == 'O')
				t->k = HOLE;
			else if (c == 's')
				t->k = SAND;
			else if (c == 'i')
				t->k = ICE;
			else if (c == 'c')
				t->k = CRUMBLE;
			else if (c == 'w')
				t->k = SNOW;
			else if (c == 'k')
				t->k = BUTTON;
			else if (c == 'g')
				t->k = GATE;
			else if (c == 'X')
				t->k = CHECK;
			else if (c == 'G') {
				t->k = GOAL;
				L.gx = px;
				L.gy = py;
			} else if (c == 'S') {
				L.sx = px;
				L.sy = py;
			} else if (c == '=' || c == '~') {
				t->k = LASER;
				t->d = c == '~';
			} else if ((p = strchr(dirs, c)) && c) {
				t->k = WATER;
				t->d = (uint8_t)(p - dirs);
			} else if ((p = strchr(belts, c)) && c) {
				t->k = BELT;
				t->d = (uint8_t)(p - belts);
			} else if ((p = strchr(ramps, c)) && c) {
				t->k = RAMP;
				t->d = (uint8_t)(p - ramps);
			} else if ((p = strchr(portals, c)) && c) {
				int i = (int)(p - portals);

				t->k = PORTAL;
				t->d = (uint8_t)i;
				L.portal[i / 2][i % 2][0] = x;
				L.portal[i / 2][i % 2][1] = y;
			} else if (c == '*' && L.n_stars < 4) {
				L.star[L.n_stars].x = px;
				L.star[L.n_stars++].y = py;
			} else if (c == 'B' && L.n_bump < MAX_OBJ) {
				L.bump[L.n_bump].x = px;
				L.bump[L.n_bump++].y = py;
			} else if (c == 'M' && L.n_magnet < 8) {
				L.magnet[L.n_magnet].x = px;
				L.magnet[L.n_magnet++].y = py;
			} else if (c == 'H') {
				L.hole_x = px;
				L.hole_y = py;
				L.has_hole = 1;
			} else if ((c == 'b' || c == 'q') && L.n_block < 8) {
				L.block[L.n_block].x = x * T * FP;
				L.block[L.n_block].y = y * T * FP;
				L.block[L.n_block].v = BLOCK_V;
				L.block[L.n_block++].vert = c == 'q';
			}
			if (t->k == WATER || t->k == BELT || t->k == PORTAL || t->k == LASER || t->k == GOAL)
				L.anim_rows |= 1u << y;
		}
	/* a laser beam runs along the wall it closes */
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			struct tile *t = &L.t[y][x], *l = at(x - 1, y), *r = at(x + 1, y);

			if (t->k == LASER && !(l->k == WALL || l->k == LASER || r->k == WALL || r->k == LASER))
				t->d |= 2;
		}
	for (int i = 0; i < L.n_bump; i++)
		L.anim_rows |= 1u << (L.bump[i].y / T);
	for (int i = 0; i < L.n_stars; i++)
		L.anim_rows |= 1u << (L.star[i].y / T);
	if (L.has_hole)
		for (int r = (L.hole_y - 100) / T; r <= (L.hole_y + 100) / T; r++)
			if (r >= 0 && r < N)
				L.anim_rows |= 1u << r;
	L.anim_rows |= 1;		/* the HUD's clock */
}

/* ---- the game --------------------------------------------------------------- */

static void place_marble(int x, int y)
{
	B.x = x * FP;
	B.y = y * FP;
	B.vx = B.vy = 0;
	B.air = 0;
	B.portal_lock = 1;
}

static void tile_cache(void);

static void start_level(int li)
{
	load_level(li);
	tile_cache();
	place_marble(L.sx, L.sy);
	G.mode = READY;
	G.t = G.time = 0;
	G.lives = LIVES;
	G.zero_n = G.zsum_x = G.zsum_y = 0;
	G.pop = POP_NONE;
	mark_all();
}

static int score_of(void)
{
	int bonus = levels[L.li].par * 60 - G.time;

	return 500 + L.stars * 250 + G.lives * 100 + (bonus > 0 ? bonus / 6 : 0);
}

static void win(void)
{
	int li = L.li;

	G.mode = WON;
	G.t = 0;
	G.score = score_of();
	G.new_best = G.score > G.best[li];
	if (G.new_best)
		G.best[li] = G.score;
	if (!G.best_t[li] || G.time < G.best_t[li])
		G.best_t[li] = G.time;
	if (L.stars > G.best_s[li])
		G.best_s[li] = L.stars;
	if (li + 1 > G.unlocked && li + 1 < N_LEVELS)
		G.unlocked = li + 1;
	save_scores();
	mark_all();
}

static void fall(int x, int y)
{
	G.mode = FALL;
	G.t = 0;
	B.fall_x = x;
	B.fall_y = y;
}

/* The tilt as an acceleration, FP per tick per tick. */
static void tilt_accel(int *ax, int *ay)
{
	int tx = G.tilt_x, ty = G.tilt_y, k = sens_pct[G.sens] * L.gravity;

	if (G.control || G.no_sensor) {
		tx = (G.arrows & 2 ? ARROW_TILT : 0) - (G.arrows & 8 ? ARROW_TILT : 0);
		ty = (G.arrows & 4 ? ARROW_TILT : 0) - (G.arrows & 1 ? ARROW_TILT : 0);
	}
	tx = iabs(tx) < DEADZONE ? 0 : tx;
	ty = iabs(ty) < DEADZONE ? 0 : ty;
	*ax = tx * k / 640000;
	*ay = ty * k / 640000;
}

/* Pulled towards (x, y) px: a at the centre, nothing from reach on. */
static void pull(int x, int y, int reach, int a, int *ax, int *ay)
{
	int dx = x * FP - B.x, dy = y * FP - B.y, d2 = (dx / FP) * (dx / FP) + (dy / FP) * (dy / FP), d;

	if (d2 >= reach * reach || d2 == 0)
		return;
	d = isqrt((unsigned)d2);
	*ax += dx / FP * a * (reach - d) / (reach * (d ? d : 1));
	*ay += dy / FP * a * (reach - d) / (reach * (d ? d : 1));
}

static int solid(const struct tile *t)
{
	return t->k == WALL || t->k == SNOW || t->k == GATE;
}

/*
 * Out of the rectangle [x0, x1) x [y0, y1) (FP): the marble is pushed out
 * along the shortest way and bounces off with e/256 of its speed. (bvx,
 * bvy) is the rectangle's own speed: a stone pushes. 1 when it touched.
 */
static int push_out(int x0, int y0, int x1, int y1, int e, int bvx, int bvy)
{
	int qx = B.x < x0 ? x0 : B.x > x1 ? x1 : B.x, qy = B.y < y0 ? y0 : B.y > y1 ? y1 : B.y;
	int dx = B.x - qx, dy = B.y - qy, nx, ny, d, vn, bn;
	unsigned d2;

	if (iabs(dx) >= R * FP || iabs(dy) >= R * FP)
		return 0;
	d2 = (unsigned)(dx / 16) * (unsigned)(dx / 16) + (unsigned)(dy / 16) * (unsigned)(dy / 16);
	if (d2 >= (unsigned)(R * 16) * (R * 16))
		return 0;
	if (d2 == 0) {			/* the centre inside: out the nearest side */
		int l = B.x - x0, r = x1 - B.x, u = B.y - y0, w = y1 - B.y, m = l;

		m = r < m ? r : m;
		m = u < m ? u : m;
		m = w < m ? w : m;
		nx = m == l ? -256 : m == r ? 256 : 0;
		ny = nx ? 0 : m == u ? -256 : 256;
		d = -m;
	} else {
		d = isqrt(d2) * 16;
		nx = dx * 256 / d;
		ny = dy * 256 / d;
	}
	B.x += nx * (R * FP - d) / 256;
	B.y += ny * (R * FP - d) / 256;
	vn = (B.vx * nx + B.vy * ny) / 256;
	bn = (bvx * nx + bvy * ny) / 256;
	if (vn < bn) {
		int dv = (vn - bn) * (256 + (bn - vn < 160 ? 0 : e)) / 256;	/* resting: no bounce */

		B.vx -= dv * nx / 256;
		B.vy -= dv * ny / 256;
	}
	return 1;
}

static void collide(void)
{
	int cx = B.x / FP / T, cy = B.y / FP / T;

	for (int i = 0; i < L.n_block; i++) {
		int bx = L.block[i].x, by = L.block[i].y, v = L.block[i].v;

		push_out(bx, by, bx + T * FP, by + T * FP, 77, L.block[i].vert ? 0 : v,
			 L.block[i].vert ? v : 0);
	}
	for (int ty = cy - 1; ty <= cy + 1; ty++)
		for (int tx = cx - 1; tx <= cx + 1; tx++) {
			const struct tile *t = at(tx, ty);

			if (solid(t))
				push_out(tx * T * FP, ty * T * FP, (tx + 1) * T * FP, (ty + 1) * T * FP,
					 t->k == SNOW ? 0 : 102, 0, 0);
		}
	if (B.air)
		return;
	for (int i = 0; i < L.n_bump; i++) {
		int dx = B.x - L.bump[i].x * FP, dy = B.y - L.bump[i].y * FP, d, nx, ny, vn;
		int lim = (R + BUMP_R) * FP;

		if (iabs(dx) >= lim || iabs(dy) >= lim)
			continue;
		d = isqrt((unsigned)((dx / 16) * (dx / 16) + (dy / 16) * (dy / 16))) * 16;
		if (d >= lim || d == 0)
			continue;
		nx = dx * 256 / d;
		ny = dy * 256 / d;
		B.x += nx * (lim - d) / 256;
		B.y += ny * (lim - d) / 256;
		vn = (B.vx * nx + B.vy * ny) / 256;
		if (vn < 4 * FP) {		/* kicked away: 1.5 times as fast, 4 px a tick at least */
			int out = -vn * 3 / 2 < 4 * FP ? 4 * FP : -vn * 3 / 2;

			B.vx += (out - vn) * nx / 256;
			B.vy += (out - vn) * ny / 256;
		}
		if (!L.bump[i].hit)
			mark(L.bump[i].y - T, L.bump[i].y + T);
		L.bump[i].hit = 12;
	}
}

static void move_blocks(void)
{
	for (int i = 0; i < L.n_block; i++) {
		int *p = L.block[i].vert ? &L.block[i].y : &L.block[i].x, old = *p, x, y, hit = 0;

		mark(L.block[i].y / FP - 2, L.block[i].y / FP + T + 2);
		*p += L.block[i].v;
		x = L.block[i].x / FP;
		y = L.block[i].y / FP;
		for (int k = 0; k < 4; k++)		/* its corners on a wall: turn back */
			hit |= solid(at((x + (k & 1) * (T - 1)) / T, (y + (k >> 1) * (T - 1)) / T));
		if (hit) {
			*p = old;
			L.block[i].v = -L.block[i].v;
		}
		mark(L.block[i].y / FP - 2, L.block[i].y / FP + T + 2);
	}
}

/* the marble's rows on screen, its shadow and a jump included */
static void mark_marble(void)
{
	int y = B.y / FP;

	if (B.air)
		mark(y - 46, y + 38);
	else
		mark(y - R - 4, y + R + 8);
}

static void physics(void)
{
	int ax = 0, ay = 0, f, steps, otx = B.x / FP / T, oty = B.y / FP / T;
	struct tile *u = at(otx, oty);

	if (!B.air) {
		tilt_accel(&ax, &ay);
		f = WD()->friction;
		switch (u->k) {
		case SAND: f = 70; break;
		case ICE: case CRUMBLE: f = 2; break;
		case WATER: f = 20; ax += dx4[u->d] * WATER_PUSH; ay += dy4[u->d] * WATER_PUSH; break;
		case BELT: ax += dx4[u->d] * BELT_PUSH; ay += dy4[u->d] * BELT_PUSH; break;
		case RAMP: ax -= dx4[u->d] * SLOPE; ay -= dy4[u->d] * SLOPE; break;
		}
		if (u->k == CRUMBLE)
			u->f = 1;
	} else {
		f = 1;
		B.air--;
	}
	for (int i = 0; i < L.n_magnet; i++)
		pull(L.magnet[i].x, L.magnet[i].y, MAGNET_R, MAGNET_A, &ax, &ay);
	if (L.has_hole)
		pull(L.hole_x, L.hole_y, HOLE_R, HOLE_A, &ax, &ay);
	B.vx += ax;
	B.vy += ay;
	B.vx -= B.vx * f / 1024;
	B.vy -= B.vy * f / 1024;
	B.vx = clamp(B.vx, -VMAX, VMAX);
	B.vy = clamp(B.vy, -VMAX, VMAX);
	steps = (iabs(B.vx) > iabs(B.vy) ? iabs(B.vx) : iabs(B.vy)) / STEP_MAX + 1;
	for (int s = 0; s < steps; s++) {
		B.x += B.vx / steps;
		B.y += B.vy / steps;
		collide();
	}
	B.vx = clamp(B.vx, -VMAX, VMAX);
	B.vy = clamp(B.vy, -VMAX, VMAX);
	{
		int tx = B.x / FP / T, ty = B.y / FP / T;

		if ((tx != otx || ty != oty) && !B.air) {
			/* up a ramp and over its top edge fast enough: a jump */
			if (u->k == RAMP && (dx4[u->d] ? tx == otx + dx4[u->d] : ty == oty + dy4[u->d])) {
				int along = B.vx * dx4[u->d] + B.vy * dy4[u->d];

				if (along >= VJUMP)
					B.air = B.air_n = clamp(along * 12 / FP, 18, 60);
			}
			if (u->k == CRUMBLE && u->f) {		/* rolled off cracked ice: it breaks */
				u->k = HOLE;
				u->f = 1;
				mark(oty * T, oty * T + T - 1);
			}
		}
	}
}

/* What the marble's centre is on: stars, buttons, portals, holes, the goal. */
static void triggers(void)
{
	int x = B.x / FP, y = B.y / FP, tx = x / T, ty = y / T;
	struct tile *t = at(tx, ty);

	for (int i = 0; i < L.n_stars; i++) {
		int dx = x - L.star[i].x, dy = y - L.star[i].y;

		if (!L.star[i].got && dx * dx + dy * dy < 26 * 26) {
			L.star[i].got = 1;
			L.stars++;
			mark(L.star[i].y - T, L.star[i].y + T);
			mark(0, 0);
		}
	}
	if (t->k != PORTAL)
		B.portal_lock = 0;
	else if (!B.portal_lock) {
		int p = t->d / 2, e = t->d % 2, ox = L.portal[p][!e][0], oy = L.portal[p][!e][1];

		mark_marble();
		B.x = (ox * T + T / 2) * FP;
		B.y = (oy * T + T / 2) * FP;
		B.portal_lock = 1;
		mark_marble();
		return;
	}
	if (B.air)
		return;
	if (t->k == CHECK && !t->f) {
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
				if (L.t[j][i].k == CHECK && L.t[j][i].f) {
					L.t[j][i].f = 0;
					mark(j * T, j * T);
				}
		t->f = 1;
		L.sx = tx * T + T / 2;
		L.sy = ty * T + T / 2;
		mark(ty * T, ty * T);
	}
	if (t->k == BUTTON && !t->f) {
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
				if (L.t[j][i].k == GATE) {
					L.t[j][i].k = OPEN;
					mark(j * T, j * T);
				}
		t->f = 1;
		mark(ty * T, ty * T);
	}
	if (t->k == HOLE) {
		fall(tx * T + T / 2, ty * T + T / 2);
		return;
	}
	if (t->k == LASER && laser_on(t)) {
		int off = t->d & 2 ? x - (tx * T + T / 2) : y - (ty * T + T / 2);

		if (iabs(off) < 12) {
			fall(x, y);
			return;
		}
	}
	if (L.has_hole) {
		int dx = x - L.hole_x, dy = y - L.hole_y;

		if (dx * dx + dy * dy < 18 * 18) {
			fall(L.hole_x, L.hole_y);
			return;
		}
	}
	if ((x - L.gx) * (x - L.gx) + (y - L.gy) * (y - L.gy) < 18 * 18)
		win();
}

static void step(void)
{
	G.t++;
	if (G.t % ANIM_T == 0) {
		G.anim++;
		G.dirty |= L.anim_rows;
	}
	switch (G.mode) {
	case READY:
		if (G.t > READY_T - 30) {		/* the last half second: how it is held */
			G.zsum_x += G.raw_x;
			G.zsum_y += G.raw_y;
			G.zero_n++;
		}
		if (G.t >= READY_T) {
			if (G.neutral == 0) {	/* skipped with Enter: as it is now */
				G.zero_x = G.zero_n ? G.zsum_x / G.zero_n : G.raw_x;
				G.zero_y = G.zero_n ? G.zsum_y / G.zero_n : G.raw_y;
			} else {
				G.zero_x = G.zero_y = 0;
			}
			G.mode = PLAY;
			G.t = 0;
			mark_all();
		}
		return;
	case PLAY:
		G.time++;
		if (L.laser && G.time % L.laser == 0)
			G.dirty |= L.anim_rows;
		for (int i = 0; i < L.n_bump; i++)
			if (L.bump[i].hit && !--L.bump[i].hit)
				mark(L.bump[i].y - T, L.bump[i].y + T);
		move_blocks();
		mark_marble();
		physics();
		triggers();
		mark_marble();
		return;
	case FALL:
		mark_marble();
		B.x += (B.fall_x * FP - B.x) / 6;
		B.y += (B.fall_y * FP - B.y) / 6;
		mark_marble();
		if (G.t >= FALL_T) {
			if (--G.lives <= 0) {
				G.mode = OVER;
				G.t = 0;
				mark_all();
			} else {
				place_marble(L.sx, L.sy);
				G.mode = PLAY;
				G.t = 0;
				mark_marble();
				mark(0, 0);
			}
		}
		return;
	case OVER:
		if (G.t >= OVER_T)
			start_level(L.li);
		return;
	}
}

/* ---- the sensor: filtered, less its neutral ---------------------------- */

static void sensor(int raw_x, int raw_y)
{
	G.raw_x += (raw_x - G.raw_x) / 3;
	G.raw_y += (raw_y - G.raw_y) / 3;
	G.tilt_x = G.raw_x - G.zero_x;
	G.tilt_y = G.raw_y - G.zero_y;
}

/* ---- keys ------------------------------------------------------------------- */

static int running(void)
{
	return G.pop == POP_NONE && G.mode != TITLE && G.mode != WON;
}

static void open_pop(int pop)
{
	G.help_back = G.pop;
	G.pop = pop;
	G.pf = 0;
	G.arrows = 0;
	mark_all();
}

static int arrow_bit(int code)
{
	return code == KEY_UP ? 1 : code == KEY_RIGHT ? 2 : code == KEY_DOWN ? 4 : code == KEY_LEFT ? 8 : 0;
}

/* an arrow let go (presses also come through key()) */
static void key_up(int code)
{
	G.arrows &= ~arrow_bit(code);
}

static int pop_n(void)
{
	return G.pop == POP_PAUSE ? P_N : G.pop == POP_SETTINGS ? 3 : G.pop == POP_SENS ? 3 :
	       G.pop == POP_NEUTRAL ? 2 : G.pop == POP_RESULTS ? N_LEVELS : 0;
}

/* One key press; 1 to quit. */
static int key(int code)
{
	if (G.pop == POP_HELP) {		/* only an explanation: Esc or the diamond closes it */
		if (code == KEY_ESC || code == KEY_SYM_HELP) {
			G.pop = G.help_back;
			G.pf = G.help_back == POP_PAUSE ? P_KEYS : 0;
			mark_all();
		}
		return 0;
	}
	if (G.pop) {
		int n = pop_n();

		mark_all();
		if (code == KEY_UP || code == KEY_DOWN) {
			G.pf = (G.pf + (code == KEY_UP ? n - 1 : 1)) % n;
			return 0;
		}
		if (code == KEY_SYM_HELP) {
			open_pop(POP_HELP);
			return 0;
		}
		if (code == KEY_ESC) {
			G.pop = G.pop == POP_SENS || G.pop == POP_NEUTRAL ? POP_SETTINGS : POP_NONE;
			G.pf = 0;
			return 0;
		}
		if (code != KEY_ENTER)
			return 0;
		switch (G.pop) {
		case POP_PAUSE:
			G.pop = POP_NONE;
			if (G.pf == P_QUIT)
				return 1;
			if (G.pf == P_RESTART)
				start_level(L.li);
			else if (G.pf == P_LEVELS) {
				G.mode = TITLE;
				G.sel = L.li;
			} else if (G.pf == P_KEYS)
				open_pop(POP_HELP), G.help_back = POP_PAUSE;
			break;
		case POP_SETTINGS:
			if (G.pf == 0) {
				G.control = !G.control;
				G.arrows = 0;
				save_settings();
			} else {
				G.pop = G.pf == 1 ? POP_SENS : POP_NEUTRAL;
				G.pf = G.pf == 1 ? G.sens : G.neutral;
			}
			break;
		case POP_SENS:
		case POP_NEUTRAL:
			if (G.pop == POP_SENS)
				G.sens = G.pf;
			else
				G.neutral = G.pf;
			save_settings();
			G.pf = G.pop == POP_SENS ? 1 : 2;
			G.pop = POP_SETTINGS;
			break;
		case POP_RESULTS:
			if (G.pf <= G.unlocked) {
				G.pop = POP_NONE;
				start_level(G.pf);
			}
			break;
		}
		return 0;
	}
	if (code == KEY_SYM_HELP) {
		open_pop(POP_HELP);
		G.help_back = POP_NONE;
		return 0;
	}
	if (code == KEY_SYM_SETTINGS) {
		open_pop(POP_SETTINGS);
		return 0;
	}
	if (code == KEY_SYM_LOG) {
		open_pop(POP_RESULTS);
		G.pf = G.mode == TITLE ? G.sel : L.li;
		return 0;
	}
	if (G.mode == TITLE) {
		int r = G.sel / 3, c = G.sel % 3;

		mark_all();
		if (code == KEY_ESC)
			return 1;
		if (code == KEY_UP)
			r = (r + 5) % 6;
		else if (code == KEY_DOWN)
			r = (r + 1) % 6;
		else if (code == KEY_LEFT)
			c = (c + 2) % 3;
		else if (code == KEY_RIGHT)
			c = (c + 1) % 3;
		else if ((code == KEY_ENTER || code == KEY_SPACE) && G.sel <= G.unlocked)
			start_level(G.sel);
		if (G.mode == TITLE)
			G.sel = r * 3 + c;
		return 0;
	}
	if (code == KEY_ESC) {
		open_pop(POP_PAUSE);
		return 0;
	}
	if (G.mode == WON && (code == KEY_ENTER || code == KEY_SPACE)) {
		if (L.li + 1 < N_LEVELS)
			start_level(L.li + 1);
		else {
			G.mode = TITLE;
			mark_all();
		}
		return 0;
	}
	if (G.mode == READY && (code == KEY_ENTER || code == KEY_SPACE)) {
		G.t = READY_T - 1;
		return 0;
	}
	if (G.mode == OVER && (code == KEY_ENTER || code == KEY_SPACE)) {
		start_level(L.li);
		return 0;
	}
	G.arrows |= arrow_bit(code);
	return 0;
}

/* ---- drawing: shapes ------------------------------------------------------- */

/* a disk at alpha a (shadows): one square root per row */
static void disk_a(int cx, int cy, int r, uint32_t c, unsigned a)
{
	for (int dy = -r; dy <= r; dy++) {
		int y = cy + dy, hw;

		if (y < fb_y0 || y >= fb_y1)
			continue;
		hw = isqrt((unsigned)(r * r - dy * dy));
		for (int x = cx - hw; x <= cx + hw; x++)
			put(x, y, c, a);
	}
}

static void circle(int cx, int cy, int r, uint32_t c)
{
	box(cx - r, cy - r, 2 * r, 2 * r, r, c, 255, 0, 0, 0);
}

static void ring(int cx, int cy, int r, int bw, uint32_t c)
{
	box(cx - r, cy - r, 2 * r, 2 * r, r, 0, 0, c, bw, 0);
}

/* a four-pointed sparkle: the stars */
static void sparkle(int cx, int cy, int r, uint32_t c)
{
	for (int i = 0; i <= r; i++) {
		int w = (r - i) / 4;

		fill(cx - w, cy - i, 2 * w + 1, 1, c);
		fill(cx - w, cy + i, 2 * w + 1, 1, c);
		fill(cx - i, cy - w, 1, 2 * w + 1, c);
		fill(cx + i, cy - w, 1, 2 * w + 1, c);
	}
}

/* ---- the tiles, rendered once per level ------------------------------------ */

/*
 * The cache holds a tile's look without its neighbours' influence; walls,
 * holes and snowbanks get their open sides' edges when drawn. Rendered
 * through wbdraw into the band (x 0..39), then copied out.
 */
enum { CF_FLOOR, CF_SAND = 4, CF_ICE = 6, CF_CRUMBLE = 8, CF_WALL = 9, CF_SNOW = 10, CF_HOLE = 11,
       CF_BROKEN = 12, CF_WATER = 13, CF_BELT = 14, CF_RAMP = 15, CF_BUTTON = 19, CF_GATE = 21,
       CF_OPEN = 22, CF_CHECK = 23, CF_N = 25 };

static uint16_t cache[CF_N][T * T];

#define BAND_H	T
static uint16_t band[W * BAND_H];

static void draw_floor(int v)
{
	const struct world *w = WD();
	int wi = world_of(L.li);

	fill(0, 0, T, T, v == 1 ? w->alt : w->floor);
	seed = 977u * (unsigned)(wi * 8 + v) + 1;
	switch (wi) {
	case 0:				/* planks: grain, a seam every other tile */
		for (int i = 0; i < 6; i++)
			fill(0, (int)(rnd() % T), T, 1, w->grain);
		if (v & 1)
			fill(0, T - 1, T, 1, w->wall_lo);
		break;
	case 1:				/* dunes: leaning stripes */
		for (int y = 0; y < T; y += 10)
			for (int x = 0; x < T; x++)
				put(x, (y + x / 3 + v * 3) % T, w->grain, 140);
		break;
	case 2:				/* snow: speckles */
		for (int i = 0; i < 14; i++)
			fill((int)(rnd() % (T - 2)), (int)(rnd() % (T - 2)), 2, 2, i & 1 ? w->grain : w->alt);
		break;
	case 3:				/* slabs with moss */
		fill(0, 0, T, 1, w->grain);
		fill(0, 0, 1, T, w->grain);
		for (int i = 0; i < 5; i++)
			box((int)(rnd() % 30), (int)(rnd() % 30), 6 + (int)(rnd() % 6), 4 + (int)(rnd() % 4), 2,
			    0x7A9A52, 120, 0, 0, 0);
		break;
	case 4:				/* plates with rivets */
		fill(0, 0, T, 1, w->wall_hi);
		fill(0, T - 1, T, 1, w->wall_lo);
		for (int i = 0; i < 4; i++)
			circle(5 + (i & 1) * 30, 5 + (i >> 1) * 30, 2, w->grain);
		break;
	case 5:				/* space: a faint grid, a star or two */
		fill(0, 0, T, 1, w->grain);
		fill(0, 0, 1, T, w->grain);
		for (int i = 0; i < 2; i++)
			fill(2 + (int)(rnd() % 34), 2 + (int)(rnd() % 34), 1 + (v == i), 1 + (v == i),
			     i ? WB_COLOR_TEXT_LOW : 0x3A3A58);
		break;
	}
}

static void draw_cache_tile(int i)
{
	const struct world *w = WD();

	if (i < CF_SAND) {
		draw_floor(i);
	} else if (i < CF_ICE) {
		fill(0, 0, T, T, C_SAND);
		seed = 31u * (unsigned)i;
		for (int k = 0; k < 26; k++)
			fill((int)(rnd() % (T - 2)), (int)(rnd() % (T - 2)), 2, 2, k % 3 ? C_SAND_DOT : 0xE8BE7E);
	} else if (i < CF_CRUMBLE) {
		fill(0, 0, T, T, C_ICE);
		for (int k = 0; k < T; k++) {
			put(k, (k + (i & 1) * 14) % T, C_ICE_HI, 200);
			put(k, (k + 6 + (i & 1) * 14) % T, C_ICE_HI, 120);
		}
	} else if (i == CF_CRUMBLE) {
		fill(0, 0, T, T, C_ICE);
		for (int k = 0; k < 14; k++) {
			fill(6 + k, 8 + k + (k & 2), 2, 1, C_CRACK);
			fill(26 - k / 2, 20 + k, 1, 2, C_CRACK);
			fill(20 + k, 30 - (k & 3), 2, 1, C_CRACK);
		}
	} else if (i == CF_WALL) {
		fill(0, 0, T, T, w->wall);
		switch (world_of(L.li)) {
		case 0:
			for (int k = 0; k < 4; k++)
				fill(0, 6 + k * 9, T, 1, w->wall_lo);
			break;
		case 1:
			for (int k = 0; k < 3; k++)
				fill(0, 12 + k * 12, T, 2, w->wall_lo);
			break;
		case 2:
			for (int k = 0; k < T; k++)
				put(k, (k + 20) % T, w->wall_hi, 160);
			break;
		case 3:			/* bricks */
			fill(0, 19, T, 2, w->wall_lo);
			fill(0, 39, T, 1, w->wall_lo);
			fill(19, 0, 2, 19, w->wall_lo);
			fill(0, 21, 1, 18, w->wall_lo);
			break;
		case 4:
			for (int k = 0; k < 4; k++)
				circle(8 + (k & 1) * 24, 8 + (k >> 1) * 24, 2, w->wall_hi);
			break;
		}
	} else if (i == CF_SNOW) {
		fill(0, 0, T, T, C_SNOW);
		seed = 5;
		for (int k = 0; k < 8; k++)
			fill((int)(rnd() % (T - 3)), (int)(rnd() % (T - 3)), 3, 2, 0xE4EEF8);
	} else if (i == CF_HOLE || i == CF_BROKEN) {
		fill(0, 0, T, T, w->hole);
		if (i == CF_BROKEN)		/* bits of ice floating */
			for (int k = 0; k < 3; k++)
				box(6 + k * 11, 10 + (k & 1) * 14, 9, 6, 2, C_ICE, 110, 0, 0, 0);
	} else if (i == CF_WATER) {
		fill(0, 0, T, T, C_WATER);
	} else if (i == CF_BELT) {
		fill(0, 0, T, T, C_BELT);
	} else if (i < CF_BUTTON) {		/* a ramp, low at the start, high at its top edge */
		int d = i - CF_RAMP;

		for (int k = 0; k < T; k++) {
			uint32_t c = mix(C_RAMP_HI, C_RAMP, (unsigned)(k * 255 / (T - 1)));

			if (d == UP)
				fill(0, T - 1 - k, T, 1, c);
			else if (d == DOWN)
				fill(0, k, T, 1, c);
			else if (d == LEFT)
				fill(T - 1 - k, 0, 1, T, c);
			else
				fill(k, 0, 1, T, c);
		}
		for (int k = 4; k < T; k += 9)	/* the planks' seams, across the slope */
			if (d == UP || d == DOWN)
				fill(0, k, T, 1, 0x6E4524);
			else
				fill(k, 0, 1, T, 0x6E4524);
	} else if (i < CF_GATE) {		/* a button, up and down */
		draw_floor(0);
		box(6, 6, 28, 28, 6, C_GOLD_LO, 255, 0, 0, 0);
		box(9, i == CF_BUTTON ? 7 : 10, 22, 22, 11, i == CF_BUTTON ? C_GOLD : C_GOLD_LO, 255,
		    i == CF_BUTTON ? 0xFFF0A0 : C_GOLD, 2, 0);
	} else if (i == CF_GATE) {
		draw_floor(0);
		fill(0, 4, T, 4, C_GATE);
		fill(0, 32, T, 4, C_GATE);
		for (int k = 0; k < 3; k++) {
			fill(5 + k * 13, 0, 5, T, C_GATE);
			fill(6 + k * 13, 0, 1, T, C_GATE_HI);
		}
	} else if (i == CF_OPEN) {
		draw_floor(0);
		for (int k = 0; k < 3; k++)
			fill(5 + k * 13, T - 4, 5, 4, C_GATE);
	} else {				/* a flag: off, on */
		draw_floor(0);
		fill(13, 6, 3, 28, 0x3A3A44);
		for (int k = 0; k < 12; k++)
			fill(16, 7 + k / 2, 15 - k, 1, i == CF_CHECK ? WB_COLOR_TEXT_LOW : w->accent);
		fill(10, 33, 9, 3, 0x3A3A44);
	}
}

static void tile_cache(void)
{
	uint16_t *keep = fb;
	int k0 = fb_y0, k1 = fb_y1;

	fb = band;
	fb_y0 = 0;
	fb_y1 = T;
	for (int i = 0; i < CF_N; i++) {
		draw_cache_tile(i);
		for (int y = 0; y < T; y++)
			memcpy(cache[i] + y * T, band + y * W, T * 2);
	}
	fb = keep;
	fb_y0 = k0;
	fb_y1 = k1;
}

static void blit(int tx, int ty, int ci)
{
	for (int y = 0; y < T; y++) {
		int sy = ty * T + y;

		if (sy >= fb_y0 && sy < fb_y1)
			memcpy(fb + sy * W + tx * T, cache[ci] + y * T, T * 2);
	}
}

/* which sides of (tx, ty) open onto something that is not like it: 1 up 2 right 4 down 8 left */
static int open_sides(int tx, int ty, int (*same)(const struct tile *))
{
	int m = 0;

	for (int d = 0; d < 4; d++)
		if (!same(at(tx + dx4[d], ty + dy4[d])))
			m |= 1 << d;
	return m;
}

static int is_wall(const struct tile *t)
{
	return t->k == WALL;
}

static int is_snow(const struct tile *t)
{
	return t->k == SNOW || t->k == WALL;
}

static int is_hole(const struct tile *t)
{
	return t->k == HOLE || t->k == WALL;
}

static void edges(int x, int y, int m, int e, uint32_t hi, uint32_t lo)
{
	if (m & 1)
		fill(x, y, T, e, hi);
	if (m & 8)
		fill(x, y, e, T, hi);
	if (m & 4)
		fill(x, y + T - e, T, e, lo);
	if (m & 2)
		fill(x + T - e, y, e, T, lo);
}

static void draw_tile(int tx, int ty)
{
	const struct world *w = WD();
	const struct tile *t = &L.t[ty][tx];
	int x = tx * T, y = ty * T, m, ph = G.anim;

	switch (t->k) {
	case WALL:
		blit(tx, ty, CF_WALL);
		m = open_sides(tx, ty, is_wall);
		if (world_of(L.li) == 5) {
			edges(x, y, m, 2, WB_COLOR_CYAN, WB_COLOR_PINK);
		} else {
			edges(x, y, m, 4, w->wall_hi, w->wall_lo);
			if (world_of(L.li) == 4 && (m & 1))	/* the factory: hazard stripes on top */
				for (int k = 0; k < T; k++)
					fill(x + k, y, 1, 6, ((k + tx * T) / 8) & 1 ? 0x111111 : C_BELT_HI);
		}
		break;
	case SNOW:
		blit(tx, ty, CF_SNOW);
		edges(x, y, open_sides(tx, ty, is_snow), 5, 0xFFFFFF, C_SNOW_LO);
		break;
	case HOLE:
		blit(tx, ty, t->f ? CF_BROKEN : CF_HOLE);
		m = open_sides(tx, ty, is_hole);
		if (m & 1)			/* depth: the far side's shadow */
			for (int k = 0; k < 10; k++)
				fill(x, y + k, T, 1, mix(w->rim, w->hole, (unsigned)(255 - k * 25)));
		if (m & 8)
			fill(x, y, 3, T, w->rim);
		if (m & 2)
			fill(x + T - 3, y, 3, T, w->rim);
		if (world_of(L.li) == 5)
			edges(x, y, m, 1, WB_COLOR_PINK, WB_COLOR_PINK);
		break;
	case SAND:
		blit(tx, ty, CF_SAND + ((tx + ty) & 1));
		break;
	case ICE:
		blit(tx, ty, CF_ICE + ((tx * 3 + ty) & 1));
		break;
	case CRUMBLE:
		blit(tx, ty, CF_CRUMBLE);
		break;
	case WATER:
	case BELT:
		blit(tx, ty, t->k == WATER ? CF_WATER : CF_BELT);
		for (int k = 0; k < 3; k++) {	/* ripples or chevrons, moving along */
			int a = (k * 14 + ph * (t->k == WATER ? 2 : 4)) % 42 - 2, d = t->d;
			uint32_t c = t->k == WATER ? C_WATER_HI : C_BELT_HI;

			if (d == UP || d == LEFT)
				a = T - 1 - a;
			for (int j = 0; j < 8; j++) {
				int o = t->k == WATER ? sine(j * 4 + k * 8) / 21 : (j < 4 ? j : 7 - j);
				int s = 6 + j * 4;

				if (d == UP || d == DOWN)
					fill(x + s, y + clamp(a + (d == DOWN ? -o : o), 0, T - 2), 3, 2, c);
				else
					fill(x + clamp(a + (d == RIGHT ? -o : o), 0, T - 2), y + s, 2, 3, c);
			}
		}
		if (t->k == BELT)
			edges(x, y, (t->d == UP || t->d == DOWN) ? 10 : 5, 2, 0x3A3E45, 0x3A3E45);
		break;
	case RAMP:
		blit(tx, ty, CF_RAMP + t->d);
		break;
	case BUTTON:
		blit(tx, ty, CF_BUTTON + t->f);
		break;
	case GATE:
		blit(tx, ty, CF_GATE);
		break;
	case OPEN:
		blit(tx, ty, CF_OPEN);
		break;
	case CHECK:
		blit(tx, ty, CF_CHECK + t->f);
		break;
	default:
		blit(tx, ty, CF_FLOOR + ((tx * 7 + ty * 3) & 3));
		break;
	}
}

/* things on the floor, drawn after the row's tiles: they may overlap rows */
static void draw_things(void)
{
	const struct world *w = WD();
	int ph = G.anim;

	for (int ty = 0; ty < N; ty++)
		for (int tx = 0; tx < N; tx++) {
			const struct tile *t = &L.t[ty][tx];
			int cx = tx * T + T / 2, cy = ty * T + T / 2;

			if ((cy + T) < fb_y0 || (cy - T) >= fb_y1)
				continue;
			if (t->k == GOAL) {
				int p = ph % 7;

				ring(cx, cy, 13 + p, 2, mix(w->accent, w->floor, (unsigned)(240 - p * 30)));
				ring(cx, cy, 15, 5, w->accent);
				circle(cx, cy, 7, mix(w->accent, 0xFFFFFF, 120));
			} else if (t->k == PORTAL) {
				uint32_t c = t->d & 1 ? WB_COLOR_PINK : WB_COLOR_CYAN;

				circle(cx, cy, 16, mix(c, WB_COLOR_VOID, 60));
				ring(cx, cy, 16, 3, c);
				for (int k = 0; k < 3; k++) {
					int a = ph * 3 + k * 11;

					circle(cx + sine(a) / 6, cy + sine(a + 8) / 6, 2, c);
				}
			} else if (t->k == LASER) {
				int on = laser_on(t), vert = t->d & 2, soon = !on && L.laser &&
					   L.laser - G.time % L.laser < 30;

				if (vert) {
					fill(cx - 5, ty * T, 10, 4, 0x3A3A44);
					fill(cx - 5, ty * T + T - 4, 10, 4, 0x3A3A44);
				} else {
					fill(tx * T, cy - 5, 4, 10, 0x3A3A44);
					fill(tx * T + T - 4, cy - 5, 4, 10, 0x3A3A44);
				}
				if (on || (soon && ph & 1)) {
					int a = on ? 255 : 90;

					for (int k = -4; k <= 4; k++) {
						unsigned al = (unsigned)(a * (5 - iabs(k)) / 5);

						for (int j = 0; j < T; j++)
							if (vert)
								put(cx + k, ty * T + j, iabs(k) < 2 ? 0xFFE0E0 : C_LASER, al);
							else
								put(tx * T + j, cy + k, iabs(k) < 2 ? 0xFFE0E0 : C_LASER, al);
					}
				}
			}
		}
	for (int i = 0; i < L.n_magnet; i++) {
		int cx = L.magnet[i].x, cy = L.magnet[i].y;

		ring(cx, cy, 30, 1, mix(C_MAGNET, w->floor, 90));
		ring(cx, cy, 50, 1, mix(C_MAGNET, w->floor, 50));
		box(cx - 15, cy - 15, 30, 30, 15, mix(C_MAGNET, w->floor, 110), 255, C_MAGNET, 2, 0);
		box(cx - 9, cy - 6, 18, 15, 8, C_BUMPER, 255, 0, 0, 0);	/* a horseshoe */
		box(cx - 4, cy - 6, 8, 10, 4, mix(C_MAGNET, w->floor, 110), 255, 0, 0, 0);
		fill(cx - 9, cy - 9, 5, 9, C_BUMPER);
		fill(cx + 4, cy - 9, 5, 9, C_BUMPER);
		fill(cx - 9, cy - 10, 5, 4, 0xE6E8EE);
		fill(cx + 4, cy - 10, 5, 4, 0xE6E8EE);
	}
	for (int i = 0; i < L.n_bump; i++) {
		int cx = L.bump[i].x, cy = L.bump[i].y, hit = L.bump[i].hit;

		circle(cx, cy, BUMP_R + 2 + hit / 4, hit ? 0xFFFFFF : 0x7A1E28);
		circle(cx, cy, BUMP_R - 1, hit ? 0xFF9AA4 : C_BUMPER);
		circle(cx - 3, cy - 3, 4, 0xFFC2C8);
	}
	if (L.has_hole) {
		int cx = L.hole_x, cy = L.hole_y;

		for (int k = 4; k >= 1; k--)
			ring(cx, cy, 18 + k * 16 + (ph % 4) * 4, 1, mix(WB_COLOR_PINK, w->floor, (unsigned)(160 - k * 30)));
		circle(cx, cy, 22, WB_COLOR_PINK);
		circle(cx, cy, 18, 0x000000);
		for (int k = 0; k < 4; k++) {
			int a = ph * 2 + k * 8;

			circle(cx + sine(a) * 28 / 64, cy + sine(a + 8) * 28 / 64, 2, WB_COLOR_CYAN);
		}
	}
	for (int i = 0; i < L.n_block; i++) {
		int x = L.block[i].x / FP, y = L.block[i].y / FP;

		box(x + 1, y + 3, T - 2, T - 2, 6, 0x000000, 90, 0, 0, 0);
		box(x + 1, y + 1, T - 2, T - 4, 6, w->wall, 255, w->wall_hi, 2, 0);
		fill(x + 8, y + 12, T - 16, 2, w->wall_lo);
		fill(x + 8, y + 24, T - 16, 2, w->wall_lo);
	}
	for (int i = 0; i < L.n_stars; i++)
		if (!L.star[i].got) {
			int cx = L.star[i].x, cy = L.star[i].y, r = 13 + sine(ph * 2 + i * 8) / 32;

			disk_a(cx, cy, 10, C_STAR, 60);
			sparkle(cx, cy, r, C_STAR);
			sparkle(cx, cy, r / 2, 0xFFFFFF);
		}
}

static void draw_marble(void)
{
	const struct world *w = WD();
	int x = B.x / FP, y = B.y / FP, r = R, up = 0;

	if (G.mode == FALL) {
		r = R * (FALL_T - G.t) / FALL_T;
		if (r < 1)
			return;
	} else if (B.air && B.air_n) {
		int done = B.air_n - B.air;

		up = 4 * 22 * done * B.air / (B.air_n * B.air_n);	/* a parabola, 22 px high */
		r = R + up / 5;
	}
	if (y + r + 10 < fb_y0 || y - r - up - 4 >= fb_y1)
		return;
	if (G.mode != FALL)
		disk_a(x + 3 + up / 3, y + 4 + up / 2, r, w->shadow, up ? 70 : 110);
	y -= up;
	box(x - r, y - r, 2 * r, 2 * r, r, G.mode == FALL ? mix(w->ball, 0, 160) : w->ball, 255, w->ball_rim, 2,
	    0);
	if (r > 4) {
		circle(x - r / 3, y - r / 3, r / 3, w->ball_hi);
		circle(x - r / 3 - 1, y - r / 3 - 1, r / 8 + 1, 0xFFFFFF);
	}
}

/* ---- drawing: the HUD, cards, title and pop-ups ------------------------- */

/* Text at twice a font's size (as in Flappy). */
static int big_w(const struct wb_font *f, const char *s)
{
	return 2 * text_w(f, s, 0);
}

static unsigned cov(const struct wb_glyph *g, const uint8_t *a, int i, int j)
{
	return i < 0 || j < 0 || i >= g->w || j >= g->h ? 0 : a[j * g->w + i];
}

static void big_text(const struct wb_font *f, int x, int y, const char *s, uint32_t c)
{
	int x16 = x * 16;

	while (*s) {
		const struct wb_glyph *g = glyph(f, next_cp(&s));
		const uint8_t *a = f->bits + g->off;
		int gx = (x16 + 8) / 16 + 2 * g->x, gy = y + 2 * g->y;

		for (int j = -1; j <= 2 * g->h; j++) {
			int sj = j >> 1, nj = j & 1 ? sj + 1 : sj - 1;

			if (gy + j < fb_y0 || gy + j >= fb_y1)
				continue;
			for (int i = -1; i <= 2 * g->w; i++) {
				int si = i >> 1, ni = i & 1 ? si + 1 : si - 1;
				unsigned v = (9 * cov(g, a, si, sj) + 3 * cov(g, a, ni, sj) +
					      3 * cov(g, a, si, nj) + cov(g, a, ni, nj)) / 16;

				put(gx + i, gy + j, c, v);
			}
		}
		x16 += 2 * g->adv16;
	}
}

static void centred(const struct wb_font *f, int y, const char *s, uint32_t c)
{
	text(f, (W - text_w(f, s, 0)) / 2, y, s, c, 0);
}

static char *level_label(char *b, int li)		/* "2-3" */
{
	b[0] = (char)('1' + li / 3);
	b[1] = '-';
	b[2] = (char)('1' + li % 3);
	b[3] = 0;
	return b + 3;
}

static void hud(void)
{
	const struct world *w = WD();
	char b[40];
	int x, cy = T / 2;

	if (fb_y0 >= T)
		return;
	fill(0, 0, W, T, WB_COLOR_CONSOLE);
	fill(0, T - 1, W, 1, WB_COLOR_LINE);
	level_label(b, L.li);
	x = text(&f_mono16b, 20, text_top(&f_mono16b, cy), b, w->accent, 0) + 12;
	text(&f_ui18, x, text_top(&f_ui18, cy), levels[L.li].name, WB_COLOR_TEXT_HI, 0);
	b[0] = 0;
	cat_time(b, G.time);
	text(&f_mono16b, (W - text_w(&f_mono16b, b, 0)) / 2 + 40, text_top(&f_mono16b, cy), b,
	     WB_COLOR_TEXT_HI, 0);
	x = W - 20;
	for (int i = L.n_stars - 1; i >= 0; i--, x -= 26)
		sparkle(x - 10, cy, 10, L.star[i].got ? C_STAR : WB_COLOR_LINE);
	x -= 14;
	for (int i = LIVES - 1; i >= 0; i--, x -= 22)
		box(x - 18, cy - 8, 16, 16, 8, i < G.lives ? w->ball : WB_COLOR_SURFACE2, 255,
		    i < G.lives ? w->ball_rim : WB_COLOR_LINE, 1, 0);
}

static void card_frame(int y, int h)
{
	box(110, y, W - 220, h, WB_RADIUS_LG, WB_COLOR_SURFACE1, 240, WB_COLOR_LINE, 1, 0);
}

static void ready_card(void)
{
	const struct world *w = WD();
	char b[48] = "WORLD ";
	int y = 230, done = G.t * (W - 300) / READY_T;

	card_frame(y, 260);
	cat(cat(cat_u(b, (unsigned)(world_of(L.li) + 1)), ": "), w->name);
	text(&f_mono14, (W - text_w(&f_mono14, b, 22)) / 2, y + 32, b, w->accent, 22);
	b[0] = 0;
	level_label(b, L.li);
	centred(&f_mono16b, y + 62, b, WB_COLOR_TEXT_MID);
	centred(&f_ui28b, y + 86, levels[L.li].name, WB_COLOR_TEXT_HI);
	text_wrap(&f_ui18, 150, y + 140, W - 300, 26, levels[L.li].hint, WB_COLOR_TEXT_MID, 2, 1);
	centred(&f_ui16, y + 200, G.no_sensor ? "No tilt sensor: use the arrows" :
		G.control ? "Roll with the arrows" : "Hold the badge as you like to play", WB_COLOR_TEXT_LOW);
	box(150, y + 232, W - 300, 6, 3, WB_COLOR_LINE, 255, 0, 0, 0);
	if (done > 6)
		box(150, y + 232, done, 6, 3, w->accent, 255, 0, 0, 0);
}

static void won_card(void)
{
	const struct world *w = WD();
	char b[48], *n;
	int y = 200, bonus = levels[L.li].par * 60 - G.time;

	card_frame(y, 320);
	text(&f_mono14, (W - text_w(&f_mono14, "LEVEL COMPLETE", 22)) / 2, y + 32, "LEVEL COMPLETE",
	     w->accent, 22);
	n = utoa((unsigned)G.score, b + sizeof(b));
	big_text(&f_ui36b, (W - big_w(&f_ui36b, n)) / 2, y + 60, n, WB_COLOR_TEXT_HI);
	b[0] = 0;
	cat(cat_time(cat(b, "Time "), G.time), "   Stars ");
	cat(cat_u(cat(cat_u(b, (unsigned)L.stars), "/"), (unsigned)L.n_stars), "   Lives ");
	cat_u(b, (unsigned)G.lives);
	centred(&f_mono16b, y + 170, b, WB_COLOR_TEXT_MID);
	b[0] = 0;
	cat(cat_u(cat(cat_u(cat(cat_u(cat(b, "500 + "), (unsigned)L.stars * 250), " stars + "),
				(unsigned)G.lives * 100), " lives + "), (unsigned)(bonus > 0 ? bonus / 6 : 0)), " time");
	centred(&f_ui16, y + 204, b, WB_COLOR_TEXT_LOW);
	if (G.new_best)
		centred(&f_ui22, y + 240, "New best!", WB_COLOR_YELLOW);
	else {
		b[0] = 0;
		cat_u(cat(b, "Best "), (unsigned)G.best[L.li]);
		centred(&f_ui22, y + 240, b, WB_COLOR_TEXT_MID);
	}
}

static void over_card(void)
{
	card_frame(250, 180);
	text(&f_mono14, (W - text_w(&f_mono14, "OUT OF LIVES", 22)) / 2, 282, "OUT OF LIVES",
	     WB_COLOR_PINK, 22);
	centred(&f_ui28b, 312, "Try again", WB_COLOR_TEXT_HI);
	centred(&f_ui18, 362, "The level starts over", WB_COLOR_TEXT_MID);
}

static void title(void)
{
	char b[48];
	int x0 = 54, y0 = 210, cw = 196, ch = 64, gap = 10;

	fill(0, fb_y0, W, fb_y1 - fb_y0, WB_COLOR_VOID);
	seed = 7;
	for (int i = 0; i < 70; i++) {
		int x = (int)(rnd() % W), y = (int)(rnd() % (H - 60)), c = (int)(rnd() % 7);

		fill(x, y, 1 + (c == 0), 1 + (c == 0), c < 2 ? WB_COLOR_TEXT_MID : c < 3 ? WB_COLOR_CYAN :
		     c < 4 ? WB_COLOR_PINK : 0x3A3A58);
	}
	text(&f_mono14, (W - text_w(&f_mono14, "GAMES", 22)) / 2, 40, "GAMES", WB_COLOR_CYAN, 22);
	big_text(&f_ui36b, (W - big_w(&f_ui36b, "Marble Orbit")) / 2, 62, "Marble Orbit", WB_COLOR_TEXT_HI);
	b[0] = 0;
	cat_u(cat(b, "High score "), (unsigned)total_score());
	centred(&f_mono16b, 164, b, WB_COLOR_YELLOW);
	for (int i = 0; i < N_LEVELS; i++) {
		const struct world *w = &worlds[i / 3];
		int x = x0 + (i % 3) * (cw + gap), y = y0 + (i / 3) * (ch + gap), locked = i > G.unlocked;
		char l[4];

		if (y + ch < fb_y0 || y >= fb_y1)
			continue;
		if (i == G.sel)
			box_focus(x, y, cw, ch, WB_RADIUS_MD, &GLOW_ROW);
		else
			box_plain(x, y, cw, ch, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		fill(x + 12, y + 14, 4, ch - 28, locked ? WB_COLOR_LINE : w->accent);
		level_label(l, i);
		text(&f_mono13, x + 26, y + 12, l, locked ? WB_COLOR_TEXT_LOW : w->accent, 0);
		text(&f_ui18, x + 26 + 30, y + 9, wb_fit(&f_ui18, levels[i].name, cw - 66, b, sizeof(b)),
		     locked ? WB_COLOR_TEXT_LOW : WB_COLOR_TEXT_HI, 0);
		if (locked) {
			text(&f_ui16, x + 26, y + 36, "Locked", WB_COLOR_TEXT_LOW, 0);
			continue;
		}
		for (int s = 0; s < 3; s++)
			sparkle(x + 34 + s * 20, y + 45, 7, s < G.best_s[i] ? C_STAR : WB_COLOR_LINE);
		if (G.best[i]) {
			b[0] = 0;
			cat_time(b, G.best_t[i]);
			text(&f_mono13, x + cw - 14 - text_w(&f_mono13, b, 0), y + 38, b, WB_COLOR_TEXT_MID, 0);
		}
	}
}

static void draw_help(void)
{
	static const struct help_key tilt[] = {
		{ "Tilt", "Roll the marble" },
		{ "Esc", "Pause: resume, restart, levels, quit" },
		{ "#4", "Results of every level" },
		{ "#5", "Settings: controls, sensitivity" },
		{ "#6", "These keys (the game waits)" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key arrows[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "Roll the marble" },
		{ "Esc", "Pause: resume, restart, levels, quit" },
		{ "#4", "Results of every level" },
		{ "#5", "Settings: controls, sensitivity" },
		{ "#6", "These keys (the game waits)" },
		{ "Logo", "Back to the menu" },
	};

	help_popup("Marble Orbit", "Roll into the ring. Stars, lives left and time under par add to the score; "
		   "a hole costs a life.", G.control || G.no_sensor ? arrows : tilt, 6);
}

static void draw_pop(void)
{
	static const struct hint pick[] = { { "Enter", "choose", 0 }, { "Esc", "back", 1 } };
	static struct menu_item it[N_LEVELS];
	char t[40];

	memset(it, 0, sizeof(it));
	switch (G.pop) {
	case POP_HELP:
		draw_help();
		break;
	case POP_PAUSE:
		it[0].label = "Resume";
		it[1].label = "Restart level";
		it[2].label = "Levels";
		it[3].label = "Keys";
		it[4].label = "Quit";
		it[4].sep = 1;
		menu_popup("MARBLE ORBIT", "Paused", 0, it, 0, P_N, G.pf, -1, 0, 0, pick, 2);
		break;
	case POP_SETTINGS:
		it[0].label = "Controls";
		strcpy(it[0].value, G.control ? "Arrows" : "Tilt");
		it[1].label = "Sensitivity";
		strcpy(it[1].value, sens_names[G.sens]);
		it[2].label = "Neutral";
		strcpy(it[2].value, G.neutral ? "Flat" : "At start");
		menu_popup("SETTINGS", "Marble Orbit", 0, it, 0, 3, G.pf, -1, 0, 0, pick, 2);
		break;
	case POP_SENS:
		for (int i = 0; i < 3; i++)
			it[i].label = sens_names[i];
		menu_popup("SETTINGS", "Sensitivity", "3 options", it, 0, 3, G.pf, G.sens, 0, 0, pick, 2);
		break;
	case POP_NEUTRAL:
		it[0].label = neutral_names[0];
		it[0].sub = "tilt from there";
		it[1].label = neutral_names[1];
		it[1].sub = "lying on a table";
		menu_popup("SETTINGS", "Neutral", "2 options", it, 0, 2, G.pf, G.neutral, 0, 0, pick, 2);
		break;
	case POP_RESULTS: {
		static char labels[N_LEVELS][32], subs[N_LEVELS][12];

		for (int i = 0; i < N_LEVELS; i++) {
			level_label(labels[i], i);
			cat(cat(labels[i], " "), levels[i].name);
			it[i].label = labels[i];
			subs[i][0] = 0;
			if (G.best[i])
				cat_time(subs[i], G.best_t[i]);
			it[i].sub = subs[i];
			if (i > G.unlocked)
				strcpy(it[i].value, "Locked");
			else if (G.best[i]) {
				char n[12];

				strcpy(it[i].value, utoa((unsigned)G.best[i], n + sizeof(n)));
			}
			it[i].sep = i && i % 3 == 0;
		}
		t[0] = 0;
		cat_u(cat(t, "High score "), (unsigned)total_score());
		menu_popup("RESULTS", t, 0, it, 0, N_LEVELS, G.pf, -1, 0, 0, pick, 2);
		break;
	}
	}
}

static void draw_band(void)
{
	static const struct hint title_h[] = { { "Enter", "play", 0 }, { "#4", "results", 0 }, { "Esc", "quit", 1 } };
	static const struct hint won_h[] = { { "Enter", "next level", 0 }, { "Esc", "pause", 1 } };
	static const struct hint ready_h[] = { { "Enter", "start", 0 }, { "Esc", "pause", 1 } };

	if (G.mode == TITLE) {
		title();
		hint_bar(title_h, 3);
	} else {
		int r0 = fb_y0 / T, r1 = (fb_y1 - 1) / T;

		for (int ty = r0; ty <= r1; ty++)
			for (int tx = 0; tx < N; tx++)
				draw_tile(tx, ty);
		draw_things();
		draw_marble();
		hud();
		if (G.mode == READY) {
			ready_card();
			hint_bar(ready_h, 2);
		} else if (G.mode == WON) {
			won_card();
			hint_bar(won_h, 2);
		} else if (G.mode == OVER) {
			over_card();
		}
	}
	if (G.pop)
		draw_pop();
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

#ifdef MARBLE_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

/* The rows marked (all of them on the host), one band each. */
static void draw(void)
{
	uint32_t rows = G.dirty;

#ifdef MARBLE_HOST
	rows = (1u << N) - 1;
#endif
	G.dirty = 0;
	for (int r = 0; r < N; r++) {
		int y0 = r * T, y1 = y0 + T;

		if (!(rows & (1u << r)))
			continue;
		fb = band - (long)y0 * W;
		fb_y0 = y0;
		fb_y1 = y1;
		draw_band();
#ifdef MARBLE_HOST
		memcpy(screen + y0 * W, band, (size_t)(y1 - y0) * W * 2);
#else
		pwrite(fb_fd, band, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
	}
}

/* ---- the host test ------------------------------------------------------ */

#ifdef MARBLE_HOST
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

/*
 * The test's autopilot: does each level go with this physics? A search
 * over the tiles (ramps jump their gaps, portals, gates once a button is
 * down, wide of a black hole) gives the next tile; the tilt steers towards
 * it, with a run-up for a ramp and a wait for a laser.
 */
static int walkable(int x, int y)
{
	const struct tile *t = at(x, y);
	int cx = x * T + T / 2 - L.hole_x, cy = y * T + T / 2 - L.hole_y;

	if (solid(t) || t->k == HOLE)
		return 0;
	for (int i = 0; i < L.n_bump; i++)
		if (L.bump[i].x / T == x && L.bump[i].y / T == y)
			return 0;
	return !L.has_hole || cx * cx + cy * cy > 120 * 120;
}

/* the next tile from (x, y) towards (gx, gy); 0 when there is no way */
static int next_tile(int x, int y, int gx, int gy, int *nx, int *ny)
{
	static int16_t from[N * N], q[N * N];
	int head = 0, tail = 0, g = gy * N + gx;

	memset(from, -1, sizeof(from));
	from[y * N + x] = (int16_t)(y * N + x);
	q[tail++] = (int16_t)(y * N + x);
	while (head < tail && from[g] < 0) {
		int c = q[head++], cx = c % N, cy = c / N, nb[12], n = 0;
		const struct tile *t = at(cx, cy);

		if (t->k == RAMP)		/* the jump first: off a ramp, keep going */
			for (int k = 2; k <= 6; k++) {
				int jx = cx + dx4[t->d] * k, jy = cy + dy4[t->d] * k;

				if (solid(at(jx, jy)))
					break;
				if (walkable(jx, jy)) {
					nb[n++] = jy * N + jx;
					break;
				}
			}
		for (int d = 0; d < 4; d++)
			if (walkable(cx + dx4[d], cy + dy4[d]))
				nb[n++] = (cy + dy4[d]) * N + cx + dx4[d];
		if (t->k == PORTAL)
			nb[n++] = L.portal[t->d / 2][!(t->d % 2)][1] * N + L.portal[t->d / 2][!(t->d % 2)][0];
		for (int i = 0; i < n; i++)
			if (from[nb[i]] < 0) {
				from[nb[i]] = (int16_t)c;
				q[tail++] = (int16_t)nb[i];
			}
	}
	if (from[g] < 0)
		return 0;
	while (from[g] != y * N + x)
		g = from[g];
	*nx = g % N;
	*ny = g / N;
	return 1;
}

static void autopilot(void)
{
	int x = B.x / FP, y = B.y / FP, tx = x / T, ty = y / T, gx = L.gx / T, gy = L.gy / T, nx, ny;
	int vx = 0, vy = 0, k = sens_pct[G.sens] * L.gravity;

	if (G.mode != PLAY)
		return;
	for (int j = 0; j < N; j++)		/* gates shut: the button first */
		for (int i = 0; i < N; i++)
			if (L.t[j][i].k == GATE)
				for (int b = 0; b < N * N; b++)
					if (L.t[b / N][b % N].k == BUTTON && !L.t[b / N][b % N].f && !next_tile(tx, ty, gx, gy, &nx, &ny)) {
						gx = b % N;
						gy = b / N;
					}
	if (!next_tile(tx, ty, gx, gy, &nx, &ny))
		nx = tx, ny = ty;
	if (at(nx, ny)->k == RAMP && at(tx, ty)->k != RAMP) {	/* onto a ramp: from behind it */
		int d = at(nx, ny)->d, bx = nx - dx4[d], by = ny - dy4[d];

		if ((bx != tx || by != ty) && walkable(bx, by))
			nx = bx, ny = by;
	}
	{
		const struct tile *t = at(tx, ty), *n = at(nx, ny);
		int dx = nx * T + T / 2 - x, dy = ny * T + T / 2 - y, d = isqrt((unsigned)(dx * dx + dy * dy));
		int far = iabs(nx - tx) + iabs(ny - ty) > 1, sp = d > 30 ? 640 : d * 20;

		if (n->k == RAMP || (t->k == RAMP && far))	/* a run-up */
			sp = 1100;
		if (n->k == LASER && (laser_on(n) || L.laser - G.time % L.laser < 40) && t->k != LASER)
			sp = 0, dx = tx * T + T / 2 - x, dy = ty * T + T / 2 - y, d = 1;
		if (d) {
			vx = dx * sp / d;
			vy = dy * sp / d;
		}
		if (sp == 0) {
			vx = dx * 20;
			vy = dy * 20;
		}
	}
	/* the tilt that brings the speed there in a few ticks, at most a g */
	G.tilt_x = clamp((vx - B.vx) * 640000 / (k * 6), -4096, 4096);
	G.tilt_y = clamp((vy - B.vy) * 640000 / (k * 6), -4096, 4096);
}

static char *line_int(char *o, const char *name, int v)
{
	o = cat(cat(o, name), " ");
	if (v < 0)
		o = cat(o, "-");
	return cat(cat_u(o, (unsigned)iabs(v)), "\n");
}

/*
 * marble-host SCRIPT [OUT.ppm]: SCRIPT is words: xN, yN (the tilt in sensor
 * counts, 4096 a g, signed), kN (key code N pressed), rN (released), tN (N
 * ticks), pN (N ticks of the autopilot, until the goal), lN (level N, past
 * the card), aN (anim frame N, for screenshots).
 * Prints the state; the screen goes to OUT.ppm. APP_CONFIG holds the files,
 * MARBLE_MAP replaces level 0's map.
 */
int main(int argc, char **argv)
{
	const char *s = argc > 1 ? argv[1] : "";
	static char out[2048];
	static const char *const modes[] = { "title", "ready", "play", "fall", "won", "over" };
	static const char *const pops[] = { "", " pause", " help", " settings", " sensitivity", " neutral",
					    " results" };
	int quit = 0;

	load_conf();
	G.sel = G.unlocked;
	while (*s && !quit) {
		int n = 0, neg = 0;
		char c = *s++;

		if (*s == '-')
			neg = 1, s++;
		while (*s >= '0' && *s <= '9')
			n = n * 10 + *s++ - '0';
		n = neg ? -n : n;
		while (*s == ' ')
			s++;
		if (c == 'x' || c == 'y') {	/* past the filter at once */
			if (c == 'x')
				G.raw_x = n;
			else
				G.raw_y = n;
			sensor(G.raw_x, G.raw_y);
		} else if (c == 'k') {
			quit = key(n);
		} else if (c == 'r') {
			key_up(n);
		} else if (c == 'l') {
			start_level(n);
			G.zero_x = G.zero_y = 0;
			G.mode = PLAY;
			G.t = 0;
		} else if (c == 'a') {
			G.anim = n;
		} else if (c == 't' || c == 'p') {
			for (int i = 0; i < n && G.mode != WON; i++) {
				if (running())
					step();
				sensor(G.raw_x, G.raw_y);
				if (c == 'p')
					autopilot();
			}
		}
	}
	if (quit)
		write(1, "closed\n", 7);
	draw();
	if (argc > 2 && argv[2][0])
		save_ppm(argv[2]);
	out[0] = 0;
	cat(cat(cat(out, "mode "), modes[G.mode]), pops[G.pop]);
	cat(out, "\n");
	{
		char *o = out + strlen(out);

		o = line_int(o, "level", L.li);
		o = line_int(o, "x", B.x / FP);
		o = line_int(o, "y", B.y / FP);
		o = line_int(o, "vx", B.vx);
		o = line_int(o, "vy", B.vy);
		o = line_int(o, "air", B.air);
		o = line_int(o, "lives", G.lives);
		o = line_int(o, "stars", L.stars);
		o = line_int(o, "time", G.time);
		o = line_int(o, "score", G.score);
		o = line_int(o, "best", G.best[L.li]);
		o = line_int(o, "total", total_score());
		o = line_int(o, "unlocked", G.unlocked);
		o = line_int(o, "sel", G.sel);
		o = line_int(o, "focus", G.pf);
		o = line_int(o, "control", G.control);
		o = line_int(o, "sens", G.sens);
		{
			int gates = 0, holes = 0;

			for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++) {
					gates += L.t[j][i].k == GATE;
					holes += L.t[j][i].k == HOLE;
				}
			o = line_int(o, "gates", gates);
			line_int(o, "holes", holes);
		}
	}
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
 * The accelerometer: the IIO device named bmi270 (as sleepd's find_accel),
 * its x and y read as text. Its numbering changes from boot to boot.
 */
#define IIO_DIR		"/sys/bus/iio/devices"
static int ax_fd = -1, ay_fd = -1;

static int open_sensor(void)
{
	DIR *d = opendir(IIO_DIR);
	struct dirent *e;
	char path[96], name[16];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		int fd, n;

		if (strncmp(e->d_name, "iio:device", 10) || strlen(e->d_name) > 40)
			continue;
		strcat(strcat(strcpy(path, IIO_DIR "/"), e->d_name), "/name");
		fd = open(path, O_RDONLY | O_CLOEXEC);
		n = fd >= 0 ? (int)read(fd, name, sizeof(name) - 1) : 0;
		if (fd >= 0)
			close(fd);
		if (n < 6 || memcmp(name, "bmi270", 6))
			continue;
		strcat(strcat(strcpy(path, IIO_DIR "/"), e->d_name), "/in_accel_x_raw");
		ax_fd = open(path, O_RDONLY | O_CLOEXEC);
		strcat(strcat(strcpy(path, IIO_DIR "/"), e->d_name), "/in_accel_y_raw");
		ay_fd = open(path, O_RDONLY | O_CLOEXEC);
		found = ax_fd >= 0 && ay_fd >= 0;
	}
	closedir(d);
	return found;
}

static int read_axis(int fd, int *v)
{
	char b[16];
	int n = (int)pread(fd, b, sizeof(b) - 1, 0), r = 0, neg = 0, i = 0;

	if (n <= 0)
		return 0;
	b[n] = 0;
	if (b[0] == '-')
		neg = 1, i = 1;
	for (; b[i] >= '0' && b[i] <= '9'; i++)
		r = r * 10 + b[i] - '0';
	*v = neg ? -r : r;
	return 1;
}

/*
 * Test hook, as in Flappy: decimal key codes written to this
 * FIFO count as key presses; a code plus 1000 is that key let go (the
 * arrows). Made when the game starts, root only, gone when it ends.
 */
#define KEYS_FIFO	"/tmp/marble.keys"
static char fifo_buf[128];
static int fifo_len;

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
		if (i == fifo_len) {
			i = start;
			break;
		}
		i++;
		if (digits && !done) {
			if (v >= 1000)
				key_up(v - 1000);
			else
				done = key(v);
		}
	}
	memmove(fifo_buf, fifo_buf + i, (size_t)(fifo_len - i));
	fifo_len -= i;
	return done;
}

static void log_stats(int frames, unsigned draw_s, unsigned draw_us, int reads, unsigned read_us)
{
	char line[160] = "marble: ";
	int tenths = frames ? (int)((draw_s * 10000 + draw_us / 100) / (unsigned)frames) : 0;
	int rt = reads ? (int)(read_us / (unsigned)reads) : 0;

	cat_u(line, (unsigned)frames);
	cat(line, " frames, ");
	cat_u(line, (unsigned)(tenths / 10));
	cat(line, ".");
	cat_u(line, (unsigned)(tenths % 10));
	cat(line, " ms to draw one; ");
	cat_u(line, (unsigned)reads);
	cat(line, " sensor reads, ");
	cat_u(line, (unsigned)rt);
	cat(line, G.no_sensor ? " us each (no sensor)\n" : " us each\n");
	write(2, line, strlen(line));
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p[2];
	int ev, fifo = -1, nfds = 1, quit = 0, was_running = 0, frames = 0, reads = 0, tries = 0;
	long long tick_due = 0, last_frame = 0, last_read = 0, last_try = 0;
	unsigned draw_s = 0, draw_us = 0, read_us = 0;

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
	load_conf();
	G.sel = G.unlocked;
	G.no_sensor = !open_sensor();
	mark_all();
	while (!stop_sig && !quit) {
		long long now = now_us();
		int run = running(), timeout = -1, n = 0;

		/* the sensor may come late (its firmware upload is retried): look for a minute */
		if (G.no_sensor && tries < 60 && now - last_try > 1000000) {
			last_try = now;
			tries++;
			G.no_sensor = !open_sensor();
		}
		if (!G.no_sensor && !G.control && (run || G.mode == READY) && now - last_read >= 30000) {
			int x, y;

			last_read = now;
			if (read_axis(ax_fd, &x) && read_axis(ay_fd, &y))
				sensor(x, y);
			read_us += (unsigned)(now_us() - now);
			reads++;
		}
		if (run && !was_running)
			tick_due = now;
		was_running = run;
		if (run) {
			while (tick_due <= now && n++ < 8) {
				step();
				tick_due += TICK_US;
			}
			if (tick_due <= now)
				tick_due = now + TICK_US;
		}
		if (G.dirty && (!run || now - last_frame >= FRAME_US - 2000)) {
			long long t0 = now_us();

			draw();
			last_frame = now;
			draw_us += (unsigned)(now_us() - t0);
			if (draw_us >= 1000000) {
				draw_s++;
				draw_us -= 1000000;
			}
			frames++;
		}
		if (running() || G.no_sensor) {
			timeout = (int)(last_frame + FRAME_US - now_us()) / 1000;
			timeout = timeout < 0 ? 0 : timeout > 1000 ? 1000 : timeout;
		}
		if (poll(p, (nfds_t)nfds, timeout) <= 0)
			continue;
		if (p[0].revents & POLLIN) {
			struct ev evs[16];
			int got = (int)read(ev, evs, sizeof(evs)) / (int)sizeof(evs[0]);

			for (int q = 0; q < got && !quit; q++) {
				if (evs[q].type != 1)
					continue;
				if (evs[q].value == 1)
					quit = key(evs[q].code);
				else if (evs[q].value == 0)
					key_up(evs[q].code);
			}
		}
		if (nfds > 1 && (p[1].revents & POLLIN)) {
			int r = (int)read(fifo, fifo_buf + fifo_len, sizeof(fifo_buf) - (size_t)fifo_len);

			if (r > 0) {
				fifo_len += r;
				quit |= fifo_keys();
			}
			if (fifo_len == (int)sizeof(fifo_buf))
				fifo_len = 0;
		}
	}
	if (fifo >= 0)
		unlink(KEYS_FIFO);
	log_stats(frames, draw_s, draw_us, reads, read_us);
	return 0;
}
#endif
