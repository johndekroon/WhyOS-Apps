/* SPDX-License-Identifier: MIT */
/* Write: languages and highlighting (syntax.h). */
#include <string.h>
#include <strings.h>

#include "syntax.h"

/* states that go on to the next line */
enum { S_NONE, S_BLOCK, S_TRIPLE_DQ, S_TRIPLE_SQ, S_TEMPLATE, S_TAG };

struct langdef {
	const char *name, *exts, *also;	/* exts as the Language list shows them */
	const char *const *kw, *const *consts;
	const char *line1, *line2;	/* line comments */
	int block;			/* C-style block comments */
	const char *quotes;
	int indent, runs;
};

static const char *const KW_SH[] = { "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
	"done", "case", "esac", "in", "function", "return", "exit", "local", "export", "select",
	"break", "continue", 0 };
static const char *const KW_PY[] = { "and", "as", "assert", "async", "await", "break", "class",
	"continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
	"import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
	"while", "with", "yield", 0 };
static const char *const CO_PY[] = { "True", "False", "None", 0 };
static const char *const KW_JS[] = { "break", "case", "catch", "class", "const", "continue",
	"debugger", "default", "delete", "do", "else", "export", "extends", "finally", "for",
	"function", "if", "import", "in", "instanceof", "let", "new", "return", "super", "switch",
	"this", "throw", "try", "typeof", "var", "void", "while", "with", "yield", "async", "await",
	"of", "static", "get", "set", 0 };
static const char *const CO_JS[] = { "true", "false", "null", "undefined", "NaN", 0 };
static const char *const KW_PHP[] = { "abstract", "and", "array", "as", "break", "case", "catch",
	"class", "const", "continue", "default", "do", "echo", "else", "elseif", "empty", "endif",
	"endforeach", "endwhile", "extends", "final", "finally", "fn", "for", "foreach", "function",
	"global", "if", "implements", "include", "include_once", "isset", "list", "match", "namespace",
	"new", "or", "print", "private", "protected", "public", "require", "require_once", "return",
	"static", "switch", "throw", "trait", "try", "unset", "use", "var", "while", "yield", 0 };
static const char *const CO_PHP[] = { "true", "false", "null", "TRUE", "FALSE", "NULL", 0 };
static const char *const KW_C[] = { "auto", "break", "case", "char", "const", "continue", "default",
	"do", "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long",
	"register", "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch",
	"typedef", "union", "unsigned", "void", "volatile", "while", "bool", "size_t", "uint8_t",
	"uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", 0 };
static const char *const CO_C[] = { "true", "false", "NULL", 0 };

static const struct langdef L[L_COUNT] = {
	[L_PLAIN] = { "Plain text", ".txt .log", ".csv", 0, 0, 0, 0, 0, 0, 4, 0 },
	[L_MARKDOWN] = { "Markdown", ".md", ".markdown", 0, 0, 0, 0, 0, 0, 2, 0 },
	[L_SHELL] = { "Shell", ".sh", ".bash", KW_SH, 0, "#", 0, 0, "\"'", 4, 1 },
	[L_PYTHON] = { "Python", ".py", 0, KW_PY, CO_PY, "#", 0, 0, "\"'", 4, 1 },
	[L_PHP] = { "PHP", ".php", 0, KW_PHP, CO_PHP, "//", "#", 1, "\"'", 4, 0 },
	[L_HTML] = { "HTML", ".html", ".htm .xhtml", 0, 0, 0, 0, 0, 0, 2, 2 },
	[L_CSS] = { "CSS", ".css", 0, 0, 0, 0, 0, 1, "\"'", 2, 0 },
	[L_JS] = { "JavaScript", ".js", ".mjs", KW_JS, CO_JS, "//", 0, 1, "\"'`", 2, 0 },
	[L_JSON] = { "JSON", ".json", 0, 0, 0, 0, 0, 0, "\"", 2, 0 },
	[L_YAML] = { "YAML", ".yml", ".yaml", 0, 0, 0, 0, 0, 0, 2, 0 },
	[L_CONF] = { "Config", ".conf .ini", ".cfg", 0, 0, 0, 0, 0, 0, 4, 0 },
	[L_C] = { "C", ".c .h", 0, KW_C, CO_C, "//", 0, 1, "\"'", 4, 0 },
};

