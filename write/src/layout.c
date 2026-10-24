/* SPDX-License-Identifier: MIT */
/* Write: laying out lines into rows (layout.h). */
#include <stdlib.h>
#include <string.h>

#include "layout.h"

static unsigned char *cls_buf;	/* syntax classes of one line: only the state is kept */
static int cls_cap;

void lay_init(struct lay *L)
{
	memset(L, 0, sizeof(*L));
	L->width = 600;
}

void lay_free(struct lay *L)
{
	free(L->ys);
	memset(L, 0, sizeof(*L));
}

void lay_style(const struct lay *L, enum blk kind, int in_code, struct lstyle *s)
{
	int sm = L->small;

	memset(s, 0, sizeof(*s));
	(void)in_code;
	if (L->mode == M_SOURCE) {
		s->font = s->bold_font = sm ? FONT_MONO18 : FONT_MONO20;
		s->lh = sm ? 26 : 28;
		s->mono = 1;
		return;
	}
	if (L->mode == M_CODE || kind == B_CODE) {
		s->font = s->bold_font = sm ? FONT_MONO16 : FONT_MONO18;
		s->lh = sm ? 24 : 27;
		s->mono = 1;
		s->indent = L->mode == M_CODE ? 0 : 12;
		return;
	}
	switch (kind) {
	case B_FENCE:
		s->font = s->bold_font = FONT_MONO14;
		s->lh = 24;
		s->mono = 1;
		return;
	case B_H1:
		s->font = s->bold_font = sm ? FONT_UI28B : FONT_UI36B;
		s->lh = sm ? 36 : 44;
		s->after = 6;
		return;
	case B_H2:
		s->font = s->bold_font = sm ? FONT_UI24B : FONT_UI28B;
		s->lh = sm ? 32 : 36;
		s->after = 4;
		return;
	case B_H3:
		s->font = s->bold_font = sm ? FONT_UI20B : FONT_UI24B;
		s->lh = sm ? 28 : 32;
		s->after = 4;
		return;
	case B_BLANK:
		s->font = s->bold_font = sm ? FONT_UI20 : FONT_UI24;
		s->lh = sm ? 14 : 18;
		return;
	default:
		s->font = sm ? FONT_UI20 : FONT_UI24;
		s->bold_font = sm ? FONT_UI20B : FONT_UI24B;
		s->lh = (sm ? 28 : 32) + (L->full ? 2 : 0);
		if (kind == B_BULLET || kind == B_NUMBER || kind == B_QUOTE)
			s->indent = sm ? 28 : 32;
		return;
	}
}

int lay_font_of(const struct lstyle *s, int style)
{
	return (style & ST_B) ? s->bold_font : s->font;
}

