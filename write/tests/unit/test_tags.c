/* SPDX-License-Identifier: MIT */
/* The element still open in HTML (src/tags.c), for "</" in the code view. */
#include "t.h"
#include "tags.h"

static char name[64];

static int open_at(const char *s)
{
	name[0] = 0;
	return tags_open(s, strlen(s), name, sizeof(name));
}

static const char *open_name(const char *s)
{
	return open_at(s) < 0 ? "" : name;
}

TEST(innermost_open_element)
{
	STREQ(open_name("<html><body><p>hi"), "p");
	STREQ(open_name("<ul><li>a</li>"), "ul");
	EQ(open_at("<ul>\n  <li>a"), 7);	/* where its "<" is */
	STREQ(open_name("<p>a</p>"), "");
	STREQ(open_name(""), "");
	STREQ(open_name("a < b"), "");
}

TEST(void_and_self_closing_elements_stay_closed)
{
	STREQ(open_name("<div><br><img src=x><hr/>"), "div");
	STREQ(open_name("<div><meta charset=utf-8><input type=text>"), "div");
	STREQ(open_name("<svg><circle r=\"1\"/>"), "svg");
}

TEST(comments_declarations_and_php_are_skipped)
{
	STREQ(open_name("<!DOCTYPE html>\n<html>"), "html");
	STREQ(open_name("<div><!-- <p> -->"), "div");
	STREQ(open_name("<div><?php $a->b(); ?>"), "div");
}

TEST(attributes_quotes_and_lines)
{
	STREQ(open_name("<div title=\"a>b\"><span class='x>y'>"), "span");
	STREQ(open_name("<div\n  class=\"box\">\n"), "div");
	STREQ(open_name("<div><p class="), "div");	/* a tag still being typed */
}

TEST(script_and_style_hold_text)
{
	STREQ(open_name("<div><script>if (a<b) s = '<p>';"), "script");
	STREQ(open_name("<div><style>a > b {}</style>"), "div");
}

TEST(closing_tags_match_by_name)
{
	STREQ(open_name("<DIV><p></P>"), "DIV");	/* case does not matter, the name is kept */
	STREQ(open_name("<div><p>text</div>"), "");	/* </div> also ends the <p> in it */
	STREQ(open_name("<div><span></b>"), "span");	/* a stray end tag changes nothing */
	STREQ(open_name("<li>a<li>b"), "li");
}