static int has_ext(const char *list, const char *ext)
{
	size_t n = strlen(ext);

	while (list && *list) {
		const char *e = strchr(list, ' ');
		size_t len = e ? (size_t)(e - list) : strlen(list);

		if (len == n && !strncasecmp(list, ext, n))
			return 1;
		list = e ? e + 1 : 0;
	}
	return 0;
}

enum lang lang_of_path(const char *path)
{
	const char *base = strrchr(path, '/'), *dot;

	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	if (!dot || dot == base)
		return L_PLAIN;		/* README, Makefile: text without an extension */
	for (int l = 0; l < L_COUNT; l++)
		if (has_ext(L[l].exts, dot) || has_ext(L[l].also, dot))
			return (enum lang)l;
	return L_NONE;
}

const char *lang_name(enum lang l)
{
	return l >= 0 && l < L_COUNT ? L[l].name : "Plain text";
}

const char *lang_exts(enum lang l)
{
	return l >= 0 && l < L_COUNT ? L[l].exts : "";
}

int lang_indent(enum lang l)
{
	return l >= 0 && l < L_COUNT ? L[l].indent : 4;
}

int lang_runs(enum lang l)
{
	return l >= 0 && l < L_COUNT ? L[l].runs : 0;
}

/* ---- helpers ---------------------------------------------------------------------- */

static int is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int is_alnum(char c)
{
	return is_alpha(c) || is_digit(c);
}

static int is_sp(char c)
{
	return c == ' ' || c == '\t';
}

static void mark(unsigned char *cls, int a, int b, int c)
{
	for (int i = a; i < b; i++)
		cls[i] = (unsigned char)c;
}

static int in_list(const char *const *list, const char *s, int n)
{
	for (; list && *list; list++)
		if ((int)strlen(*list) == n && !strncmp(*list, s, (size_t)n))
			return 1;
	return 0;
}

static int starts(const char *s, int n, int i, const char *w)
{
	int k = (int)strlen(w);

	return i + k <= n && !memcmp(s + i, w, (size_t)k);
}

static int next_nonspace(const char *s, int n, int i)
{
	while (i < n && is_sp(s[i]))
		i++;
	return i < n ? s[i] : 0;
}

/* a quoted string from i (the quote); returns its end */
static int quoted(const char *s, int n, int i)
{
	char q = s[i++];

	while (i < n && s[i] != q)
		i += s[i] == '\\' && i + 1 < n ? 2 : 1;
	return i < n ? i + 1 : n;
}

static int number_end(const char *s, int n, int i)
{
	while (i < n && (is_alnum(s[i]) || s[i] == '.'))
		i++;
	return i;
}

/* ---- C-like languages: shell, Python, PHP, JavaScript, C ------------------------- */

