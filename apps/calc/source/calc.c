// SPDX-License-Identifier: MIT
/*
 * calc — a calculator for the badge, drawn with libwb in the style of the
 * canvas "Menu's met frontpaneel" (its board Calculator.dc.html), as an app
 * from the store (sdapps/README.md).
 *
 * You type a sum on the keyboard and Enter (or =) works it out. What it
 * understands:
 *
 *   + - * / ^ (or **), brackets (a missing ")" at the end is added),
 *   n% (n/100), n! (factorial), "mod", 2pi or 3(4+5) as a product,
 *   0x1f and 0b101, 1.5e3, the names pi, e and ans (the last result),
 *   sqrt cbrt abs exp ln log log2 sin cos tan asin acos atan sinh cosh
 *   tanh round floor ceil.
 *
 * Starting a sum with + * / or ^ continues from the last result. The
 * angle unit is degrees; Tab switches to radians and back. A whole
 * number result also shows in hexadecimal and binary. Up and Down bring
 * back earlier sums, Esc clears the line (on an empty line it closes).
 * The symbol keys follow the design (Docs/06-design-system.md 6.6): the
 * circle opens the log (the sums so far), the cloud its settings (the
 * angle unit, a pop-up list), the diamond the help; the left three do
 * nothing here.
 *
 * The screen, over the frontpanel's figures: a display (the sum before
 * with its result, the sum being typed, big its result) and under it the
 * keys of a calculator, each with the keyboard's key that does the same;
 * the key pressed last lights. They only show: the keyboard types.
 *
 * The panel is drawn one band of BAND_H rows at a time through wbdraw.h's
 * clipping, and only the bands that changed, so there is no 1 MB frame
 * buffer next to it (pop-ups are then glass, unblurred). Built with
 * -DCALC_HOST, it takes keys from its first argument and writes the screen
 * as PPM to its second: the host test (calc/test_calc.py).
 *
 * FLAT/NOMMU constraints (overlay-src/Makefile): no stdio.
 */
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wb.h"
#include "lora_state.h"
#include "wbstatus.h"
#include "keyrep.h"

#ifndef CALC_HOST
#include <poll.h>
#endif

/* ---- layout (the board Calculator.dc.html) ----------------------------- */

#define HEAD_X		54			/* the eyebrow, and the title under it */
#define HEAD_Y		64
#define TITLE_Y		(HEAD_Y + 18 + 4 + 2)
#define CHIP_H		28			/* DEG or RAD, right of the title */
#define CHIP_Y		(TITLE_Y + 18 - CHIP_H / 2)
#define DSP_X		48			/* the display: the sum before, the */
#define DSP_Y		128			/* sum being typed and its result */
#define DSP_W		624
#define DSP_H		168
#define DSP_L		(DSP_X + 22)
#define DSP_R		(DSP_X + DSP_W - 22)
#define DSP_TEXT_W	580
#define HIST_CY		(DSP_Y + 42)		/* centres of its three lines */
#define LINE_CY		(DSP_Y + 72)
#define RES_CY		(DSP_Y + DSP_H - 16 - 30)
#define KEYS_Y		312			/* the keys: 4 across, 5 down */
#define KEY_W		150
#define KEY_H		62
#define KEY_GAP		8
#define KEY_GLOW	24			/* how far a key's glow reaches */
#define LOG_ROW		52			/* a sum in the log */

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_EQUAL	13
#define KEY_BACKSPACE	14
#define KEY_TAB		15
#define KEY_ENTER	28
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_FN		0x1d0

/* ---- the sums ----------------------------------------------------------- */

#define IN_MAX		120
#define HIST_MAX	50

static struct {
	char in[IN_MAX + 1];		/* the line being typed */
	int len, cur;
	char draft[IN_MAX + 1];		/* the line, while Up shows older ones */
	int back;			/* 0, or how far back Up went */
	struct { char sum[IN_MAX + 1], res[40]; } hist[HIST_MAX];
	int n_hist;
	double ans;
	int have_ans, rad;
	const char *err;		/* after Enter: what is wrong */
	char preview[40], bases[96];
	int sc;				/* SC_CALC, SC_LOG, SC_SETTINGS */
	int help;			/* the diamond's pop-up is open */
	int pick, pick_focus;		/* the angle unit's pop-up is open */
	int lit;			/* the key on the screen pressed last, + 1 */
} C;

enum { SC_CALC, SC_LOG, SC_SETTINGS };

/* ---- numbers as text ---------------------------------------------------- */

/*
 * v with DIG significant digits, trailing zeros dropped; plain from 1e-6
 * to 1e15, else as 1.5e+20. Digits come from doubles (no 64-bit division,
 * which hangs with this toolchain: sensorpanel.c).
 */
#define DIG	15	/* what a double holds for sure */

static void fmt_num(double v, char *out)
{
	char d[DIG], *o = out;
	double a = fabs(v), m;
	int e, nd = DIG;

	if (a == 0) {
		strcpy(out, "0");
		return;
	}
	if (v < 0)
		*o++ = '-';
	e = (int)floor(log10(a));
	m = nearbyint(a / pow(10, e - (DIG - 1)));
	if (m >= 1e15) {
		m = nearbyint(m / 10);
		e++;
	} else if (m < 1e14) {		/* log10 a hair low */
		m = nearbyint(a / pow(10, e - DIG));
		e--;
	}
	for (int i = DIG - 1; i >= 0; i--) {
		d[i] = (char)('0' + (int)fmod(m, 10));
		m = floor(m / 10);
	}
	while (nd > 1 && d[nd - 1] == '0')
		nd--;
	if (e >= -6 && e < DIG) {
		if (e < 0) {
			*o++ = '0';
			*o++ = '.';
			for (int i = -1; i > e; i--)
				*o++ = '0';
			for (int i = 0; i < nd; i++)
				*o++ = d[i];
		} else {
			for (int i = 0; i <= e; i++)
				*o++ = d[i];
			if (nd > e + 1) {
				*o++ = '.';
				for (int i = e + 1; i < nd; i++)
					*o++ = d[i];
			}
		}
	} else {
		*o++ = d[0];
		if (nd > 1) {
			*o++ = '.';
			for (int i = 1; i < nd; i++)
				*o++ = d[i];
		}
		*o++ = 'e';
		*o++ = e < 0 ? '-' : '+';
		e = abs(e);
		if (e >= 100)
			*o++ = (char)('0' + e / 100);
		if (e >= 10)
			*o++ = (char)('0' + e / 10 % 10);
		*o++ = (char)('0' + e % 10);
	}
	*o = 0;
}

