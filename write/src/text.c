/* SPDX-License-Identifier: MIT */
/* Write: the text buffer and its undo history (text.h). */
#include <stdlib.h>
#include <string.h>

#include "text.h"

struct undo_op {
	unsigned step;		/* the undo step it belongs to */
	int del;		/* 1: text was deleted at at, 0: inserted */
	struct pos at;
	char *s;
	size_t n;
	struct pos before, after;	/* the step's caret, on its first op */
};

int pos_cmp(struct pos a, struct pos b)
{
	return a.line != b.line ? (a.line < b.line ? -1 : 1) : (a.col < b.col ? -1 : a.col > b.col);
}

/* ---- lines ---------------------------------------------------------------- */

static int line_reserve(struct tline *l, int need)
{
	char *s;
	int cap;

	if (need + 1 <= l->cap)
		return 0;
	cap = l->cap ? l->cap : 16;
	while (cap < need + 1)
		cap *= 2;
	s = realloc(l->s, (size_t)cap);
	if (!s)
		return -1;
	l->s = s;
	l->cap = cap;
	return 0;
}

static int line_set(struct tline *l, const char *s, int n)
{
	if (line_reserve(l, n))
		return -1;
	memcpy(l->s, s, (size_t)n);
	l->s[n] = 0;
	l->len = n;
	l->h = -1;
	l->words = -1;
	l->st_in = l->st_out = -1;
	return 0;
}

static int lines_reserve(struct text *t, int need)
{
	struct tline *l;
	int cap;

	if (need <= t->cap)
		return 0;
	cap = t->cap ? t->cap : 64;
	while (cap < need)
		cap *= 2;
	l = realloc(t->l, (size_t)cap * sizeof(*l));
	if (!l)
		return -1;
	memset(l + t->cap, 0, (size_t)(cap - t->cap) * sizeof(*l));
	t->l = l;
	t->cap = cap;
	return 0;
}

static void changed(struct text *t, int line)
{
	if (t->changed < 0 || line < t->changed)
		t->changed = line;
	t->l[line].h = -1;
	t->l[line].words = -1;
}

/* make room for k new lines after line i (empty, unknown height) */
static int lines_open(struct text *t, int i, int k)
{
	if (lines_reserve(t, t->n + k))
		return -1;
	memmove(t->l + i + 1 + k, t->l + i + 1, (size_t)(t->n - i - 1) * sizeof(*t->l));
	for (int j = i + 1; j <= i + k; j++) {
		memset(&t->l[j], 0, sizeof(t->l[j]));
		t->l[j].h = -1;
		t->l[j].words = -1;
		t->l[j].st_in = t->l[j].st_out = -1;
	}
	t->n += k;
	return 0;
}

static void lines_close(struct text *t, int i, int k)	/* drop lines i+1..i+k */
{
	for (int j = i + 1; j <= i + k; j++)
		free(t->l[j].s);
	memmove(t->l + i + 1, t->l + i + 1 + k, (size_t)(t->n - i - 1 - k) * sizeof(*t->l));
	t->n -= k;
	memset(t->l + t->n, 0, (size_t)k * sizeof(*t->l));
}

void text_init(struct text *t)
{
	memset(t, 0, sizeof(*t));
	t->changed = -1;
	if (!lines_reserve(t, 1)) {
		t->n = 1;
		line_set(&t->l[0], "", 0);
	}
}

static void undo_clear(struct text *t, int from)
{
	for (int i = from; i < t->n_ops; i++)
		free(t->ops[i].s);
	t->n_ops = from;
	if (t->n_done > from)
		t->n_done = from;
}

void text_free(struct text *t)
{
	for (int i = 0; i < t->n; i++)
		free(t->l[i].s);
	free(t->l);
	undo_clear(t, 0);
	free(t->ops);
	memset(t, 0, sizeof(*t));
}

