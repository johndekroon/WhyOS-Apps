// SPDX-License-Identifier: MIT
/*
 * sums.c — the calculator's own logic: a sum worked out, and numbers as
 * text (sums.h). No screen and no keys: calc.c describes those, and
 * tests/test_sums.py checks this on its own.
 *
 * What it understands:
 *
 *   + - * / ^ (or **), brackets (a missing ")" at the end is added),
 *   n% (n/100), n! (factorial), "mod", 2pi or 3(4+5) as a product,
 *   0x1f and 0b101, 1.5e3, the names pi, e and ans (the last result),
 *   sqrt cbrt abs exp ln log log2 sin cos tan asin acos atan sinh cosh
 *   tanh round floor ceil.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sums.h"

int sums_rad;
double sums_ans;
int sums_have_ans;

/* ---- numbers as text ---------------------------------------------------- */

/*
 * v with DIG significant digits, trailing zeros dropped; plain from 1e-6
 * to 1e15, else as 1.5e+20. Digits come from doubles (no 64-bit division,
 * which hangs with this toolchain: sensorpanel.c).
 */
#define DIG	15	/* what a double holds for sure */

void fmt_num(double v, char *out)
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
void fmt_bases(double v, char *out)
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
	return sums_rad ? x : x * (M_PI / 180);
}

static double angle_out(double x)
{
	return sums_rad ? x : x * (180 / M_PI);
}

/* sin 180° is 1.2e-16 in doubles: 0, as on a calculator */
static double trig(double v)
{
	return !sums_rad && fabs(v) < 1e-14 ? 0 : v;
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
		if (!sums_rad && fmod(fabs(x), 180) == 90)
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
			if (!sums_have_ans)
				fail("No answer yet for ans");
			return sums_ans;
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
const char *calc(const char *s, double *v)
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