/* A whole number below 2^53 in base 16 and (below 2^32) base 2. */
static void fmt_bases(double v, char *out)
{
	static const char hex[] = "0123456789abcdef";
	char t[64];
	double a = fabs(v);
	int n = 0;
	char *o = out;

	*out = 0;
	if (a != floor(a) || a >= 9007199254740992.0 || a < 10)
		return;
	memcpy(o, "HEX  ", 5);
	o += 5;
	if (v < 0)
		*o++ = '-';
	for (double m = a; m >= 1; m = floor(m / 16))
		t[n++] = hex[(int)fmod(m, 16)];
	*o++ = '0';
	*o++ = 'x';
	while (n)
		*o++ = t[--n];
	if (a < 4294967296.0) {
		memcpy(o, "    BIN  ", 9);
		o += 9;
		if (v < 0)
			*o++ = '-';
		for (double m = a; m >= 1; m = floor(m / 2))
			t[n++] = (char)('0' + (int)fmod(m, 2));
		for (int i = n - 1; i >= 0; i--) {
			*o++ = t[i];
			if (i && i % 4 == 0)
				*o++ = ' ';
		}
	}
	*o = 0;
}

/* ---- working a sum out -------------------------------------------------- */

static const char *P;		/* where the parser is */
static const char *E;		/* the first thing that went wrong */

static void fail(const char *why)
{
	if (!E)
		E = why;
}

static void skip(void)
{
	while (*P == ' ')
		P++;
}

static int is_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

static double expr(void);
static double unary(void);

/* A name: letters, then digits (log2). */
static int name(char *out, int size)
{
	int n = 0;

	while ((is_alpha(*P) || (n && is_digit(*P))) && n < size - 1)
		out[n++] = (char)(*P >= 'A' && *P <= 'Z' ? *P + 32 : *P), P++;
	out[n] = 0;
	return n;
}

static double angle_in(double x)
{
	return C.rad ? x : x * (M_PI / 180);
}

static double angle_out(double x)
{
	return C.rad ? x : x * (180 / M_PI);
}

/* sin 180° is 1.2e-16 in doubles: 0, as on a calculator */
static double trig(double v)
{
	return !C.rad && fabs(v) < 1e-14 ? 0 : v;
}

static double func(const char *f, double x)
{
	if (!strcmp(f, "sqrt")) {
		if (x < 0)
			fail("No square root of a negative number");
		return sqrt(x);
	}
	if (!strcmp(f, "cbrt")) return cbrt(x);
	if (!strcmp(f, "abs")) return fabs(x);
	if (!strcmp(f, "exp")) return exp(x);
	if (!strcmp(f, "ln") || !strcmp(f, "log") || !strcmp(f, "log2")) {
		if (x <= 0)
			fail("No logarithm of zero or less");
		return f[1] == 'n' ? log(x) : f[3] == '2' ? log2(x) : log10(x);
	}
	if (!strcmp(f, "sin")) return trig(sin(angle_in(x)));
	if (!strcmp(f, "cos")) return trig(cos(angle_in(x)));
	if (!strcmp(f, "tan")) {
		if (!C.rad && fmod(fabs(x), 180) == 90)
			fail("No tangent of 90 degrees");
		return trig(tan(angle_in(x)));
	}
	if (!strcmp(f, "asin") || !strcmp(f, "acos")) {
		if (x < -1 || x > 1)
			fail("Outside -1 to 1");
		return angle_out(f[1] == 's' ? asin(x) : acos(x));
	}
	if (!strcmp(f, "atan")) return angle_out(atan(x));
	if (!strcmp(f, "sinh")) return sinh(x);
	if (!strcmp(f, "cosh")) return cosh(x);
	if (!strcmp(f, "tanh")) return tanh(x);
	if (!strcmp(f, "round")) return round(x);
	if (!strcmp(f, "floor")) return floor(x);
	if (!strcmp(f, "ceil")) return ceil(x);
	return NAN;
}

static int is_func(const char *f)
{
	static const char *const fs[] = {
		"sqrt", "cbrt", "abs", "exp", "ln", "log", "log2", "sin", "cos", "tan",
		"asin", "acos", "atan", "sinh", "cosh", "tanh", "round", "floor", "ceil",
	};

	for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++)
		if (!strcmp(f, fs[i]))
			return 1;
	return 0;
}

/* 0x1f, 0b101, or a decimal number */
static double number(void)
{
	if (P[0] == '0' && (P[1] == 'x' || P[1] == 'X' || P[1] == 'b' || P[1] == 'B')) {
		int base = P[1] == 'x' || P[1] == 'X' ? 16 : 2, n = 0;
		double v = 0;

		for (P += 2;; P++, n++) {
			int c = *P, dv = is_digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
				 c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;

			if (dv >= base)
				break;
			v = v * base + dv;
		}
		if (!n)
			fail(base == 16 ? "Hex digits after 0x" : "Binary digits after 0b");
		return v;
	} else {
		char *end;
		double v = strtod(P, &end);

		if (end == P) {
			fail("A number is wrong");
			P++;
			return 0;
		}
		P = end;
		return v;
	}
}

static double primary(void)
{
	char id[8];

	skip();
	if (is_digit(*P) || (*P == '.' && is_digit(P[1])))
		return number();
	if (*P == '(') {
		double v;

		P++;
		v = expr();
		skip();
		if (*P == ')')
			P++;
		else if (*P)
			fail("A \")\" is missing");
		return v;
	}
	if (is_alpha(*P)) {
		const char *at = P;

		name(id, sizeof(id));
		if (is_func(id)) {
			skip();
			if (!*P) {
				fail("A number after the function");
				return 0;
			}
			return func(id, unary());
		}
		if (!strcmp(id, "pi"))
			return M_PI;
		if (!strcmp(id, "e"))
			return M_E;
		if (!strcmp(id, "ans")) {
			if (!C.have_ans)
				fail("No answer yet for ans");
			return C.ans;
		}
		P = at;
		fail("Unknown name");
		P += strlen(id);
		while (is_alpha(*P) || is_digit(*P))
			P++;
		return 0;
	}
	fail(*P ? "Something is wrong there" : "The sum ends too soon");
	return 0;
}

/* n! and n% after a number */
static double postfix(void)
{
	double v = primary();

	for (; !E;) {
		skip();
		if (*P == '!') {
			P++;
			if (v < 0 || v != floor(v))
				fail("Factorial of a whole number only");
			else if (v > 170)
				fail("Too large");
			v = tgamma(v + 1);
		} else if (*P == '%') {
			P++;
			v /= 100;
		} else {
			return v;
		}
	}
	return v;
}

/* a ^ b, right to left: 2^3^2 is 2^9; -2^2 is -4 */
static double power(void)
{
	double v = postfix();

	skip();
	if (*P == '^' || (P[0] == '*' && P[1] == '*')) {
		P += *P == '^' ? 1 : 2;
		return pow(v, unary());
	}
	return v;
}

static double unary(void)
{
	skip();
	if (*P == '-') {
		P++;
		return -unary();
	}
	if (*P == '+') {
		P++;
		return unary();
	}
	return power();
}

