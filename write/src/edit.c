/* SPDX-License-Identifier: MIT */
/* Write: editing a document (edit.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edit.h"

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

static int is_word(char c)
{
	unsigned char u = (unsigned char)c;

	return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' ||
	       u >= 0x80;
}

static struct pos P(int line, int col)
{
	struct pos p = { line, col };

	return p;
}

static int count_words(const char *s, int n)
{
	int w = 0;

	for (int i = 0; i < n; i++)
		w += !is_space(s[i]) && (i == 0 || is_space(s[i - 1]));
	return w;
}

/* UTF-8: the start of the character before / after byte k */
static int char_prev(const char *s, int k)
{
	if (k <= 0)
		return 0;
	k--;
	while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80)
		k--;
	return k;
}

static int char_next(const char *s, int n, int k)
{
	if (k >= n)
		return n;
	k++;
	while (k < n && ((unsigned char)s[k] & 0xc0) == 0x80)
		k++;
	return k;
}

/* ---- a line's visible text, copied out of the scratch parse ---------------- */

struct vbuf {
	char *vis;
	unsigned char *st;
	int n, cap;
	char prefix[24];	/* the block marker as it is in the source */
	enum blk kind;
	int number;
};

static int vb_reserve(struct vbuf *v, int need)
{
	int cap = v->cap ? v->cap : 64;
	char *vis;
	unsigned char *st;

	if (need + 1 <= v->cap)
		return 0;
	while (cap < need + 1)
		cap *= 2;
	vis = realloc(v->vis, (size_t)cap);
	if (vis)
		v->vis = vis;
	st = realloc(v->st, (size_t)cap);
	if (st)
		v->st = st;
	if (!vis || !st)
		return -1;
	v->cap = cap;
	return 0;
}

static int vb_put(struct vbuf *v, const char *s, const unsigned char *st, int style, int n)
{
	if (vb_reserve(v, v->n + n))
		return -1;
	memcpy(v->vis + v->n, s, (size_t)n);
	for (int i = 0; i < n; i++)
		v->st[v->n + i] = (unsigned char)(st ? st[i] & 7 : style);
	v->n += n;
	v->vis[v->n] = 0;
	return 0;
}

static void vb_free(struct vbuf *v)
{
	free(v->vis);
	free(v->st);
	memset(v, 0, sizeof(*v));
}

/* the block marker of line i, from the parse m of it */
static void vb_prefix(struct vbuf *v, const struct ed *e, int line, const struct mdline *m)
{
	int pl = m->prefix < (int)sizeof(v->prefix) ? m->prefix : (int)sizeof(v->prefix) - 1;

	memcpy(v->prefix, e->t.l[line].s, (size_t)pl);
	v->prefix[pl] = 0;
	v->kind = m->kind;
	v->number = m->number;
}

/* ---- basics ------------------------------------------------------------------ */

void ed_init(struct ed *e)
{
	memset(e, 0, sizeof(*e));
	text_init(&e->t);
	md_init(&e->m);
	md_init(&e->m2);
	e->tstyle = -1;
	e->indent = 4;
}

void ed_free(struct ed *e)
{
	text_free(&e->t);
	md_free(&e->m);
	md_free(&e->m2);
	memset(e, 0, sizeof(*e));
}

int ed_in_code(const struct ed *e, int line)
{
	int in = 0;

	for (int i = 0; i < line && i < e->t.n; i++)
		if (md_is_fence(e->t.l[i].s, e->t.l[i].len))
			in = !in;
	return in;
}

struct mdline *ed_parse(struct ed *e, int line)
{
	const struct tline *l = &e->t.l[line];

	return md_parse(&e->m, l->s, l->len, ed_in_code(e, line)) ? 0 : &e->m;
}

static int code_line(struct ed *e, int line)
{
	const struct tline *l = &e->t.l[line];

	return e->mode != M_RICH || md_is_fence(l->s, l->len) || ed_in_code(e, line);
}

int ed_vis_of(struct ed *e, struct pos p)
{
	struct mdline *m;

	if (e->mode != M_RICH)
		return p.col;
	m = ed_parse(e, p.line);
	return m ? md_vis(m, p.col) : p.col;
}

struct pos ed_pos_of(struct ed *e, int line, int k)
{
	struct mdline *m;

	if (line < 0)
		line = 0;
	if (line >= e->t.n)
		line = e->t.n - 1;
	if (k < 0)
		k = 0;
	if (e->mode != M_RICH)
		return P(line, k > e->t.l[line].len ? e->t.l[line].len : k);
	m = ed_parse(e, line);
	return P(line, m ? md_src(m, k) : (k > e->t.l[line].len ? e->t.l[line].len : k));
}

static struct pos norm(struct ed *e, struct pos p)
{
	if (p.line >= e->t.n)
		p = P(e->t.n - 1, e->t.l[e->t.n - 1].len);
	if (p.col > e->t.l[p.line].len)
		p.col = e->t.l[p.line].len;
	return e->mode == M_RICH ? ed_pos_of(e, p.line, ed_vis_of(e, p)) : p;
}

