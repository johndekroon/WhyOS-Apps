/* SPDX-License-Identifier: MIT */
/* Find and replace (src/find.c, design/write/README.md §8). */
#include "t.h"
#include "find.h"

static struct ed E;
static struct find F;

static const char NOTES[] =
	"# Hacker camp notes\n"
	"Bring the badge **fully charged** and pack a *spare* cable.\n"
	"- Badge and lanyard\n"
	"1. Badge hacking for beginners\n";

static void load(enum mode mode, const char *s)
{
	ed_free(&E);
	ed_init(&E);
	EQ(ed_load(&E, s, strlen(s), mode), 0);
	find_free(&F);
	find_init(&F);
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

static void query(const char *q)
{
	snprintf(F.q, sizeof(F.q), "%s", q);
	find_run(&F, &E);
}

TEST(finds_ignoring_case)
{
	load(M_RICH, NOTES);
	query("badge");
	EQ(F.n, 3);
	EQ(F.m[0].line, 1);
	EQ(F.m[0].k0, 10);		/* in the visible text */
	EQ(F.m[1].line, 2);
	EQ(F.m[2].line, 3);
	F.match_case = 1;
	find_run(&F, &E);
	EQ(F.n, 1);
}

TEST(whole_words)
{
	load(M_RICH, "bad badge bad");
	query("bad");
	EQ(F.n, 3);
	F.whole_word = 1;
	find_run(&F, &E);
	EQ(F.n, 2);
}

TEST(rich_searches_what_shows)
{
	load(M_RICH, NOTES);
	query("fully charged");
	EQ(F.n, 1);
	query("**");
	EQ(F.n, 0);
	load(M_SOURCE, NOTES);
	query("**");
	EQ(F.n, 2);
}

TEST(nearest_next_previous_wrap)
{
	char c[32];

	load(M_RICH, NOTES);
	ed_caret_vis(&E, 2, 3, 0);
	query("badge");
	find_nearest(&F, &E);
	EQ(F.cur, 2);			/* the first one after the caret */
	find_count(&F, c, sizeof(c));
	STREQ(c, "3 of 3");
	find_step(&F, &E, 1);
	EQ(F.cur, 0);			/* wraps */
	find_step(&F, &E, -1);
	EQ(F.cur, 2);
	EQ(E.sel, 1);			/* the current match is selected */
	EQ(ed_sel_words(&E), 1);
	query("zebra");
	find_count(&F, c, sizeof(c));
	STREQ(c, "no matches");
	query("");
	EQ(F.n, 0);
}

TEST(replace_keeps_a_capital)
{
	char out[64];

	find_case(out, sizeof(out), "Badge", "device", 0);
	STREQ(out, "Device");
	find_case(out, sizeof(out), "badge", "device", 0);
	STREQ(out, "device");
	find_case(out, sizeof(out), "Badge", "device", 1);
	STREQ(out, "device");
	find_case(out, sizeof(out), "Badge", "", 0);
	STREQ(out, "");
}

TEST(replace_one_then_move_on)
{
	load(M_RICH, NOTES);
	query("badge");
	find_nearest(&F, &E);
	EQ(F.cur, 0);
	EQ(find_replace(&F, &E, "device"), 1);
	STREQ(src(), "# Hacker camp notes\n"
		     "Bring the device **fully charged** and pack a *spare* cable.\n"
		     "- Badge and lanyard\n"
		     "1. Badge hacking for beginners\n");
	EQ(F.n, 2);
	EQ(F.cur, 0);			/* now the next one */
	EQ(F.m[0].line, 2);
}

TEST(replace_inside_a_style)
{
	load(M_RICH, "pack **the badge** now");
	query("badge");
	find_nearest(&F, &E);
	find_replace(&F, &E, "device");
	STREQ(src(), "pack **the device** now");
}

TEST(replace_all_is_one_undo)
{
	load(M_RICH, NOTES);
	query("badge");
	EQ(find_replace_all(&F, &E, "device"), 3);
	STREQ(src(), "# Hacker camp notes\n"
		     "Bring the device **fully charged** and pack a *spare* cable.\n"
		     "- Device and lanyard\n"
		     "1. Device hacking for beginners\n");
	EQ(F.n, 0);
	EQ(ed_undo(&E), 1);
	STREQ(src(), NOTES);
}

TEST(replace_all_with_the_query_inside)
{
	load(M_SOURCE, "a a a");
	query("a");
	EQ(find_replace_all(&F, &E, "aa"), 3);	/* replaced text is not searched again */
	STREQ(src(), "aa aa aa");
}

TEST(code_lines_in_rich_are_searched_raw)
{
	load(M_RICH, "```\nx = **y**\n```\n");
	query("**y**");
	EQ(F.n, 1);
}