static int generic(const struct langdef *d, enum lang l, const char *s, int n, int state,
		   unsigned char *cls)
{
	const char *punct = l == L_SHELL ? "()[]{};|&<>=" : "()[]{}<>=+-*/%:;,.!&|^~?@";
	int i = 0;

	if (state == S_BLOCK) {
		const char *e = 0;

		for (int k = 0; k + 1 < n; k++)
			if (s[k] == '*' && s[k + 1] == '/') {
				e = s + k;
				break;
			}
		if (!e) {
			mark(cls, 0, n, C_COMMENT);
			return S_BLOCK;
		}
		i = (int)(e - s) + 2;
		mark(cls, 0, i, C_COMMENT);
	} else if (state == S_TRIPLE_DQ || state == S_TRIPLE_SQ || state == S_TEMPLATE) {
		const char *close = state == S_TRIPLE_DQ ? "\"\"\"" : state == S_TRIPLE_SQ ? "'''" : "`";
		int k = 0;

		while (k < n && !starts(s, n, k, close))
			k += s[k] == '\\' && k + 1 < n ? 2 : 1;
		if (k >= n) {
			mark(cls, 0, n, C_STRING);
			return state;
		}
		i = k + (int)strlen(close);
		mark(cls, 0, i, C_STRING);
	}
	if (l == L_C) {		/* a preprocessor line: the directive is a keyword */
		int k = i;

		while (k < n && is_sp(s[k]))
			k++;
		if (k < n && s[k] == '#') {
			int e = k + 1;

			while (e < n && is_alpha(s[e]))
				e++;
			mark(cls, k, e, C_KEYWORD);
			i = e;
			while (i < n && is_sp(s[i]))
				i++;
			if (i < n && s[i] == '<') {
				int c = i;

				while (c < n && s[c] != '>')
					c++;
				c = c < n ? c + 1 : n;
				mark(cls, i, c, C_STRING);
				i = c;
			}
		}
	}
	while (i < n) {
		char c = s[i];
		int e;

		if ((d->line1 && starts(s, n, i, d->line1) &&
		     (l != L_SHELL || i == 0 || is_sp(s[i - 1]))) ||
		    (d->line2 && starts(s, n, i, d->line2))) {
			mark(cls, i, n, C_COMMENT);
			return S_NONE;
		}
		if (d->block && starts(s, n, i, "/*")) {
			int k = i + 2;

			while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/'))
				k++;
			if (k + 1 >= n) {
				mark(cls, i, n, C_COMMENT);
				return S_BLOCK;
			}
			mark(cls, i, k + 2, C_COMMENT);
			i = k + 2;
			continue;
		}
		if (l == L_PYTHON && (starts(s, n, i, "\"\"\"") || starts(s, n, i, "'''"))) {
			const char *q = s[i] == '"' ? "\"\"\"" : "'''";
			int k = i + 3;

			while (k < n && !starts(s, n, k, q))
				k += s[k] == '\\' && k + 1 < n ? 2 : 1;
			if (k >= n) {
				mark(cls, i, n, C_STRING);
				return s[i] == '"' ? S_TRIPLE_DQ : S_TRIPLE_SQ;
			}
			mark(cls, i, k + 3, C_STRING);
			i = k + 3;
			continue;
		}
		if (c == '`' && l == L_JS) {
			int k = i + 1;

			while (k < n && s[k] != '`')
				k += s[k] == '\\' && k + 1 < n ? 2 : 1;
			if (k >= n) {
				mark(cls, i, n, C_STRING);
				return S_TEMPLATE;
			}
			mark(cls, i, k + 1, C_STRING);
			i = k + 1;
			continue;
		}
		if (d->quotes && strchr(d->quotes, c) && c) {
			e = quoted(s, n, i);
			mark(cls, i, e, C_STRING);
			i = e;
			continue;
		}
		if (l == L_PHP && (starts(s, n, i, "<?php") || starts(s, n, i, "?>"))) {
			e = i + (s[i] == '<' ? 5 : 2);
			mark(cls, i, e, C_KEYWORD);
			i = e;
			continue;
		}
		if (is_digit(c) && (i == 0 || !is_alnum(s[i - 1]))) {
			e = number_end(s, n, i);
			mark(cls, i, e, C_NUMBER);
			i = e;
			continue;
		}
		if (is_alpha(c) || ((l == L_PHP || l == L_SHELL) && c == '$')) {
			e = i + 1;
			while (e < n && is_alnum(s[e]))
				e++;
			if (c == '$')
				mark(cls, i, e, C_TEXT);
			else if (in_list(d->kw, s + i, e - i))
				mark(cls, i, e, C_KEYWORD);
			else if (in_list(d->consts, s + i, e - i))
				mark(cls, i, e, C_NUMBER);
			else if (l != L_SHELL && next_nonspace(s, n, e) == '(')
				mark(cls, i, e, C_FUNC);
			else
				mark(cls, i, e, C_TEXT);
			i = e;
			continue;
		}
		cls[i] = (unsigned char)(c && strchr(punct, c) ? C_PUNCT : C_TEXT);
		i++;
	}
	return S_NONE;
}

/* ---- HTML ------------------------------------------------------------------------- */