void ed_set_caret(struct ed *e, struct pos p, int select)
{
	if (select && !e->sel) {
		e->anchor = e->caret;
		e->sel = 1;
	}
	e->caret = norm(e, p);
	if (!select)
		e->sel = 0;
	if (e->sel && !pos_cmp(e->caret, e->anchor))
		e->sel = 0;
	e->tstyle = -1;
	text_break(&e->t);
}

void ed_caret_vis(struct ed *e, int line, int k, int select)
{
	ed_set_caret(e, ed_pos_of(e, line, k), select);
}

void ed_sel_range(const struct ed *e, struct pos *a, struct pos *b)
{
	if (!e->sel) {
		*a = *b = e->caret;
	} else if (pos_cmp(e->anchor, e->caret) < 0) {
		*a = e->anchor;
		*b = e->caret;
	} else {
		*a = e->caret;
		*b = e->anchor;
	}
}

int ed_guess_indent(const struct ed *e)
{
	int best = -1;

	for (int i = 0; i < e->t.n && i < 500; i++) {
		const struct tline *l = &e->t.l[i];
		int k = 0;

		if (l->len && l->s[0] == '\t')
			return 0;
		while (k < l->len && l->s[k] == ' ')
			k++;
		if (k && k < l->len && (best < 0 || k < best))
			best = k;
	}
	return best > 8 ? 8 : best;
}

int ed_load(struct ed *e, const char *data, size_t len, enum mode mode)
{
	if (text_set(&e->t, data, len))
		return -1;
	e->mode = mode;
	e->caret = e->anchor = P(0, 0);
	e->sel = 0;
	e->tstyle = -1;
	if (mode == M_CODE) {
		int g = ed_guess_indent(e);

		e->indent = g < 0 ? 4 : g;
	}
	e->caret = norm(e, e->caret);
	return 0;
}

void ed_set_mode(struct ed *e, enum mode mode)
{
	e->mode = mode;
	e->caret = norm(e, e->caret);
	e->anchor = norm(e, e->anchor);
	if (!pos_cmp(e->caret, e->anchor))
		e->sel = 0;
}

/* the inherited style at visible offset k */
static int style_before(const struct mdline *m, int k)
{
	if (!m->n)
		return 0;
	return (k > 0 ? m->st[k - 1] : m->st[0]) & 7;
}

int ed_style_at(struct ed *e)
{
	struct pos a, b;
	int all = 7, any = 0;

	if (e->mode != M_RICH)
		return 0;
	if (!e->sel) {
		struct mdline *m;

		if (e->tstyle >= 0)
			return e->tstyle;
		m = ed_parse(e, e->caret.line);
		return m && m->kind != B_CODE && m->kind != B_FENCE ? style_before(m, md_vis(m, e->caret.col)) : 0;
	}
	ed_sel_range(e, &a, &b);
	for (int i = a.line; i <= b.line; i++) {
		struct mdline *m = ed_parse(e, i);
		int k0, k1;

		if (!m || m->kind == B_CODE || m->kind == B_FENCE)
			continue;
		k0 = i == a.line ? md_vis(m, a.col) : 0;
		k1 = i == b.line ? md_vis(m, b.col) : m->n;
		for (int k = k0; k < k1; k++)
			if (!is_space(m->vis[k])) {
				all &= m->st[k];
				any = 1;
			}
	}
	return any ? all & 7 : 0;
}

enum blk ed_block_at(struct ed *e)
{
	struct mdline *m = ed_parse(e, e->caret.line);

	if (!m)
		return B_BODY;
	return m->kind == B_BLANK ? B_BODY : m->kind;
}

/* the fences decide which lines are code: when they change, every count does */
static unsigned fence_sig(const struct ed *e)
{
	unsigned h = 2166136261u;

	for (int i = 0; i < e->t.n; i++)
		if (md_is_fence(e->t.l[i].s, e->t.l[i].len))
			h = (h ^ (unsigned)i) * 16777619u;
	return h;
}

int ed_words(struct ed *e)
{
	static unsigned sig;
	unsigned s = fence_sig(e);
	int in = 0, w = 0;

	if (s != sig) {
		for (int i = 0; i < e->t.n; i++)
			e->t.l[i].words = -1;
		sig = s;
	}
	for (int i = 0; i < e->t.n; i++) {
		struct tline *l = &e->t.l[i];
		int fence = md_is_fence(l->s, l->len);

		if (l->words < 0) {
			if (e->mode == M_CODE || md_parse(&e->m, l->s, l->len, in))
				l->words = count_words(l->s, l->len);
			else
				l->words = count_words(e->m.vis, e->m.n);
		}
		w += l->words;
		if (fence)
			in = !in;
	}
	return w;
}

int ed_sel_words(struct ed *e)
{
	struct pos a, b;
	int w = 0;

	if (!e->sel)
		return 0;
	ed_sel_range(e, &a, &b);
	for (int i = a.line; i <= b.line; i++) {
		if (e->mode == M_RICH) {
			struct mdline *m = ed_parse(e, i);
			int k0, k1;

			if (!m)
				continue;
			k0 = i == a.line ? md_vis(m, a.col) : 0;
			k1 = i == b.line ? md_vis(m, b.col) : m->n;
			w += count_words(m->vis + k0, k1 - k0);
		} else {
			int c0 = i == a.line ? a.col : 0, c1 = i == b.line ? b.col : e->t.l[i].len;

			w += count_words(e->t.l[i].s + c0, c1 - c0);
		}
	}
	return w;
}

