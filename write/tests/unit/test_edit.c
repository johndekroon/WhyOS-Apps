/* SPDX-License-Identifier: MIT */
/* Editing (src/edit.c): typing, Enter and Backspace, styles, paragraphs, the clipboard. */
#include "t.h"
#include "edit.h"

static struct ed E;

static void load(enum mode mode, const char *s)
{
	ed_free(&E);
	ed_init(&E);
	EQ(ed_load(&E, s, strlen(s), mode), 0);
}

static const char *src(void)
{
	static char buf[4096];
	size_t n;
	char *s = text_get(&E.t, &n);

	snprintf(buf, sizeof(buf), "%.*s", (int)n, s);
	free(s);
	return buf;
}

/* the caret at visible offset k of a line (rich), or byte k (source, code) */
static void at(int line, int k)
{
	ed_caret_vis(&E, line, k, 0);
}

static void sel(int l0, int k0, int l1, int k1)
{
	ed_caret_vis(&E, l0, k0, 0);
	ed_caret_vis(&E, l1, k1, 1);
}

static void type(const char *s)
{
	for (; *s; s++)
		EQ(ed_type(&E, s, 1), 0);
}

static int caret_k(void)
{
	return ed_vis_of(&E, E.caret);
}

TEST(typing_inserts)
{
	load(M_RICH, "Bring badge");
	at(0, 6);
	type("the ");
	STREQ(src(), "Bring the badge");
	EQ(caret_k(), 10);
}

TEST(typing_a_block_marker_converts_the_line)
{
	load(M_RICH, "Packing list");
	at(0, 0);
	type("## ");
	STREQ(src(), "## Packing list");
	EQ(caret_k(), 0);			/* the marker is hidden now */
	EQ(ed_block_at(&E), B_H2);
	load(M_RICH, "");
	type("- milk");
	STREQ(src(), "- milk");
	EQ(ed_block_at(&E), B_BULLET);
}

TEST(typing_closing_markers_makes_a_style)
{
	load(M_RICH, "");
	type("a **bold** b");
	STREQ(src(), "a **bold** b");
	EQ(caret_k(), 8);			/* "a bold b" */
	at(0, 4);
	EQ(ed_style_at(&E), ST_B);
}

TEST(typing_continues_the_style_before)
{
	load(M_RICH, "**ab** c");
	at(0, 2);
	type("x");
	STREQ(src(), "**abx** c");
	at(0, 0);
	type("y");
	STREQ(src(), "**yabx** c");		/* at the start: the first letter's */
}

TEST(ctrl_b_without_selection_styles_what_comes)
{
	load(M_RICH, "go ");
	at(0, 3);
	EQ(ed_toggle(&E, ST_B, 0), 0);
	EQ(ed_style_at(&E), ST_B);
	type("now");
	STREQ(src(), "go **now**");
	ed_toggle(&E, ST_B, 0);
	type(" then");
	STREQ(src(), "go **now** then");
}

TEST(toggle_on_a_selection)
{
	load(M_RICH, "Bring fully charged now");
	sel(0, 6, 0, 19);
	ed_toggle(&E, ST_B, 0);
	STREQ(src(), "Bring **fully charged** now");
	EQ(E.sel, 1);
	EQ(ed_vis_of(&E, E.anchor), 6);	/* the selection stays on the same text */
	EQ(caret_k(), 19);
	ed_toggle(&E, ST_B, 0);
	STREQ(src(), "Bring fully charged now");
}

TEST(toggle_on_a_mixed_selection_sets_it)
{
	load(M_RICH, "a **b** c");
	sel(0, 0, 0, 5);
	ed_toggle(&E, ST_B, 0);
	STREQ(src(), "**a b c**");
}

TEST(toggle_across_lines)
{
	load(M_RICH, "# one\ntwo");
	sel(0, 1, 1, 2);
	ed_toggle(&E, ST_I, 0);
	STREQ(src(), "# o*ne*\n*tw*o");
}

TEST(toggle_the_word_at_the_caret)
{
	load(M_RICH, "pack a spare cable");
	at(0, 9);
	ed_toggle(&E, ST_U, 1);
	STREQ(src(), "pack a <u>spare</u> cable");
}

TEST(backspace_removes_an_emptied_style)
{
	load(M_RICH, "a **x** b");
	at(0, 3);
	EQ(ed_backspace(&E), 0);
	STREQ(src(), "a  b");
	EQ(caret_k(), 2);
}

TEST(backspace_at_the_start_drops_the_marker)
{
	load(M_RICH, "- item");
	at(0, 0);
	ed_backspace(&E);
	STREQ(src(), "item");
	load(M_RICH, "## Head");
	at(0, 0);
	ed_backspace(&E);
	STREQ(src(), "Head");
}

TEST(backspace_at_the_start_joins_lines)
{
	load(M_RICH, "one\n**two**");
	at(1, 0);
	ed_backspace(&E);
	STREQ(src(), "one**two**");
	EQ(caret_k(), 3);
	load(M_RICH, "one\n\n");		/* an empty line, then the final newline */
	at(1, 0);
	ed_backspace(&E);
	STREQ(src(), "one\n");
}