static int at_mod(void)
{
	return (P[0] == 'm' || P[0] == 'M') && (P[1] == 'o' || P[1] == 'O') &&
	       (P[2] == 'd' || P[2] == 'D') && !is_alpha(P[3]);
}

static double term(void)
{
	double v = unary();

	while (!E) {
		skip();
		if (*P == '*' && P[1] != '*') {
			P++;
			v *= unary();
		} else if (*P == '/') {
			double d;

			P++;
			d = unary();
			if (d == 0)
				fail("Division by zero");
			v /= d;
		} else if (at_mod()) {
			double d;

			P += 3;
			d = unary();
			if (d == 0)
				fail("Division by zero");
			v = fmod(v, d);
		} else if (is_digit(*P) || *P == '.' || *P == '(' || is_alpha(*P)) {
			v *= power();	/* 2pi, 3(4+5), 2 sqrt 2 */
		} else {
			return v;
		}
	}
	return v;
}

static double expr(void)
{
	double v = term();

	while (!E) {
		skip();
		if (*P == '+') {
			P++;
			v += term();
		} else if (*P == '-') {
			P++;
			v -= term();
		} else {
			return v;
		}
	}
	return v;
}

/* The sum s worked out into *v; 0, or what is wrong. */
static const char *calc(const char *s, double *v)
{
	P = s;
	E = 0;
	skip();
	if (!*P)
		return "Nothing to work out";
	*v = expr();
	skip();
	if (*P == ')')
		fail("A \"(\" is missing");
	else if (*P)
		fail("Something is wrong there");
	if (!E && isnan(*v))
		E = "Not a number";
	if (!E && isinf(*v))
		E = "Too large";
	return E;
}

/* ---- the line ----------------------------------------------------------- */

static void preview(void)
{
	double v;

	C.preview[0] = C.bases[0] = 0;
	if (C.len && !calc(C.in, &v)) {
		fmt_num(v, C.preview);
		fmt_bases(v, C.bases);
	} else if (!C.len && C.have_ans) {
		fmt_bases(C.ans, C.bases);	/* the last result's */
	}
}

static void set_line(const char *s)
{
	strncpy(C.in, s, IN_MAX);
	C.in[IN_MAX] = 0;
	C.len = C.cur = (int)strlen(C.in);
	C.err = 0;
	preview();
}

static void insert(const char *s)
{
	int n = (int)strlen(s);

	if (C.len + n > IN_MAX)
		return;
	memmove(C.in + C.cur + n, C.in + C.cur, (size_t)(C.len - C.cur + 1));
	memcpy(C.in + C.cur, s, (size_t)n);
	C.len += n;
	C.cur += n;
	C.err = 0;
	C.back = 0;
	preview();
}

static void erase_at(int at)
{
	if (at < 0 || at >= C.len)
		return;
	memmove(C.in + at, C.in + at + 1, (size_t)(C.len - at));
	C.len--;
	if (C.cur > at)
		C.cur--;
	C.err = 0;
	C.back = 0;
	preview();
}

static void enter(void)
{
	double v;
	const char *why = calc(C.in, &v);

	if (why) {
		C.err = why;
		return;
	}
	if (C.n_hist == HIST_MAX) {
		memmove(C.hist, C.hist + 1, sizeof(C.hist[0]) * (HIST_MAX - 1));
		C.n_hist--;
	}
	strcpy(C.hist[C.n_hist].sum, C.in);
	fmt_num(v, C.hist[C.n_hist].res);
	C.n_hist++;
	C.ans = v;
	C.have_ans = 1;
	C.back = 0;
	set_line("");
}

/* Up (dir 1) and Down (-1) through the earlier sums */
static void recall(int dir)
{
	int b = C.back + dir;

	if (b < 0 || b > C.n_hist)
		return;
	if (C.back == 0)
		strcpy(C.draft, C.in);
	C.back = b;
	set_line(b ? C.hist[C.n_hist - b].sum : C.draft);
	C.back = b;
}

/* ---- drawing ------------------------------------------------------------ */

static struct {
	char hhmm[6];
	int clock_ok, wifi_up /* bars, -1 none: wbstatus.h */, lora;
} U;

/*
 * The keys on the screen, as the board lays them out. Each shows the key
 * of the keyboard that does the same at its bottom right (the digits, the
 * point, + and the brackets are their own); pressing one lights it.
 */
enum { K_DIGIT, K_OP, K_FN, K_EQ };

static const struct { const char *label, *hint; int kind; } keys[20] = {
	{ "C", "Esc", K_FN }, { "(", 0, K_OP }, { ")", 0, K_OP }, { "\xc3\xb7", "/", K_OP },
	{ "7", 0, K_DIGIT }, { "8", 0, K_DIGIT }, { "9", 0, K_DIGIT }, { "\xc3\x97", "*", K_OP },
	{ "4", 0, K_DIGIT }, { "5", 0, K_DIGIT }, { "6", 0, K_DIGIT }, { "\xe2\x88\x92", "-", K_OP },
	{ "1", 0, K_DIGIT }, { "2", 0, K_DIGIT }, { "3", 0, K_DIGIT }, { "+", 0, K_OP },
	{ "0", 0, K_DIGIT }, { ".", 0, K_DIGIT }, { "\xe2\x8c\xab", "Bksp", K_FN }, { "=", "Enter", K_EQ },
};

#define KEY_C		0
#define KEY_ERASE	18
#define KEY_IS		19

/* The key on the screen for a character typed; -1 if it has none. */
static int grid_key(char ch)
{
	if (ch == '/')
		return 3;
	if (ch == '*')
		return 7;
	if (ch == '-')
		return 11;
	for (int i = 0; i < 20; i++)
		if (keys[i].label[0] == ch && !keys[i].label[1])
			return i;
	return -1;
}

static int key_y(int i)
{
	return KEYS_Y + i / 4 * (KEY_H + KEY_GAP);
}

/*
 * Glass in a colour (the board's keys and display): the void at tint over
 * what is drawn, a sheen of colour sheen from s0 at the top to s1 halfway
 * and 0 at the bottom, a 1 px edge of edge_c at edge and a white highlight
 * inside the top at hi. wbglass.h's glass() as its GLASS_TILE draws it, but
 * with these as parameters: libwb's kinds are white only.
 */
struct calc_skin { uint8_t tint, s0, s1, edge, hi; uint32_t sheen, edge_c; };

static const struct calc_skin SK_DISPLAY = { 227, 18, 5, 66, 51, 0xFFFFFF, 0xFFFFFF };	/* 89 % */
static const struct calc_skin SK_DIGIT = { 217, 18, 5, 66, 51, 0xFFFFFF, 0xFFFFFF };
static const struct calc_skin SK_OP = { 217, 46, 15, 77, 46, WB_COLOR_CYAN, WB_COLOR_CYAN };
static const struct calc_skin SK_FN = { 217, 46, 15, 77, 46, WB_COLOR_PINK, WB_COLOR_PINK };

