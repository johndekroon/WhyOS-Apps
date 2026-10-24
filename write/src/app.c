/* SPDX-License-Identifier: MIT */
/* Write: the app's states and keys (app.h). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"

/* ui.c sets the real font metrics; without them (the tests) every character is 12 px */
int (*app_adv16)(void *ctx, int font, unsigned cp);

static int fake_adv16(void *ctx, int font, unsigned cp)
{
	(void)ctx;
	(void)font;
	(void)cp;
	return 12 * 16;
}

static const enum blk PARAS[] = { B_BODY, B_H1, B_H2, B_H3, B_BULLET, B_NUMBER, B_QUOTE, B_CODE };
#define N_PARAS	8
#define N_MARKS	3

/* ---- places ------------------------------------------------------------------ */

static const char *env_or(const char *k, const char *d)
{
	const char *v = getenv(k);

	return v && *v ? v : d;
}

const char *app_clip_path(void)
{
	return env_or("WRITE_CLIP", "/tmp/write.clip");
}

static const char *log_path(void)
{
	return env_or("WRITE_LOG", "/tmp/write.log");
}

static void config_path(char *out, int n)
{
	const char *c = getenv("APP_CONFIG");

	if (c && *c)
		snprintf(out, (size_t)n, "%s/settings", c);
	else
		snprintf(out, (size_t)n, "%s/.whyos/config/write/settings", env_or("HOME", "/root"));
}

static void mkdirs(const char *path)	/* the folders above path */
{
	char p[512];

	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
}

static void join(char *out, int n, const char *dir, const char *name)
{
	snprintf(out, (size_t)n, "%s/%s", strcmp(dir, "/") ? dir : "", name);
}

/* ---- the log (●) -------------------------------------------------------------- */

void app_log(struct app *a, const char *fmt, ...)
{
	char line[120], stamp[8];
	time_t t = time(0);
	struct tm tm;
	va_list ap;
	FILE *f;
	struct stat st;

	localtime_r(&t, &tm);
	snprintf(stamp, sizeof(stamp), "%02d:%02d", tm.tm_hour, tm.tm_min);
	va_start(ap, fmt);
	{
		char body[110];

		vsnprintf(body, sizeof(body), fmt, ap);
		snprintf(line, sizeof(line), "%s  %s", stamp, body);
	}
	va_end(ap);
	if (a->n_log == LOG_LINES) {
		memmove(a->log[0], a->log[1], sizeof(a->log[0]) * (LOG_LINES - 1));
		a->n_log--;
	}
	snprintf(a->log[a->n_log++], sizeof(a->log[0]), "%s", line);
	f = fopen(log_path(), !stat(log_path(), &st) && st.st_size > 16 * 1024 ? "w" : "a");
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
}

static void log_load(struct app *a)
{
	FILE *f = fopen(log_path(), "r");
	char line[160];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (a->n_log == LOG_LINES) {
			memmove(a->log[0], a->log[1], sizeof(a->log[0]) * (LOG_LINES - 1));
			a->n_log--;
		}
		snprintf(a->log[a->n_log++], sizeof(a->log[0]), "%s", line);
	}
	fclose(f);
}

/* ---- settings (♣) --------------------------------------------------------------- */

static void settings_load(struct app *a)
{
	char p[512], line[64];
	FILE *f;

	a->small = 0;
	a->lineno = 1;
	config_path(p, sizeof(p));
	f = fopen(p, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "small=", 6))
			a->small = atoi(line + 6) != 0;
		else if (!strncmp(line, "lineno=", 7))
			a->lineno = atoi(line + 7) != 0;
	}
	fclose(f);
}

static void settings_save(struct app *a)
{
	char p[512], body[64], err[128];
	int n;

	config_path(p, sizeof(p));
	mkdirs(p);
	n = snprintf(body, sizeof(body), "small=%d\nlineno=%d\n", a->small, a->lineno);
	if (file_save(p, body, (size_t)n, err, sizeof(err)))
		app_log(a, "error: settings not saved: %s", err);
}

/* ---- the document ---------------------------------------------------------------- */

static void error_box(struct app *a, const char *title, const char *text)
{
	snprintf(a->err_title, sizeof(a->err_title), "%s", title);
	snprintf(a->err_text, sizeof(a->err_text), "%s", text);
	a->ov = OV_ERROR;
	a->focus = 0;
}

static void lay_config(struct app *a)
{
	int x, y, w, h;

	app_geometry(a, &x, &y, &w, &h);
	a->lay.mode = a->e.mode;
	a->lay.lang = a->lang;
	a->lay.width = w;
	a->lay.small = a->small;
	a->lay.full = a->full;
}

static void set_lang(struct app *a, enum lang l)
{
	if (l == L_NONE)
		l = L_PLAIN;
	a->lang = l;
	a->is_md = l == L_MARKDOWN;
	if (a->is_md) {
		if (a->e.mode == M_CODE)
			ed_set_mode(&a->e, M_RICH);
	} else {
		int g;

		ed_set_mode(&a->e, M_CODE);
		g = ed_guess_indent(&a->e);
		a->e.indent = g >= 0 ? g : lang_indent(l);
	}
	a->redraw = 1;
}

static void set_doc(struct app *a, const char *path, const char *data, size_t len)
{
	enum lang l = path ? lang_of_path(path) : L_MARKDOWN;

	if (l == L_NONE)
		l = L_PLAIN;
	ed_load(&a->e, data, len, l == L_MARKDOWN ? M_RICH : M_CODE);
	set_lang(a, l);
	if (path) {
		snprintf(a->path, sizeof(a->path), "%s", path);
		path_split(path, a->dir, sizeof(a->dir), a->name, sizeof(a->name));
	} else {
		a->path[0] = 0;
		snprintf(a->dir, sizeof(a->dir), "%s", a->home);
		snprintf(a->name, sizeof(a->name), "Untitled");
	}
	a->saved_at = 0;
	if (path) {
		struct stat st;

		if (!stat(path, &st))
			a->saved_at = st.st_mtime;
	}
	find_free(&a->f);
	find_init(&a->f);
	a->find_open = 0;
	a->replaced = 0;
	a->scroll = 0;
	a->want_x = -1;
	a->redraw = 1;
	lay_config(a);
	lay_reset(&a->lay, &a->e);
}

