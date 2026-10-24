/* SPDX-License-Identifier: MIT */
/* Markdown lines (src/md.c): blocks, inline styles, the caret map, writing back. */
#include "t.h"
#include "md.h"

static struct mdline M;

static void parse(const char *s)
{
	md_free(&M);
	md_init(&M);
	EQ(md_parse(&M, s, (int)strlen(s), 0), 0);
}

/* the styles as a string: one letter per visible byte, '.' plain */
static const char *styles(void)
{
	static char buf[512];
	int i;

	for (i = 0; i < M.n && i < (int)sizeof(buf) - 1; i++) {
		int s = M.st[i];

		buf[i] = s == (ST_B | ST_I | ST_U) ? 'A' : s == (ST_B | ST_I) ? 'X' : s == (ST_B | ST_U) ? 'Y' :
			 s == (ST_I | ST_U) ? 'Z' : s == ST_B ? 'b' : s == ST_I ? 'i' : s == ST_U ? 'u' : '.';
	}
	buf[i] = 0;
	return buf;
}

static int block(const char *s, int in_code, int *prefix, int *number)
{
	return md_block(s, (int)strlen(s), in_code, prefix, number);
}

TEST(blocks)
{
	int p, n;

	EQ(block("# Title", 0, &p, &n), B_H1);
	EQ(p, 2);
	EQ(block("## Sub", 0, &p, &n), B_H2);
	EQ(p, 3);
	EQ(block("### Third", 0, &p, &n), B_H3);
	EQ(block("#### Four", 0, &p, &n), B_BODY);	/* Write knows three levels */
	EQ(block("#hashtag", 0, &p, &n), B_BODY);
	EQ(p, 0);
	EQ(block("#", 0, &p, &n), B_H1);		/* an empty heading */
	EQ(p, 1);
	EQ(block("- item", 0, &p, &n), B_BULLET);
	EQ(p, 2);
	EQ(block("* item", 0, &p, &n), B_BULLET);
	EQ(block("+ item", 0, &p, &n), B_BULLET);
	EQ(block("-item", 0, &p, &n), B_BODY);
	EQ(block("12. twelve", 0, &p, &n), B_NUMBER);
	EQ(p, 4);
	EQ(n, 12);
	EQ(block("3) three", 0, &p, &n), B_NUMBER);
	EQ(n, 3);
	EQ(block("2024. was a year", 0, &p, &n), B_NUMBER);
	EQ(block("1.5 litre", 0, &p, &n), B_BODY);
	EQ(block("> quote", 0, &p, &n), B_QUOTE);
	EQ(p, 2);
	EQ(block(">", 0, &p, &n), B_QUOTE);
	EQ(p, 1);
	EQ(block("```", 0, &p, &n), B_FENCE);
	EQ(block("```python", 0, &p, &n), B_FENCE);
	EQ(block("", 0, &p, &n), B_BLANK);
	EQ(block("body text", 0, &p, &n), B_BODY);
	EQ(block("# not a heading", 1, &p, &n), B_CODE);	/* inside a code block */
	EQ(p, 0);
	EQ(block("```", 1, &p, &n), B_FENCE);
}

TEST(plain_text_is_all_visible)
{
	parse("Bring the badge");
	EQ(M.kind, B_BODY);
	EQ(M.n, 15);
	STREQ(M.vis, "Bring the badge");
	STREQ(styles(), "...............");
}

TEST(prefix_is_hidden)
{
	parse("## Packing list");
	EQ(M.kind, B_H2);
	STREQ(M.vis, "Packing list");
	EQ(M.src[0], 3);
}

TEST(bold_italic_underline)
{
	parse("a **b** *c* <u>d</u> e");
	STREQ(M.vis, "a b c d e");
	STREQ(styles(), "..b.i.u..");
	parse("***both***");
	STREQ(M.vis, "both");
	STREQ(styles(), "XXXX");
	parse("**<u>text</u>**");
	STREQ(M.vis, "text");
	STREQ(styles(), "YYYY");
	parse("**a*b***");
	STREQ(M.vis, "ab");
	STREQ(styles(), "bX");
	parse("*a***b**");
	STREQ(M.vis, "ab");
	STREQ(styles(), "ib");
	parse("Bring the badge **fully charged** and");
	STREQ(M.vis, "Bring the badge fully charged and");
}