static void calc_glass(int x, int y, int w, int h, const struct calc_skin *s)
{
	static const uint8_t bayer[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
	};
	const int r = WB_SQUARE, mid = h / 2;

	for (int j = 0; j < h; j++) {
		int py = y + j, lo = j < r || j >= h - r ? r : 1;
		unsigned sa, k16, m16[3], kr, mr[3], re = 0;
		uint32_t rc = 0;
		uint16_t *q = &fb[py * W];

		if (py < fb_y0 || py >= fb_y1)
			continue;
		sa = j < mid ? s->s0 - (s->s0 - s->s1) * (unsigned)j / (unsigned)mid :
		     s->s1 * (unsigned)(h - j) / (unsigned)(h - mid);
		/* the void and the sheen over v, as v * k + m in 16.16 */
		k16 = (unsigned)(((255u - s->tint) * (255 - sa) * 65536ull + 32512) / 65025);
		for (int c = 0; c < 3; c++) {
			unsigned t = (WB_COLOR_VOID >> (16 - 8 * c)) & 255, sv = (s->sheen >> (16 - 8 * c)) & 255;

			m16[c] = (unsigned)(((t * s->tint * (255 - sa) + sv * sa * 255u) * 65536ull + 32512) / 65025);
		}
		/* the edge along the top and the bottom, the highlight under it */
		if (j == 0 || j == h - 1) {
			re = s->edge;
			rc = s->edge_c;
		} else if (j == 1) {
			re = s->hi;
			rc = 0xFFFFFF;
		}
		kr = (unsigned)((uint64_t)k16 * (255 - re) / 255);
		for (int c = 0; c < 3; c++)
			mr[c] = (unsigned)((uint64_t)m16[c] * (255 - re) / 255) +
				((rc >> (16 - 8 * c)) & 255) * re * 257u;
		glass_span_fb(q, x + lo, x + w - lo, bayer[py & 3], kr, mr);
		/* the sides and the corners, a pixel at a time */
		for (int i = 0; i < w; i++) {
			int px = x + i, dth = bayer[py & 3][px & 3];
			unsigned a, ai, ah, e, hl, rgb[3];

			if (i == lo) {
				i = w - lo - 1;
				continue;
			}
			a = cover(rr_dist(px, py, x, y, w, h, r));
			if (!a)
				continue;
			ai = cover(rr_dist(px, py, x + 1, y + 1, w - 2, h - 2, r - 1));
			ah = cover(rr_dist(px, py, x + 1, y + 2, w - 2, h - 4, r - 1));
			e = a > ai ? div255((a - ai) * s->edge) : 0;
			hl = j < mid && ai > ah ? div255((ai - ah) * s->hi) : 0;
			rgb565_split(q[px], &rgb[0], &rgb[1], &rgb[2]);
			for (int c = 0; c < 3; c++) {
				int sh = 16 - 8 * c;
				unsigned v = (rgb[c] * k16 + m16[c] + 32768) >> 16;

				v = over8(v > 255 ? 255 : v, s->edge_c, sh, e);
				v = over8(v, 0xFFFFFF, sh, hl);
				rgb[c] = div255(v * a + rgb[c] * (255 - a));
			}
			rgb[0] = (rgb[0] + dth / 2) >> 3;
			rgb[1] = (rgb[1] + dth / 4) >> 2;
			rgb[2] = (rgb[2] + dth / 2) >> 3;
			q[px] = (uint16_t)((rgb[0] > 31 ? 31 : rgb[0]) << 11 | (rgb[1] > 63 ? 63 : rgb[1]) << 5 |
					   (rgb[2] > 31 ? 31 : rgb[2]));
		}
	}
}

/* The = key's glow: 12 px at 30 %, pressed 22 px at 60 % (from wb_assets.h's
 * 18 px at 40 %, as wbglass.h makes GLOW_FOCUS). */
static uint8_t glow_is[sizeof(glow_row) * 12 / 18], glow_is_lit[sizeof(glow_row) * 22 / 18];
static const struct glow GLOW_IS = { glow_is, sizeof(glow_is) };
static const struct glow GLOW_IS_LIT = { glow_is_lit, sizeof(glow_is_lit) };

static void calc_glow_init(void)
{
	for (unsigned i = 0; i < sizeof(glow_is); i++)
		glow_is[i] = (uint8_t)(glow_row[i * 18 / 12] * 30 / 40);
	for (unsigned i = 0; i < sizeof(glow_is_lit); i++)
		glow_is_lit[i] = (uint8_t)(glow_row[i * 18 / 22] * 60 / 40 > 255 ? 255 :
					   glow_row[i * 18 / 22] * 60 / 40);
}

/* c at a over b: the board's 60 % hints, over the key's own colour */
static uint32_t mix(uint32_t c, uint32_t b, unsigned a)
{
	uint32_t o = 0;

	for (int sh = 0; sh < 24; sh += 8)
		o |= (uint32_t)div255(((c >> sh) & 255) * a + ((b >> sh) & 255) * (255 - a)) << sh;
	return o;
}

/* The distance from (px, py) to the segment s (x0, y0, x1, y1), all in
 * 1/16 px. */
static int seg_d16(int px, int py, const int *s)
{
	long dx = s[2] - s[0], dy = s[3] - s[1], qx = px - s[0], qy = py - s[1];
	long l2 = dx * dx + dy * dy, t = l2 ? qx * dx + qy * dy : 0, ex, ey;

	t = t < 0 ? 0 : t > l2 ? l2 : t;
	ex = qx - (l2 ? dx * t / l2 : 0);
	ey = qy - (l2 ? dy * t / l2 : 0);
	return isqrt((unsigned)(ex * ex + ey * ey));
}

/*
 * ÷, ×, − (U+2212) and ⌫, which the fonts lack (their ⌫ is a "?" in a
 * box): strokes as thick as font f's +, as wide, centred on cx and on the
 * + of a line centred on cy. ⌫ is the design's erase key (Lucide
 * "delete", square).
 */
