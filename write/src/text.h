/* SPDX-License-Identifier: MIT */
/*
 * Write: the text buffer. A document is an array of lines without their
 * line ends; a position is a line and a byte offset in it. Every change
 * goes through text_insert and text_delete, which keep the undo history.
 *
 * Changes are grouped into undo steps by text_begin/text_end; typing
 * (G_TYPE, G_SPACE) merges into the step before when it continues it at
 * the same caret. UNDO_STEPS steps are kept.
 *
 * Allocation failures are returned (-1), not fatal: the badge has no
 * swap, and losing the user's text over one keystroke is worse than
 * refusing the keystroke.
 */
#ifndef WRITE_TEXT_H
#define WRITE_TEXT_H

#include <stddef.h>

#define UNDO_STEPS	100
#define UNDO_BYTES	(512 * 1024)	/* and at most this much text in them */

struct pos { int line, col; };

struct tline {
	char *s;		/* NUL-terminated, len bytes */
	int len, cap;
	int h;			/* the layout's height of this line, -1: unknown */
	int words;		/* its words in the rich view, -1: unknown */
};

enum group { G_OTHER, G_TYPE, G_SPACE, G_DELETE };

struct undo_op;

struct text {
	struct tline *l;
	int n, cap;
	int crlf;		/* the file had \r\n line ends: written back so */
	int final_nl;		/* it ended with a line end */
	int changed;		/* first line changed since the caller reset it to -1 */

	struct undo_op *ops;	/* applied ops, then undone ones (redo) */
	int n_ops, cap_ops, n_done;
	unsigned seq, cur, saved;	/* step ids: the last applied, the saved one */
	int group_kind;
	struct pos group_end;	/* where the last step left the caret */
	struct pos group_before;
	int open;		/* inside text_begin/text_end */
};

int pos_cmp(struct pos a, struct pos b);

void text_init(struct text *t);
void text_free(struct text *t);
int text_set(struct text *t, const char *data, size_t len);	/* clears undo */
char *text_get(const struct text *t, size_t *len);	/* malloc'd, line ends as loaded */
size_t text_size(const struct text *t);

int text_insert(struct text *t, struct pos at, const char *s, size_t n, struct pos *end);
int text_delete(struct text *t, struct pos a, struct pos b);
char *text_copy(const struct text *t, struct pos a, struct pos b, size_t *n);

void text_begin(struct text *t, enum group kind, struct pos caret);
void text_end(struct text *t, struct pos caret);
void text_break(struct text *t);	/* the next typing starts a new step */
int text_undo(struct text *t, struct pos *caret);
int text_redo(struct text *t, struct pos *caret);
int text_dirty(const struct text *t);
void text_saved(struct text *t);

#endif
