// SPDX-License-Identifier: MIT
/*
 * calc — a calculator for the badge, drawn like the menu (wbdraw.h and the
 * design's fonts), as an app from the store (sdapps/README.md).
 *
 * You type a sum on the keyboard and Enter (or =) works it out; the sums
 * stay above it as a paper tape. What it understands:
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
 * circle opens the log (the calculator keeps none: an empty screen), the
 * cloud its settings (the angle unit, a pop-up list), the diamond the
 * help; the left three do nothing here.
 *
 * The panel is drawn one band of BAND_H rows at a time through wbdraw.h's
 * clipping, as appterm does, so there is no 1 MB frame buffer next to it.
 * Built with -DCALC_HOST, it takes keys from its first argument and writes
 * the screen as PPM to its second: the host test (calc/test_calc.py).
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

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"
#include "lora_state.h"
#include "wbstatus.h"

#ifndef CALC_HOST
#include <poll.h>
#endif

/* ---- layout ------------------------------------------------------------- */

#define PANEL_X		16
#define PANEL_W		(W - 2 * PANEL_X)
#define PANEL_Y		118
#define PANEL_H		500
#define PAD		24
#define RIGHT		(PANEL_X + PANEL_W - PAD)
#define LEFT		(PANEL_X + PAD)
#define TAPE_Y		(PANEL_Y + 16)		/* the tape: earlier sums */
#define ENTRY_H		58
#define LINE_Y		(PANEL_Y + 368)		/* the rule above the input */
#define INPUT_Y		(LINE_Y + 14)		/* the sum being typed */
#define PREVIEW_Y	(INPUT_Y + 52)		/* its result, or what is wrong */
#define BASES_Y		(PREVIEW_Y + 34)	/* hexadecimal and binary */
#define HELP_Y		(PANEL_Y + PANEL_H + 14)

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

/* s at most w px wide: its end, with "…" in front when cut */
static const char *fit(const struct wb_font *f, const char *s, int w, int *cut)
{
	*cut = 0;
	while (*s && text_w(f, s, 0) > w) {
		s++;
		*cut = 1;
	}
	return s;
}

static void draw_tape(void)
{
	int y = LINE_Y - 12;		/* bottom of the newest entry */

	if (!C.n_hist) {
		static const char *const ex[] = {
			"12 * 7 + 3", "sqrt(2)", "15% * 80", "2pi * 4", "0xff + 1", "sin 30",
		};
		const struct wb_font *f = &f_ui20;
		int ty = TAPE_Y + 8;

		text(f, LEFT, ty, "Type a sum and press Enter.", WB_COLOR_TEXT_MID, 0);
		ty += 40;
		eyebrow(LEFT, ty, "FOR EXAMPLE");
		ty += 30;
		for (unsigned i = 0; i < sizeof(ex) / sizeof(ex[0]); i++, ty += 30)
			text(&f_mono18, LEFT, ty, ex[i], WB_COLOR_TEXT_HI, 0);
		return;
	}
	for (int i = C.n_hist - 1; i >= 0 && y - ENTRY_H >= TAPE_Y; i--, y -= ENTRY_H) {
		const struct wb_font *fs = &f_mono16b, *fr = &f_ui22;
		int cut, recalled = C.back && i == C.n_hist - C.back;
		const char *s = fit(fs, C.hist[i].sum, PANEL_W - 2 * PAD - 20, &cut);
		char res[48] = "= ";
		int sw = text_w(fs, s, 0), x;

		strcat(res, C.hist[i].res);
		x = RIGHT - sw;
		if (cut)
			text(fs, x - text_w(fs, "\xe2\x80\xa6", 0), y - ENTRY_H + 6, "\xe2\x80\xa6",
			     WB_COLOR_TEXT_LOW, 0);
		text(fs, x, y - ENTRY_H + 6, s, recalled ? WB_COLOR_CYAN : WB_COLOR_TEXT_LOW, 0);
		text(fr, RIGHT - text_w(fr, res, 0), y - ENTRY_H + 26, res, WB_COLOR_TEXT_HI, 0);
	}
}

/* The width of in[a..b) in font f. */
static int span_w(const struct wb_font *f, int a, int b)
{
	char t[IN_MAX + 1];

	memcpy(t, C.in + a, (size_t)(b - a));
	t[b - a] = 0;
	return text_w(f, t, 0);
}