static void calc_sym(const struct wb_font *f, const char *s, int cx, int cy, uint32_t c)
{
	const struct wb_glyph *g = glyph(f, '+');
	const uint8_t *a = f->bits + g->off;
	int i0 = g->w, i1 = -1, j0 = g->h, j1 = -1, L, t = 0, x16 = cx * 16, y16, n = 0, reach;
	int seg[9][5];		/* x0, y0, x1, y1 from the centre, half the stroke */
	unsigned char k = (unsigned char)s[1], k2 = (unsigned char)s[2];

	for (int j = 0; j < g->h; j++)		/* the +'s ink */
		for (int i = 0; i < g->w; i++)
			if (a[j * g->w + i] > 64) {
				i0 = i < i0 ? i : i0;
				i1 = i > i1 ? i : i1;
				j0 = j < j0 ? j : j0;
				j1 = j > j1 ? j : j1;
			}
	if (i1 < 0)
		return;
	for (int i = 0; i < g->w; i++)		/* across its upright */
		t += a[(j0 + (j1 - j0) / 5) * g->w + i];
	t = t * 16 / 255;
	L = (i1 - i0 + 1) * 8;
	y16 = (text_top(f, cy) + g->y + j0) * 16 + (j1 - j0 + 1) * 8;
#define SEG(a, b, c_, d, w2) do { int *q = seg[n++]; q[0] = a; q[1] = b; q[2] = c_; q[3] = d; q[4] = w2; } while (0)
	if (k == 0x97) {			/* × */
		int m = L * 7 / 10;

		SEG(-m, -m, m, m, t / 2);
		SEG(-m, m, m, -m, t / 2);
	} else if (k == 0x88) {			/* − */
		SEG(-L + t / 2, 0, L - t / 2, 0, t / 2);
	} else if (k == 0xb7) {			/* ÷ */
		SEG(-L + t / 2, 0, L - t / 2, 0, t / 2);
		SEG(0, -L * 5 / 8, 0, -L * 5 / 8, t * 3 / 4);
		SEG(0, L * 5 / 8, 0, L * 5 / 8, t * 3 / 4);
	} else if (k == 0x8c && k2 == 0xab) {	/* ⌫, from a 24 grid: x 2..21, y 5..19 */
		static const signed char p[7][4] = {
			{ 2, 12, 9, 5 }, { 9, 5, 21, 5 }, { 21, 5, 21, 19 }, { 21, 19, 9, 19 },
			{ 9, 19, 2, 12 }, { 12, 9, 18, 15 }, { 18, 9, 12, 15 },
		};
		int u = L * 2 * 17 / 10 / 19;	/* 1.7 times the + across */

		for (int i = 0; i < 7; i++)
			SEG((p[i][0] * 2 - 23) * u / 2, (p[i][1] - 12) * u, (p[i][2] * 2 - 23) * u / 2,
			    (p[i][3] - 12) * u, t * 3 / 8);
	}
#undef SEG
	reach = (L * 17 / 10 + t) / 16 + 2;
	for (int py = y16 / 16 - reach; py <= y16 / 16 + reach; py++)
		for (int px = cx - reach; px <= cx + reach; px++) {
			int d = 1 << 20;

			if (py < fb_y0 || py >= fb_y1)
				continue;
			for (int i = 0; i < n; i++) {
				int e = seg_d16(px * 16 + 8 - x16, py * 16 + 8 - y16, seg[i]) - seg[i][4];

				d = e < d ? e : d;
			}
			put(px, py, c, cover(d));
		}
}

static void draw_key(int i)
{
	const struct wb_font *f = &f_ui28b, *fh = &f_mono13;
	int x = DSP_X + i % 4 * (KEY_W + KEY_GAP), y = key_y(i), lit = C.lit == i + 1;
	int kind = keys[i].kind;
	uint32_t fg, under;

	if (y - KEY_GLOW >= fb_y1 || y + KEY_H + KEY_GLOW <= fb_y0)
		return;
	if (kind == K_EQ) {
		box_shaped(x, y, KEY_W, KEY_H, WB_SQUARE, WB_COLOR_CYAN, 255, WB_COLOR_CYAN, 1,
			   lit ? &GLOW_IS_LIT : &GLOW_IS);
		fg = WB_COLOR_VOID;
		under = WB_COLOR_CYAN;
	} else {
		if (lit)
			wb_card(x, y, KEY_W, KEY_H, WB_SQUARE, 1, &GLOW_FOCUS);
		else
			calc_glass(x, y, KEY_W, KEY_H, kind == K_OP ? &SK_OP : kind == K_FN ? &SK_FN : &SK_DIGIT);
		fg = lit || kind == K_OP ? WB_COLOR_CYAN : kind == K_FN ? WB_COLOR_PINK : WB_COLOR_TEXT_HI;
		under = WB_COLOR_VOID;
	}
	if ((unsigned char)keys[i].label[0] >= 0xc3)	/* not in the font */
		calc_sym(f, keys[i].label, x + KEY_W / 2, y + KEY_H / 2, fg);
	else
		text(f, x + (KEY_W - text_w(f, keys[i].label, 0)) / 2, text_top(f, y + KEY_H / 2),
		     keys[i].label, fg, 0);
	if (keys[i].hint)
		text(fh, x + KEY_W - 10 - text_w(fh, keys[i].hint, 0), y + KEY_H - 6 - fh->ascent - fh->descent,
		     keys[i].hint, mix(fg, under, 153), 0);
}

/* s right-aligned at right, at most w px wide: its end, with "…" in front
 * when cut */
static void text_right(const struct wb_font *f, int right, int y, const char *s, uint32_t c, int w)
{
	int cut = 0, ew = text_w(f, "\xe2\x80\xa6", 0);

	while (*s && text_w(f, s, 0) > w - (cut ? ew : 0)) {
		s++;
		cut = 1;
	}
	right -= text_w(f, s, 0);
	text(f, right, y, s, c, 0);
	if (cut)
		text(f, right - ew, y, "\xe2\x80\xa6", c, 0);
}

/* The width of in[a..b) in font f. */
static int span_w(const struct wb_font *f, int a, int b)
{
	char t[IN_MAX + 1];

	memcpy(t, C.in + a, (size_t)(b - a));
	t[b - a] = 0;
	return text_w(f, t, 0);
}