int text_set(struct text *t, const char *data, size_t len)
{
	struct text nt;
	size_t i = 0;

	text_init(&nt);
	if (!nt.n)
		return -1;
	nt.n = 0;
	if (len && data[len - 1] == '\n') {
		nt.final_nl = 1;
		len--;
	}
	for (;;) {
		const char *nl = memchr(data + i, '\n', len - i);
		size_t e = nl ? (size_t)(nl - data) : len, n = e - i;

		if (lines_reserve(&nt, nt.n + 1))
			goto fail;
		if (n && data[e - 1] == '\r') {
			nt.crlf = 1;
			n--;
		}
		memset(&nt.l[nt.n], 0, sizeof(nt.l[nt.n]));
		if (line_set(&nt.l[nt.n], data + i, (int)n))
			goto fail;
		nt.n++;
		if (!nl)
			break;
		i = e + 1;
	}
	text_free(t);
	*t = nt;
	return 0;
fail:
	text_free(&nt);
	return -1;
}

size_t text_size(const struct text *t)
{
	size_t n = 0;

	for (int i = 0; i < t->n; i++)
		n += (size_t)t->l[i].len + (i + 1 < t->n);
	return n;
}

char *text_get(const struct text *t, size_t *len)
{
	size_t eol = t->crlf ? 2 : 1, n = 0;
	char *s, *p;

	for (int i = 0; i < t->n; i++)
		n += (size_t)t->l[i].len + eol;
	s = malloc(n + 1);
	if (!s)
		return 0;
	p = s;
	for (int i = 0; i < t->n; i++) {
		memcpy(p, t->l[i].s, (size_t)t->l[i].len);
		p += t->l[i].len;
		if (i + 1 < t->n || t->final_nl) {
			if (t->crlf)
				*p++ = '\r';
			*p++ = '\n';
		}
	}
	*p = 0;
	*len = (size_t)(p - s);
	return s;
}

/* ---- the raw edits (no undo) ---------------------------------------------- */

static int raw_insert(struct text *t, struct pos at, const char *s, size_t n, struct pos *end)
{
	struct tline *l = &t->l[at.line];
	int k = 0, tail_len = l->len - at.col;
	char *tail;
	const char *p, *e = s + n;

	for (p = s; p < e; p++)
		k += *p == '\n';
	if (!k) {
		if (line_reserve(l, l->len + (int)n))
			return -1;
		memmove(l->s + at.col + n, l->s + at.col, (size_t)tail_len + 1);
		memcpy(l->s + at.col, s, n);
		l->len += (int)n;
		changed(t, at.line);
		end->line = at.line;
		end->col = at.col + (int)n;
		return 0;
	}
	tail = malloc((size_t)tail_len + 1);
	if (!tail || lines_open(t, at.line, k)) {
		free(tail);
		return -1;
	}
	l = &t->l[at.line];
	memcpy(tail, l->s + at.col, (size_t)tail_len + 1);
	l->len = at.col;
	l->s[at.col] = 0;
	p = s;
	for (int j = 0; j <= k; j++) {
		const char *nl = memchr(p, '\n', (size_t)(e - p));
		int m = (int)((nl ? nl : e) - p);
		struct tline *d = &t->l[at.line + j];
		int keep = j == k ? tail_len : 0;

		if (line_reserve(d, d->len + m + keep))
			goto fail;
		memcpy(d->s + d->len, p, (size_t)m);
		d->len += m;
		d->s[d->len] = 0;
		if (j == k) {
			end->line = at.line + j;
			end->col = d->len;
			memcpy(d->s + d->len, tail, (size_t)tail_len + 1);
			d->len += tail_len;
		}
		changed(t, at.line + j);
		p += m + 1;
	}
	free(tail);
	return 0;
fail:
	/* put the line back together and drop the half-made ones */
	l = &t->l[at.line];
	for (int j = 1; j <= k; j++)
		free(t->l[at.line + j].s), t->l[at.line + j].s = 0;
	lines_close(t, at.line, k);
	if (!line_reserve(l, at.col + tail_len)) {
		memcpy(l->s + at.col, tail, (size_t)tail_len + 1);
		l->len = at.col + tail_len;
	}
	free(tail);
	return -1;
}

