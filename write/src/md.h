/* SPDX-License-Identifier: MIT */
/*
 * Write: one markdown line, as the rich view shows it (design/write/README.md §3).
 *
 * The source line is the truth. md_parse finds the line's block (heading,
 * list item, quote, a code block's line) and its visible text: the line
 * without the block marker and without the inline markers Write knows
 * (**bold**, *italic*, ***both***, <u>underline</u>), each visible byte
 * with its style and its offset in the source. Everything else (links,
 * tables, a lone star) is visible text, so it is never rewritten.
 *
 * md_inline/md_line write visible text and styles back as markdown, for
 * the lines whose styles the user changed; what they write reads back the
 * same (tests/unit/test_md.c checks it on random text).
 */
#ifndef WRITE_MD_H
#define WRITE_MD_H

enum blk { B_BLANK, B_BODY, B_H1, B_H2, B_H3, B_BULLET, B_NUMBER, B_QUOTE, B_FENCE, B_CODE };

enum { ST_B = 1, ST_I = 2, ST_U = 4 };

struct mdline {
	enum blk kind;
	int prefix;		/* bytes of the block marker ("## ", "- ", "12. ") */
	int number;		/* B_NUMBER: its number */
	int n;			/* visible bytes */
	int len;		/* the source line's length */
	char *vis;		/* the visible text, NUL-terminated */
	unsigned char *st;	/* the style of each visible byte */
	int *src;		/* the source offset of each visible byte */
	unsigned char *w;	/* ... and its source bytes (2 for an escape) */
	int cap;
};

void md_init(struct mdline *m);
void md_free(struct mdline *m);

int md_is_fence(const char *s, int len);
enum blk md_block(const char *s, int len, int in_code, int *prefix, int *number);
int md_parse(struct mdline *m, const char *s, int len, int in_code);

int md_src(const struct mdline *m, int k);	/* caret k (0..n) to a source offset */
int md_vis(const struct mdline *m, int col);	/* a source offset to the caret */

const char *md_prefix(enum blk kind, int number, char buf[16]);
void md_normalize(const char *vis, const unsigned char *st, int n, unsigned char *out);
int md_inline(const char *vis, const unsigned char *st, int n, char *out, int cap);
int md_line_p(const char *prefix, enum blk kind, const char *vis, const unsigned char *st, int n,
	      char *out, int cap);
int md_line(enum blk kind, int number, const char *vis, const unsigned char *st, int n,
	    char *out, int cap);

#endif