static void new_doc(struct app *a)
{
	set_doc(a, 0, "", 0);
}

static int do_open(struct app *a, const char *path)
{
	char err[128];
	char *data;
	size_t len;

	if (file_load(path, &data, &len, err, sizeof(err))) {
		char t[300];

		snprintf(t, sizeof(t), "%s: %s.", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, err);
		error_box(a, "Could not open", t);
		app_log(a, "error: could not open %s: %s", path, err);
		return -1;
	}
	set_doc(a, path, data, len);
	free(data);
	return 0;
}

static int empty_new(struct app *a)
{
	return !a->path[0] && text_size(&a->e.t) == 0;
}

static int dirty(struct app *a)
{
	return text_dirty(&a->e.t) && !empty_new(a);
}

static int save_to(struct app *a, const char *path)
{
	char err[128];
	size_t len;
	char *data = text_get(&a->e.t, &len);
	int words = ed_words(&a->e);

	if (!data) {
		error_box(a, "Could not save", "Not enough memory.");
		return -1;
	}
	if (file_save(path, data, len, err, sizeof(err))) {
		char t[300];

		free(data);
		snprintf(t, sizeof(t), "%s: %s.", path, err);
		error_box(a, "Could not save", t);
		app_log(a, "error: could not save %s: %s", path, err);
		return -1;
	}
	free(data);
	text_saved(&a->e.t);
	a->saved_at = time(0);
	snprintf(a->path, sizeof(a->path), "%s", path);
	path_split(path, a->dir, sizeof(a->dir), a->name, sizeof(a->name));
	if (a->e.mode == M_CODE)
		app_log(a, "saved %s (%d lines)", path, a->e.t.n);
	else
		app_log(a, "saved %s (%d word%s)", path, words, words == 1 ? "" : "s");
	return 0;
}

static void after_continue(struct app *a)
{
	enum after af = a->after;

	a->after = AF_NONE;
	switch (af) {
	case AF_CLOSE:
		a->quit = 1;
		break;
	case AF_NEW:
		new_doc(a);
		break;
	case AF_OPEN:
		do_open(a, a->after_path);
		break;
	case AF_RUN:
		a->run = lang_runs(a->lang);
		break;
	default:
		break;
	}
}

/* do af now, or first ask what to do with the changes */
static void request(struct app *a, enum after af, const char *path)
{
	a->after = af;
	snprintf(a->after_path, sizeof(a->after_path), "%s", path ? path : "");
	if (dirty(a)) {
		a->ov = OV_DISCARD;
		a->focus = 0;
		return;
	}
	after_continue(a);
}

/* ---- Save as ----------------------------------------------------------------------- */

static void sa_check(struct app *a)
{
	char p[700], full[200];

	a->sa_exists = 0;
	if (!name_ok(a->sa_name, a->sa_err, sizeof(a->sa_err)))
		return;
	snprintf(full, sizeof(full), "%s%s", a->sa_name, a->sa_ext);
	join(p, sizeof(p), a->sa_dir, full);
	if (file_exists(p)) {
		a->sa_exists = 1;
		snprintf(a->sa_err, sizeof(a->sa_err), "%s already exists in %s. Enter asks whether to replace it.",
			 full, a->sa_dir);
	} else {
		a->sa_err[0] = 0;
	}
}

static void first_heading(struct app *a, char *out, int n)
{
	out[0] = 0;
	if (!a->is_md)
		return;
	for (int i = 0; i < a->e.t.n; i++) {
		struct mdline *m = ed_parse(&a->e, i);

		if (m && m->kind >= B_H1 && m->kind <= B_H3 && m->n) {
			snprintf(out, (size_t)n, "%s", m->vis);
			return;
		}
	}
}

static void open_saveas(struct app *a)
{
	char heading[128];

	snprintf(a->sa_dir, sizeof(a->sa_dir), "%s", a->dir[0] ? a->dir : a->home);
	if (a->path[0]) {
		snprintf(a->sa_ext, sizeof(a->sa_ext), "%s", path_ext(a->path));
	} else if (a->is_md) {
		snprintf(a->sa_ext, sizeof(a->sa_ext), ".md");
	} else {
		const char *x = lang_exts(a->lang);

		snprintf(a->sa_ext, sizeof(a->sa_ext), "%.*s", (int)strcspn(x, " "), x);
	}
	first_heading(a, heading, sizeof(heading));
	name_suggest(a->sa_dir, a->path[0] ? a->name : 0, heading, a->sa_ext, a->sa_name, sizeof(a->sa_name));
	a->sa_caret = (int)strlen(a->sa_name);
	a->sa_focus = 0;
	sa_check(a);
	a->ov = OV_SAVEAS;
}

static void sa_save(struct app *a)
{
	char p[700], full[200];

	snprintf(full, sizeof(full), "%s%s", a->sa_name, a->sa_ext);
	join(p, sizeof(p), a->sa_dir, full);
	a->ov = OV_NONE;
	if (save_to(a, p))
		return;
	after_continue(a);
}

static void app_save(struct app *a)
{
	if (!a->path[0]) {
		open_saveas(a);
		return;
	}
	if (!save_to(a, a->path))
		after_continue(a);
}

/* ---- Open ------------------------------------------------------------------------- */

static int ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return !n;
}

static const char *const DRIVES[] = { "/mnt/sd", "/media/sd", "/data", "/tmp", "/", 0 };

static void op_add(struct app *a, const char *name, int dir, int items, time_t mtime, int isnew, int here)
{
	struct open_row *r;

	if (a->op_n % 32 == 0) {
		struct open_row *v = realloc(a->op, (size_t)(a->op_n + 32) * sizeof(*v));

		if (!v)
			return;
		a->op = v;
	}
	r = &a->op[a->op_n++];
	memset(r, 0, sizeof(*r));
	snprintf(r->name, sizeof(r->name), "%s", name);
	r->dir = dir;
	r->items = items;
	r->mtime = mtime;
	r->isnew = isnew;
	r->here = here;
}