TEST(stars_that_are_not_markers)
{
	parse("5 * 3 = 15");
	STREQ(M.vis, "5 * 3 = 15");
	STREQ(styles(), "..........");
	parse("**unclosed");
	STREQ(M.vis, "**unclosed");
	parse("a ** b");
	STREQ(M.vis, "a ** b");
	parse("****");
	STREQ(M.vis, "****");
	parse("<u>open");
	STREQ(M.vis, "<u>open");
	parse("close</u>");
	STREQ(M.vis, "close</u>");
	parse("***a**");			/* the star left over stays outside, as CommonMark */
	STREQ(M.vis, "*a");
	STREQ(styles(), ".b");
}

TEST(escapes_and_code_spans)
{
	parse("\\*not italic\\*");
	STREQ(M.vis, "*not italic*");
	STREQ(styles(), "............");
	parse("call `f(*args*)` now");
	STREQ(M.vis, "call `f(*args*)` now");
	parse("C:\\path");			/* not punctuation: the backslash stays */
	STREQ(M.vis, "C:\\path");
	parse("`open *x*");			/* no closing backtick: markers count */
	STREQ(M.vis, "`open x");
}

TEST(code_lines_are_literal)
{
	md_free(&M);
	md_init(&M);
	EQ(md_parse(&M, "**x** # y", 9, 1), 0);
	EQ(M.kind, B_CODE);
	STREQ(M.vis, "**x** # y");
}

TEST(unknown_markdown_stays_literal)
{
	parse("see [the wiki](https://example.org) | a | b |");
	STREQ(M.vis, "see [the wiki](https://example.org) | a | b |");
	parse("#### deep");
	STREQ(M.vis, "#### deep");
}

TEST(caret_maps_to_the_source)
{
	/* "Bring **fully** now": visible "Bring fully now" */
	parse("Bring **fully** now");
	EQ(md_src(&M, 0), 0);
	EQ(md_src(&M, 6), 6);		/* after the space, outside the bold */
	EQ(md_src(&M, 7), 9);		/* after "f", inside */
	EQ(md_src(&M, 11), 13);		/* after "fully", before the closing ** */
	EQ(md_src(&M, 12), 16);		/* after the space that follows it */
	EQ(md_src(&M, M.n), 19);
	parse("**bold**");
	EQ(md_src(&M, 0), 2);		/* the start takes the first letter's style */
	parse("# ");
	EQ(M.n, 0);
	EQ(md_src(&M, 0), 2);
	parse("");
	EQ(md_src(&M, 0), 0);
}

TEST(source_maps_to_the_caret)
{
	parse("Bring **fully** now");
	EQ(md_vis(&M, 0), 0);
	EQ(md_vis(&M, 6), 6);
	EQ(md_vis(&M, 7), 6);		/* inside the marker */
	EQ(md_vis(&M, 8), 6);		/* before "f" */
	EQ(md_vis(&M, 9), 7);
	EQ(md_vis(&M, 19), 15);
	parse("## Head");
	EQ(md_vis(&M, 0), 0);
	EQ(md_vis(&M, 3), 0);
	EQ(md_vis(&M, 4), 1);
}

static const char *ser(const char *vis, const char *sty)
{
	static char out[512];
	unsigned char st[128];
	int n = (int)strlen(vis);

	for (int i = 0; i < n; i++)
		st[i] = sty[i] == 'b' ? ST_B : sty[i] == 'i' ? ST_I : sty[i] == 'u' ? ST_U :
			sty[i] == 'X' ? ST_B | ST_I : sty[i] == 'Y' ? ST_B | ST_U : 0;
	EQ(md_inline(vis, st, n, out, sizeof(out)) < (int)sizeof(out), 1);
	return out;
}