static void raw_delete(struct text *t, struct pos a, struct pos b)
{
	struct tline *la = &t->l[a.line], *lb = &t->l[b.line];

	if (a.line == b.line) {
		memmove(la->s + a.col, la->s + b.col, (size_t)(la->len - b.col) + 1);
		la->len -= b.col - a.col;
	} else {
		int tail = lb->len - b.col;

		/* shrinking line a never fails: its tail fits where its old end was, or grows */
		if (line_reserve(la, a.col + tail))
			return;
		memcpy(la->s + a.col, lb->s + b.col, (size_t)tail + 1);
		la->len = a.col + tail;
		lines_close(t, a.line, b.line - a.line);
	}
	changed(t, a.line);
}

char *text_copy(const struct text *t, struct pos a, struct pos b, size_t *n)
{
	size_t len = 0;
	char *s, *p;

	for (int i = a.line; i <= b.line; i++) {
		int c0 = i == a.line ? a.col : 0, c1 = i == b.line ? b.col : t->l[i].len;

		len += (size_t)(c1 - c0) + (i < b.line);
	}
	s = malloc(len + 1);
	if (!s)
		return 0;
	p = s;
	for (int i = a.line; i <= b.line; i++) {
		int c0 = i == a.line ? a.col : 0, c1 = i == b.line ? b.col : t->l[i].len;

		memcpy(p, t->l[i].s + c0, (size_t)(c1 - c0));
		p += c1 - c0;
		if (i < b.line)
			*p++ = '\n';
	}
	*p = 0;
	*n = len;
	return s;
}

/* ---- undo ------------------------------------------------------------------ */

static size_t undo_bytes(const struct text *t)
{
	size_t n = 0;

	for (int i = 0; i < t->n_done; i++)
		n += t->ops[i].n;
	return n;
}

static int steps_done(const struct text *t)
{
	int n = 0;

	for (int i = 0; i < t->n_done; i++)
		n += i == 0 || t->ops[i].step != t->ops[i - 1].step;
	return n;
}

/* forget the oldest steps beyond the limits */
static void undo_trim(struct text *t)
{
	while (t->n_done && (steps_done(t) > UNDO_STEPS || undo_bytes(t) > UNDO_BYTES)) {
		unsigned first = t->ops[0].step;
		int k = 0;

		while (k < t->n_done && t->ops[k].step == first)
			free(t->ops[k++].s);
		if (t->saved == first || t->saved == 0)
			t->saved = (unsigned)-1;	/* the saved state can no longer be reached */
		memmove(t->ops, t->ops + k, (size_t)(t->n_ops - k) * sizeof(*t->ops));
		t->n_ops -= k;
		t->n_done -= k;
	}
}

static int record(struct text *t, int del, struct pos at, const char *s, size_t n)
{
	struct undo_op *o;

	if (!t->open)
		return 0;
	undo_clear(t, t->n_done);	/* a new edit ends what could be redone */
	if (t->n_ops == t->cap_ops) {
		int cap = t->cap_ops ? t->cap_ops * 2 : 64;
		struct undo_op *ops = realloc(t->ops, (size_t)cap * sizeof(*ops));

		if (!ops)
			return -1;
		t->ops = ops;
		t->cap_ops = cap;
	}
	o = &t->ops[t->n_ops];
	memset(o, 0, sizeof(*o));
	o->s = malloc(n + 1);
	if (!o->s)
		return -1;
	memcpy(o->s, s, n);
	o->s[n] = 0;
	o->n = n;
	o->del = del;
	o->at = at;
	o->step = t->cur;
	o->before = t->group_before;
	t->n_ops++;
	t->n_done = t->n_ops;
	return 0;
}