static void op_load(struct app *a, const char *focus_name)
{
	struct entry *e = 0;
	int n, first_file = -1;

	a->op_n = 0;
	a->op_err[0] = 0;
	if (a->op_drives) {
		op_add(a, a->home, 1, 0, 0, 0, 0);
		for (int i = 0; DRIVES[i]; i++) {
			struct stat st;

			if (!stat(DRIVES[i], &st) && S_ISDIR(st.st_mode))
				op_add(a, DRIVES[i], 1, 0, 0, 0, 0);
		}
		a->op_focus = 0;
		return;
	}
	if (!a->op_filter[0])
		op_add(a, a->op_folders ? "This folder" : "New document", 0, 0, 0, !a->op_folders, a->op_folders);
	n = list_dir(a->op_dir, a->op_folders, &e, a->op_err, sizeof(a->op_err));
	for (int i = 0; i < n; i++) {
		if (!ci_has(e[i].name, a->op_filter))
			continue;
		if (!e[i].dir && first_file < 0)
			first_file = a->op_n;
		op_add(a, e[i].name, e[i].dir, e[i].items, e[i].mtime, 0, 0);
	}
	free(e);
	a->op_focus = first_file >= 0 ? first_file : 0;
	if (a->op_filter[0] && a->op_n)
		a->op_focus = 0;
	for (int i = 0; focus_name && i < a->op_n; i++)
		if (!strcmp(a->op[i].name, focus_name))
			a->op_focus = i;
}

static void open_screen(struct app *a, const char *dir, int folders)
{
	char here[200];

	snprintf(a->op_dir, sizeof(a->op_dir), "%s", dir);
	a->op_folders = folders;
	a->op_drives = 0;
	a->op_filter[0] = 0;
	a->screen = SC_OPEN;
	a->ov = OV_NONE;
	snprintf(here, sizeof(here), "%s", !strcmp(a->dir, dir) && a->path[0] ? a->name : "");
	op_load(a, here[0] ? here : 0);
}

static void op_close(struct app *a)
{
	a->screen = SC_EDIT;
	a->redraw = 1;
	if (a->op_folders) {
		a->ov = OV_SAVEAS;
		sa_check(a);
	}
}

static void op_up(struct app *a)
{
	char parent[512], base[200];

	path_split(a->op_dir, parent, sizeof(parent), base, sizeof(base));
	snprintf(a->op_dir, sizeof(a->op_dir), "%s", parent);
	op_load(a, base);
}

static void op_enter(struct app *a)
{
	struct open_row *r;
	char p[700];

	if (a->op_focus < 0 || a->op_focus >= a->op_n)
		return;
	r = &a->op[a->op_focus];
	if (a->op_drives) {
		snprintf(a->op_dir, sizeof(a->op_dir), "%s", r->name);
		a->op_drives = 0;
		a->op_filter[0] = 0;
		op_load(a, 0);
		return;
	}
	if (r->here) {
		snprintf(a->sa_dir, sizeof(a->sa_dir), "%s", a->op_dir);
		op_close(a);
		return;
	}
	if (r->isnew) {
		a->screen = SC_EDIT;
		request(a, AF_NEW, 0);
		return;
	}
	join(p, sizeof(p), a->op_dir, r->name);
	if (r->dir) {
		snprintf(a->op_dir, sizeof(a->op_dir), "%s", p);
		a->op_filter[0] = 0;
		op_load(a, 0);
		return;
	}
	a->screen = SC_EDIT;
	a->redraw = 1;
	request(a, AF_OPEN, p);
}

static void op_key(struct app *a, const struct key *k)
{
	switch (k->code) {
	case K_UP:
		if (a->op_focus > 0)
			a->op_focus--;
		return;
	case K_DOWN:
		if (a->op_focus < a->op_n - 1)
			a->op_focus++;
		return;
	case K_LEFT:
		a->op_focus = a->op_focus > 7 ? a->op_focus - 7 : 0;
		return;
	case K_RIGHT:
		a->op_focus = a->op_focus + 7 < a->op_n ? a->op_focus + 7 : (a->op_n ? a->op_n - 1 : 0);
		return;
	case K_ENTER:
		op_enter(a);
		return;
	case K_F2:
		snprintf(a->op_dir, sizeof(a->op_dir), "%s", a->home);
		a->op_drives = 0;
		a->op_filter[0] = 0;
		op_load(a, 0);
		return;
	case K_F3:
		a->op_drives = 1;
		a->op_filter[0] = 0;
		op_load(a, 0);
		return;
	case K_ESC:
		if (a->op_filter[0]) {
			a->op_filter[0] = 0;
			op_load(a, 0);
		} else if (a->op_drives) {
			a->op_drives = 0;
			op_load(a, 0);
		} else if (!strcmp(a->op_dir, a->home) || !strcmp(a->op_dir, "/")) {
			op_close(a);
		} else {
			op_up(a);
		}
		return;
	case K_BACKSPACE:
		if (a->op_filter[0]) {
			a->op_filter[strlen(a->op_filter) - 1] = 0;
			op_load(a, 0);
		}
		return;
	}
	if (k->ctrl && k->code == 49) {		/* Ctrl+N */
		a->screen = SC_EDIT;
		request(a, AF_NEW, 0);
		return;
	}
	if (k->text[0] && !k->ctrl && !k->alt && !a->op_drives &&
	    strlen(a->op_filter) + strlen(k->text) < sizeof(a->op_filter)) {
		strcat(a->op_filter, k->text);
		op_load(a, 0);
	}
}

/* ---- menus ------------------------------------------------------------------------- */

static void row(struct app *a, enum act act, const char *label, const char *value, int chev)
{
	struct menu_row *r = &a->rows[a->n_rows++];

	memset(r, 0, sizeof(*r));
	r->act = act;
	r->label = label;
	snprintf(r->value, sizeof(r->value), "%s", value ? value : "");
	r->chev = chev;
}

static void sep(struct app *a)
{
	struct menu_row *r = &a->rows[a->n_rows++];

	memset(r, 0, sizeof(*r));
	r->sep = 1;
}