/* the text the caret moves over on a line: visible text in M_RICH */
static const char *walk_text(struct ed *e, int line, int *n)
{
	if (!code_line(e, line)) {
		struct mdline *m = ed_parse(e, line);

		if (m) {
			*n = m->n;
			return m->vis;
		}
	}
	*n = e->t.l[line].len;
	return e->t.l[line].s;
}

int ed_word_at(struct ed *e, char *out, int cap)
{
	int line = e->caret.line, n, k0, k1;
	const char *v;

	out[0] = 0;
	if (e->sel) {	/* the selection's part on the caret's line */
		struct pos a, b;

		ed_sel_range(e, &a, &b);
		v = walk_text(e, line, &n);
		k0 = a.line == line ? (code_line(e, line) ? a.col : md_vis(&e->m, a.col)) : 0;
		k1 = b.line == line ? (code_line(e, line) ? b.col : md_vis(&e->m, b.col)) : n;
	} else {
		v = walk_text(e, line, &n);
		k0 = k1 = code_line(e, line) ? e->caret.col : md_vis(&e->m, e->caret.col);
		while (k0 > 0 && is_word(v[k0 - 1]))
			k0--;
		while (k1 < n && is_word(v[k1]))
			k1++;
	}
	if (k1 - k0 >= cap)
		k1 = k0 + cap - 1;
	if (k1 < k0)
		k1 = k0;
	memcpy(out, v + k0, (size_t)(k1 - k0));
	out[k1 - k0] = 0;
	return k1 - k0;
}

/* ---- writing lines back ------------------------------------------------------ */

static int set_line(struct ed *e, int line, const char *s, int n)
{
	if (text_delete(&e->t, P(line, 0), P(line, e->t.l[line].len)))
		return -1;
	return text_insert(&e->t, P(line, 0), s, (size_t)n, 0);
}

/* make line show v (with v's block marker): keep the source if it already does */
static int fix_line(struct ed *e, int line, const struct vbuf *v)
{
	struct mdline *m = ed_parse(e, line);
	int cap, len, same, ret;
	char *out;

	if (!m)
		return -1;
	if (m->kind == B_CODE || m->kind == B_FENCE)
		return 0;
	same = m->n == v->n && !memcmp(m->vis, v->vis, (size_t)v->n) &&
	       (int)strlen(v->prefix) == m->prefix && !memcmp(e->t.l[line].s, v->prefix, (size_t)m->prefix);
	for (int i = 0; same && i < v->n; i++)
		same = (m->st[i] & 7) == v->st[i];
	if (same)
		return 0;
	cap = v->n * 6 + (int)sizeof(v->prefix) + 64;
	out = malloc((size_t)cap);
	if (!out)
		return -1;
	len = md_line_p(v->prefix, v->prefix[0] ? v->kind : B_BODY, v->vis, v->st, v->n, out, cap);
	ret = len < 0 || len >= cap ? -1 : set_line(e, line, out, len);
	free(out);
	return ret;
}

/* the visible text of line, its marker and parse fields, into v */
static int vb_line(struct ed *e, int line, struct vbuf *v)
{
	struct mdline *m = ed_parse(e, line);

	v->n = 0;
	if (!m)
		return -1;
	vb_prefix(v, e, line, m);
	return vb_put(v, m->vis, m->st, 0, m->n);
}

/* ---- typing -------------------------------------------------------------------- */

/*
 * Insert raw (source text) at the caret of a rich line, which should then show
 * vis (n bytes) there in styles sts, or all in style (-1: the caret's). With
 * convert, a line that shows less than that converted itself: closing
 * markers or a block marker were typed.
 */
static int rich_insert(struct ed *e, const char *raw, int rn, const char *vis, const unsigned char *sts,
		       int n, int style, int convert)
{
	int line = e->caret.line, k, col, ret = -1;
	struct vbuf v = { 0 }, want = { 0 };
	struct mdline *m;
	struct pos end;

	if (vb_line(e, line, &v))
		goto out;
	m = ed_parse(e, line);
	k = md_vis(m, e->caret.col);
	col = md_src(m, k);
	if (style < 0)
		style = style_before(m, k);
	memcpy(want.prefix, v.prefix, sizeof(v.prefix));
	want.kind = v.kind;
	want.number = v.number;
	if (vb_put(&want, v.vis, v.st, 0, k) || vb_put(&want, vis, sts, style, n) ||
	    vb_put(&want, v.vis + k, v.st + k, 0, v.n - k))
		goto out;
	if (text_insert(&e->t, P(line, col), raw, (size_t)rn, &end))
		goto out;
	m = ed_parse(e, line);
	if (!m)
		goto out;
	if (convert && m->n < want.n) {
		e->caret = ed_pos_of(e, line, md_vis(m, end.col));
		ret = 0;
		goto out;
	}
	if (fix_line(e, line, &want))
		goto out;
	e->caret = ed_pos_of(e, line, k + n);
	ret = 0;
out:
	vb_free(&v);
	vb_free(&want);
	return ret;
}

