/* SPDX-License-Identifier: MIT */
/* store: JSON reading (jsmn, third_party/jsmn.h) and writing. */
#include <stdlib.h>
#include <string.h>

#define JSMN_STRICT
#include "../third_party/jsmn.h"
#include "store.h"

int json_parse(struct json *j, const char *s, size_t len)
{
	jsmn_parser p;
	int n;

	jsmn_init(&p);
	n = jsmn_parse(&p, s, len, NULL, 0);
	if (n <= 0)
		return -1;
	j->s = s;
	j->t = xmalloc((size_t)n * sizeof(jsmntok_t));
	j->n = n;
	jsmn_init(&p);
	if (jsmn_parse(&p, s, len, j->t, (unsigned)n) != n) {
		free(j->t);
		j->t = 0;
		return -1;
	}
	return 0;
}

void json_free(struct json *j)
{
	free(j->t);
	j->t = 0;
	j->n = 0;
}

/* the token after the whole value at i */
int json_skip(const struct json *j, int i)
{
	int left = 1;

	while (left > 0 && i < j->n) {
		if (j->t[i].type == JSMN_OBJECT)
			left += j->t[i].size * 2;
		else if (j->t[i].type == JSMN_ARRAY)
			left += j->t[i].size;
		left--;
		i++;
	}
	return i;
}

int json_eq(const struct json *j, int tok, const char *s)
{
	const jsmntok_t *t = &j->t[tok];
	size_t n = strlen(s);

	return t->type == JSMN_STRING && (size_t)(t->end - t->start) == n &&
	       !memcmp(j->s + t->start, s, n);
}

int json_get(const struct json *j, int obj, const char *key)
{
	int i;

	if (obj < 0 || obj >= j->n || j->t[obj].type != JSMN_OBJECT)
		return -1;
	i = obj + 1;
	for (int k = 0; k < j->t[obj].size; k++) {
		if (json_eq(j, i, key))
			return i + 1;
		i = json_skip(j, i + 1);
	}
	return -1;
}