/* The sum being typed, with its cursor, right-aligned on the display. */
static void draw_line(void)
{
	const struct wb_font *f = &f_ui22;
	int max = DSP_TEXT_W - 30, a = 0, b = C.len, x, cx, top = text_top(f, LINE_CY);
	char vis[IN_MAX + 1];

	/* what fits: the end of the line, or from the cursor on */
	while (a < C.cur && span_w(f, a, b) > max)
		a++;
	while (b > C.cur && span_w(f, a, b) > max)
		b--;
	memcpy(vis, C.in + a, (size_t)(b - a));
	vis[b - a] = 0;
	x = DSP_R - text_w(f, vis, 0);
	if (a > 0)
		text(f, x - text_w(f, "\xe2\x80\xa6", 0), top, "\xe2\x80\xa6", WB_COLOR_PLACEHOLDER, 0);
	if (!C.len && !C.n_hist)	/* nothing yet: what to do */
		text(f, DSP_R - 10 - text_w(f, "Type a sum", 0), top, "Type a sum", WB_COLOR_PLACEHOLDER, 0);
	text(f, x, top, vis, C.err ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	cx = x + span_w(f, a, C.cur);
	fill(cx, top + 2, 2, f->ascent + f->descent - 2, WB_COLOR_CYAN);
}

/*
 * The display, as the board's: in the top left a whole result in
 * hexadecimal and binary; right-aligned the sum before (or the one Up
 * brought back, in cyan) with its result, the sum being typed, and big
 * its result ("…" while it is not a sum yet), what is wrong after Enter,
 * or the last result.
 */
static void draw_display(void)
{
	const struct wb_font *fh = &f_mono14, *fr = &f_ui36b;
	char buf[IN_MAX + 48];
	const char *res;
	uint32_t rc = WB_COLOR_TEXT_HI;

	if (DSP_Y >= fb_y1 || DSP_Y + DSP_H <= fb_y0)
		return;
	calc_glass(DSP_X, DSP_Y, DSP_W, DSP_H, &SK_DISPLAY);
	if (C.bases[0])
		text(&f_mono13, DSP_L, DSP_Y + 14, wb_fit(&f_mono13, C.bases, DSP_TEXT_W, buf, sizeof(buf)),
		     WB_COLOR_PLACEHOLDER, 0);
	if (C.n_hist) {
		int i = C.back ? C.n_hist - C.back : C.n_hist - 1;

		strcpy(buf, C.hist[i].sum);
		strcat(buf, " = ");
		strcat(buf, C.hist[i].res);
		text_right(fh, DSP_R, text_top(fh, HIST_CY), buf, C.back ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER,
			   DSP_TEXT_W);
	}
	draw_line();
	if (C.err) {
		text(&f_ui22, DSP_R - text_w(&f_ui22, C.err, 0), text_top(&f_ui22, RES_CY), C.err,
		     WB_COLOR_ERROR, 0);
		return;
	}
	if (C.len && C.preview[0]) {
		strcpy(buf, "= ");
		strcat(buf, C.preview);
		res = buf;
	} else if (C.len) {
		res = "\xe2\x80\xa6";
		rc = WB_COLOR_PLACEHOLDER;
	} else if (C.have_ans) {
		fmt_num(C.ans, buf);
		res = buf;
	} else {
		res = "0";
	}
	if (text_w(fr, res, -5) > DSP_TEXT_W)
		fr = &f_ui28b;
	text(fr, DSP_R - text_w(fr, res, -5), text_top(fr, RES_CY), res, rc, -5);
}

/* status bar, as home's: WhyOS left; LoRa, Wi-Fi and the clock right */
static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
	     text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
	status_right(U.hhmm, U.clock_ok, U.wifi_up, U.lora);
}

/* The calculator: the header, the display, the keys, the hint bar. */
static void draw_calc(void)
{
	static const struct hint hints[] = {
		{ "0\xe2\x80\x93" "9 + - * /", "type", 0 }, { "Enter", "=", 0 }, { "Esc", "clear", 1 },
	};
	static const struct hint empty_hints[] = {
		{ "0\xe2\x80\x93" "9 + - * /", "type", 0 }, { "Enter", "=", 0 }, { "Esc", "home", 1 },
	};
	const char *chip = C.rad ? "RAD" : "DEG";
	int cw = text_w(&f_mono14, chip, 0) + 24, cx = DSP_X + DSP_W - cw;

	status_bar();
	eyebrow(HEAD_X, HEAD_Y, "APP");
	text(&f_ui28b, HEAD_X, TITLE_Y, "Calculator", WB_COLOR_TEXT_HI, -10);
	calc_glass(cx, CHIP_Y, cw, CHIP_H, &SK_OP);
	text(&f_mono14, cx + 12, text_top(&f_mono14, CHIP_Y + CHIP_H / 2), chip, WB_COLOR_CYAN, 0);
	draw_display();
	for (int i = 0; i < 20; i++)
		draw_key(i);
	if (C.len || C.err)
		hint_bar(hints, 3);
	else
		hint_bar(empty_hints, 3);
}

/* The circle key's screen: the sums so far, a row each, the newest at the
 * bottom (as many as fit). */
static void draw_log(void)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, top = WB_ROWS_TOP;
	int n = (H - WB_HINT_BAR_H - 24 - top - 16) / LOG_ROW, first;

	status_bar();
	wb_header("CALCULATOR \xe2\x80\xba", "Log", 0);
	if (!C.n_hist) {
		text(&f_ui20, x + 6, top + 8, "No sums yet.", WB_COLOR_TEXT_MID, 0);
		hint_bar(hints, 1);
		return;
	}
	n = C.n_hist < n ? C.n_hist : n;
	first = C.n_hist - n;
	wb_card(x, top, w, n * LOG_ROW + 16, WB_SQUARE, 0, 0);
	for (int i = first; i < C.n_hist; i++) {
		char res[48] = "= ";
		int cy = top + 8 + (i - first) * LOG_ROW + LOG_ROW / 2, rw;

		strcat(res, C.hist[i].res);
		rw = text_w(&f_ui22, res, 0);
		if (i > first)
			fill(x + 20, cy - LOG_ROW / 2, w - 40, 1, 0x26262F);
		text(&f_ui22, x + w - 20 - rw, text_top(&f_ui22, cy), res, WB_COLOR_TEXT_HI, 0);
		text_right(&f_mono16b, x + w - 20 - rw - 24, text_top(&f_mono16b, cy), C.hist[i].sum,
			   WB_COLOR_PLACEHOLDER, w - 40 - rw - 24);
	}
	hint_bar(hints, 1);
}

static const char *const units[2] = { "Degrees", "Radians" };

/* The cloud key's screen: the angle unit, chosen in a pop-up list. */
static void draw_settings(void)
{
	static const struct hint hints[] = { { "Enter", "change", 0 }, { "Esc", "back", 1 } };

	status_bar();
	wb_header("CALCULATOR \xe2\x80\xba", "Settings", 0);
	wb_row(WB_CONTENT_PAD, WB_ROWS_TOP, W - 2 * WB_CONTENT_PAD, WB_LIST_ROW_H, 0, "Angle unit",
	       "For sin, cos, tan and their inverses. Tab switches it too.", units[C.rad], WB_ROW_OPEN, 0, 1);
	hint_bar(hints, 2);
}