TEST(delete_forward)
{
	load(M_RICH, "ab\ncd");
	at(0, 2);
	EQ(ed_delete(&E), 0);
	STREQ(src(), "abcd");
	at(0, 0);
	ed_delete(&E);
	STREQ(src(), "bcd");
}

TEST(enter_continues_a_list)
{
	load(M_RICH, "- one");
	at(0, 3);
	EQ(ed_newline(&E), 0);
	STREQ(src(), "- one\n- ");
	EQ(E.caret.line, 1);
	EQ(caret_k(), 0);
	ed_newline(&E);				/* on an empty item: the list ends */
	STREQ(src(), "- one\n");
	EQ(E.caret.line, 1);
}

TEST(enter_numbers_on)
{
	load(M_RICH, "1. a\n2. b");
	at(0, 1);
	ed_newline(&E);
	STREQ(src(), "1. a\n2. \n3. b");
	type("x");
	STREQ(src(), "1. a\n2. x\n3. b");
}

TEST(enter_splits_a_style)
{
	load(M_RICH, "**abcd**");
	at(0, 2);
	ed_newline(&E);
	STREQ(src(), "**ab**\n**cd**");
	EQ(E.caret.line, 1);
	EQ(caret_k(), 0);
}

TEST(enter_after_a_heading_is_body)
{
	load(M_RICH, "# Title");
	at(0, 5);
	ed_newline(&E);
	STREQ(src(), "# Title\n");
	at(0, 0);
	ed_newline(&E);				/* at its start: a line before it */
	STREQ(src(), "\n# Title\n");
	EQ(E.caret.line, 1);
}

TEST(enter_keeps_unknown_markdown)
{
	load(M_RICH, "see [x](y) and z");
	at(0, 10);
	ed_newline(&E);
	STREQ(src(), "see [x](y)\n and z");
}

TEST(paragraph_styles)
{
	load(M_RICH, "text");
	EQ(ed_block(&E, B_H2, 0), 0);
	STREQ(src(), "## text");
	ed_block(&E, B_BULLET, 0);
	STREQ(src(), "- text");
	ed_block(&E, B_BODY, 0);
	STREQ(src(), "text");
	load(M_RICH, "a\nb\nc");
	sel(0, 0, 2, 1);
	ed_block(&E, B_NUMBER, 0);
	STREQ(src(), "1. a\n2. b\n3. c");
	ed_block(&E, B_NUMBER, 1);		/* the shortcut toggles a list off */
	STREQ(src(), "a\nb\nc");
}

TEST(numbering_continues_from_the_line_before)
{
	load(M_RICH, "1. a\nb");
	at(1, 0);
	ed_block(&E, B_NUMBER, 0);
	STREQ(src(), "1. a\n2. b");
}

TEST(code_block_style)
{
	load(M_RICH, "x = 1\ny = 2");
	sel(0, 0, 1, 1);
	ed_block(&E, B_CODE, 0);
	STREQ(src(), "```\nx = 1\ny = 2\n```");
	at(1, 0);
	ed_block(&E, B_BODY, 0);		/* out of the block: the rest stays code */
	STREQ(src(), "x = 1\n```\ny = 2\n```");
	at(2, 0);
	ed_block(&E, B_BODY, 0);
	STREQ(src(), "x = 1\ny = 2");
}

TEST(copy_keeps_styles)
{
	size_t n;
	int words;
	char *c;

	load(M_RICH, "Bring **fully charged** now");
	sel(0, 6, 0, 11);
	c = ed_copy(&E, &n, &words);
	STREQ(c, "**fully**");
	EQ(words, 1);
	free(c);
	sel(0, 0, 0, 23);
	c = ed_copy(&E, &n, &words);
	STREQ(c, "Bring **fully charged** now");
	EQ(words, 4);
	free(c);
}

TEST(copy_whole_lines_with_their_markers)
{
	size_t n;
	int words;
	char *c;

	load(M_RICH, "# A\n- b\nc");
	sel(0, 0, 2, 1);
	c = ed_copy(&E, &n, &words);
	STREQ(c, "# A\n- b\nc");
	free(c);
}

TEST(paste_keeps_styles)
{
	load(M_RICH, "go ");
	at(0, 3);
	EQ(ed_paste(&E, "**now**", 7), 0);
	STREQ(src(), "go **now**");
	EQ(caret_k(), 6);
	load(M_RICH, "**ab**");
	at(0, 1);
	ed_paste(&E, "x", 1);			/* plain text in a bold word turns bold */
	STREQ(src(), "**axb**");
}

TEST(paste_lines)
{
	load(M_RICH, "ab");
	at(0, 1);
	ed_paste(&E, "1\n# 2\n3", 7);
	STREQ(src(), "a1\n# 2\n3b");
	EQ(E.caret.line, 2);
	EQ(caret_k(), 1);
}

TEST(cut_deletes)
{
	size_t n;
	int words;
	char *c;

	load(M_RICH, "one two three");
	sel(0, 4, 0, 8);
	c = ed_cut(&E, &n, &words);
	STREQ(c, "two ");
	STREQ(src(), "one three");
	free(c);
}

