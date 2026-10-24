/* SPDX-License-Identifier: MIT */
/* Write: markdown lines (md.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md.h"

struct tok {
	int at, len;
	char k;			/* s: a star run, o: <u>, c: </u>, e: an escape, q: a code span */
	unsigned char open, close;	/* a star run may open / close */
	unsigned char want;	/* the styles a star run toggles */
	unsigned char dis;	/* ... minus these: their opener was never closed */
	unsigned char eff;	/* what it toggles in the end */
};

struct scratch {
	struct tok *t;
	int cap;
};

static struct scratch S;

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

static int is_punct(char c)
{
	return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') ||
	       (c >= '{' && c <= '~');
}

void md_init(struct mdline *m)
{
	memset(m, 0, sizeof(*m));
}

void md_free(struct mdline *m)
{
	free(m->vis);
	free(m->st);
	free(m->src);
	free(m->w);
	memset(m, 0, sizeof(*m));
}

static int reserve(struct mdline *m, int len)
{
	int cap = m->cap ? m->cap : 64;
	char *vis;
	unsigned char *st, *w;
	int *src;

	if (len + 1 <= m->cap && len + 1 <= S.cap)
		return 0;
	while (cap < len + 1)
		cap *= 2;
	if (cap > S.cap) {
		struct tok *t = realloc(S.t, (size_t)cap * sizeof(*t));

		if (!t)
			return -1;
		S.t = t;
		S.cap = cap;
	}
	if (cap <= m->cap)
		return 0;
	vis = realloc(m->vis, (size_t)cap);
	if (vis)
		m->vis = vis;
	st = realloc(m->st, (size_t)cap);
	if (st)
		m->st = st;
	w = realloc(m->w, (size_t)cap);
	if (w)
		m->w = w;
	src = realloc(m->src, (size_t)cap * sizeof(int));
	if (src)
		m->src = src;
	if (!vis || !st || !w || !src)
		return -1;
	m->cap = cap;
	return 0;
}

/* ---- blocks ---------------------------------------------------------------- */

int md_is_fence(const char *s, int len)
{
	return len >= 3 && !memcmp(s, "```", 3);
}

enum blk md_block(const char *s, int len, int in_code, int *prefix, int *number)
{
	int i = 0;

	*prefix = 0;
	*number = 0;
	if (md_is_fence(s, len))
		return B_FENCE;
	if (in_code)
		return B_CODE;
	if (!len)
		return B_BLANK;
	if (s[0] == '#') {
		while (i < len && s[i] == '#')
			i++;
		if (i <= 3 && (i == len || s[i] == ' ')) {
			*prefix = i + (i < len);
			return i == 1 ? B_H1 : i == 2 ? B_H2 : B_H3;
		}
		return B_BODY;
	}
	if ((s[0] == '-' || s[0] == '*' || s[0] == '+') && len >= 2 && s[1] == ' ') {
		*prefix = 2;
		return B_BULLET;
	}
	if (s[0] >= '0' && s[0] <= '9') {
		int n = 0;

		while (i < len && i < 9 && s[i] >= '0' && s[i] <= '9')
			n = n * 10 + (s[i++] - '0');
		if (i + 1 < len && (s[i] == '.' || s[i] == ')') && s[i + 1] == ' ') {
			*prefix = i + 2;
			*number = n;
			return B_NUMBER;
		}
		return B_BODY;
	}
	if (s[0] == '>' && (len == 1 || s[1] == ' ')) {
		*prefix = len == 1 ? 1 : 2;
		return B_QUOTE;
	}
	return B_BODY;
}

const char *md_prefix(enum blk kind, int number, char buf[16])
{
	switch (kind) {
	case B_H1: return "# ";
	case B_H2: return "## ";
	case B_H3: return "### ";
	case B_BULLET: return "- ";
	case B_QUOTE: return "> ";
	case B_NUMBER:
		snprintf(buf, 16, "%d. ", number);
		return buf;
	default: return "";
	}
}

/* ---- inline markers -------------------------------------------------------- */