static unsigned decode(const char *s, int n, int *k)
{
	const unsigned char *p = (const unsigned char *)s + *k;
	unsigned cp = p[0];
	int len = 1;

	if (cp >= 0xf0 && *k + 4 <= n) {
		cp = ((cp & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63);
		len = 4;
	} else if (cp >= 0xe0 && *k + 3 <= n) {
		cp = ((cp & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63);
		len = 3;
	} else if (cp >= 0xc0 && *k + 2 <= n) {
		cp = ((cp & 31) << 6) | (p[1] & 63);
		len = 2;
	}
	*k += len;
	return cp;
}

static int adv(struct lay *L, int font, unsigned cp)
{
	if (cp == '\t')
		return 4 * L->adv16(L->ctx, font, ' ');
	return L->adv16(L->ctx, font, cp);
}

const char *lay_text(struct lay *L, struct ed *e, int line, int state, int *n, enum blk *kind,
		     const unsigned char **st)
{
	const struct tline *l = &e->t.l[line];

	if (L->mode == M_RICH && !md_parse(&e->m, l->s, l->len, state & 1)) {
		*n = e->m.n;
		*kind = e->m.kind;
		*st = e->m.st;
		return e->m.vis;
	}
	*n = l->len;
	*kind = B_BODY;
	*st = 0;
	return l->s;
}

/* the indent of a numbered item: wide enough for its number */
static int number_indent(struct lay *L, const struct ed *e, int line, const struct lstyle *s)
{
	const char *p = e->t.l[line].s;
	int w = 0;

	for (; *p >= '0' && *p <= '9'; p++)
		w += L->adv16(L->ctx, FONT_MONO20, (unsigned char)*p);
	w += L->adv16(L->ctx, FONT_MONO20, '.');
	w = (w + 8) / 16 + 14;
	return w > s->indent ? w : s->indent;
}

static int row_setup(struct lay *L, struct ed *e, int line, int state, const char **text, int *n,
		     const unsigned char **st, struct lstyle *s)
{
	enum blk kind;

	*text = lay_text(L, e, line, state, n, &kind, st);
	lay_style(L, kind, state & 1, s);
	if (kind == B_NUMBER && L->mode == M_RICH)
		s->indent = number_indent(L, e, line, s);
	return kind;
}

int lay_rows(struct lay *L, struct ed *e, int line, int state, struct row *rows, int max, int *h)
{
	const char *text;
	const unsigned char *st;
	struct lstyle s;
	int n, k = 0, k0 = 0, x16 = 0, last_break = -1, nr = 0, avail;

	row_setup(L, e, line, state, &text, &n, &st, &s);
	avail = (L->width - s.indent) * 16;
	if (avail < 40 * 16)
		avail = 40 * 16;
	while (k < n && nr < max - 1) {
		int kn = k;
		unsigned cp = decode(text, n, &kn);
		int w = adv(L, lay_font_of(&s, st ? st[k] : 0), cp);

		if (x16 + w > avail && k > k0) {
			int end;

			if (cp == ' ')
				end = kn;		/* a space at the edge hangs past it */
			else if (last_break > k0)
				end = last_break;
			else
				end = k;
			rows[nr++] = (struct row){ k0, end, 0, s.lh, s.indent };
			k0 = k = end;
			x16 = 0;
			last_break = -1;
			continue;
		}
		x16 += w;
		if (cp == ' ' || cp == '\t')
			last_break = kn;
		k = kn;
	}
	rows[nr++] = (struct row){ k0, n, 0, s.lh, s.indent };	/* the rest, also past ROWS_MAX */
	for (int i = 0; i < nr; i++)
		rows[i].y = i * s.lh;
	*h = nr * s.lh + s.after;
	return nr;
}

static unsigned settings_sig(const struct lay *L)
{
	return (unsigned)L->mode | (unsigned)(L->lang + 1) << 2 | (unsigned)L->small << 8 |
	       (unsigned)L->full << 9 | (unsigned)L->width << 10;
}

void lay_reset(struct lay *L, struct ed *e)
{
	for (int i = 0; i < e->t.n; i++) {
		e->t.l[i].h = -1;
		e->t.l[i].st_in = e->t.l[i].st_out = -1;
	}
	L->sig = settings_sig(L);
}

static int next_state(struct lay *L, const struct tline *l, int state)
{
	if (L->mode != M_CODE)
		return md_is_fence(l->s, l->len) ? !(state & 1) : state;
	if (l->len > cls_cap) {
		unsigned char *c = realloc(cls_buf, (size_t)l->len + 64);

		if (!c)
			return 0;
		cls_buf = c;
		cls_cap = l->len + 64;
	}
	return syn_line(L->lang, l->s, l->len, state, cls_buf);
}

void lay_ensure(struct lay *L, struct ed *e)
{
	static struct row rows[ROWS_MAX];
	int state = 0, y = 0;

	if (L->sig != settings_sig(L))
		lay_reset(L, e);
	if (e->t.n + 1 > L->cap) {
		int cap = (e->t.n + 1) * 2;
		int *ys = realloc(L->ys, (size_t)cap * sizeof(int));

		if (!ys)
			return;
		L->ys = ys;
		L->cap = cap;
	}
	for (int i = 0; i < e->t.n; i++) {
		struct tline *l = &e->t.l[i];

		if (l->h < 0 || l->st_in != state) {
			int h;

			lay_rows(L, e, i, state, rows, ROWS_MAX, &h);
			l->h = h;
			l->st_in = state;
			l->st_out = next_state(L, l, state);
		}
		L->ys[i] = y;
		y += l->h;
		state = l->st_out;
	}
	L->ys[e->t.n] = y;
	L->n = e->t.n;
	L->total = y;
}

int lay_state_in(const struct ed *e, int line)
{
	int s = e->t.l[line].st_in;

	return s >= 0 ? s : ed_in_code(e, line);
}

int lay_line_at(const struct lay *L, int y)
{
	int lo = 0, hi = L->n - 1;

	if (L->n <= 0 || y < 0)
		return 0;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;

		if (L->ys[mid] <= y)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

int lay_x(struct lay *L, struct ed *e, int line, const struct row *r, int k)
{
	const char *text;
	const unsigned char *st;
	struct lstyle s;
	int n, x16 = 0, i;

	row_setup(L, e, line, lay_state_in(e, line), &text, &n, &st, &s);
	for (i = r->k0; i < k && i < n; ) {
		int j = i;
		unsigned cp = decode(text, n, &j);

		x16 += adv(L, lay_font_of(&s, st ? st[i] : 0), cp);
		i = j;
	}
	return r->x + (x16 + 8) / 16;
}

int lay_k(struct lay *L, struct ed *e, int line, const struct row *r, int x)
{
	const char *text;
	const unsigned char *st;
	struct lstyle s;
	int n, x16 = r->x * 16, i, last;

	row_setup(L, e, line, lay_state_in(e, line), &text, &n, &st, &s);
	last = r->k1;
	if (r->k1 < n) {	/* not the line's last row: stay before its end */
		last = r->k1 - 1;
		while (last > r->k0 && ((unsigned char)text[last] & 0xc0) == 0x80)
			last--;
	}
	for (i = r->k0; i < last; ) {
		int j = i;
		unsigned cp = decode(text, n, &j);
		int w = adv(L, lay_font_of(&s, st ? st[i] : 0), cp);

		if (x * 16 < x16 + w / 2)
			return i;
		x16 += w;
		i = j;
	}
	return last < r->k0 ? r->k0 : last;
}

/* the caret's row: k on a wrapped row's edge belongs to the next row */
static int row_of(const struct row *rows, int nr, int k)
{
	for (int i = 0; i < nr; i++)
		if (k < rows[i].k1 || i == nr - 1)
			return i;
	return nr - 1;
}

static int k_of(struct lay *L, struct ed *e, struct pos p)
{
	const char *text;
	const unsigned char *st;
	enum blk kind;
	int n;

	text = lay_text(L, e, p.line, lay_state_in(e, p.line), &n, &kind, &st);
	(void)text;
	return L->mode == M_RICH ? md_vis(&e->m, p.col) : p.col;
}

static struct pos pos_of(struct lay *L, struct ed *e, int line, int k)
{
	const char *text;
	const unsigned char *st;
	enum blk kind;
	int n;
	struct pos p = { line, k };

	text = lay_text(L, e, line, lay_state_in(e, line), &n, &kind, &st);
	(void)text;
	if (L->mode == M_RICH)
		p.col = md_src(&e->m, k);
	return p;
}

int lay_caret(struct lay *L, struct ed *e, struct pos p, int *x, int *y, int *h)
{
	static struct row rows[ROWS_MAX];
	int hh, nr, k, r;

	nr = lay_rows(L, e, p.line, lay_state_in(e, p.line), rows, ROWS_MAX, &hh);
	k = k_of(L, e, p);
	r = row_of(rows, nr, k);
	*x = lay_x(L, e, p.line, &rows[r], k);
	*y = (p.line < L->n ? L->ys[p.line] : 0) + rows[r].y;
	*h = rows[r].h;
	return r;
}

struct pos lay_vmove(struct lay *L, struct ed *e, int dir, int *want_x)
{
	static struct row rows[ROWS_MAX];
	int x, y, h, r, nr, line = e->caret.line;

	r = lay_caret(L, e, e->caret, &x, &y, &h);
	if (*want_x < 0)
		*want_x = x;
	nr = lay_rows(L, e, line, lay_state_in(e, line), rows, ROWS_MAX, &h);
	r += dir;
	if (r < 0 || r >= nr) {
		line += dir;
		if (line < 0)
			return pos_of(L, e, 0, 0);
		if (line >= e->t.n) {
			nr = lay_rows(L, e, e->t.n - 1, lay_state_in(e, e->t.n - 1), rows, ROWS_MAX, &h);
			return pos_of(L, e, e->t.n - 1, rows[nr - 1].k1);
		}
		nr = lay_rows(L, e, line, lay_state_in(e, line), rows, ROWS_MAX, &h);
		r = dir > 0 ? 0 : nr - 1;
	}
	return pos_of(L, e, line, lay_k(L, e, line, &rows[r], *want_x));
}