static void open_options(struct app *a)
{
	a->n_rows = 0;
	row(a, A_SAVE, "Save", "Ctrl+S", 0);
	row(a, A_SAVE_AS, "Save as\xe2\x80\xa6", "Ctrl+Shift+S", 1);
	sep(a);
	row(a, A_NEW, "New", "Ctrl+N", 0);
	row(a, A_OPEN, "Open\xe2\x80\xa6", "Ctrl+O", 1);
	sep(a);
	row(a, A_FIND, "Find and replace", "Ctrl+F", 0);
	row(a, A_LANGUAGE, "Language", lang_name(a->lang), 1);	/* also to make a new document code */
	row(a, A_FULL, "Full screen", "Ctrl+Shift+F", 0);
	a->ov = OV_OPTIONS;
	a->focus = 0;
}

static void open_find_menu(struct app *a)
{
	a->n_rows = 0;
	row(a, A_MATCH_CASE, "Match case", "Alt+C", 0);
	a->rows[0].toggle = 1;
	a->rows[0].on = a->f.match_case;
	row(a, A_WHOLE_WORD, "Whole word", "Alt+W", 0);
	a->rows[1].toggle = 1;
	a->rows[1].on = a->f.whole_word;
	sep(a);
	row(a, A_REPLACE_ALL, "Replace all", "", 0);
	a->ov = OV_FIND_MENU;
	a->focus = 0;
}

static void settings_rows(struct app *a)
{
	a->n_rows = 0;
	row(a, A_TEXT_SIZE, "Text size", a->small ? "Small" : "Normal", 0);
	row(a, A_LINE_NUMBERS, "Line numbers in markdown", "", 0);
	a->rows[1].toggle = 1;
	a->rows[1].on = a->lineno;
}

static void menu_move(struct app *a, int dir)
{
	int f = a->focus;

	do
		f += dir;
	while (f >= 0 && f < a->n_rows && a->rows[f].sep);
	if (f >= 0 && f < a->n_rows)
		a->focus = f;
}

static void toggle_full(struct app *a)
{
	a->full = !a->full;
	a->hint_until = a->full ? a->now + 3000 : 0;
	a->redraw = 1;
}

/* ---- find ----------------------------------------------------------------------- */

static int find_fresh;		/* the query is selected: typing replaces it */

static void find_update(struct app *a)
{
	find_run(&a->f, &a->e);
	find_nearest(&a->f, &a->e);
	if (a->f.cur >= 0)
		find_select(&a->f, &a->e);
}

static void find_show(struct app *a, int focus)
{
	char w[128];

	if (a->e.sel && ed_word_at(&a->e, w, sizeof(w)) > 0) {
		snprintf(a->f.q, sizeof(a->f.q), "%s", w);
		find_fresh = 0;
	} else {
		find_fresh = a->f.q[0] != 0;
	}
	a->find_open = 1;
	a->find_focus = focus;
	a->replaced = 0;
	a->redraw = 1;
	find_update(a);
}

static void field_type(char *f, size_t cap, const char *s)
{
	if (strlen(f) + strlen(s) < cap)
		strcat(f, s);
}

static void field_erase(char *f)
{
	size_t n = strlen(f);

	while (n > 0 && ((unsigned char)f[n - 1] & 0xc0) == 0x80)
		n--;
	if (n > 0)
		n--;
	f[n] = 0;
}

static void replace_all(struct app *a)
{
	if (!a->f.n)
		find_run(&a->f, &a->e);
	a->replaced = find_replace_all(&a->f, &a->e, a->f.repl);
	a->redraw = 1;
}

static void act(struct app *a, enum act act);

static int find_key(struct app *a, const struct key *k)
{
	char *field = a->find_focus ? a->f.repl : a->f.q;
	size_t cap = a->find_focus ? sizeof(a->f.repl) : sizeof(a->f.q);

	if (k->alt && (k->code == 46 || k->code == 17)) {	/* Alt+C, Alt+W */
		if (k->code == 46)
			a->f.match_case = !a->f.match_case;
		else
			a->f.whole_word = !a->f.whole_word;
		find_update(a);
		return 1;
	}
	if (k->ctrl) {
		if (k->code == 33 && !k->shift) {	/* Ctrl+F */
			a->find_focus = 0;
			return 1;
		}
		if (k->code == 35) {	/* Ctrl+H */
			a->find_focus = 1;
			return 1;
		}
		return 0;		/* Ctrl+Z, Ctrl+S ...: the editor's */
	}
	switch (k->code) {
	case K_ESC:
		a->find_open = 0;
		a->replaced = 0;
		a->redraw = 1;
		return 1;
	case K_UP: case K_DOWN: case K_TAB:
		a->find_focus = !a->find_focus;
		return 1;
	case K_ENTER:
		if (a->find_focus && !k->shift) {
			find_replace(&a->f, &a->e, a->f.repl);
		} else {
			if (!a->f.n)
				find_run(&a->f, &a->e);
			find_step(&a->f, &a->e, k->shift ? -1 : 1);
		}
		return 1;
	case K_F1:
		open_find_menu(a);
		return 1;
	case K_F2:
		if (a->find_focus)
			replace_all(a);
		return 1;
	case K_BACKSPACE:
		field_erase(field);
		if (!a->find_focus) {
			find_fresh = 0;
			find_update(a);
		}
		return 1;
	case K_LEFT: case K_RIGHT:
		return 1;
	}
	if (k->code >= K_F3 && k->code <= K_F6)
		return 0;		/* ● ♣ ◆: as in the editor */
	if (k->text[0]) {
		if (!a->find_focus && find_fresh)
			a->f.q[0] = 0;
		find_fresh = 0;
		field_type(field, cap, k->text);
		if (!a->find_focus)
			find_update(a);
		return 1;
	}
	return 1;
}

/* ---- styles in either markdown view -------------------------------------------- */

static void toggle_style(struct app *a, int flag, int word)
{
	enum mode m = a->e.mode;

	if (m == M_CODE)
		return;
	if (m == M_SOURCE)
		ed_set_mode(&a->e, M_RICH);
	ed_toggle(&a->e, flag, word);
	if (m == M_SOURCE) {
		int t = a->e.tstyle;

		ed_set_mode(&a->e, M_SOURCE);
		a->e.tstyle = t;
	}
}