/* The diamond key: what this screen does and its keys. */
static void draw_help(void)
{
	static const struct help_key calc_keys[] = {
		{ "Enter", "Work the sum out (= does too)" },
		{ "\xe2\x86\x91\xe2\x86\x93", "Bring back earlier sums" },
		{ "\xe2\x86\x90\xe2\x86\x92", "Move the cursor (with Fn: to an end)" },
		{ "Backspace", "Erase before the cursor" },
		{ "Tab", "Degrees or radians" },
		{ "Esc", "Clear the line; on an empty line, close" },
		{ "#4", "Log: the sums so far" },
		{ "#5", "Settings: the angle unit" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key set_keys[] = {
		{ "Enter", "Choose the angle unit" },
		{ "Esc", "Back to the calculator" },
		{ "#4", "Log" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key log_keys[] = {
		{ "Esc", "Back to the calculator" },
		{ "#5", "Settings" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};

	if (C.sc == SC_SETTINGS)
		help_popup("Settings", "The calculator's settings. They last until it closes.",
			   set_keys, sizeof(set_keys) / sizeof(set_keys[0]));
	else if (C.sc == SC_LOG)
		help_popup("Log", "The sums worked out since the calculator opened, the newest at the bottom.",
			   log_keys, sizeof(log_keys) / sizeof(log_keys[0]));
	else
		help_popup("Calculator", "Type a sum and press Enter. Also: ^ % ! mod pi e ans sqrt ln log "
			   "sin cos tan. + * / or ^ first goes on from the last result.",
			   calc_keys, sizeof(calc_keys) / sizeof(calc_keys[0]));
}

static int scene_first;		/* the next band is a frame's first */

/* The screen in rows fb_y0..fb_y1: the scene behind, then the rest. */
static void draw_screen(void)
{
	static const struct hint pick_hints[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };

	if (scene_first) {
		scene_first = 0;
		wb_scene();
	} else {
		scene_draw();
	}
	if (C.sc == SC_LOG)
		draw_log();
	else if (C.sc == SC_SETTINGS)
		draw_settings();
	else
		draw_calc();
	/* drawn in bands, the pop-ups are glass, unblurred (wbglass.h) */
	if (C.sc == SC_SETTINGS && C.pick) {
		struct menu_item it[2] = { { .label = "Degrees" }, { .label = "Radians" } };

		menu_popup("SETTINGS", "Angle unit", "2 options", it, 0, 2, C.pick_focus, C.rad,
			   0, 0, pick_hints, 2);
	}
	if (C.help)
		draw_help();
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

/* The screen is drawn a band of BAND_H rows at a time, only the bands that
 * changed (dirty). */
#define BAND_H		40
#define NBANDS		(H / BAND_H)
#define ALL_BANDS	((1u << NBANDS) - 1)

static uint16_t band[W * BAND_H];
static uint32_t dirty;		/* the bands to draw again, a bit each */
#ifdef CALC_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void out_rows(const uint16_t *rows, int y0, int y1)
{
#ifdef CALC_HOST
	memcpy(screen + y0 * W, rows, (size_t)(y1 - y0) * W * 2);
#else
	pwrite(fb_fd, rows, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
}

static void mark(int y0, int y1)
{
	for (int b = (y0 < 0 ? 0 : y0) / BAND_H; b < NBANDS && b * BAND_H < y1; b++)
		dirty |= 1u << b;
}

/* a key on the screen, with its glow */
static void mark_key(int lit)
{
	if (lit)
		mark(key_y(lit - 1) - KEY_GLOW, key_y(lit - 1) + KEY_H + KEY_GLOW);
}

/* What key() returning r changed, to draw again; lit: C.lit before it.
 * Typing draws the display, the hint bar (Esc's word: clear or close) and
 * the keys that lit or went out; Enter, Tab and the like the DEG/RAD chip
 * too. */
static void changed(int r, int lit)
{
	if (r == 3) {
		dirty = ALL_BANDS;
	} else if (C.sc == SC_CALC && !C.help) {
		mark(DSP_Y, DSP_Y + DSP_H);
		mark(H - WB_HINT_BAR_H, H);
		if (r == 0)
			mark(CHIP_Y, CHIP_Y + CHIP_H);
	}
	if (lit != C.lit) {
		mark_key(lit);
		mark_key(C.lit);
	}
}

static void flush(void)
{
	if (wb_scene_turned())		/* the panel turned the old picture */
		dirty = ALL_BANDS;
	if (!dirty)
		return;
	scene_first = 1;
	for (int b = 0; b < NBANDS; b++) {
		int y0 = b * BAND_H;

		if (!(dirty & 1u << b))
			continue;
		fb = band - (long)y0 * W;
		fb_y0 = y0;
		fb_y1 = y0 + BAND_H;
		draw_screen();
		out_rows(band, y0, y0 + BAND_H);
	}
	dirty = 0;
}

/* ---- keys --------------------------------------------------------------- */

static struct { int shift, fn; } K;

/* US layout, as home's key_char. */
static char key_char(int code, int shift)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == 57)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo))
		return 0;
	return shift ? hi[code] : lo[code];
}

/* An x typed after a number is "times" (3x4), except the 0 of 0x1f. */
static int times_x(void)
{
	int i = C.cur;

	if (!i || !is_digit(C.in[i - 1]))
		return 0;
	return !(C.in[i - 1] == '0' && (i == 1 || (!is_digit(C.in[i - 2]) && C.in[i - 2] != '.')));
}

/*
 * The keys of the pop-ups and of the log and settings screens; -1 when the
 * calculator itself takes it.
 */
static int other_key(int code)
{
	if (C.help) {		/* only an explanation: Esc or the diamond closes it */
		if (code == KEY_ESC || code == KEY_SYM_HELP)
			C.help = 0;
		return 3;
	}
	if (C.pick) {
		if (code == KEY_UP || code == KEY_DOWN)
			C.pick_focus ^= 1;
		else if (code == KEY_ENTER) {
			C.rad = C.pick_focus;
			preview();
			C.pick = 0;
		} else if (code == KEY_ESC)
			C.pick = 0;
		return 3;
	}
	switch (code) {
	case KEY_SYM_LOG:
		C.sc = SC_LOG;
		return 3;
	case KEY_SYM_SETTINGS:
		C.sc = SC_SETTINGS;
		return 3;
	case KEY_SYM_HELP:
		C.help = 1;
		return 3;
	}
	if (C.sc == SC_CALC)
		return -1;
	if (code == KEY_ESC)
		C.sc = SC_CALC;
	else if (code == KEY_ENTER && C.sc == SC_SETTINGS) {
		C.pick = 1;
		C.pick_focus = C.rad;
	}
	return 3;
}

/* One key; 0 to go on, 1 to close, 2 when only the line changed, 3 for the
 * whole screen. A key that is on the screen lights there (C.lit). */
static int key(int code)
{
	char ch, s[4] = "";
	int r = other_key(code);

	if (r >= 0)
		return r;
	C.lit = 0;
	switch (code) {
	case KEY_ENTER:
		C.lit = KEY_IS + 1;
		enter();
		return 0;
	case KEY_ESC:
		if (!C.len && !C.err)
			return 1;
		C.lit = KEY_C + 1;
		C.back = 0;
		set_line("");
		return 0;
	case KEY_TAB:
		C.rad = !C.rad;
		preview();
		return 0;
	case KEY_BACKSPACE:
		C.lit = KEY_ERASE + 1;
		erase_at(K.fn ? C.cur : C.cur - 1);
		return 2;
	case KEY_LEFT:
		C.cur = K.fn ? 0 : C.cur > 0 ? C.cur - 1 : 0;
		return 2;
	case KEY_RIGHT:
		C.cur = K.fn ? C.len : C.cur < C.len ? C.cur + 1 : C.len;
		return 2;
	case KEY_UP:
		recall(1);
		return 0;
	case KEY_DOWN:
		recall(-1);
		return 0;
	}
	if (code == KEY_EQUAL && !K.shift) {
		C.lit = KEY_IS + 1;
		enter();
		return 0;
	}
	ch = key_char(code, K.shift);
	if (!ch)
		return 2;
	if (ch == 'x' && times_x())
		ch = '*';
	if (!strchr("0123456789.+-*/^()%! ", ch) && !is_alpha(ch))
		return 2;
	C.lit = grid_key(ch) + 1;
	/* an operator first continues from the last result ("-" first is a
	 * negative number) */
	if (!C.len && C.have_ans && strchr("+*/^%!", ch))
		insert("ans");
	s[0] = ch;
	insert(s);
	return 2;
}

/* ---- the host test ------------------------------------------------------ */

#ifdef CALC_HOST
static void save_ppm(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	static uint8_t rgb[W * H * 3];
	static const char hdr[] = "P6\n720 720\n255\n";

	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];

		rgb[i * 3] = (uint8_t)((p >> 11) << 3);
		rgb[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) << 2);
		rgb[i * 3 + 2] = (uint8_t)((p & 31) << 3);
	}
	if (fd < 0 || write(fd, hdr, sizeof(hdr) - 1) < 0 || write(fd, rgb, sizeof(rgb)) < 0)
		_exit(1);
	close(fd);
}