int text_insert(struct text *t, struct pos at, const char *s, size_t n, struct pos *end)
{
	struct pos e;

	if (!end)
		end = &e;
	*end = at;
	if (!n)
		return 0;
	if (record(t, 0, at, s, n))
		return -1;
	if (raw_insert(t, at, s, n, end)) {
		if (t->open) {
			free(t->ops[--t->n_ops].s);
			t->n_done = t->n_ops;
		}
		return -1;
	}
	return 0;
}

int text_delete(struct text *t, struct pos a, struct pos b)
{
	size_t n;
	char *s;

	if (pos_cmp(a, b) >= 0)
		return 0;
	if (t->open) {
		s = text_copy(t, a, b, &n);
		if (!s)
			return -1;
		if (record(t, 1, a, s, n)) {
			free(s);
			return -1;
		}
		free(s);
	}
	raw_delete(t, a, b);
	return 0;
}

void text_begin(struct text *t, enum group kind, struct pos caret)
{
	int merge = kind != G_OTHER && (int)kind == t->group_kind && t->n_done &&
		    t->ops[t->n_done - 1].step == t->cur && !pos_cmp(caret, t->group_end) &&
		    t->n_done == t->n_ops;

	t->open = 1;
	if (merge)
		return;
	t->cur = ++t->seq;
	t->group_kind = kind;
	t->group_before = caret;
}

void text_end(struct text *t, struct pos caret)
{
	t->open = 0;
	t->group_end = caret;
	for (int i = t->n_done - 1; i >= 0 && t->ops[i].step == t->cur; i--)
		t->ops[i].after = caret;
	undo_trim(t);
}

void text_break(struct text *t)
{
	t->group_kind = G_OTHER;
}

/* the step that applying the ops before index i leaves */
static unsigned step_before(const struct text *t, int i)
{
	return i > 0 ? t->ops[i - 1].step : 0;
}

int text_undo(struct text *t, struct pos *caret)
{
	unsigned step;

	if (!t->n_done)
		return 0;
	step = t->ops[t->n_done - 1].step;
	while (t->n_done && t->ops[t->n_done - 1].step == step) {
		struct undo_op *o = &t->ops[--t->n_done];
		struct pos end;

		if (o->del) {
			if (raw_insert(t, o->at, o->s, o->n, &end)) {
				t->n_done++;
				return 0;
			}
		} else {
			{
				struct pos b = o->at;
				const char *p = o->s;

				for (size_t i = 0; i < o->n; i++, p++) {
					if (*p == '\n') {
						b.line++;
						b.col = 0;
					} else {
						b.col++;
					}
				}
				raw_delete(t, o->at, b);
			}
		}
		*caret = o->before;
	}
	t->cur = step_before(t, t->n_done);
	t->group_kind = G_OTHER;
	return 1;
}

int text_redo(struct text *t, struct pos *caret)
{
	unsigned step;

	if (t->n_done == t->n_ops)
		return 0;
	step = t->ops[t->n_done].step;
	while (t->n_done < t->n_ops && t->ops[t->n_done].step == step) {
		struct undo_op *o = &t->ops[t->n_done];
		struct pos end;

		if (o->del) {
			struct pos b = o->at;

			for (size_t i = 0; i < o->n; i++) {
				if (o->s[i] == '\n') {
					b.line++;
					b.col = 0;
				} else {
					b.col++;
				}
			}
			raw_delete(t, o->at, b);
		} else if (raw_insert(t, o->at, o->s, o->n, &end)) {
			return 0;
		}
		t->n_done++;
		*caret = o->after;
	}
	t->cur = step;
	t->group_kind = G_OTHER;
	return 1;
}

int text_dirty(const struct text *t)
{
	return t->cur != t->saved;
}

void text_saved(struct text *t)
{
	t->saved = t->cur;
}
