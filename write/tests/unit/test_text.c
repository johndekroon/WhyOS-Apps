/* SPDX-License-Identifier: MIT */
/* The text buffer (src/text.c): lines, editing, undo and redo, saved state. */
#include "t.h"
#include "text.h"

static struct text T;

static void set(const char *s)
{
	text_free(&T);
	text_init(&T);
	EQ(text_set(&T, s, strlen(s)), 0);
}

/* the whole text as a string (static buffer) */
static const char *all(void)
{
	static char buf[4096];
	size_t n;
	char *s = text_get(&T, &n);

	snprintf(buf, sizeof(buf), "%.*s", (int)n, s);
	free(s);
	return buf;
}

static struct pos P(int line, int col)
{
	struct pos p = { line, col };

	return p;
}

TEST(set_splits_lines)
{
	set("one\ntwo\n\nfour");
	EQ(T.n, 4);
	STREQ(T.l[0].s, "one");
	STREQ(T.l[2].s, "");
	STREQ(T.l[3].s, "four");
	EQ(T.l[3].len, 4);
}

TEST(round_trip_keeps_line_ends)
{
	set("a\nb\n");
	EQ(T.n, 2);			/* the final newline is no extra line */
	STREQ(all(), "a\nb\n");
	set("a\r\nb\r\n");
	EQ(T.crlf, 1);
	STREQ(T.l[0].s, "a");
	STREQ(all(), "a\r\nb\r\n");
	set("no newline");
	STREQ(all(), "no newline");
	set("");
	EQ(T.n, 1);
	STREQ(all(), "");
}

TEST(insert_in_a_line)
{
	struct pos end;

	set("held");
	EQ(text_insert(&T, P(0, 2), "llo wor", 7, &end), 0);
	STREQ(all(), "hello world");
	EQ(end.line, 0);
	EQ(end.col, 9);
}

TEST(insert_lines)
{
	struct pos end;

	set("first\nlast");
	EQ(text_insert(&T, P(0, 3), "A\nB\nC", 5, &end), 0);
	STREQ(all(), "firA\nB\nCst\nlast");
	EQ(T.n, 4);
	EQ(end.line, 2);
	EQ(end.col, 1);
	EQ(text_insert(&T, P(3, 4), "\n", 1, &end), 0);
	STREQ(all(), "firA\nB\nCst\nlast\n");	/* an empty last line */
	EQ(T.n, 5);
	EQ(end.line, 4);
	EQ(end.col, 0);
}

TEST(delete_in_and_across_lines)
{
	set("hello world\nsecond\nthird");
	EQ(text_delete(&T, P(0, 5), P(0, 11)), 0);
	STREQ(all(), "hello\nsecond\nthird");
	EQ(text_delete(&T, P(0, 3), P(2, 2)), 0);
	STREQ(all(), "helird");
	EQ(T.n, 1);
	EQ(text_delete(&T, P(0, 0), P(0, 0)), 0);	/* nothing */
	STREQ(all(), "helird");
}

TEST(copy_a_range)
{
	size_t n;
	char *s;

	set("abc\ndef\nghi");
	s = text_copy(&T, P(0, 1), P(2, 1), &n);
	EQ(n, 8);
	OK(!memcmp(s, "bc\ndef\ng", 8));
	free(s);
	s = text_copy(&T, P(1, 1), P(1, 1), &n);
	EQ(n, 0);
	free(s);
}

TEST(positions_compare)
{
	OK(pos_cmp(P(0, 5), P(1, 0)) < 0);
	OK(pos_cmp(P(2, 1), P(2, 0)) > 0);
	EQ(pos_cmp(P(3, 3), P(3, 3)), 0);
}

TEST(undo_and_redo_restore_text_and_caret)
{
	struct pos c, end;

	set("abc");
	text_begin(&T, G_OTHER, P(0, 3));
	text_insert(&T, P(0, 3), "\ndef", 4, &end);
	text_end(&T, end);
	text_begin(&T, G_OTHER, end);
	text_delete(&T, P(0, 1), P(1, 1));
	text_end(&T, P(0, 1));
	STREQ(all(), "aef");
	EQ(text_undo(&T, &c), 1);
	STREQ(all(), "abc\ndef");
	EQ(c.line, 1);			/* the caret before the delete */
	EQ(c.col, 3);
	EQ(text_undo(&T, &c), 1);
	STREQ(all(), "abc");
	EQ(c.col, 3);
	EQ(text_undo(&T, &c), 0);	/* nothing left */
	EQ(text_redo(&T, &c), 1);
	STREQ(all(), "abc\ndef");
	EQ(c.line, 1);			/* the caret after the insert */
	EQ(text_redo(&T, &c), 1);
	STREQ(all(), "aef");
	EQ(text_redo(&T, &c), 0);
}