static enum blk para_at(struct app *a)
{
	enum blk b = ed_block_at(&a->e);

	return b == B_FENCE ? B_CODE : b;
}

const char *app_para_name(enum blk kind)
{
	switch (kind) {
	case B_H1: return "Heading 1";
	case B_H2: return "Heading 2";
	case B_H3: return "Heading 3";
	case B_BULLET: return "Bulleted list";
	case B_NUMBER: return "Numbered list";
	case B_QUOTE: return "Quote";
	case B_CODE: case B_FENCE: return "Code block";
	default: return "Body";
	}
}

static void open_style(struct app *a)
{
	enum blk b = para_at(a);

	a->ov = OV_STYLE;
	a->focus = N_MARKS;
	for (int i = 0; i < N_PARAS; i++)
		if (PARAS[i] == b)
			a->focus = N_MARKS + i;
}

static void style_key(struct app *a, const struct key *k)
{
	switch (k->code) {
	case K_UP:
		if (a->focus > N_MARKS)
			a->focus--;
		else if (a->focus == N_MARKS)
			a->focus = 0;
		break;
	case K_DOWN:
		if (a->focus < N_MARKS)
			a->focus = N_MARKS;
		else if (a->focus < N_MARKS + N_PARAS - 1)
			a->focus++;
		break;
	case K_LEFT:
		if (a->focus > 0 && a->focus < N_MARKS)
			a->focus--;
		break;
	case K_RIGHT:
		if (a->focus < N_MARKS - 1)
			a->focus++;
		break;
	case K_ENTER:
		a->ov = OV_NONE;
		if (a->focus < N_MARKS)
			toggle_style(a, a->focus == 0 ? ST_B : a->focus == 1 ? ST_I : ST_U, 1);
		else
			ed_block(&a->e, PARAS[a->focus - N_MARKS], 0);
		break;
	case K_ESC: case K_F2:
		a->ov = OV_NONE;
		break;
	}
}

/* ---- running a script ---------------------------------------------------------- */

static void run(struct app *a)
{
	if (!lang_runs(a->lang))
		return;
	if (!a->path[0]) {
		a->after = AF_RUN;
		open_saveas(a);
		return;
	}
	if (text_dirty(&a->e.t) && save_to(a, a->path))
		return;
	a->run = lang_runs(a->lang);
}

/* ---- the clipboard ----------------------------------------------------------------- */

static void clip_put(struct app *a, char *s, size_t n, int words, const char *verb)
{
	char err[128];

	if (!s)
		return;
	if (file_save(app_clip_path(), s, n, err, sizeof(err)))
		app_log(a, "error: clipboard: %s", err);
	else
		snprintf(a->clip_label, sizeof(a->clip_label), "%s %d word%s", verb, words, words == 1 ? "" : "s");
	free(s);
}

static void clip_paste(struct app *a)
{
	char *s, err[128];
	size_t n;

	if (file_load(app_clip_path(), &s, &n, err, sizeof(err)))
		return;
	if (ed_paste(&a->e, s, n))
		snprintf(a->notice, sizeof(a->notice), "Not enough memory");
	free(s);
}

/* ---- the editor's keys ------------------------------------------------------------- */

static void act(struct app *a, enum act what)
{
	switch (what) {
	case A_SAVE:
		app_save(a);
		break;
	case A_SAVE_AS:
		open_saveas(a);
		break;
	case A_NEW:
		request(a, AF_NEW, 0);
		break;
	case A_OPEN:
		open_screen(a, a->dir[0] ? a->dir : a->home, 0);
		break;
	case A_FIND:
		find_show(a, 0);
		break;
	case A_FULL:
		toggle_full(a);
		break;
	case A_LANGUAGE:
		a->ov = OV_LANGUAGE;
		a->focus = a->lang;
		break;
	case A_MATCH_CASE:
		a->f.match_case = !a->f.match_case;
		find_update(a);
		break;
	case A_WHOLE_WORD:
		a->f.whole_word = !a->f.whole_word;
		find_update(a);
		break;
	case A_REPLACE_ALL:
		replace_all(a);
		break;
	case A_TEXT_SIZE:
		a->small = !a->small;
		settings_save(a);
		settings_rows(a);
		a->redraw = 1;
		break;
	case A_LINE_NUMBERS:
		a->lineno = !a->lineno;
		settings_save(a);
		settings_rows(a);
		a->redraw = 1;
		break;
	default:
		break;
	}
}

static int ctrl_key(struct app *a, const struct key *k)
{
	size_t n;
	int words;

	switch (k->code) {
	case 30:		/* A */
		ed_select_all(&a->e);
		return 1;
	case 46:		/* C */
		if (a->e.sel)
			clip_put(a, ed_copy(&a->e, &n, &words), n, words, "Copied");
		return 1;
	case 45:		/* X */
		if (a->e.sel)
			clip_put(a, ed_cut(&a->e, &n, &words), n, words, "Cut");
		return 1;
	case 47:		/* V */
		clip_paste(a);
		return 1;
	case 44:		/* Z */
		if (k->shift)
			ed_redo(&a->e);
		else
			ed_undo(&a->e);
		return 1;
	case 21:		/* Y */
		ed_redo(&a->e);
		return 1;
	case 48:		/* B */
		toggle_style(a, ST_B, 0);
		return 1;
	case 23:		/* I */
		toggle_style(a, ST_I, 0);
		return 1;
	case 22:		/* U */
		toggle_style(a, ST_U, 0);
		return 1;
	case K_0: case K_1: case K_1 + 1: case K_1 + 2:
		ed_block(&a->e, k->code == K_0 ? B_BODY : (enum blk)(B_H1 + k->code - K_1), 0);
		return 1;
	case K_8:
		if (k->shift)
			ed_block(&a->e, B_BULLET, 1);
		return 1;
	case K_7:
		if (k->shift)
			ed_block(&a->e, B_NUMBER, 1);
		return 1;
	case 49:		/* N */
		request(a, AF_NEW, 0);
		return 1;
	case 24:		/* O */
		act(a, A_OPEN);
		return 1;
	case 31:		/* S */
		if (k->shift)
			open_saveas(a);
		else
			app_save(a);
		return 1;
	case 33:		/* F */
		if (k->shift)
			toggle_full(a);
		else
			find_show(a, 0);
		return 1;
	case 35:		/* H */
		find_show(a, 1);
		return 1;
	}
	return 0;
}