int ed_delete_sel(struct ed *e);
static int delete_sel(struct ed *e);

int ed_type(struct ed *e, const char *s, int n)
{
	int had_sel = e->sel, ret;

	text_begin(&e->t, had_sel ? G_OTHER : n == 1 && is_space(s[0]) ? G_SPACE : G_TYPE, e->caret);
	if (had_sel && delete_sel(e)) {
		text_end(&e->t, e->caret);
		return -1;
	}
	if (code_line(e, e->caret.line)) {
		struct pos end;

		ret = text_insert(&e->t, e->caret, s, (size_t)n, &end);
		if (!ret)
			e->caret = end;
	} else {
		ret = rich_insert(e, s, n, s, 0, n, e->tstyle, e->tstyle < 0);
	}
	text_end(&e->t, e->caret);
	return ret;
}

/* ---- deleting ------------------------------------------------------------------- */

/* the selection, inside an open undo step */
static int delete_sel(struct ed *e)
{
	struct pos a, b;
	struct vbuf v = { 0 }, w = { 0 };
	int ka, kb, ret = -1;

	ed_sel_range(e, &a, &b);
	e->sel = 0;
	if (code_line(e, a.line) && e->mode != M_RICH) {
		ret = text_delete(&e->t, a, b);
		if (!ret)
			e->caret = a;
		return ret;
	}
	if (vb_line(e, a.line, &v) || vb_line(e, b.line, &w))
		goto out;
	ka = ed_vis_of(e, a);
	kb = ed_vis_of(e, b);
	v.n = ka;
	if (vb_put(&v, w.vis + kb, w.st + kb, 0, w.n - kb))
		goto out;
	if (text_delete(&e->t, a, b))
		goto out;
	if (!code_line(e, a.line) && fix_line(e, a.line, &v))
		goto out;
	e->caret = ed_pos_of(e, a.line, ka);
	ret = 0;
out:
	vb_free(&v);
	vb_free(&w);
	return ret;
}

int ed_delete_sel(struct ed *e)
{
	int ret;

	if (!e->sel)
		return 0;
	text_begin(&e->t, G_OTHER, e->caret);
	ret = delete_sel(e);
	text_end(&e->t, e->caret);
	return ret;
}

/* delete visible bytes k0..k1 of the caret's line (M_RICH) */
static int rich_cut(struct ed *e, int line, int k0, int k1)
{
	struct vbuf v = { 0 };
	struct mdline *m;
	int c0, c1, ret = -1;

	if (vb_line(e, line, &v))
		goto out;
	m = ed_parse(e, line);
	c0 = m->src[k0];
	c1 = m->src[k1 - 1] + m->w[k1 - 1];
	memmove(v.vis + k0, v.vis + k1, (size_t)(v.n - k1));
	memmove(v.st + k0, v.st + k1, (size_t)(v.n - k1));
	v.n -= k1 - k0;
	v.vis[v.n] = 0;
	if (text_delete(&e->t, P(line, c0), P(line, c1)) || fix_line(e, line, &v))
		goto out;
	e->caret = ed_pos_of(e, line, k0);
	ret = 0;
out:
	vb_free(&v);
	return ret;
}

int ed_backspace(struct ed *e)
{
	int line = e->caret.line, ret = 0;

	if (e->sel)
		return ed_delete_sel(e);
	text_begin(&e->t, G_DELETE, e->caret);
	if (code_line(e, line)) {
		const struct tline *l = &e->t.l[line];
		int col = e->caret.col, from;

		if (!col) {
			if (line > 0) {
				struct pos j = P(line - 1, e->t.l[line - 1].len);

				ret = text_delete(&e->t, j, P(line, 0));
				if (!ret)
					e->caret = norm(e, j);
			}
		} else {
			int spaces = 1;

			for (int i = 0; i < col; i++)
				spaces &= l->s[i] == ' ';
			from = e->mode == M_CODE && spaces && e->indent > 0 ? col - ((col - 1) % e->indent + 1)
									    : char_prev(l->s, col);
			ret = text_delete(&e->t, P(line, from), e->caret);
			if (!ret)
				e->caret = P(line, from);
		}
	} else {
		struct mdline *m = ed_parse(e, line);
		int k = m ? md_vis(m, e->caret.col) : 0;

		if (!m) {
			ret = -1;
		} else if (k > 0) {
			ret = rich_cut(e, line, char_prev(m->vis, k), k);
		} else if (m->prefix) {
			ret = text_delete(&e->t, P(line, 0), P(line, m->prefix));
			if (!ret)
				e->caret = ed_pos_of(e, line, 0);
		} else if (line > 0) {
			struct pos j = P(line - 1, e->t.l[line - 1].len);

			ret = text_delete(&e->t, j, P(line, 0));
			if (!ret)
				e->caret = norm(e, j);
		}
	}
	text_end(&e->t, e->caret);
	return ret;
}