static int tokenize(const char *s, int from, int len, struct tok *t)
{
	int n = 0, i = from;

	while (i < len) {
		char c = s[i];

		if (c == '\\' && i + 1 < len && is_punct(s[i + 1])) {
			t[n++] = (struct tok){ .at = i, .len = 2, .k = 'e' };
			i += 2;
		} else if (c == '`') {
			int r = 0, j;

			while (i + r < len && s[i + r] == '`')
				r++;
			for (j = i + r; j < len; ) {
				int q = 0;

				while (j + q < len && s[j + q] == '`')
					q++;
				if (q == r)
					break;
				j += q ? q : 1;
			}
			if (j < len) {
				t[n++] = (struct tok){ .at = i, .len = j + r - i, .k = 'q' };
				i = j + r;
			} else {
				i += r;
			}
		} else if (c == '<' && i + 3 <= len && !memcmp(s + i, "<u>", 3)) {
			t[n++] = (struct tok){ .at = i, .len = 3, .k = 'o' };
			i += 3;
		} else if (c == '<' && i + 4 <= len && !memcmp(s + i, "</u>", 4)) {
			t[n++] = (struct tok){ .at = i, .len = 4, .k = 'c' };
			i += 4;
		} else if (c == '*') {
			int r = 0;

			while (i + r < len && s[i + r] == '*')
				r++;
			if (r <= 3)
				t[n++] = (struct tok){ .at = i, .len = r, .k = 's',
					.open = i + r < len && !is_space(s[i + r]),
					.close = i > from && !is_space(s[i - 1]),
					.want = r == 1 ? ST_I : r == 2 ? ST_B : ST_B | ST_I };
			i += r;
		} else {
			i++;
		}
	}
	return n;
}

/* which markers pair up: an opener left open is literal, and it is tried again */
static void resolve(struct tok *t, int n)
{
	for (;;) {
		int st = 0, opener[8];

		for (int i = 0; i < n; i++) {
			struct tok *k = &t[i];

			k->eff = 0;
			if (k->k == 's') {
				for (int f = ST_B; f <= ST_I; f <<= 1) {
					if (!(k->want & f) || (k->dis & f))
						continue;
					if (st & f) {
						if (k->close) {
							st &= ~f;
							k->eff |= f;
						}
					} else if (k->open) {
						st |= f;
						k->eff |= f;
						opener[f] = i;
					}
				}
			} else if (k->k == 'o' && !k->dis && !(st & ST_U)) {
				st |= ST_U;
				k->eff = ST_U;
				opener[ST_U] = i;
			} else if (k->k == 'c' && (st & ST_U)) {
				st &= ~ST_U;
				k->eff = ST_U;
			}
		}
		if (!st)
			return;
		for (int f = ST_B; f <= ST_U; f <<= 1)
			if (st & f)
				t[opener[f]].dis |= f;
	}
}

static void emit(struct mdline *m, char c, int at, int w, int st)
{
	m->vis[m->n] = c;
	m->st[m->n] = (unsigned char)st;
	m->src[m->n] = at;
	m->w[m->n] = (unsigned char)w;
	m->n++;
}

int md_parse(struct mdline *m, const char *s, int len, int in_code)
{
	int ntok, ti = 0, st = 0, i;
	struct tok *t;

	if (reserve(m, len))
		return -1;
	m->kind = md_block(s, len, in_code, &m->prefix, &m->number);
	m->n = 0;
	m->len = len;
	if (m->kind == B_FENCE || m->kind == B_CODE) {
		for (i = 0; i < len; i++)
			emit(m, s[i], i, 1, 0);
		m->vis[m->n] = 0;
		return 0;
	}
	t = S.t;
	ntok = tokenize(s, m->prefix, len, t);
	resolve(t, ntok);
	i = m->prefix;
	while (i < len) {
		if (ti < ntok && t[ti].at == i) {
			struct tok *k = &t[ti++];

			switch (k->k) {
			case 'e':
				emit(m, s[i + 1], i, 2, st);
				break;
			case 'q':
				for (int j = 0; j < k->len; j++)
					emit(m, s[i + j], i + j, 1, st);
				break;
			case 'o':
			case 'c':
				if (k->eff)
					st ^= ST_U;
				else
					for (int j = 0; j < k->len; j++)
						emit(m, s[i + j], i + j, 1, st);
				break;
			case 's': {
				int used = (k->eff & ST_B ? 2 : 0) + (k->eff & ST_I ? 1 : 0);
				int lit = k->len - used, opens = (k->eff & ~st) != 0;

				if (opens) {	/* a literal star stays outside the style */
					for (int j = 0; j < lit; j++)
						emit(m, '*', i + j, 1, st);
					st ^= k->eff;
				} else {
					st ^= k->eff;
					for (int j = 0; j < lit; j++)
						emit(m, '*', i + used + j, 1, st);
				}
				break;
			}
			}
			i = k->at + k->len;
		} else {
			emit(m, s[i], i, 1, st);
			i++;
		}
	}
	m->vis[m->n] = 0;
	return 0;
}

int md_src(const struct mdline *m, int k)
{
	if (!m->n)
		return m->prefix;
	if (k <= 0)
		return m->src[0];
	if (k > m->n)
		k = m->n;
	return m->src[k - 1] + m->w[k - 1];
}