static void move_key(struct app *a, const struct key *k)
{
	int sel = k->fn || k->shift;
	int horiz = k->code == K_LEFT || k->code == K_RIGHT;

	if (horiz) {
		int right = k->code == K_RIGHT;

		if (k->alt)
			ed_move(&a->e, right ? MV_END : MV_HOME, sel);
		else if (k->ctrl)
			ed_move(&a->e, right ? MV_WORD_RIGHT : MV_WORD_LEFT, sel);
		else
			ed_move(&a->e, right ? MV_RIGHT : MV_LEFT, sel);
		a->want_x = -1;
		return;
	}
	if (k->alt) {
		ed_move(&a->e, k->code == K_DOWN ? MV_DOC_END : MV_DOC_START, sel);
		a->want_x = -1;
		return;
	}
	{
		int want = a->want_x;
		struct pos p;

		lay_config(a);
		lay_ensure(&a->lay, &a->e);
		if (a->e.sel && !sel) {	/* an arrow without select: to that side first */
			struct pos s0, s1;

			ed_sel_range(&a->e, &s0, &s1);
			ed_set_caret(&a->e, k->code == K_UP ? s0 : s1, 0);
		}
		p = lay_vmove(&a->lay, &a->e, k->code == K_DOWN ? 1 : -1, &want);
		ed_set_caret(&a->e, p, sel);
		a->want_x = want;
	}
}

static void edit_key(struct app *a, const struct key *k)
{
	if (a->find_open && find_key(a, k))
		return;
	if (k->ctrl && ctrl_key(a, k))
		return;
	switch (k->code) {
	case K_ESC:
		if (a->e.sel)
			ed_set_caret(&a->e, a->e.caret, 0);
		else if (a->find_open)
			a->find_open = 0, a->redraw = 1;
		else if (a->full)
			toggle_full(a);
		else
			request(a, AF_CLOSE, 0);
		return;
	case K_F1:
		open_options(a);
		return;
	case K_F2:
		if (a->e.mode == M_CODE)
			run(a);
		else
			open_style(a);
		return;
	case K_F3:
		if (a->is_md) {
			ed_set_mode(&a->e, a->e.mode == M_RICH ? M_SOURCE : M_RICH);
			a->redraw = 1;
		}
		return;
	case K_F4:
		a->ov = OV_LOG;
		return;
	case K_F5:
		settings_rows(a);
		a->ov = OV_SETTINGS;
		a->focus = 0;
		return;
	case K_F6:
		a->ov = OV_HELP;
		return;
	case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
		move_key(a, k);
		return;
	case K_ENTER:
		if (ed_newline(&a->e))
			snprintf(a->notice, sizeof(a->notice), "Not enough memory");
		a->want_x = -1;
		return;
	case K_BACKSPACE:
		if (k->fn ? ed_delete(&a->e) : ed_backspace(&a->e))
			snprintf(a->notice, sizeof(a->notice), "Not enough memory");
		a->want_x = -1;
		return;
	case K_TAB:
		ed_tab(&a->e);
		return;
	}
	if (k->text[0] && !k->ctrl && !k->alt) {
		if (ed_type(&a->e, k->text, (int)strlen(k->text)))
			snprintf(a->notice, sizeof(a->notice), "Not enough memory");
		a->want_x = -1;
	}
}

/* ---- overlays ------------------------------------------------------------------------ */

static void sa_key(struct app *a, const struct key *k)
{
	int n = (int)strlen(a->sa_name);

	switch (k->code) {
	case K_ESC:
		a->ov = OV_NONE;
		a->after = AF_NONE;
		return;
	case K_DOWN:
		a->sa_focus = 1;
		return;
	case K_UP:
		a->sa_focus = 0;
		return;
	case K_ENTER:
		if (a->sa_focus) {
			open_screen(a, a->sa_dir, 1);
			return;
		}
		sa_check(a);
		if (a->sa_err[0] && !a->sa_exists)
			return;
		if (a->sa_exists) {
			a->ov = OV_REPLACE_ASK;
			a->focus = 0;
			return;
		}
		sa_save(a);
		return;
	case K_LEFT:
		while (a->sa_caret > 0 && ((unsigned char)a->sa_name[--a->sa_caret] & 0xc0) == 0x80)
			;
		return;
	case K_RIGHT:
		if (a->sa_caret < n)
			while (++a->sa_caret < n && ((unsigned char)a->sa_name[a->sa_caret] & 0xc0) == 0x80)
				;
		return;
	case K_BACKSPACE:
		if (a->sa_focus == 0 && a->sa_caret > 0) {
			int from = a->sa_caret;

			while (from > 0 && ((unsigned char)a->sa_name[--from] & 0xc0) == 0x80)
				;
			memmove(a->sa_name + from, a->sa_name + a->sa_caret, (size_t)(n - a->sa_caret) + 1);
			a->sa_caret = from;
			sa_check(a);
		}
		return;
	}
	if (k->text[0] && !k->ctrl && !k->alt && a->sa_focus == 0) {
		int t = (int)strlen(k->text);

		if (n + t >= (int)sizeof(a->sa_name))
			return;
		memmove(a->sa_name + a->sa_caret + t, a->sa_name + a->sa_caret, (size_t)(n - a->sa_caret) + 1);
		memcpy(a->sa_name + a->sa_caret, k->text, (size_t)t);
		a->sa_caret += t;
		sa_check(a);
	}
}