int ed_delete(struct ed *e)
{
	int line = e->caret.line, ret = 0;

	if (e->sel)
		return ed_delete_sel(e);
	text_begin(&e->t, G_DELETE, e->caret);
	if (code_line(e, line) || e->caret.col == e->t.l[line].len) {
		const struct tline *l = &e->t.l[line];

		if (e->caret.col < l->len) {
			ret = text_delete(&e->t, e->caret, P(line, char_next(l->s, l->len, e->caret.col)));
		} else if (line + 1 < e->t.n) {
			int skip = 0;

			if (!code_line(e, line + 1)) {	/* the next line's marker goes */
				struct mdline *m = ed_parse(e, line + 1);

				skip = m ? m->prefix : 0;
			}
			ret = text_delete(&e->t, e->caret, P(line + 1, skip));
		}
	} else {
		struct mdline *m = ed_parse(e, line);
		int k = m ? md_vis(m, e->caret.col) : 0;

		ret = !m ? -1 : k < m->n ? rich_cut(e, line, k, char_next(m->vis, m->n, k)) : 0;
	}
	text_end(&e->t, e->caret);
	return ret;
}

/* ---- new lines -------------------------------------------------------------------- */

static void renumber(struct ed *e, int line)
{
	struct mdline *m = ed_parse(e, line);
	int prev;

	if (!m || m->kind != B_NUMBER)
		return;
	prev = m->number;
	for (int i = line + 1; i < e->t.n; i++) {
		char buf[16];
		int digits = 0, n;

		m = ed_parse(e, i);
		if (!m || m->kind != B_NUMBER)
			break;
		if (m->number != prev + 1) {
			while (digits < e->t.l[i].len && e->t.l[i].s[digits] >= '0' && e->t.l[i].s[digits] <= '9')
				digits++;
			n = snprintf(buf, sizeof(buf), "%d", prev + 1);
			text_delete(&e->t, P(i, 0), P(i, digits));
			text_insert(&e->t, P(i, 0), buf, (size_t)n, 0);
		}
		prev++;
	}
}

static int code_newline(struct ed *e)
{
	const struct tline *l = &e->t.l[e->caret.line];
	char buf[128];
	int ws = 0, n, last = e->caret.col - 1;
	struct pos end;

	buf[0] = '\n';
	while (ws < l->len && ws < e->caret.col && is_space(l->s[ws]) && ws < 100)
		ws++;
	memcpy(buf + 1, l->s, (size_t)ws);
	n = 1 + ws;
	while (last >= 0 && is_space(l->s[last]))
		last--;
	if (e->mode == M_CODE && last >= 0 && strchr(":{[(", l->s[last])) {
		if (!e->indent) {
			buf[n++] = '\t';
		} else {
			for (int i = 0; i < e->indent; i++)
				buf[n++] = ' ';
		}
	}
	if (text_insert(&e->t, e->caret, buf, (size_t)n, &end))
		return -1;
	e->caret = end;
	return 0;
}

static int rich_newline(struct ed *e)
{
	int line = e->caret.line, k, ret = -1;
	struct vbuf left = { 0 }, right = { 0 };
	struct mdline *m;
	char np[24] = "", ins[32];
	enum blk nk = B_BODY;

	if (vb_line(e, line, &left))
		goto out;
	m = ed_parse(e, line);
	k = md_vis(m, e->caret.col);
	if ((left.kind == B_BULLET || left.kind == B_NUMBER || left.kind == B_QUOTE) && !left.n) {
		ret = set_line(e, line, "", 0);	/* Enter on an empty item ends the list */
		e->caret = P(line, 0);
		goto out;
	}
	if (left.kind >= B_H1 && left.kind <= B_H3 && k == 0 && left.n) {
		ret = text_insert(&e->t, P(line, 0), "\n", 1, 0);
		e->caret = ed_pos_of(e, line + 1, 0);
		goto out;
	}
	if (left.kind == B_BULLET || left.kind == B_QUOTE) {
		snprintf(np, sizeof(np), "%s", left.prefix);
		nk = left.kind;
	} else if (left.kind == B_NUMBER) {
		size_t d = strspn(left.prefix, "0123456789");

		snprintf(np, sizeof(np), "%d%c ", left.number + 1, left.prefix[d] ? left.prefix[d] : '.');
		nk = B_NUMBER;
	}
	memcpy(right.prefix, np, sizeof(np));
	right.kind = nk;
	if (vb_put(&right, left.vis + k, left.st + k, 0, left.n - k))
		goto out;
	left.n = k;
	snprintf(ins, sizeof(ins), "\n%s", np);
	if (text_insert(&e->t, P(line, md_src(m, k)), ins, strlen(ins), 0))
		goto out;
	if (fix_line(e, line, &left) || fix_line(e, line + 1, &right))
		goto out;
	if (nk == B_NUMBER)
		renumber(e, line + 1);
	e->caret = ed_pos_of(e, line + 1, 0);
	ret = 0;
out:
	vb_free(&left);
	vb_free(&right);
	return ret;
}

