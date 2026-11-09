// SPDX-License-Identifier: MIT
/* The calculator's sums on their own (sums.c), without a screen. */
#include "whyos/check.h"
#include "../../sums.h"

static char out[64];

/* s worked out as the calculator shows it, or what is wrong */
static const char *work(const char *s)
{
	double v;
	const char *why = calc(s, &v);

	if (why)
		return why;
	fmt_num(v, out);
	return out;
}

CHECK_TEST(precedence)
{
	CHECK_EQ_STR(work("12*7+3"), "87");
	CHECK_EQ_STR(work("2^3^2"), "512");
	CHECK_EQ_STR(work("-2^2"), "-4");
	CHECK_EQ_STR(work("2**10"), "1024");
}

CHECK_TEST(postfix_and_names)
{
	CHECK_EQ_STR(work("15%*80"), "12");
	CHECK_EQ_STR(work("5!"), "120");
	CHECK_EQ_STR(work("7 mod 3"), "1");
	CHECK_EQ_STR(work("2pi"), "6.28318530717959");
	CHECK_EQ_STR(work("3(4+5)"), "27");
	CHECK_EQ_STR(work("(1+2"), "3");
}

CHECK_TEST(bases_and_exponents)
{
	CHECK_EQ_STR(work("0xff+1"), "256");
	CHECK_EQ_STR(work("0b101"), "5");
	CHECK_EQ_STR(work("1e20*3"), "3e+20");
	CHECK_EQ_STR(work("2/3*1e-9"), "6.66666666666667e-10");
	CHECK_EQ_STR(work("123456789012345"), "123456789012345");
	CHECK_EQ_STR(work("0.1+0.2"), "0.3");
	CHECK_EQ_STR(work("1/3"), "0.333333333333333");
}

CHECK_TEST(functions)
{
	sums_rad = 0;
	CHECK_EQ_STR(work("sin 30"), "0.5");
	CHECK_EQ_STR(work("cos 90"), "0");
	CHECK_EQ_STR(work("tan 45"), "1");
	CHECK_EQ_STR(work("log 1000"), "3");
	CHECK_EQ_STR(work("ln e"), "1");
	CHECK_EQ_STR(work("log2 1024"), "10");
	CHECK_EQ_STR(work("sqrt 16"), "4");
	sums_rad = 1;
	CHECK_EQ_STR(work("sin(pi/2)"), "1");
	sums_rad = 0;
}

CHECK_TEST(errors)
{
	sums_have_ans = 0;
	CHECK_EQ_STR(work("10/0"), "Division by zero");
	CHECK_EQ_STR(work("foo+1"), "Unknown name");
	CHECK_EQ_STR(work("1+2)"), "A \"(\" is missing");
	CHECK_EQ_STR(work("1+"), "The sum ends too soon");
	CHECK_EQ_STR(work("sqrt(-1)"), "No square root of a negative number");
	CHECK_EQ_STR(work("171!"), "Too large");
	CHECK_EQ_STR(work("ans"), "No answer yet for ans");
	CHECK_EQ_STR(work("tan 90"), "No tangent of 90 degrees");
	CHECK_EQ_STR(work(""), "Nothing to work out");
}

CHECK_TEST(bases_shown)
{
	char b[96];

	fmt_bases(255, b);
	CHECK_EQ_STR(b, "HEX  0xff    BIN  1111 1111");
	fmt_bases(2.5, b);
	CHECK_EQ_STR(b, "");
	fmt_bases(5, b);
	CHECK_EQ_STR(b, "");
}

CHECK_MAIN
