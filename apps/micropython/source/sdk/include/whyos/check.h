/* SPDX-License-Identifier: MIT */
/*
 * whyos/check.h: unit tests for an app's own logic, on the computer,
 * without its screen. A test file includes this and the app's logic
 * files, not its main():
 *
 *	#include "whyos/check.h"
 *	#include "../../sums.h"
 *
 *	CHECK_TEST(adds)
 *	{
 *		CHECK_EQ_STR(work("1+2"), "3");
 *	}
 *
 *	CHECK_MAIN
 *
 * Built and run by the app's tests (whyos_testing.unit()); a failed check
 * prints where and what, and the run exits 1.
 */
#pragma once

#include <stdio.h>
#include <string.h>

struct check_test { const char *name; void (*fn)(void); };

static struct check_test check_tests[256];
static int check_n, check_failed, check_this;

#define CHECK_TEST(name)							\
	static void check_##name(void);						\
	__attribute__((constructor)) static void check_add_##name(void)	\
	{									\
		check_tests[check_n++] = (struct check_test){ #name, check_##name }; \
	}									\
	static void check_##name(void)

#define CHECK(c)								\
	do {									\
		if (!(c)) {							\
			printf("%s:%d: %s\n", __FILE__, __LINE__, #c);		\
			check_this = 1;						\
		}								\
	} while (0)

#define CHECK_EQ(a, b)								\
	do {									\
		long long check_a = (a), check_b = (b);				\
		if (check_a != check_b) {					\
			printf("%s:%d: %s is %lld, not %lld\n", __FILE__, __LINE__, #a, check_a, check_b); \
			check_this = 1;						\
		}								\
	} while (0)

#define CHECK_EQ_STR(a, b)							\
	do {									\
		const char *check_a = (a), *check_b = (b);			\
		if (!check_a || !check_b || strcmp(check_a, check_b)) {		\
			printf("%s:%d: %s is \"%s\", not \"%s\"\n", __FILE__, __LINE__, #a, \
			       check_a ? check_a : "(null)", check_b ? check_b : "(null)"); \
			check_this = 1;						\
		}								\
	} while (0)

#define CHECK_MAIN								\
	int main(void)								\
	{									\
		for (int i = 0; i < check_n; i++) {				\
			check_this = 0;						\
			check_tests[i].fn();					\
			printf("%s %s\n", check_this ? "FAIL" : "ok  ", check_tests[i].name); \
			check_failed += check_this;				\
		}								\
		printf("%d of %d failed\n", check_failed, check_n);		\
		return check_failed != 0;					\
	}