int ed_newline(struct ed *e)
{
	int ret = 0;

	text_begin(&e->t, G_OTHER, e->caret);
	if (e->sel)
		ret = delete_sel(e);
	if (!ret)
		ret = code_line(e, e->caret.line) ? code_newline(e) : rich_newline(e);
	text_end(&e->t, e->caret);
	return ret;
}

int ed_tab(struct ed *e)
{
	char buf[16];
	int n;

	if (e->mode == M_RICH)
		return 0;
	if (e->mode == M_SOURCE || !e->indent) {
		buf[0] = '\t';
		n = 1;
	} else {
		n = e->indent - e->caret.col % e->indent;
		memset(buf, ' ', (size_t)n);
	}
	return ed_type(e, buf, n);
}

/* ---- the clipboard ------------------------------------------------------------------- */

char *ed_copy(struct ed *e, size_t *n, int *words)
{
	struct pos a, b;
	char *out = 0;
	size_t len = 0, cap = 0;

	*n = 0;
	*words = 0;
	if (!e->sel)
		return 0;
	*words = ed_sel_words(e);
	ed_sel_range(e, &a, &b);
	if (e->mode != M_RICH)
		return text_copy(&e->t, a, b, n);
	for (int i = a.line; i <= b.line; i++) {
		const struct tline *l = &e->t.l[i];
		struct mdline *m = ed_parse(e, i);
		const char *piece;
		char *tmp = 0;
		int pn;

		if (!m)
			goto fail;
		if (m->kind == B_CODE || m->kind == B_FENCE) {
			int c0 = i == a.line ? a.col : 0, c1 = i == b.line ? b.col : l->len;

			piece = l->s + c0;
			pn = c1 - c0;
		} else {
			int k0 = i == a.line ? md_vis(m, a.col) : 0, k1 = i == b.line ? md_vis(m, b.col) : m->n;

			if (k0 == 0 && k1 == m->n) {
				piece = l->s;
				pn = l->len;
			} else {
				int tc = (k1 - k0) * 6 + m->prefix + 64;

				tmp = malloc((size_t)tc);
				if (!tmp)
					goto fail;
				memcpy(tmp, l->s, (size_t)(k0 ? 0 : m->prefix));
				pn = k0 ? 0 : m->prefix;
				pn += md_inline(m->vis + k0, m->st + k0, k1 - k0, tmp + pn, tc - pn);
				piece = tmp;
			}
		}
		if (len + (size_t)pn + 2 > cap) {
			char *o;

			cap = (len + (size_t)pn + 2) * 2;
			o = realloc(out, cap);
			if (!o) {
				free(tmp);
				goto fail;
			}
			out = o;
		}
		memcpy(out + len, piece, (size_t)pn);
		len += (size_t)pn;
		if (i < b.line)
			out[len++] = '\n';
		out[len] = 0;
		free(tmp);
	}
	*n = len;
	return out;
fail:
	free(out);
	return 0;
}

char *ed_cut(struct ed *e, size_t *n, int *words)
{
	char *c = ed_copy(e, n, words);

	if (c && ed_delete_sel(e)) {
		free(c);
		return 0;
	}
	return c;
}

int ed_paste(struct ed *e, const char *s, size_t n)
{
	int ret = 0;

	if (!n)
		return 0;
	text_begin(&e->t, G_OTHER, e->caret);
	if (e->sel)
		ret = delete_sel(e);
	if (ret)
		goto out;
	if (code_line(e, e->caret.line) || memchr(s, '\n', n)) {
		struct pos end;

		ret = text_insert(&e->t, e->caret, s, n, &end);
		if (!ret)
			e->caret = norm(e, end);
	} else {
		/* the clipboard's styles, or plain text in the caret's style */
		char *copy = malloc(n + 3);
		int any = 0;

		if (!copy) {
			ret = -1;
			goto out;
		}
		memcpy(copy, "x ", 2);
		memcpy(copy + 2, s, n);
		copy[n + 2] = 0;
		if (md_parse(&e->m2, copy, (int)n + 2, 0)) {
			free(copy);
			ret = -1;
			goto out;
		}
		for (int i = 2; i < e->m2.n; i++)
			any |= e->m2.st[i] & 7;
		{
			int cn = e->m2.n - 2;
			char *cv = malloc((size_t)cn + 1);
			unsigned char *cs = malloc((size_t)cn + 1);

			if (!cv || !cs) {
				ret = -1;
			} else {
				memcpy(cv, e->m2.vis + 2, (size_t)cn);
				memcpy(cs, e->m2.st + 2, (size_t)cn);
				/* insert the source, then check it shows as cv */
				ret = rich_insert(e, s, (int)n, cv, any ? cs : 0, cn, -1, 0);
			}
			free(cv);
			free(cs);
		}
		free(copy);
	}
out:
	text_end(&e->t, e->caret);
	return ret;
}

/* ---- styles ------------------------------------------------------------------------ */