TEST(typing_replaces_the_selection)
{
	load(M_RICH, "first\nsecond");
	sel(0, 2, 1, 3);
	type("X");
	STREQ(src(), "fiXond");
	EQ(E.sel, 0);
}

TEST(undo_a_toggle_and_typing)
{
	load(M_RICH, "word");
	sel(0, 0, 0, 4);
	ed_toggle(&E, ST_B, 0);
	STREQ(src(), "**word**");
	EQ(ed_undo(&E), 1);
	STREQ(src(), "word");
	EQ(ed_redo(&E), 1);
	STREQ(src(), "**word**");
	load(M_RICH, "");
	type("one two");
	ed_undo(&E);
	STREQ(src(), "one ");
	ed_undo(&E);
	ed_undo(&E);
	STREQ(src(), "");
}

TEST(caret_moves_over_hidden_markers)
{
	load(M_RICH, "a **b** c");
	at(0, 1);
	ed_move(&E, MV_RIGHT, 0);
	EQ(caret_k(), 2);
	ed_move(&E, MV_RIGHT, 0);
	EQ(caret_k(), 3);
	ed_move(&E, MV_LEFT, 0);
	ed_move(&E, MV_LEFT, 0);
	EQ(caret_k(), 1);
}

TEST(moves_by_word_line_and_document)
{
	load(M_RICH, "one two  three\nnext");
	at(0, 0);
	ed_move(&E, MV_WORD_RIGHT, 0);
	EQ(caret_k(), 3);
	ed_move(&E, MV_WORD_RIGHT, 0);
	EQ(caret_k(), 7);
	ed_move(&E, MV_WORD_LEFT, 0);
	EQ(caret_k(), 4);
	ed_move(&E, MV_END, 0);
	EQ(caret_k(), 14);
	ed_move(&E, MV_RIGHT, 0);		/* past the end: the next line */
	EQ(E.caret.line, 1);
	EQ(caret_k(), 0);
	ed_move(&E, MV_LEFT, 0);
	EQ(E.caret.line, 0);
	ed_move(&E, MV_DOC_END, 0);
	EQ(E.caret.line, 1);
	EQ(caret_k(), 4);
	ed_move(&E, MV_DOC_START, 0);
	EQ(E.caret.line, 0);
	EQ(caret_k(), 0);
}

TEST(moves_over_utf8)
{
	load(M_SOURCE, "caf\xc3\xa9!");
	at(0, 3);
	ed_move(&E, MV_RIGHT, 0);
	EQ(E.caret.col, 5);
	ed_move(&E, MV_LEFT, 0);
	EQ(E.caret.col, 3);
	at(0, 5);
	ed_backspace(&E);
	STREQ(src(), "caf!");
}

TEST(select_with_moves)
{
	load(M_RICH, "abc def");
	at(0, 0);
	ed_move(&E, MV_WORD_RIGHT, 1);
	EQ(E.sel, 1);
	EQ(ed_sel_words(&E), 1);
	ed_move(&E, MV_RIGHT, 0);		/* an arrow without select: to that side */
	EQ(E.sel, 0);
	EQ(caret_k(), 3);
	ed_select_all(&E);
	EQ(E.sel, 1);
	EQ(caret_k(), 7);
}

TEST(style_of_a_selection)
{
	load(M_RICH, "**ab** c");
	sel(0, 0, 0, 2);
	EQ(ed_style_at(&E), ST_B);
	sel(0, 0, 0, 4);
	EQ(ed_style_at(&E), 0);
}

TEST(word_count_ignores_markers)
{
	load(M_RICH, "# Hello\n**big** world\n\n- a\n");
	EQ(ed_words(&E), 4);
	load(M_SOURCE, "**a** b");
	EQ(ed_words(&E), 2);
}

TEST(source_mode_is_raw)
{
	load(M_SOURCE, "");
	type("**x");
	STREQ(src(), "**x");
	ed_backspace(&E);
	ed_backspace(&E);
	STREQ(src(), "*");
	ed_newline(&E);
	STREQ(src(), "*\n");
}

TEST(code_indents)
{
	load(M_CODE, "def f():");
	E.indent = 4;
	at(0, 8);
	ed_newline(&E);
	STREQ(src(), "def f():\n    ");
	type("x");
	ed_newline(&E);
	STREQ(src(), "def f():\n    x\n    ");
	ed_backspace(&E);			/* in the indent: back one level */
	STREQ(src(), "def f():\n    x\n");
	ed_tab(&E);
	STREQ(src(), "def f():\n    x\n    ");
	E.indent = 0;
	ed_tab(&E);
	STREQ(src(), "def f():\n    x\n    \t");
}

TEST(indent_guess)
{
	load(M_CODE, "a:\n  b\n    c\n");
	EQ(ed_guess_indent(&E), 2);
	load(M_CODE, "a:\n\tb\n");
	EQ(ed_guess_indent(&E), 0);
	load(M_CODE, "nothing\n");
	EQ(ed_guess_indent(&E), -1);
}