/* "launch.exec": one key at a time */
int json_path(const struct json *j, int obj, const char *dotted)
{
	char key[64];

	while (obj >= 0 && *dotted) {
		size_t n = strcspn(dotted, ".");

		if (n >= sizeof(key))
			return -1;
		memcpy(key, dotted, n);
		key[n] = 0;
		obj = json_get(j, obj, key);
		dotted += n + (dotted[n] == '.');
	}
	return obj;
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

static char *put_utf8(char *o, unsigned cp)
{
	if (cp < 0x80) {
		*o++ = (char)cp;
	} else if (cp < 0x800) {
		*o++ = (char)(0xC0 | cp >> 6);
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*o++ = (char)(0xE0 | cp >> 12);
		*o++ = (char)(0x80 | (cp >> 6 & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*o++ = (char)(0xF0 | cp >> 18);
		*o++ = (char)(0x80 | (cp >> 12 & 0x3F));
		*o++ = (char)(0x80 | (cp >> 6 & 0x3F));
		*o++ = (char)(0x80 | (cp & 0x3F));
	}
	return o;
}

static unsigned hex4(const char *p)
{
	unsigned v = 0;

	for (int i = 0; i < 4; i++) {
		int h = hexval(p[i]);

		if (h < 0)
			return 0xFFFD;
		v = v << 4 | (unsigned)h;
	}
	return v;
}

/* unescaped copy of a string token (its length bounds the result) */
char *json_strdup(const struct json *j, int tok)
{
	const jsmntok_t *t;
	const char *p, *end;
	char *out, *o;

	if (tok < 0 || tok >= j->n || j->t[tok].type != JSMN_STRING)
		return NULL;
	t = &j->t[tok];
	p = j->s + t->start;
	end = j->s + t->end;
	o = out = xmalloc((size_t)(end - p) + 1);
	while (p < end) {
		if (*p != '\\') {
			*o++ = *p++;
			continue;
		}
		p++;
		if (p >= end)
			break;
		switch (*p) {
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u':
			if (end - p >= 5) {
				unsigned cp = hex4(p + 1);

				p += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 7 && p[1] == '\\' && p[2] == 'u') {
					unsigned lo = hex4(p + 3);

					if (lo >= 0xDC00 && lo < 0xE000) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						p += 6;
					}
				}
				o = put_utf8(o, cp);	/* 4 bytes at most for 6 or 12 in */
			}
			break;
		default: *o++ = *p; break;
		}
		p++;
	}
	*o = 0;
	return out;
}

int json_str(const struct json *j, int tok, char *out, size_t size)
{
	char *s = json_strdup(j, tok);

	if (!s) {
		if (size)
			out[0] = 0;
		return -1;
	}
	snprintf(out, size, "%s", s);
	free(s);
	return 0;
}

long long json_int(const struct json *j, int tok, long long def)
{
	const jsmntok_t *t;
	long long v = 0;
	int neg = 0;

	if (tok < 0 || tok >= j->n || j->t[tok].type != JSMN_PRIMITIVE)
		return def;
	t = &j->t[tok];
	for (int i = t->start; i < t->end; i++) {
		char c = j->s[i];

		if (i == t->start && c == '-')
			neg = 1;
		else if (c >= '0' && c <= '9')
			v = v * 10 + (c - '0');
		else
			return def;
	}
	return neg ? -v : v;
}

int json_bool(const struct json *j, int tok, int def)
{
	if (tok < 0 || tok >= j->n || j->t[tok].type != JSMN_PRIMITIVE)
		return def;
	return j->s[j->t[tok].start] == 't' ? 1 : j->s[j->t[tok].start] == 'f' ? 0 : def;
}

int json_len(const struct json *j, int tok)
{
	return tok < 0 || tok >= j->n ? 0 : j->t[tok].size;
}

int json_item(const struct json *j, int arr, int i)
{
	int t;

	if (arr < 0 || arr >= j->n || j->t[arr].type != JSMN_ARRAY || i >= j->t[arr].size)
		return -1;
	t = arr + 1;
	while (i-- > 0)
		t = json_skip(j, t);
	return t;
}

int json_is_obj(const struct json *j, int tok)
{
	return tok >= 0 && tok < j->n && j->t[tok].type == JSMN_OBJECT;
}

int json_is_arr(const struct json *j, int tok)
{
	return tok >= 0 && tok < j->n && j->t[tok].type == JSMN_ARRAY;
}

/* ---- writer ---- */

static void put(struct jw *w, const char *s, size_t n)
{
	if (w->len + n + 1 > w->cap) {
		w->cap = (w->len + n + 1) * 2;
		w->buf = xrealloc(w->buf, w->cap);
	}
	memcpy(w->buf + w->len, s, n);
	w->len += n;
	w->buf[w->len] = 0;
}

static void puts_(struct jw *w, const char *s)
{
	put(w, s, strlen(s));
}

void jw_init(struct jw *w)
{
	memset(w, 0, sizeof(*w));
	w->first[0] = 1;
	w->indent = 1;
}

void jw_free(struct jw *w)
{
	free(w->buf);
	w->buf = 0;
}

void jw_quote(struct jw *w, const char *s)
{
	put(w, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char e[8];

		if (c == '"' || c == '\\') {
			e[0] = '\\';
			e[1] = (char)c;
			put(w, e, 2);
		} else if (c == '\n') {
			puts_(w, "\\n");
		} else if (c == '\t') {
			puts_(w, "\\t");
		} else if (c < 0x20) {
			snprintf(e, sizeof(e), "\\u%04x", c);
			puts_(w, e);
		} else {
			put(w, (const char *)&c, 1);
		}
	}
	put(w, "\"", 1);
}

static void key(struct jw *w, const char *k)
{
	if (!w->first[w->depth])
		put(w, ",", 1);
	w->first[w->depth] = 0;
	if (w->indent && w->depth) {
		put(w, "\n", 1);
		for (int i = 0; i < w->depth; i++)
			put(w, " ", 1);
	}
	if (k) {
		jw_quote(w, k);
		put(w, ": ", 2);
	}
}

static void open_(struct jw *w, const char *k, char kind)
{
	key(w, k);
	put(w, kind == ']' ? "[" : "{", 1);
	w->depth++;
	w->first[w->depth] = 1;
	w->kind[w->depth] = kind;
}

void jw_obj(struct jw *w, const char *k)
{
	open_(w, k, '}');
}

void jw_arr(struct jw *w, const char *k)
{
	open_(w, k, ']');
}

void jw_end(struct jw *w)
{
	int empty = w->first[w->depth];
	char close = w->kind[w->depth];

	w->depth--;
	if (w->indent && !empty) {
		put(w, "\n", 1);
		for (int i = 0; i < w->depth; i++)
			put(w, " ", 1);
	}
	put(w, &close, 1);
	if (!w->depth && w->indent)
		put(w, "\n", 1);
}

void jw_str(struct jw *w, const char *k, const char *v)
{
	key(w, k);
	jw_quote(w, v ? v : "");
}

void jw_int(struct jw *w, const char *k, long long v)
{
	char b[24];

	key(w, k);
	snprintf(b, sizeof(b), "%lld", v);
	puts_(w, b);
}

void jw_bool(struct jw *w, const char *k, int v)
{
	key(w, k);
	puts_(w, v ? "true" : "false");
}

void jw_raw(struct jw *w, const char *k, const char *raw, size_t n)
{
	key(w, k);
	put(w, raw, n);
}