/* calc-host KEYS OUT.ppm: KEYS is typed; \n Enter, \e Esc, \t Tab, \b
 * Backspace, \u Up, \d Down, \l Left, \r Right, \1..\6 the symbol keys.
 * Prints the screen, the angle unit, the line, the preview and the tape. */
int main(int argc, char **argv)
{
	const char *k = argc > 1 ? argv[1] : "";

	memcpy(U.hhmm, "12:34", 6);
	U.clock_ok = 1;
	wb_init();
	calc_glow_init();
	dirty = ALL_BANDS;
	flush();
	for (; *k; k++) {
		int code = -1;

		if (*k == '\\' && k[1]) {
			k++;
			code = *k == 'n' ? KEY_ENTER : *k == 'e' ? KEY_ESC : *k == 't' ? KEY_TAB :
			       *k == 'b' ? KEY_BACKSPACE : *k == 'u' ? KEY_UP : *k == 'd' ? KEY_DOWN :
			       *k == 'l' ? KEY_LEFT : *k == 'r' ? KEY_RIGHT :
			       *k >= '1' && *k <= '6' ? KEY_SYM_MENU + (*k - '1') : -1;
		} else {
			for (int c = 0; c < 64 && code < 0; c++) {
				if (key_char(c, 0) == *k)
					code = c, K.shift = 0;
				else if (key_char(c, 1) == *k)
					code = c, K.shift = 1;
			}
			if (*k == ' ')
				code = 57, K.shift = 0;
		}
		if (code >= 0) {
			int lit = C.lit, r = key(code);

			if (r == 1) {
				write(1, "closed\n", 7);
				break;
			}
			changed(r, lit);	/* drawn as the badge draws it */
			flush();
		}
		K.shift = 0;
	}
	{
		/* what the badge drew key by key must be the screen drawn whole */
		static uint16_t drawn[W * H];

		memcpy(drawn, screen, sizeof(screen));
		dirty = ALL_BANDS;
		flush();
		if (memcmp(drawn, screen, sizeof(screen)))
			write(1, "stale\n", 6);
	}
	if (argc > 2)
		save_ppm(argv[2]);
	{
		char out[1024] = "screen ";

		strcat(out, C.sc == SC_LOG ? "log" : C.sc == SC_SETTINGS ? "settings" : "calc");
		strcat(out, C.help ? " help" : C.pick ? " pick" : "");
		strcat(out, C.rad ? "\nunit rad" : "\nunit deg");
		strcat(out, "\nline ");

		strcat(out, C.in);
		strcat(out, "\npreview ");
		strcat(out, C.preview);
		strcat(out, "\nbases ");
		strcat(out, C.bases);
		strcat(out, "\nerror ");
		strcat(out, C.err ? C.err : "");
		strcat(out, "\n");
		for (int i = 0; i < C.n_hist; i++) {
			strcat(out, C.hist[i].sum);
			strcat(out, " = ");
			strcat(out, C.hist[i].res);
			strcat(out, "\n");
		}
		write(1, out, strlen(out));
	}
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
static long last_status;		/* when the status bar was read */

static void on_signal(int s)
{
	stop_sig = s;
}

static int read_line(const char *path, char *buf, int size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n;

	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, (size_t)size - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = 0;
	return n;
}

/* the status bar; 1 if it changed */
static int status_read(void)
{
	time_t t = time(0);
	struct tm tm;
	char old[6];
	int wifi = U.wifi_up, ok = U.clock_ok, lora = U.lora;

	memcpy(old, U.hhmm, sizeof(old));
	localtime_r(&t, &tm);
	U.clock_ok = tm.tm_year >= 124;
	U.hhmm[0] = (char)('0' + tm.tm_hour / 10);
	U.hhmm[1] = (char)('0' + tm.tm_hour % 10);
	U.hhmm[2] = ':';
	U.hhmm[3] = (char)('0' + tm.tm_min / 10);
	U.hhmm[4] = (char)('0' + tm.tm_min % 10);
	U.hhmm[5] = 0;
	U.wifi_up = wifi_bars();	/* -1: not connected, else the lit arcs */
	U.lora = lora_on();
	return wifi != U.wifi_up || ok != U.clock_ok || lora != U.lora || memcmp(old, U.hhmm, sizeof(old));
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p;
	int ev;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev < 0)
		return 1;
	wb_init();
	calc_glow_init();
	status_read();
	dirty = ALL_BANDS;
	flush();
	p.fd = ev;
	p.events = POLLIN;
	last_status = wb_now_us();
	while (!stop_sig) {
		struct ev evs[16];
		int k = poll(&p, 1, 250), got, quit = 0;

		/* every 250 ms: has the badge turned (flush() looks); every 5 s
		 * the status bar */
		if (k <= 0) {
			if (wb_now_us() - last_status >= 5000000) {
				last_status = wb_now_us();
				if (status_read())
					mark(0, WB_STATUS_BAR_H);
			}
			flush();
			continue;
		}
		got = (int)read(ev, evs, sizeof(evs)) / (int)sizeof(evs[0]);
		for (int q = 0; q < got && !quit; q++) {
			const struct ev e = evs[q];
			int down = e.value != 0, r;

			if (e.type != 1)
				continue;
			if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT)
				K.shift = down;
			else if (e.code == KEY_FN)
				K.fn = down;
			else if (key_acts(e.code, e.value)) {
				int lit = C.lit;

				r = key(e.code);
				if (r == 1)
					quit = 1;
				else
					changed(r, lit);
			}
		}
		if (quit)
			break;
		flush();
	}
	return 0;
}
#endif
