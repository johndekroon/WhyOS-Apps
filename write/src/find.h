/* SPDX-License-Identifier: MIT */
/*
 * Write: find and replace (design/write/README.md §8). Matches are per line,
 * in the text the caret walks over: the visible text in M_RICH (no
 * markers), the source otherwise. Case is folded for ASCII only.
 */
#ifndef WRITE_FIND_H
#define WRITE_FIND_H

#include "edit.h"

#define FIND_MAX	9999	/* matches kept; the count says "9999+" past it */

struct match { int line, k0, k1; };

struct find {
	char q[128];
	char repl[128];
	int match_case, whole_word;
	struct match *m;
	int n, cap, more;
	int cur;		/* the current match, -1: none */
};

void find_init(struct find *f);
void find_free(struct find *f);
int find_run(struct find *f, struct ed *e);	/* every match again; keeps cur in range */
void find_nearest(struct find *f, struct ed *e);	/* cur: the first at or after the caret */
void find_step(struct find *f, struct ed *e, int dir);	/* next (1) or previous (-1), selected */
void find_select(struct find *f, struct ed *e);
void find_count(const struct find *f, char *out, int n);	/* "2 of 3", "no matches" */
void find_case(char *out, int n, const char *match, const char *repl, int match_case);
int find_replace(struct find *f, struct ed *e, const char *repl);
int find_replace_all(struct find *f, struct ed *e, const char *repl);

#endif
