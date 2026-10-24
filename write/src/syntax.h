/* SPDX-License-Identifier: MIT */
/*
 * Write: the languages of the code view (design Language.dc.html) and their
 * highlighting (Code.dc.html). syn_line gives every byte of a line a class;
 * the state carries a comment or string that goes on to the next line.
 */
#ifndef WRITE_SYNTAX_H
#define WRITE_SYNTAX_H

enum lang {
	L_PLAIN, L_MARKDOWN, L_SHELL, L_PYTHON, L_PHP, L_HTML, L_CSS, L_JS, L_JSON, L_YAML,
	L_CONF, L_C, L_COUNT, L_NONE = -1
};

enum cls { C_TEXT, C_KEYWORD, C_STRING, C_NUMBER, C_COMMENT, C_FUNC, C_PUNCT };

enum lang lang_of_path(const char *path);	/* L_NONE: not text Write knows */
const char *lang_name(enum lang l);
const char *lang_exts(enum lang l);
int lang_indent(enum lang l);
int lang_runs(enum lang l);	/* 1: ▲ runs it, 2: ▲ previews it, 0: neither */
int syn_line(enum lang l, const char *s, int n, int state, unsigned char *cls);

#endif
