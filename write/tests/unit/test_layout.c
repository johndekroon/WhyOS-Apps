/* SPDX-License-Identifier: MIT */
/* Layout (src/layout.c) with fake metrics: every character 10 px, bold too. */
#include "t.h"
#include "layout.h"

static struct ed E;
static struct lay L;
static int measured;

static int adv(void *ctx, int font, unsigned cp)
{
	(void)ctx;
	(void)font;
	(void)cp;
	measured++;
	return 160;
}

static void load(enum mode mode, const char *s, int width)
{
	ed_free(&E);
	ed_init(&E);
	EQ(ed_load(&E, s, strlen(s), mode), 0);
	lay_free(&L);
	lay_init(&L);
	L.adv16 = adv;
	L.mode = mode;
	L.width = width;
	lay_ensure(&L, &E);
}

static int rows(int line, struct row *r, int *h)
{
	return lay_rows(&L, &E, line, lay_state_in(&E, line), r, ROWS_MAX, h);
}

TEST(wraps_at_spaces)
{
	struct row r[8];
	int h;

	load(M_RICH, "aaaa bbbb cccc", 100);
	EQ(rows(0, r, &h), 2);
	EQ(r[0].k0, 0);
	EQ(r[0].k1, 10);		/* the space stays on the first row */
	EQ(r[1].k0, 10);
	EQ(r[1].k1, 14);
	EQ(r[1].y, 32);
	EQ(h, 64);
}

TEST(breaks_long_words)
{
	struct row r[8];
	int h;

	load(M_RICH, "xxxxxxxxxxxxxxxxxxxxxxxxx", 100);
	EQ(rows(0, r, &h), 3);
	EQ(r[0].k1, 10);
	EQ(r[2].k1, 25);
}

TEST(block_heights)
{
	struct row r[8];
	int h;

	load(M_RICH, "# Big\n## Mid\n### Small\ntext\n\n- item\n> q", 600);
	rows(0, r, &h);
	EQ(h, 44 + 6);
	rows(1, r, &h);
	EQ(h, 36 + 4);
	rows(2, r, &h);
	EQ(h, 32 + 4);
	rows(3, r, &h);
	EQ(h, 32);
	rows(4, r, &h);
	EQ(h, 18);			/* a blank line is the space between paragraphs */
	rows(5, r, &h);
	EQ(r[0].x, 32);			/* list text is indented */
	rows(6, r, &h);
	EQ(r[0].x, 32);
}

TEST(indent_narrows_the_row)
{
	struct row r[8];
	int h;

	load(M_RICH, "- aaaa bbbb", 100);	/* 68 px left for the text */
	EQ(rows(0, r, &h), 2);
	EQ(r[0].k1, 5);
}

TEST(code_blocks_by_fence)
{
	struct row r[8];
	int h;

	load(M_RICH, "```\n# not a heading\n```\n# heading", 600);
	rows(0, r, &h);
	EQ(h, 24);			/* the fence line */
	rows(1, r, &h);
	EQ(h, 27);			/* code, not a heading */
	rows(3, r, &h);
	EQ(h, 44 + 6);
	EQ(L.total, 24 + 27 + 24 + 50);
	EQ(L.ys[3], 75);
}

TEST(source_and_code_modes)
{
	struct row r[8];
	int h;

	load(M_SOURCE, "# Head\ntext", 600);
	rows(0, r, &h);
	EQ(h, 28);
	EQ(r[0].k1, 6);			/* the marker is text here */
	load(M_CODE, "def f():\n    pass", 600);
	rows(1, r, &h);
	EQ(h, 27);
	EQ(L.total, 54);
}

TEST(small_text_and_full_screen)
{
	struct row r[8];
	int h;

	load(M_RICH, "# Big\ntext", 600);
	L.small = 1;
	lay_reset(&L, &E);
	lay_ensure(&L, &E);
	rows(0, r, &h);
	EQ(h, 36 + 6);
	rows(1, r, &h);
	EQ(h, 28);
	L.small = 0;
	L.full = 1;
	lay_reset(&L, &E);
	lay_ensure(&L, &E);
	rows(1, r, &h);
	EQ(h, 34);
}

