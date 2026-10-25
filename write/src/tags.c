/* SPDX-License-Identifier: MIT */
/* Write: the element still open in HTML (tags.h). */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "tags.h"

#define DEPTH 64
#define NAME 32

static const char *const void_elems[] = {
	"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
	"source", "track", "wbr", 0
};

static int is_name(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
	       c == ':' || c == '_' || c == '.';
}

static int is_void(const char *name)
{
	for (int i = 0; void_elems[i]; i++)
		if (!strcasecmp(name, void_elems[i]))
			return 1;
	return 0;
}

/* the offset after `end` from i on, or n */
static size_t skip_to(const char *s, size_t n, size_t i, const char *end)
{
	size_t l = strlen(end);

	for (; i + l <= n; i++)
		if (!memcmp(s + i, end, l))
			return i + l;
	return n;
}

/* the offset after the tag's ">", quotes respected; n when it is not closed */
static size_t tag_end(const char *s, size_t n, size_t i, int *self)
{
	char q = 0;

	for (*self = 0; i < n; i++) {
		if (q) {
			q = s[i] == q ? 0 : q;
		} else if (s[i] == '"' || s[i] == '\'') {
			q = s[i];
		} else if (s[i] == '>') {
			return i + 1;
		} else if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') {
			*self = 1;
		}
	}
	return n;
}

/* the raw text of <script>/<style>: up to its end tag, case ignored */
static size_t raw_end(const char *s, size_t n, size_t i, const char *name)
{
	size_t l = strlen(name);

	for (; i + 2 + l <= n; i++)
		if (s[i] == '<' && s[i + 1] == '/' && !strncasecmp(s + i + 2, name, l))
			return i;
	return n;
}

int tags_open(const char *s, size_t n, char *name, int cap)
{
	char names[DEPTH][NAME];
	size_t at[DEPTH];
	int depth = 0;
	size_t i = 0;

	while (i < n) {
		char nm[NAME];
		size_t start = i, l = 0;
		int closing, self;

		if (s[i] != '<') {
			i++;
			continue;
		}
		if (i + 4 <= n && !memcmp(s + i, "<!--", 4)) {
			i = skip_to(s, n, i + 4, "-->");
			continue;
		}
		if (i + 1 < n && s[i + 1] == '?') {
			i = skip_to(s, n, i + 2, "?>");
			continue;
		}
		if (i + 1 < n && s[i + 1] == '!') {
			i = skip_to(s, n, i + 2, ">");
			continue;
		}
		closing = i + 1 < n && s[i + 1] == '/';
		i += 1 + closing;
		while (i < n && is_name(s[i]) && l < NAME - 1)
			nm[l++] = s[i++];
		nm[l] = 0;
		if (!l || !((nm[0] >= 'a' && nm[0] <= 'z') || (nm[0] >= 'A' && nm[0] <= 'Z')))
			continue;	/* "a < b": not a tag */
		i = tag_end(s, n, i, &self);
		if (closing) {
			for (int d = depth - 1; d >= 0; d--) {
				if (!strcasecmp(names[d], nm)) {
					depth = d;
					break;
				}
			}
			continue;
		}
		if (i >= n && s[n - 1] != '>')
			break;	/* still being typed */
		if (self || is_void(nm))
			continue;
		if (depth == DEPTH) {
			memmove(names, names + 1, sizeof(names[0]) * (DEPTH - 1));
			memmove(at, at + 1, sizeof(at[0]) * (DEPTH - 1));
			depth--;
		}
		memcpy(names[depth], nm, l + 1);
		at[depth++] = start;
		if (!strcasecmp(nm, "script") || !strcasecmp(nm, "style"))
			i = raw_end(s, n, i, nm);
	}
	if (!depth)
		return -1;
	snprintf(name, (size_t)cap, "%s", names[depth - 1]);
	return (int)at[depth - 1];
}
