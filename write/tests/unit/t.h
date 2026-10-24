/* SPDX-License-Identifier: MIT */
/*
 * A small test harness for Write's modules: each test_*.c is a program of
 * TEST(name) functions; main() runs them all and fails when one check did.
 *
 *   EQ(a, b)        integers
 *   STREQ(a, b)     strings (NULL compares as "(null)")
 *   OK(cond)        anything else
 */
#ifndef WRITE_T_H
#define WRITE_T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct t_case { const char *name; void (*fn)(void); };
static struct t_case t_cases[256];
static int t_n, t_failed, t_checks, t_case_failed;

#define TEST(f)								\
	static void f(void);						\
	__attribute__((constructor)) static void reg_##f(void)		\
	{ t_cases[t_n].name = #f; t_cases[t_n++].fn = f; }		\
	static void f(void)

#define T_FAIL(...) do {						\
	fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);			\
	fprintf(stderr, __VA_ARGS__);					\
	fputc('\n', stderr);						\
	t_case_failed = 1;						\
} while (0)

#define OK(c) do { t_checks++; if (!(c)) T_FAIL("not true: %s", #c); } while (0)
#define EQ(a, b) do {							\
	long long t_a = (long long)(a), t_b = (long long)(b);		\
	t_checks++;							\
	if (t_a != t_b)							\
		T_FAIL("%s == %lld, expected %lld (%s)", #a, t_a, t_b, #b); \
} while (0)
#define STREQ(a, b) do {						\
	const char *t_a = (a), *t_b = (b);				\
	t_checks++;							\
	if (!t_a || !t_b ? t_a != t_b : strcmp(t_a, t_b) != 0)		\
		T_FAIL("%s == \"%s\", expected \"%s\"", #a,		\
		       t_a ? t_a : "(null)", t_b ? t_b : "(null)");	\
} while (0)

int main(int argc, char **argv)
{
	for (int i = 0; i < t_n; i++) {
		if (argc > 1 && !strstr(t_cases[i].name, argv[1]))
			continue;
		t_case_failed = 0;
		t_cases[i].fn();
		if (t_case_failed) {
			fprintf(stderr, "FAIL %s\n", t_cases[i].name);
			t_failed++;
		}
	}
	fprintf(stderr, "%s: %d tests, %d checks, %d failed\n", argv[0], t_n, t_checks, t_failed);
	return t_failed != 0;
}

#endif
