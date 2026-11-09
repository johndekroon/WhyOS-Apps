// SPDX-License-Identifier: MIT
/* sums.h — the calculator's sums (sums.c) */
#ifndef SUMS_H
#define SUMS_H

extern int sums_rad;		/* angles in radians, not degrees */
extern double sums_ans;		/* the last result: "ans" */
extern int sums_have_ans;

/* The sum s worked out into *v; 0, or what is wrong. */
const char *calc(const char *s, double *v);

/* v with 15 significant digits: plain from 1e-6 to 1e15, else 1.5e+20 */
void fmt_num(double v, char *out);

/* A whole number in hexadecimal and (below 2^32) binary, or "" */
void fmt_bases(double v, char *out);

#endif