TEST(a_new_edit_drops_the_redo)
{
	struct pos c, end;

	set("x");
	text_begin(&T, G_OTHER, P(0, 1));
	text_insert(&T, P(0, 1), "y", 1, &end);
	text_end(&T, end);
	text_undo(&T, &c);
	text_begin(&T, G_OTHER, P(0, 1));
	text_insert(&T, P(0, 1), "z", 1, &end);
	text_end(&T, end);
	EQ(text_redo(&T, &c), 0);
	STREQ(all(), "xz");
}

TEST(typing_merges_into_one_step)
{
	struct pos c = P(0, 0), end;

	set("");
	for (const char *s = "hello"; *s; s++) {
		text_begin(&T, G_TYPE, c);
		text_insert(&T, c, s, 1, &end);
		text_end(&T, end);
		c = end;
	}
	text_begin(&T, G_SPACE, c);		/* a space is a step of its own */
	text_insert(&T, c, " ", 1, &end);
	text_end(&T, end);
	c = end;
	text_begin(&T, G_TYPE, c);
	text_insert(&T, c, "w", 1, &end);
	text_end(&T, end);
	STREQ(all(), "hello w");
	text_undo(&T, &c);
	STREQ(all(), "hello ");
	text_undo(&T, &c);
	STREQ(all(), "hello");
	text_undo(&T, &c);
	STREQ(all(), "");
}

TEST(typing_elsewhere_is_a_new_step)
{
	struct pos c;
	struct pos end;

	set("ab");
	text_begin(&T, G_TYPE, P(0, 0));
	text_insert(&T, P(0, 0), "x", 1, &end);
	text_end(&T, end);
	text_begin(&T, G_TYPE, P(0, 3));	/* the caret moved */
	text_insert(&T, P(0, 3), "y", 1, &end);
	text_end(&T, end);
	text_undo(&T, &c);
	STREQ(all(), "xab");
}

TEST(break_ends_a_merge)
{
	struct pos c, end;

	set("");
	text_begin(&T, G_TYPE, P(0, 0));
	text_insert(&T, P(0, 0), "a", 1, &end);
	text_end(&T, end);
	text_break(&T);
	text_begin(&T, G_TYPE, end);
	text_insert(&T, end, "b", 1, &end);
	text_end(&T, end);
	text_undo(&T, &c);
	STREQ(all(), "a");
}

TEST(dirty_follows_the_saved_state)
{
	struct pos c, end;

	set("doc");
	EQ(text_dirty(&T), 0);
	text_begin(&T, G_OTHER, P(0, 3));
	text_insert(&T, P(0, 3), "!", 1, &end);
	text_end(&T, end);
	EQ(text_dirty(&T), 1);
	text_undo(&T, &c);
	EQ(text_dirty(&T), 0);		/* back where the file is */
	text_redo(&T, &c);
	text_saved(&T);
	EQ(text_dirty(&T), 0);
	text_undo(&T, &c);
	EQ(text_dirty(&T), 1);
}

TEST(undo_keeps_a_hundred_steps)
{
	struct pos c, end;
	int n = 0;

	set("");
	for (int i = 0; i < 130; i++) {
		text_begin(&T, G_OTHER, P(0, i));
		text_insert(&T, P(0, i), "x", 1, &end);
		text_end(&T, end);
	}
	while (text_undo(&T, &c))
		n++;
	EQ(n, UNDO_STEPS);
	EQ(T.l[0].len, 130 - UNDO_STEPS);
	EQ(text_dirty(&T), 1);		/* the saved state is gone for good */
}

TEST(changes_reset_the_layout_cache)
{
	struct pos end;

	set("a\nb\nc");
	for (int i = 0; i < T.n; i++)
		T.l[i].h = 10;
	T.changed = -1;
	text_insert(&T, P(1, 1), "x", 1, &end);
	EQ(T.l[0].h, 10);
	EQ(T.l[1].h, -1);
	EQ(T.l[2].h, 10);
	EQ(T.changed, 1);
	text_insert(&T, P(0, 0), "y\n", 2, &end);
	EQ(T.l[0].h, -1);
	EQ(T.l[1].h, -1);		/* new line */
	EQ(T.changed, 0);
}

TEST(size_counts_bytes_and_newlines)
{
	set("ab\ncd");
	EQ(text_size(&T), 5);
}