static int html(const char *s, int n, int state, unsigned char *cls)
{
	int i = 0;

	mark(cls, 0, n, C_TEXT);
	if (state == S_BLOCK) {
		int k = 0;

		while (k < n && !starts(s, n, k, "-->"))
			k++;
		if (k >= n) {
			mark(cls, 0, n, C_COMMENT);
			return S_BLOCK;
		}
		mark(cls, 0, k + 3, C_COMMENT);
		i = k + 3;
		state = S_NONE;
	}
	while (i < n) {
		if (state != S_TAG && starts(s, n, i, "<!--")) {
			int k = i + 4;

			while (k < n && !starts(s, n, k, "-->"))
				k++;
			if (k >= n) {
				mark(cls, i, n, C_COMMENT);
				return S_BLOCK;
			}
			mark(cls, i, k + 3, C_COMMENT);
			i = k + 3;
			continue;
		}
		if (state != S_TAG && s[i] == '<' && i + 1 < n && (is_alpha(s[i + 1]) || s[i + 1] == '/' ||
								    s[i + 1] == '!')) {
			int decl = s[i + 1] == '!', e;

			cls[i++] = C_PUNCT;
			if (s[i] == '/' || s[i] == '!')
				cls[i++] = C_PUNCT;
			e = i;
			while (e < n && (is_alnum(s[e]) || s[e] == '-' || s[e] == ':'))
				e++;
			mark(cls, i, e, C_KEYWORD);
			i = e;
			if (decl) {	/* <!doctype html>: the rest is text */
				while (i < n && s[i] != '>')
					i++;
				if (i < n)
					cls[i++] = C_PUNCT;
				continue;
			}
			state = S_TAG;
			continue;
		}
		if (state == S_TAG) {
			char c = s[i];

			if (c == '>' || c == '/' || c == '=') {
				cls[i] = C_PUNCT;
				if (c == '>')
					state = S_NONE;
				i++;
			} else if (c == '"' || c == '\'') {
				int e = quoted(s, n, i);

				mark(cls, i, e, C_STRING);
				i = e;
			} else if (is_alpha(c)) {
				int e = i;

				while (e < n && (is_alnum(s[e]) || s[e] == '-' || s[e] == ':'))
					e++;
				mark(cls, i, e, i > 0 && s[i - 1] == '=' ? C_STRING : C_FUNC);
				i = e;
			} else {
				i++;
			}
			continue;
		}
		i++;
	}
	return state;
}

/* ---- CSS -------------------------------------------------------------------------- */

static int css(const char *s, int n, int state, unsigned char *cls)
{
	int i = 0;

	mark(cls, 0, n, C_TEXT);
	if (state == S_BLOCK) {
		int k = 0;

		while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/'))
			k++;
		if (k + 1 >= n) {
			mark(cls, 0, n, C_COMMENT);
			return S_BLOCK;
		}
		mark(cls, 0, k + 2, C_COMMENT);
		i = k + 2;
	}
	while (i < n) {
		char c = s[i];

		if (starts(s, n, i, "/*")) {
			int k = i + 2;

			while (k + 1 < n && !(s[k] == '*' && s[k + 1] == '/'))
				k++;
			if (k + 1 >= n) {
				mark(cls, i, n, C_COMMENT);
				return S_BLOCK;
			}
			mark(cls, i, k + 2, C_COMMENT);
			i = k + 2;
		} else if (c == '"' || c == '\'') {
			int e = quoted(s, n, i);

			mark(cls, i, e, C_STRING);
			i = e;
		} else if (c == '#' && i + 1 < n && is_alnum(s[i + 1])) {
			int e = i + 1;

			while (e < n && is_alnum(s[e]))
				e++;
			mark(cls, i, e, C_NUMBER);
			i = e;
		} else if (is_digit(c) && (i == 0 || !is_alnum(s[i - 1]))) {
			int e = i;

			while (e < n && (is_alnum(s[e]) || s[e] == '.' || s[e] == '%'))
				e++;
			mark(cls, i, e, C_NUMBER);
			i = e;
		} else if (is_alpha(c) || c == '-') {
			int e = i;

			while (e < n && (is_alnum(s[e]) || s[e] == '-'))
				e++;
			if (next_nonspace(s, n, e) == ':' && next_nonspace(s, n, e + 1) != ':')
				mark(cls, i, e, C_FUNC);
			i = e;
		} else {
			if (strchr("{}:;,()>+~", c) && c)
				cls[i] = C_PUNCT;
			i++;
		}
	}
	return S_NONE;
}

/* ---- JSON, YAML, config ----------------------------------------------------------- */

static int word_at(const char *s, int n, int i, const char *w)
{
	int k = (int)strlen(w);

	return starts(s, n, i, w) && (i + k >= n || !is_alnum(s[i + k]));
}