static void menu_key(struct app *a, const struct key *k, int stay_open)
{
	switch (k->code) {
	case K_UP:
		menu_move(a, -1);
		return;
	case K_DOWN:
		menu_move(a, 1);
		return;
	case K_ESC:
		a->ov = OV_NONE;
		return;
	case K_ENTER: {
		enum act what = a->rows[a->focus].act;

		if (!stay_open)
			a->ov = OV_NONE;
		act(a, what);
		return;
	}
	}
	if (a->ov == OV_SETTINGS && k->code == K_F5)
		a->ov = OV_NONE;
}

static void overlay_key(struct app *a, const struct key *k)
{
	switch (a->ov) {
	case OV_OPTIONS:
	case OV_FIND_MENU:
		if (k->code == K_F1) {
			a->ov = OV_NONE;
			return;
		}
		menu_key(a, k, 0);
		return;
	case OV_SETTINGS:
		menu_key(a, k, 1);
		return;
	case OV_STYLE:
		style_key(a, k);
		return;
	case OV_HELP:
	case OV_LOG:
		if (k->code == K_ESC || k->code == K_ENTER || k->code == K_F6 || k->code == K_F4)
			a->ov = OV_NONE;
		return;
	case OV_ERROR:
		if (k->code == K_ESC || k->code == K_ENTER)
			a->ov = OV_NONE;
		return;
	case OV_DISCARD:
		if (k->code == K_UP && a->focus > 0)
			a->focus--;
		else if (k->code == K_DOWN && a->focus < 2)
			a->focus++;
		else if (k->code == K_ESC) {
			a->ov = OV_NONE;
			a->after = AF_NONE;
		} else if (k->code == K_ENTER) {
			a->ov = OV_NONE;
			if (a->focus == 0)
				a->after = AF_NONE;
			else if (a->focus == 1)
				app_save(a);
			else
				after_continue(a);
		}
		return;
	case OV_REPLACE_ASK:
		if (k->code == K_UP)
			a->focus = 0;
		else if (k->code == K_DOWN)
			a->focus = 1;
		else if (k->code == K_ESC || (k->code == K_ENTER && a->focus == 0))
			a->ov = OV_SAVEAS;
		else if (k->code == K_ENTER)
			sa_save(a);
		return;
	case OV_LANGUAGE:
		if (k->code == K_UP && a->focus > 0)
			a->focus--;
		else if (k->code == K_DOWN && a->focus < L_COUNT - 1)
			a->focus++;
		else if (k->code == K_ESC)
			a->ov = OV_NONE;
		else if (k->code == K_ENTER) {
			a->ov = OV_NONE;
			set_lang(a, (enum lang)a->focus);
		}
		return;
	case OV_SAVEAS:
		sa_key(a, k);
		return;
	default:
		a->ov = OV_NONE;
	}
}

void app_key(struct app *a, const struct key *k)
{
	enum mode m = a->e.mode;
	int full = a->full, find = a->find_open;

	a->notice[0] = 0;
	if (a->full && a->hint_until)
		a->hint_until = 0;	/* the first key ends the hint */
	if (a->screen == SC_OPEN)
		op_key(a, k);
	else if (a->ov != OV_NONE)
		overlay_key(a, k);
	else
		edit_key(a, k);
	if (a->e.mode != m || a->full != full || a->find_open != find)
		a->redraw = 1;
	app_follow(a);
}

void app_fn(struct app *a, int down)
{
	a->fn_held = down;
}

int app_tick(struct app *a, long now)
{
	a->now = now;
	if (a->hint_until && now >= a->hint_until) {
		a->hint_until = 0;
		return 1;
	}
	return 0;
}

void app_term(struct app *a)
{
	if (dirty(a)) {
		if (a->path[0]) {
			save_to(a, a->path);
		} else if (text_size(&a->e.t)) {
			char p[600];

			rescue_path(a->home, p, sizeof(p));
			save_to(a, p);
		}
	}
	a->quit = 1;
}

/* ---- start and stop ----------------------------------------------------------------- */

void app_init(struct app *a, const char *path)
{
	memset(a, 0, sizeof(*a));
	ed_init(&a->e);
	find_init(&a->f);
	lay_init(&a->lay);
	a->lay.adv16 = app_adv16 ? app_adv16 : fake_adv16;
	snprintf(a->home, sizeof(a->home), "%s", env_or("WRITE_HOME", "/root"));
	a->want_x = -1;
	settings_load(a);
	log_load(a);
	new_doc(a);
	if (path && do_open(a, path))
		new_doc(a);
	app_follow(a);
}

void app_free(struct app *a)
{
	ed_free(&a->e);
	find_free(&a->f);
	lay_free(&a->lay);
	free(a->op);
	memset(a, 0, sizeof(*a));
}

/* ---- for drawing ---------------------------------------------------------------- */

#define FIND_H	120

void app_geometry(const struct app *a, int *x, int *y, int *w, int *h)
{
	if (a->full) {
		*x = 56;
		*y = 36;
		*w = 720 - 56 - 56 - 17;
		*h = 680 - 36;
		return;
	}
	*y = 92 + 14;
	if (a->find_open)
		*y = 92 + 10 + FIND_H + (a->replaced ? 30 : 0) + 12;
	if (a->e.mode == M_SOURCE) {
		*x = a->lineno ? 24 + 28 + 16 : 24;
	} else if (a->e.mode == M_CODE) {
		*x = 16 + 32 + 14;
	} else {
		*x = 48;
	}
	*w = 720 - *x - 48 - 16;
	*h = 664 - *y;
}

void app_follow(struct app *a)
{
	int x, y, w, h, cx, cy, ch;

	lay_config(a);
	lay_ensure(&a->lay, &a->e);
	app_geometry(a, &x, &y, &w, &h);
	lay_caret(&a->lay, &a->e, a->e.caret, &cx, &cy, &ch);
	if (a->full) {
		int target = cy - h * 2 / 3;

		/* typewriter: the caret stays two thirds down, once the text is that long */
		if (target < 0)
			target = 0;
		if (cy - a->scroll > h * 2 / 3 || cy < a->scroll)
			a->scroll = target;
	} else {
		int max = a->lay.total - h;

		if (cy < a->scroll)
			a->scroll = cy;
		if (cy + ch > a->scroll + h)
			a->scroll = cy + ch - h;
		if (a->scroll > max)
			a->scroll = max > 0 ? max : 0;
		if (cy + ch > a->scroll + h)
			a->scroll = cy + ch - h;
	}
	if (a->scroll < 0)
		a->scroll = 0;
}