static void draw_input(void)
{
	const struct wb_font *f = &f_ui36b;
	int max = PANEL_W - 2 * PAD - 30, a = 0, b = C.len, x, cx;
	char vis[IN_MAX + 1];

	/* what fits: the end of the line, or from the cursor on */
	while (a < C.cur && span_w(f, a, b) > max)
		a++;
	while (b > C.cur && span_w(f, a, b) > max)
		b--;
	memcpy(vis, C.in + a, (size_t)(b - a));
	vis[b - a] = 0;
	x = RIGHT - text_w(f, vis, 0);
	if (a > 0)
		text(f, x - text_w(f, "\xe2\x80\xa6", 0), INPUT_Y, "\xe2\x80\xa6", WB_COLOR_TEXT_LOW, 0);
	text(f, x, INPUT_Y, vis, C.err ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
	cx = x + span_w(f, a, C.cur);
	fill(cx, INPUT_Y + 2, 3, f->ascent + f->descent - 2, WB_COLOR_CYAN);

	if (C.err)
		text(&f_ui20, RIGHT - text_w(&f_ui20, C.err, 0), PREVIEW_Y, C.err, WB_COLOR_ERROR, 0);
	else if (C.preview[0]) {
		char p[48] = "= ";

		strcat(p, C.preview);
		text(&f_ui22, RIGHT - text_w(&f_ui22, p, 0), PREVIEW_Y, p, WB_COLOR_TEXT_MID, 0);
	}
	if (C.bases[0])
		text(&f_mono14, RIGHT - text_w(&f_mono14, C.bases, 0), BASES_Y, C.bases,
		     WB_COLOR_TEXT_LOW, 0);
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

static const char *const units[2] = { "Degrees", "Radians" };

/* The cloud key's screen: the angle unit, chosen in a pop-up list. */
static void draw_settings(void)
{
	static const struct hint hints[] = { { "Enter", "change", 0 }, { "Esc", "back", 1 } };
	int top = WB_STATUS_BAR_H + 20, x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, y, cy, rx;

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	eyebrow(x, top, "CALCULATOR");
	text(&f_ui36b, x, top + 18 + 6, "Settings", WB_COLOR_TEXT_HI, -11);
	y = top + 18 + 6 + 44 + 24;
	cy = y + 34;
	box_focus(x, y, w, 68, WB_RADIUS_MD, &GLOW_ROW);
	rx = x + w - 20 - 20;
	icon(&r_chevron, rx, cy - 10, WB_COLOR_TEXT_LOW, 255);
	rx -= 16 + text_w(&f_ui18, units[C.rad], 0);
	text(&f_ui18, rx, text_top(&f_ui18, cy), units[C.rad], WB_COLOR_TEXT_HI, 0);
	text(&f_ui22, x + 20, text_top(&f_ui22, cy), "Angle unit", WB_COLOR_TEXT_HI, 0);
	text(&f_ui18, x + 20, y + 68 + 12, "For sin, cos, tan and their inverses. Tab switches it too.",
	     WB_COLOR_TEXT_LOW, 0);
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
		{ "#4", "Log (the calculator keeps none)" },
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
		help_popup("Log", "The calculator keeps no log: its sums are on the tape.",
			   log_keys, sizeof(log_keys) / sizeof(log_keys[0]));
	else
		help_popup("Calculator", "Type a sum and press Enter; it goes on the tape above. "
			   "Starting with + * / or ^ goes on from the last result.",
			   calc_keys, sizeof(calc_keys) / sizeof(calc_keys[0]));
}

static void draw_band(int y0, int y1)
{
	static const struct hint hints[] = {
		{ "Enter", "=", 0 }, { "\xe2\x86\x91\xe2\x86\x93", "earlier", 0 },
		{ "Tab", "deg/rad", 0 }, { "Esc", "clear", 1 },
	};
	static const struct hint empty_hints[] = {
		{ "Enter", "=", 0 }, { "\xe2\x86\x91\xe2\x86\x93", "earlier", 0 },
		{ "Tab", "deg/rad", 0 }, { "Esc", "close", 1 },
	};
	static const struct hint pick_hints[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
	const char *chip = C.rad ? "RAD" : "DEG";

	if (C.sc == SC_LOG) {
		empty_screen("CALCULATOR", "Log", "No log yet.");
		status_bar();
	} else if (C.sc == SC_SETTINGS) {
		draw_settings();
		if (C.pick) {
			struct menu_item it[2] = { { .label = "Degrees" }, { .label = "Radians" } };

			menu_popup("CALCULATOR", "Angle unit", "2 options", it, 0, 2, C.pick_focus, C.rad,
				   0, 0, pick_hints, 2);
		}
	}
	if (C.sc != SC_CALC) {
		if (C.help)
			draw_help();
		return;
	}
	fill(0, y0, W, y1 - y0, WB_COLOR_VOID);
	status_bar();
	eyebrow(PANEL_X + 8, 54, "APPS");
	text(&f_ui28b, PANEL_X + 6, 72, "Calculator", WB_COLOR_TEXT_HI, -8);
	{
		int w = text_w(&f_mono14, chip, 0) + 24, x = W - PANEL_X - w;

		box(x, 78, w, 28, 14, WB_COLOR_VOID, 255, WB_COLOR_CYAN, 1, 0);
		text(&f_mono14, x + 12, text_top(&f_mono14, 92), chip, WB_COLOR_CYAN, 0);
	}
	box(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, WB_RADIUS_MD, WB_COLOR_CONSOLE, 255,
	    WB_COLOR_LINE, 1, 0);
	if (y0 < LINE_Y)
		draw_tape();
	fill(PANEL_X + PAD, LINE_Y, PANEL_W - 2 * PAD, 1, WB_COLOR_LINE);
	if (y1 > LINE_Y)
		draw_input();
	text(&f_mono13, LEFT, HELP_Y,
	     "+ - * / ^ ( )  n%  n!  mod  pi e ans  sqrt ln log sin cos tan",
	     WB_COLOR_TEXT_LOW, 0);
	if (C.len || C.err)
		hint_bar(hints, 4);
	else
		hint_bar(empty_hints, 4);
	if (C.help)
		draw_help();
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

#define BAND_H	40
static uint16_t band[W * BAND_H];
#ifdef CALC_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void flush_band(int y0, int y1)
{
	fb = band - (long)y0 * W;
	fb_y0 = y0;
	fb_y1 = y1;
	draw_band(y0, y1);
#ifdef CALC_HOST
	memcpy(screen + y0 * W, band, (size_t)(y1 - y0) * W * 2);
#else
	pwrite(fb_fd, band, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
}

static void flush_rows(int y0, int y1)
{
	for (int y = y0; y < y1; y += BAND_H)
		flush_band(y, y + BAND_H < y1 ? y + BAND_H : y1);
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
 * whole screen. */
static int key(int code)
{
	char ch, s[4] = "";
	int r = other_key(code);

	if (r >= 0)
		return r;
	switch (code) {
	case KEY_ENTER:
		enter();
		return 0;
	case KEY_ESC:
		if (!C.len && !C.err)
			return 1;
		C.back = 0;
		set_line("");
		return 0;
	case KEY_TAB:
		C.rad = !C.rad;
		preview();
		return 0;
	case KEY_BACKSPACE:
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
		if (code >= 0 && key(code) == 1) {
			write(1, "closed\n", 7);
			break;
		}
		K.shift = 0;
	}
	flush_rows(0, H);
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
	status_read();
	flush_rows(0, H);
	p.fd = ev;
	p.events = POLLIN;
	while (!stop_sig) {
		struct ev evs[16];
		int k = poll(&p, 1, 5000), got, tape = 0, line = 0, quit = 0, all = 0;

		if (k <= 0) {
			if (status_read())
				flush_rows(0, WB_STATUS_BAR_H);
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
			else if (down) {
				r = key(e.code);
				if (r == 1)
					quit = 1;
				else if (r == 2)
					line = 1;
				else if (r == 3)
					all = 1;
				else
					tape = 1;
			}
		}
		if (quit)
			break;
		/* typing redraws the line only: a whole screen is ~14 bands */
		if (all) {
			flush_rows(0, H);
			continue;
		}
		if (tape || line)	/* Esc's word: clear or close */
			flush_rows(H - WB_HINT_BAR_H, H);
		if (tape)
			flush_rows(PANEL_Y, PANEL_Y + PANEL_H);
		else if (line)
			flush_rows(LINE_Y + 1, PANEL_Y + PANEL_H);
		if (tape)
			flush_rows(78, 78 + 28);	/* the DEG/RAD chip */
	}
	return 0;
}
#endif