static void json(const char *s, int n, unsigned char *cls)
{
	mark(cls, 0, n, C_TEXT);
	for (int i = 0; i < n; ) {
		char c = s[i];

		if (c == '"') {
			int e = quoted(s, n, i);

			mark(cls, i, e, next_nonspace(s, n, e) == ':' ? C_FUNC : C_STRING);
			i = e;
		} else if (is_digit(c) || (c == '-' && i + 1 < n && is_digit(s[i + 1]))) {
			int e = i + 1;

			while (e < n && (is_alnum(s[e]) || s[e] == '.' || s[e] == '-' || s[e] == '+'))
				e++;
			mark(cls, i, e, C_NUMBER);
			i = e;
		} else if (word_at(s, n, i, "true") || word_at(s, n, i, "false") || word_at(s, n, i, "null")) {
			int e = i + (s[i] == 'f' ? 5 : 4);

			mark(cls, i, e, C_KEYWORD);
			i = e;
		} else {
			if (strchr("{}[]:,", c) && c)
				cls[i] = C_PUNCT;
			i++;
		}
	}
}

/* a comment from i to the end, when c starts one there */
static int comment_from(const char *s, int n, int i, const char *starts_with, unsigned char *cls)
{
	if (strchr(starts_with, s[i]) && s[i] && (i == 0 || is_sp(s[i - 1]))) {
		mark(cls, i, n, C_COMMENT);
		return 1;
	}
	return 0;
}

static void yaml(const char *s, int n, unsigned char *cls)
{
	int i = 0, key_end = -1;

	mark(cls, 0, n, C_TEXT);
	while (i < n && is_sp(s[i]))
		i++;
	if (i + 1 < n && s[i] == '-' && s[i + 1] == ' ') {
		cls[i] = C_PUNCT;
		i += 2;
	}
	for (int k = i; k < n && s[k] != '#' && s[k] != '"' && s[k] != '\''; k++)
		if (s[k] == ':' && (k + 1 == n || is_sp(s[k + 1]))) {
			key_end = k;
			break;
		}
	if (key_end > i) {
		mark(cls, i, key_end, C_FUNC);
		cls[key_end] = C_PUNCT;
		i = key_end + 1;
	}
	while (i < n) {
		if (comment_from(s, n, i, "#", cls))
			return;
		if (s[i] == '"' || s[i] == '\'') {
			int e = quoted(s, n, i);

			mark(cls, i, e, C_STRING);
			i = e;
		} else if (is_digit(s[i]) && (i == 0 || is_sp(s[i - 1]))) {
			int e = number_end(s, n, i);

			mark(cls, i, e, C_NUMBER);
			i = e;
		} else {
			i++;
		}
	}
}

static void conf(const char *s, int n, unsigned char *cls)
{
	int i = 0;

	mark(cls, 0, n, C_TEXT);
	while (i < n && is_sp(s[i]))
		i++;
	if (i < n && (s[i] == '#' || s[i] == ';')) {
		mark(cls, i, n, C_COMMENT);
		return;
	}
	if (i < n && s[i] == '[') {
		int e = i;

		while (e < n && s[e] != ']')
			e++;
		mark(cls, i, e < n ? e + 1 : n, C_KEYWORD);
		return;
	}
	for (int k = i; k < n; k++)
		if (s[k] == '=' || s[k] == ':') {
			int ke = k;

			while (ke > i && is_sp(s[ke - 1]))
				ke--;
			mark(cls, i, ke, C_FUNC);
			cls[k] = C_PUNCT;
			i = k + 1;
			break;
		}
	while (i < n) {
		if (comment_from(s, n, i, "#;", cls))
			return;
		if (s[i] == '"' || s[i] == '\'') {
			int e = quoted(s, n, i);

			mark(cls, i, e, C_STRING);
			i = e;
		} else {
			i++;
		}
	}
}

int syn_line(enum lang l, const char *s, int n, int state, unsigned char *cls)
{
	switch (l) {
	case L_SHELL: case L_PYTHON: case L_PHP: case L_JS: case L_C:
		mark(cls, 0, n, C_TEXT);
		return generic(&L[l], l, s, n, state, cls);
	case L_HTML:
		return html(s, n, state, cls);
	case L_CSS:
		return css(s, n, state, cls);
	case L_JSON:
		json(s, n, cls);
		return S_NONE;
	case L_YAML:
		yaml(s, n, cls);
		return S_NONE;
	case L_CONF:
		conf(s, n, cls);
		return S_NONE;
	default:
		mark(cls, 0, n, C_TEXT);
		return S_NONE;
	}
}