int ed_toggle(struct ed *e, int flag, int word)
{
	struct pos a, b;
	int ka, kc, all = 1, any = 0, ret = 0, line_a, line_c;
	struct vbuf v = { 0 };

	if (e->mode != M_RICH)
		return 0;
	if (!e->sel) {
		struct mdline *m = ed_parse(e, e->caret.line);
		int k0, k1;

		if (!m)
			return -1;
		k0 = k1 = md_vis(m, e->caret.col);
		while (word && k0 > 0 && is_word(m->vis[k0 - 1]))
			k0--;
		while (word && k1 < m->n && is_word(m->vis[k1]))
			k1++;
		if (k0 == k1 || m->kind == B_CODE || m->kind == B_FENCE) {
			e->tstyle = ed_style_at(e) ^ flag;
			return 0;
		}
		a = P(e->caret.line, md_src(m, k0));
		b = P(e->caret.line, md_src(m, k1));
	} else {
		ed_sel_range(e, &a, &b);
	}
	for (int i = a.line; i <= b.line; i++) {
		struct mdline *m = ed_parse(e, i);
		int k0, k1;

		if (!m)
			return -1;
		if (m->kind == B_CODE || m->kind == B_FENCE)
			continue;
		k0 = i == a.line ? md_vis(m, a.col) : 0;
		k1 = i == b.line ? md_vis(m, b.col) : m->n;
		for (int k = k0; k < k1; k++)
			if (!is_space(m->vis[k])) {
				any = 1;
				all &= (m->st[k] & flag) != 0;
			}
	}
	if (!any)
		return 0;
	/* where the selection is, in visible offsets, to put it back */
	line_a = e->anchor.line;
	line_c = e->caret.line;
	ka = ed_vis_of(e, e->anchor);
	kc = ed_vis_of(e, e->caret);
	text_begin(&e->t, G_OTHER, e->caret);
	for (int i = a.line; i <= b.line && !ret; i++) {
		struct mdline *m;
		int k0, k1;

		if (code_line(e, i) || vb_line(e, i, &v)) {
			ret = code_line(e, i) ? 0 : -1;
			continue;
		}
		m = ed_parse(e, i);
		k0 = i == a.line ? md_vis(m, a.col) : 0;
		k1 = i == b.line ? md_vis(m, b.col) : m->n;
		for (int k = k0; k < k1; k++)
			v.st[k] = (unsigned char)(all ? v.st[k] & ~flag : v.st[k] | flag);
		ret = fix_line(e, i, &v);
	}
	if (e->sel)
		e->anchor = ed_pos_of(e, line_a, ka);
	e->caret = ed_pos_of(e, line_c, kc);
	text_end(&e->t, e->caret);
	vb_free(&v);
	return ret;
}

/* ---- paragraphs ---------------------------------------------------------------------- */

static int insert_line(struct ed *e, int after, const char *s)	/* a new line after line `after` */
{
	char buf[16];
	int n = snprintf(buf, sizeof(buf), "\n%s", s);

	return text_insert(&e->t, P(after, e->t.l[after].len), buf, (size_t)n, 0);
}

static int drop_line(struct ed *e, int line)
{
	if (line + 1 < e->t.n)
		return text_delete(&e->t, P(line, 0), P(line + 1, 0));
	if (line > 0)
		return text_delete(&e->t, P(line - 1, e->t.l[line - 1].len), P(line, e->t.l[line].len));
	return set_line(e, line, "", 0);
}

int ed_block(struct ed *e, enum blk kind, int toggle)
{
	struct pos a, b;
	int la, lc, ka, kc, ret = 0, shift = 0, all = 1, num = 1;

	if (e->mode == M_CODE)
		return 0;
	ed_sel_range(e, &a, &b);
	if (e->sel && b.col == 0 && b.line > a.line)
		b.line--;
	la = e->anchor.line;
	lc = e->caret.line;
	ka = ed_vis_of(e, e->anchor);
	kc = ed_vis_of(e, e->caret);
	text_begin(&e->t, G_OTHER, e->caret);
	if (kind == B_CODE) {
		int inside = 1;

		for (int i = a.line; i <= b.line; i++)
			inside &= ed_in_code(e, i) && !md_is_fence(e->t.l[i].s, e->t.l[i].len);
		if (!inside) {
			ret = text_insert(&e->t, P(a.line, 0), "```\n", 4, 0);
			if (!ret)
				ret = insert_line(e, b.line + 1, "```");
			shift = 1;
		}
		goto done;
	}
	if (ed_in_code(e, a.line) && !md_is_fence(e->t.l[a.line].s, e->t.l[a.line].len)) {
		int f0 = a.line, f1 = a.line;

		while (f0 > 0 && !md_is_fence(e->t.l[f0].s, e->t.l[f0].len))
			f0--;
		while (f1 < e->t.n && !md_is_fence(e->t.l[f1].s, e->t.l[f1].len))
			f1++;
		if (b.line >= f1)
			b.line = f1 - 1;
		/* the end first, so the lines above keep their numbers */
		if (f1 < e->t.n) {
			if (b.line < f1 - 1)
				ret = insert_line(e, b.line, "```");
			else
				ret = drop_line(e, f1);
		} else if (b.line < f1 - 1) {
			ret = insert_line(e, b.line, "```");
		}
		if (!ret && a.line > f0 + 1) {
			ret = insert_line(e, a.line - 1, "```");
			shift = 1;
		} else if (!ret) {
			ret = drop_line(e, f0);
			shift = -1;
		}
		a.line += shift;
		b.line += shift;
	}
	if (ret)
		goto done;
	for (int i = a.line; i <= b.line; i++) {
		struct mdline *m = ed_parse(e, i);

		if (m && m->kind != B_CODE && m->kind != B_FENCE && m->kind != kind)
			all = 0;
	}
	if (toggle && all)
		kind = B_BODY;
	if (kind == B_NUMBER && a.line > 0) {
		struct mdline *m = ed_parse(e, a.line - 1);

		if (m && m->kind == B_NUMBER)
			num = m->number + 1;
	}
	for (int i = a.line; i <= b.line && !ret; i++) {
		struct mdline *m = ed_parse(e, i);
		char pb[16];
		const char *p;
		int pl;

		if (!m) {
			ret = -1;
			break;
		}
		if (m->kind == B_CODE || m->kind == B_FENCE)
			continue;
		pl = m->prefix;
		p = md_prefix(kind, num++, pb);
		ret = text_delete(&e->t, P(i, 0), P(i, pl));
		if (!ret)
			ret = text_insert(&e->t, P(i, 0), p, strlen(p), 0);
	}
	if (!ret)
		renumber(e, b.line);
done:
	if (e->sel)
		e->anchor = ed_pos_of(e, la + shift, ka);
	e->caret = ed_pos_of(e, lc + shift, kc);
	text_end(&e->t, e->caret);
	return ret;
}

