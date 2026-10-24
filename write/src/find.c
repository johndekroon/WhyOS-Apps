/* SPDX-License-Identifier: MIT */
/* Write: find and replace (find.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "find.h"

static char fold(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static int is_word(char c)
{
	unsigned char u = (unsigned char)c;

	return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' ||
	       u >= 0x80;
}

void find_init(struct find *f)
{
	memset(f, 0, sizeof(*f));
	f->cur = -1;
}

void find_free(struct find *f)
{
	free(f->m);
	memset(f, 0, sizeof(*f));
	f->cur = -1;
}

/* the text a match is counted in: what the caret walks over */
static const char *line_text(struct ed *e, int line, int *n)
{
	const struct tline *l = &e->t.l[line];

	if (e->mode == M_RICH && !md_is_fence(l->s, l->len)) {
		struct mdline *m = ed_parse(e, line);

		if (m && m->kind != B_CODE) {
			*n = m->n;
			return m->vis;
		}
	}
	*n = l->len;
	return l->s;
}

static int add(struct find *f, int line, int k0, int k1)
{
	if (f->n >= FIND_MAX) {
		f->more = 1;
		return -1;
	}
	if (f->n == f->cap) {
		int cap = f->cap ? f->cap * 2 : 64;
		struct match *m = realloc(f->m, (size_t)cap * sizeof(*m));

		if (!m)
			return -1;
		f->m = m;
		f->cap = cap;
	}
	f->m[f->n++] = (struct match){ line, k0, k1 };
	return 0;
}

int find_run(struct find *f, struct ed *e)
{
	int ql = (int)strlen(f->q);

	f->n = 0;
	f->more = 0;
	if (ql) {
		/* the code state goes line by line; ed_parse works it out per line */
		for (int i = 0; i < e->t.n; i++) {
			int n;
			const char *s = line_text(e, i, &n);

			for (int k = 0; k + ql <= n; k++) {
				int j = 0;

				if (f->match_case)
					while (j < ql && s[k + j] == f->q[j])
						j++;
				else
					while (j < ql && fold(s[k + j]) == fold(f->q[j]))
						j++;
				if (j < ql)
					continue;
				if (f->whole_word && ((k > 0 && is_word(s[k - 1])) || (k + ql < n && is_word(s[k + ql]))))
					continue;
				if (add(f, i, k, k + ql))
					goto done;
				k += ql - 1;
			}
		}
	}
done:
	if (f->cur >= f->n)
		f->cur = f->n ? f->n - 1 : -1;
	if (!f->n)
		f->cur = -1;
	return f->n;
}

static int caret_k(struct ed *e, struct pos p)
{
	const struct tline *l = &e->t.l[p.line];

	if (e->mode == M_RICH && !md_is_fence(l->s, l->len) && !ed_in_code(e, p.line))
		return ed_vis_of(e, p);
	return p.col;
}

void find_nearest(struct find *f, struct ed *e)
{
	struct pos a, b;
	int k;

	ed_sel_range(e, &a, &b);
	k = caret_k(e, a);
	f->cur = f->n ? 0 : -1;
	for (int i = 0; i < f->n; i++)
		if (f->m[i].line > a.line || (f->m[i].line == a.line && f->m[i].k0 >= k)) {
			f->cur = i;
			break;
		}
}

static struct pos pos_k(struct ed *e, int line, int k)
{
	const struct tline *l = &e->t.l[line];
	struct pos p = { line, k };

	if (e->mode == M_RICH && !md_is_fence(l->s, l->len) && !ed_in_code(e, line))
		p = ed_pos_of(e, line, k);
	return p;
}

void find_select(struct find *f, struct ed *e)
{
	const struct match *m;

	if (f->cur < 0 || f->cur >= f->n)
		return;
	m = &f->m[f->cur];
	ed_set_caret(e, pos_k(e, m->line, m->k0), 0);
	ed_set_caret(e, pos_k(e, m->line, m->k1), 1);
}

void find_step(struct find *f, struct ed *e, int dir)
{
	if (!f->n)
		return;
	if (f->cur < 0)
		f->cur = 0;
	else
		f->cur = (f->cur + dir + f->n) % f->n;
	find_select(f, e);
}

void find_count(const struct find *f, char *out, int n)
{
	if (!f->n)
		snprintf(out, (size_t)n, "no matches");
	else if (f->more)
		snprintf(out, (size_t)n, "%d of %d+", f->cur + 1, f->n);
	else
		snprintf(out, (size_t)n, "%d of %d", f->cur < 0 ? 0 : f->cur + 1, f->n);
}

void find_case(char *out, int n, const char *match, const char *repl, int match_case)
{
	snprintf(out, (size_t)n, "%s", repl);
	if (!match_case && match[0] >= 'A' && match[0] <= 'Z' && out[0] >= 'a' && out[0] <= 'z')
		out[0] = (char)(out[0] - 32);
}

static int replace_at(struct find *f, struct ed *e, int i, const char *repl)
{
	const struct match *m = &f->m[i];
	char matched[sizeof(f->q)], out[256];
	int n;
	const char *s = line_text(e, m->line, &n);

	snprintf(matched, sizeof(matched), "%.*s", m->k1 - m->k0, s + m->k0);
	find_case(out, sizeof(out), matched, repl, f->match_case);
	return ed_replace_range(e, m->line, m->k0, m->k1, out, (int)strlen(out));
}

int find_replace(struct find *f, struct ed *e, const char *repl)
{
	int ret;

	if (f->cur < 0 || f->cur >= f->n)
		return 0;
	text_begin(&e->t, G_OTHER, e->caret);
	ret = replace_at(f, e, f->cur, repl);
	text_end(&e->t, e->caret);
	if (ret)
		return -1;
	find_run(f, e);
	/* the next match after the replaced text */
	f->cur = f->n ? 0 : -1;
	for (int i = 0; i < f->n; i++)
		if (f->m[i].line > e->caret.line ||
		    (f->m[i].line == e->caret.line && f->m[i].k0 >= caret_k(e, e->caret))) {
			f->cur = i;
			break;
		}
	if (f->cur >= 0)
		find_select(f, e);
	return 1;
}

int find_replace_all(struct find *f, struct ed *e, const char *repl)
{
	int done = 0;

	if (!f->n)
		return 0;
	text_begin(&e->t, G_OTHER, e->caret);
	/* from the end, so the offsets of the matches before stay right */
	for (int i = f->n - 1; i >= 0; i--)
		if (!replace_at(f, e, i, repl))
			done++;
	text_end(&e->t, e->caret);
	find_run(f, e);
	f->cur = -1;
	return done;
}