TEST(only_changed_lines_are_measured_again)
{
	load(M_RICH, "one\ntwo\nthree", 600);
	measured = 0;
	lay_ensure(&L, &E);
	EQ(measured, 0);
	ed_caret_vis(&E, 1, 3, 0);
	ed_type(&E, "s", 1);
	lay_ensure(&L, &E);
	EQ(measured, 4);		/* "twos" */
	EQ(L.total, 96);
}

TEST(a_new_fence_changes_the_lines_after_it)
{
	load(M_RICH, "a\n# b\nc", 600);
	EQ(L.total, 32 + 50 + 32);
	ed_set_caret(&E, (struct pos){ 0, 0 }, 0);
	ed_paste(&E, "```\n", 4);
	lay_ensure(&L, &E);
	EQ(L.n, 4);
	EQ(L.total, 24 + 27 * 3);	/* everything after the fence is code */
}

TEST(lines_at_heights)
{
	load(M_RICH, "a\nb\nc", 600);
	EQ(lay_line_at(&L, 0), 0);
	EQ(lay_line_at(&L, 31), 0);
	EQ(lay_line_at(&L, 32), 1);
	EQ(lay_line_at(&L, 95), 2);
	EQ(lay_line_at(&L, 9999), 2);
	EQ(lay_line_at(&L, -5), 0);
}

TEST(x_and_k)
{
	struct row r[8];
	int h;

	load(M_RICH, "- abc", 600);
	rows(0, r, &h);
	EQ(lay_x(&L, &E, 0, &r[0], 0), 32);
	EQ(lay_x(&L, &E, 0, &r[0], 2), 52);
	EQ(lay_k(&L, &E, 0, &r[0], 0), 0);
	EQ(lay_k(&L, &E, 0, &r[0], 54), 2);
	EQ(lay_k(&L, &E, 0, &r[0], 58), 3);	/* nearer the next boundary */
	EQ(lay_k(&L, &E, 0, &r[0], 500), 3);
}

TEST(caret_position)
{
	int x, y, h;

	load(M_RICH, "one\naaaa bbbb cccc", 100);
	ed_caret_vis(&E, 1, 12, 0);
	lay_caret(&L, &E, E.caret, &x, &y, &h);
	EQ(y, 32 + 32);			/* the second row of line 1 */
	EQ(x, 20);
	EQ(h, 32);
	ed_caret_vis(&E, 1, 10, 0);	/* the start of a wrapped row */
	lay_caret(&L, &E, E.caret, &x, &y, &h);
	EQ(y, 64);
	EQ(x, 0);
}

TEST(up_and_down_by_rows)
{
	int want = -1;
	struct pos p;

	load(M_RICH, "aaaa bbbb cccc\nxy", 100);
	ed_caret_vis(&E, 0, 2, 0);
	p = lay_vmove(&L, &E, 1, &want);
	EQ(p.line, 0);
	EQ(ed_vis_of(&E, p), 12);	/* the same x on the next row */
	ed_set_caret(&E, p, 0);
	p = lay_vmove(&L, &E, 1, &want);
	EQ(p.line, 1);
	EQ(ed_vis_of(&E, p), 2);	/* as far as it goes */
	ed_set_caret(&E, p, 0);
	p = lay_vmove(&L, &E, -1, &want);
	EQ(p.line, 0);
	EQ(ed_vis_of(&E, p), 12);	/* the x it wanted, not the short line's */
	ed_set_caret(&E, p, 0);
	ed_caret_vis(&E, 0, 2, 0);
	want = -1;
	p = lay_vmove(&L, &E, -1, &want);	/* on the first row: the start */
	EQ(p.line, 0);
	EQ(ed_vis_of(&E, p), 0);
}
