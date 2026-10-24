/* SPDX-License-Identifier: MIT */
/*
 * Write: laying out lines into rows (design Main, Source, Code, Fullscreen).
 *
 * Each source line becomes one or more rows, wrapped at spaces (or anywhere
 * in a word longer than the row) to the text area's width, in the font and
 * line height of its block. Heights are cached in the lines (tline.h) and
 * recomputed only for lines that changed or whose code state did; lay_ensure
 * keeps the top of every line in ys.
 *
 * Font metrics come from a callback, so the tests use simple fake ones.
 */
#ifndef WRITE_LAYOUT_H
#define WRITE_LAYOUT_H

#include "edit.h"
#include "syntax.h"

enum font {
	FONT_UI24, FONT_UI24B, FONT_UI36B, FONT_UI28B, FONT_UI20, FONT_UI20B,
	FONT_MONO20, FONT_MONO18, FONT_MONO16, FONT_MONO14, FONT_COUNT
};

struct lay {
	int (*adv16)(void *ctx, int font, unsigned cp);	/* a character's advance in 1/16 px */
	void *ctx;
	enum mode mode;
	enum lang lang;		/* M_CODE: for the syntax state */
	int width;		/* the text area */
	int small;		/* the small text size (settings) */
	int full;		/* full screen: body text gets 34 px lines */
	int *ys;		/* the top of each line, and the end */
	int cap, n, total;
	unsigned sig;		/* the settings the cached heights were made with */
};

struct lstyle {
	int font, bold_font;	/* regular and bold */
	int lh, after;		/* line height, space after the line */
	int indent;		/* rows start this far in (lists, quotes, code) */
	int mono;
};

struct row { int k0, k1, y, h, x; };

#define ROWS_MAX	512	/* rows of one line; past them the line is cut */

void lay_init(struct lay *L);
void lay_free(struct lay *L);
void lay_style(const struct lay *L, enum blk kind, int in_code, struct lstyle *s);
int lay_font_of(const struct lstyle *s, int style);
const char *lay_text(struct lay *L, struct ed *e, int line, int state, int *n, enum blk *kind,
		     const unsigned char **st);
int lay_rows(struct lay *L, struct ed *e, int line, int state, struct row *rows, int max, int *h);
void lay_reset(struct lay *L, struct ed *e);
void lay_ensure(struct lay *L, struct ed *e);
int lay_line_at(const struct lay *L, int y);
int lay_x(struct lay *L, struct ed *e, int line, const struct row *r, int k);
int lay_k(struct lay *L, struct ed *e, int line, const struct row *r, int x);
int lay_caret(struct lay *L, struct ed *e, struct pos p, int *x, int *y, int *h);
struct pos lay_vmove(struct lay *L, struct ed *e, int dir, int *want_x);
int lay_state_in(const struct ed *e, int line);	/* the cached state entering line */

#endif