int md_vis(const struct mdline *m, int col)
{
	int lo = 0, hi = m->n;	/* the number of visible bytes that start before col */

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (m->src[mid] < col)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* ---- writing back ---------------------------------------------------------- */

void md_normalize(const char *vis, const unsigned char *st, int n, unsigned char *out)
{
	for (int i = 0; i < n; i++)
		out[i] = st[i] & (ST_B | ST_I | ST_U);
	for (int i = 0; i < n; ) {
		int j = i, left, right;

		if (!is_space(vis[i])) {
			i++;
			continue;
		}
		while (j < n && is_space(vis[j]))
			j++;
		left = i > 0 ? out[i - 1] : 0;
		right = j < n ? out[j] : 0;
		for (int k = i; k < j; k++)
			out[k] = (unsigned char)(left & right);
		i = j;
	}
}

struct outbuf {
	char *s;
	int n, cap;
};

static void put(struct outbuf *o, const char *s, int n)
{
	for (int i = 0; i < n; i++, o->n++)
		if (o->n < o->cap - 1)
			o->s[o->n] = s[i];
	if (o->cap)
		o->s[o->n < o->cap ? o->n : o->cap - 1] = 0;
}

static void write_inline(const char *vis, const unsigned char *ns, int n, int esc, struct outbuf *o)
{
	int prev = 0;

	for (int i = 0; i <= n; i++) {
		int cur = i < n ? ns[i] : 0;

		if (cur != prev) {
			int d = (prev ^ cur) & (ST_B | ST_I);

			if ((prev & ST_U) && !(cur & ST_U))
				put(o, "</u>", 4);
			if (d == (ST_B | ST_I))
				put(o, "***", 3);
			else if (d == ST_B)
				put(o, "**", 2);
			else if (d == ST_I)
				put(o, "*", 1);
			if (!(prev & ST_U) && (cur & ST_U))
				put(o, "<u>", 3);
		}
		if (i < n) {
			if (esc && strchr("\\*`<", vis[i]))
				put(o, "\\", 1);
			put(o, vis + i, 1);
		}
		prev = cur;
	}
}

int md_inline(const char *vis, const unsigned char *st, int n, char *out, int cap)
{
	unsigned char *ns = malloc((size_t)n + 1);
	struct outbuf o = { out, 0, cap };
	struct mdline m;
	int same;

	if (!ns)
		return -1;
	md_normalize(vis, st, n, ns);
	write_inline(vis, ns, n, 0, &o);
	if (o.n >= cap) {
		free(ns);
		return o.n;
	}
	/* read it back: when a literal star or backtick changed it, escape them */
	md_init(&m);
	same = 0;
	{
		/* behind "x ", which is no block marker: the caller's prefix comes first */
		char *copy = malloc((size_t)o.n + 3);

		if (copy) {
			memcpy(copy, "x ", 2);
			memcpy(copy + 2, out, (size_t)o.n + 1);
			if (!md_parse(&m, copy, o.n + 2, 0)) {
				same = m.n == n + 2 && !memcmp(m.vis + 2, vis, (size_t)n);
				for (int i = 0; same && i < n; i++)
					same = m.st[i + 2] == ns[i];
			}
			free(copy);
		}
	}
	md_free(&m);
	if (!same) {
		o.n = 0;
		write_inline(vis, ns, n, 1, &o);
	}
	free(ns);
	return o.n;
}

int md_line_p(const char *prefix, enum blk kind, const char *vis, const unsigned char *st, int n,
	      char *out, int cap)
{
	int pl = (int)strlen(prefix), len, pre, num;

	if (pl >= cap)
		return -1;
	memcpy(out, prefix, (size_t)pl + 1);
	len = md_inline(vis, st, n, out + pl, cap - pl);
	if (len < 0)
		return -1;
	len += pl;
	if (len >= cap)
		return len;
	/* body text that would read as a block marker: escape the marker */
	if ((kind == B_BODY || kind == B_BLANK) && len + 1 < cap) {
		enum blk got = md_block(out, len, 0, &pre, &num);

		if (got != B_BODY && got != B_BLANK) {
			int at = 0;

			while (at < len && out[at] >= '0' && out[at] <= '9')
				at++;
			memmove(out + at + 1, out + at, (size_t)(len - at) + 1);
			out[at] = '\\';
			len++;
		}
	}
	return len;
}

int md_line(enum blk kind, int number, const char *vis, const unsigned char *st, int n,
	    char *out, int cap)
{
	char pb[16];

	return md_line_p(md_prefix(kind, number, pb), kind, vis, st, n, out, cap);
}