static int chars_before(const char *s, int col)
{
	int n = 0;

	for (int i = 0; i < col; i++)
		n += ((unsigned char)s[i] & 0xc0) != 0x80;
	return n;
}

void app_docbar(struct app *a, struct docbar *d)
{
	memset(d, 0, sizeof(*d));
	d->name = a->name;
	d->edited = dirty(a);
	d->md_view = a->is_md ? a->e.mode == M_SOURCE : -1;
	if (a->notice[0]) {
		snprintf(d->info, sizeof(d->info), "%s", a->notice);
		d->info_kind = 3;
	} else if (a->e.sel) {
		snprintf(d->info, sizeof(d->info), "%d selected", ed_sel_words(&a->e));
		d->info_kind = 1;
	} else if (a->clip_label[0]) {
		snprintf(d->info, sizeof(d->info), "%s", a->clip_label);
		d->info_kind = 2;
	} else if (!a->path[0]) {
		snprintf(d->info, sizeof(d->info), "not saved yet");
	} else if (a->e.mode != M_RICH) {
		snprintf(d->info, sizeof(d->info), "ln %d, col %d", a->e.caret.line + 1,
			 chars_before(a->e.t.l[a->e.caret.line].s, a->e.caret.col) + 1);
	}
	if (a->e.mode == M_CODE) {
		d->lang = lang_name(a->lang);
		if (a->e.indent)
			snprintf(d->indent, sizeof(d->indent), "%d space%s", a->e.indent, a->e.indent == 1 ? "" : "s");
		else
			snprintf(d->indent, sizeof(d->indent), "tab");
		snprintf(d->count, sizeof(d->count), "%d line%s", a->e.t.n, a->e.t.n == 1 ? "" : "s");
	} else {
		int w = ed_words(&a->e);
		enum mode m = a->e.mode;

		d->para = app_para_name(para_at(a));
		a->e.mode = M_RICH;	/* the styles as the rich view sees them */
		d->style = ed_style_at(&a->e);
		a->e.mode = m;
		snprintf(d->count, sizeof(d->count), "%d word%s", w, w == 1 ? "" : "s");
	}
}

static int hint(struct hint *h, int n, int max, const char *keys, const char *label, int right, int pink)
{
	if (n < max)
		h[n] = (struct hint){ keys, label, right, pink };
	return n + 1;
}

int app_hints(const struct app *a, struct hint *h, int max)
{
	int n = 0;

	if (a->screen == SC_OPEN) {
		if (a->op_folders) {
			n = hint(h, n, max, "Enter", "choose", 0, 0);
		} else {
			n = hint(h, n, max, "#2", "home", 0, 0);
			n = hint(h, n, max, "#3", "drives", 0, 0);
			n = hint(h, n, max, "Enter", "open", 0, 0);
		}
		n = hint(h, n, max, "Esc", a->op_filter[0] ? "clear" :
			 (!strcmp(a->op_dir, a->home) || !strcmp(a->op_dir, "/") || a->op_drives) ?
			 (a->op_folders ? "back" : "close") : "up", 1, 0);
		return n < max ? n : max;
	}
	switch (a->ov) {
	case OV_NONE:
		break;
	case OV_HELP: case OV_LOG:
		return hint(h, n, max, "Esc", "close", 1, 0);
	case OV_ERROR:
		return hint(h, n, max, "Enter", "OK", 0, 0);
	case OV_STYLE:
		n = hint(h, n, max, "Enter", "apply", 0, 0);
		return hint(h, n, max, "Esc", "close", 1, 0);
	case OV_SAVEAS:
		n = hint(h, n, max, "Enter", a->sa_focus ? "choose" : "save", 0, 0);
		return hint(h, n, max, "Esc", "cancel", 1, 0);
	case OV_SETTINGS:
		n = hint(h, n, max, "Enter", "change", 0, 0);
		return hint(h, n, max, "Esc", "close", 1, 0);
	case OV_DISCARD: case OV_REPLACE_ASK: case OV_LANGUAGE:
		n = hint(h, n, max, "Enter", "choose", 0, 0);
		return hint(h, n, max, "Esc", "cancel", 1, 0);
	default:
		n = hint(h, n, max, "Enter", "choose", 0, 0);
		return hint(h, n, max, "Esc", "close", 1, 0);
	}
	n = hint(h, n, max, "#1", "options", 0, 0);
	if (a->find_open) {
		if (a->find_focus) {
			n = hint(h, n, max, "#2", "replace all", 0, 0);
			n = hint(h, n, max, "Enter", "replace", 0, 0);
		} else {
			n = hint(h, n, max, "Enter", "next", 0, 0);
			n = hint(h, n, max, "Shift+Enter", "previous", 0, 0);
		}
	} else if (a->e.mode == M_CODE) {
		if (lang_runs(a->lang))
			n = hint(h, n, max, "#2", lang_runs(a->lang) == 2 ? "preview" : "run", 0, 0);
	} else {
		n = hint(h, n, max, "#2", "style", 0, 0);
		if (a->e.sel) {
			n = hint(h, n, max, "Ctrl+C", "copy", 0, 0);
			n = hint(h, n, max, "Ctrl+X", "cut", 0, 0);
		} else {
			n = hint(h, n, max, "#3", a->e.mode == M_RICH ? "markdown" : "rich text", 0, 0);
		}
	}
	if (a->e.mode == M_CODE && a->e.sel && !a->find_open) {
		n = hint(h, n, max, "Ctrl+C", "copy", 0, 0);
		n = hint(h, n, max, "Ctrl+X", "cut", 0, 0);
	}
	if (a->fn_held)
		n = hint(h, n, max, "Fn", "", 1, 1);
	n = hint(h, n, max, "Esc", a->e.sel || a->find_open ? (a->find_open && !a->e.sel ? "close" : "cancel") :
		 a->full ? "leave" : "close", 1, 0);
	return n < max ? n : max;
}