TEST(writing_inline_styles)
{
	STREQ(ser("plain", "....."), "plain");
	STREQ(ser("a", "b"), "**a**");
	STREQ(ser("ab", "bX"), "**a*b***");
	STREQ(ser("x y", "bbb"), "**x y**");
	STREQ(ser("x ", "bb"), "**x** ");		/* no marker after a space */
	STREQ(ser(" x", "bb"), " **x**");
	STREQ(ser("u", "u"), "<u>u</u>");
	STREQ(ser("x", "Y"), "**<u>x</u>**");
	STREQ(ser("5 * 3 x", "......b"), "5 * 3 **x**");	/* stars that need no escape */
	STREQ(ser("*", "b"), "**\\***");			/* ...and one that does */
	STREQ(ser("Bring fully now", "......bbbbb...."), "Bring **fully** now");
}

TEST(writing_a_line_keeps_its_block)
{
	char out[256];
	unsigned char st[16] = { 0 };

	md_line(B_H2, 0, "Head", st, 4, out, sizeof(out));
	STREQ(out, "## Head");
	md_line(B_NUMBER, 7, "seven", st, 5, out, sizeof(out));
	STREQ(out, "7. seven");
	md_line(B_BULLET, 0, "x", st, 1, out, sizeof(out));
	STREQ(out, "- x");
	md_line(B_QUOTE, 0, "q", st, 1, out, sizeof(out));
	STREQ(out, "> q");
	md_line(B_BODY, 0, "# not a heading", st, 15, out, sizeof(out));
	STREQ(out, "\\# not a heading");
	md_line(B_BODY, 0, "1. not a list", st, 13, out, sizeof(out));
	STREQ(out, "1\\. not a list");
	md_line(B_BODY, 0, "- no", st, 4, out, sizeof(out));
	STREQ(out, "\\- no");
}

/* any styled text written out reads back the same (spaces at the edges of a
 * style lose it: there is no marker for them) */
TEST(write_then_read_round_trip)
{
	static const char alpha[] = "ab *`<u>/\\#-1. ";
	unsigned seed = 12345;
	char vis[40], out[1024];
	unsigned char st[40], want[40];

	for (int round = 0; round < 20000; round++) {
		int n = 1 + (int)((seed = seed * 1103515245 + 12345) >> 16) % 30;

		for (int i = 0; i < n; i++) {
			seed = seed * 1103515245 + 12345;
			vis[i] = alpha[(seed >> 16) % (sizeof(alpha) - 1)];
			seed = seed * 1103515245 + 12345;
			st[i] = (unsigned char)((seed >> 16) % 8);
		}
		vis[n] = 0;
		md_normalize(vis, st, n, want);
		md_line(B_BODY, 0, vis, st, n, out, sizeof(out));
		parse(out);
		if (M.n != n || memcmp(M.vis, vis, (size_t)n) || memcmp(M.st, want, (size_t)n) ||
		    M.kind != B_BODY) {
			T_FAIL("round %d: \"%s\" written as \"%s\" read as \"%s\"", round, vis, out, M.vis);
			for (int i = 0; i < n; i++)
				fprintf(stderr, "%d", want[i]);
			fprintf(stderr, " wanted, read ");
			for (int i = 0; i < M.n; i++)
				fprintf(stderr, "%d", M.st[i]);
			fputc('\n', stderr);
			break;
		}
	}
}

TEST(fence_info)
{
	OK(md_is_fence("```", 3));
	OK(md_is_fence("```py", 5));
	OK(!md_is_fence("``", 2));
	OK(!md_is_fence(" ```", 4));		/* Write only knows fences at the start */
}

TEST(prefix_of_a_block)
{
	char b[16];

	STREQ(md_prefix(B_H1, 0, b), "# ");
	STREQ(md_prefix(B_H3, 0, b), "### ");
	STREQ(md_prefix(B_NUMBER, 12, b), "12. ");
	STREQ(md_prefix(B_BODY, 0, b), "");
	STREQ(md_prefix(B_QUOTE, 0, b), "> ");
}