/* replace visible k0..k1 of line with s (find and replace); the caller groups undo */
int ed_replace_range(struct ed *e, int line, int k0, int k1, const char *s, int n)
{
	struct mdline *m;
	int style;

	e->sel = 0;
	if (code_line(e, line)) {
		struct pos end;

		if (text_delete(&e->t, P(line, k0), P(line, k1)) ||
		    text_insert(&e->t, P(line, k0), s, (size_t)n, &end))
			return -1;
		e->caret = end;
		return 0;
	}
	m = ed_parse(e, line);
	if (!m)
		return -1;
	style = k0 < m->n ? m->st[k0] & 7 : style_before(m, k0);
	if (k1 > k0 && rich_cut(e, line, k0, k1))
		return -1;
	e->caret = ed_pos_of(e, line, k0);
	return rich_insert(e, s, n, s, 0, n, style, 0);
}

/* ---- undo ------------------------------------------------------------------------------ */

int ed_undo(struct ed *e)
{
	struct pos c;

	if (!text_undo(&e->t, &c))
		return 0;
	e->sel = 0;
	e->tstyle = -1;
	e->caret = norm(e, c);
	return 1;
}

int ed_redo(struct ed *e)
{
	struct pos c;

	if (!text_redo(&e->t, &c))
		return 0;
	e->sel = 0;
	e->tstyle = -1;
	e->caret = norm(e, c);
	return 1;
}

/* ---- moving --------------------------------------------------------------------------- */

void ed_move(struct ed *e, enum move how, int select)
{
	int line = e->caret.line, n, k;
	const char *s;

	if (e->sel && !select && (how == MV_LEFT || how == MV_RIGHT)) {
		struct pos a, b;

		ed_sel_range(e, &a, &b);
		ed_set_caret(e, how == MV_LEFT ? a : b, 0);
		return;
	}
	k = code_line(e, line) ? e->caret.col : ed_vis_of(e, e->caret);
	s = walk_text(e, line, &n);
	switch (how) {
	case MV_LEFT:
		if (k > 0) {
			k = char_prev(s, k);
		} else if (line > 0) {
			line--;
			walk_text(e, line, &k);
		}
		break;
	case MV_RIGHT:
		if (k < n) {
			k = char_next(s, n, k);
		} else if (line + 1 < e->t.n) {
			line++;
			k = 0;
		}
		break;
	case MV_WORD_LEFT:
		if (k == 0 && line > 0) {
			line--;
			walk_text(e, line, &k);
			break;
		}
		while (k > 0 && !is_word(s[k - 1]))
			k--;
		while (k > 0 && is_word(s[k - 1]))
			k--;
		break;
	case MV_WORD_RIGHT:
		if (k == n && line + 1 < e->t.n) {
			line++;
			k = 0;
			break;
		}
		while (k < n && !is_word(s[k]))
			k++;
		while (k < n && is_word(s[k]))
			k++;
		break;
	case MV_HOME:
		k = 0;
		break;
	case MV_END:
		k = n;
		break;
	case MV_DOC_START:
		line = 0;
		k = 0;
		break;
	case MV_DOC_END:
		line = e->t.n - 1;
		walk_text(e, line, &k);
		break;
	}
	ed_set_caret(e, code_line(e, line) ? P(line, k) : ed_pos_of(e, line, k), select);
}

void ed_select_all(struct ed *e)
{
	int n;

	ed_set_caret(e, P(0, 0), 0);
	walk_text(e, e->t.n - 1, &n);
	ed_set_caret(e, code_line(e, e->t.n - 1) ? P(e->t.n - 1, n) : ed_pos_of(e, e->t.n - 1, n), 1);
}
