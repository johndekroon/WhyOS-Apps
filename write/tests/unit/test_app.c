/* SPDX-License-Identifier: MIT */
/* The app's states and keys (src/app.c): design/write/README.md §4-§10. */
#include <sys/stat.h>
#include <unistd.h>

#include "t.h"
#include "app.h"

static struct app A;
static char home[256], tmp[300];

static void fresh_dirs(void)
{
	char p[400];

	snprintf(home, sizeof(home), "%s/write-app-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OK(mkdtemp(home) != 0);
	snprintf(tmp, sizeof(tmp), "%s/tmp", home);
	mkdir(tmp, 0755);
	setenv("WRITE_HOME", home, 1);
	snprintf(p, sizeof(p), "%s/write.clip", tmp);
	setenv("WRITE_CLIP", p, 1);
	snprintf(p, sizeof(p), "%s/write.log", tmp);
	setenv("WRITE_LOG", p, 1);
	snprintf(p, sizeof(p), "%s/config", home);
	setenv("APP_CONFIG", p, 1);
}

static void start(const char *file, const char *body)
{
	char p[512];

	app_free(&A);
	fresh_dirs();
	if (file) {
		FILE *f;

		snprintf(p, sizeof(p), "%s/%s", home, file);
		f = fopen(p, "w");
		fputs(body, f);
		fclose(f);
	}
	app_init(&A, file ? p : 0);
}

static void put(const char *file, const char *body)
{
	char p[512];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", home, file);
	f = fopen(p, "w");
	fputs(body, f);
	fclose(f);
}

static const char *slurp(const char *file)
{
	static char buf[4096];
	char p[512];
	FILE *f;
	size_t n;

	snprintf(p, sizeof(p), file[0] == '/' ? "%s" : "%s/%s", file[0] == '/' ? file : home, file);
	if (file[0] == '/')
		snprintf(p, sizeof(p), "%s", file);
	f = fopen(p, "r");
	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = 0;
	fclose(f);
	return buf;
}

static const char *src(void)
{
	static char buf[4096];
	size_t n;
	char *s = text_get(&A.e.t, &n);

	snprintf(buf, sizeof(buf), "%.*s", (int)n, s);
	free(s);
	return buf;
}

/* keys */
static void key(int code, int shift, int ctrl, int alt, int fn, const char *text)
{
	struct key k = { code, shift, ctrl, alt, fn, "" };

	snprintf(k.text, sizeof(k.text), "%s", text ? text : "");
	app_key(&A, &k);
}

static void press(int code)
{
	key(code, 0, 0, 0, 0, 0);
}

static void type(const char *s)
{
	for (; *s; s++) {
		char t[2] = { *s, 0 };

		if (*s == '\n')
			press(K_ENTER);
		else
			key(0, 0, 0, 0, 0, t);
	}
}

/* Ctrl+letter: the code of the letter's key (US layout) */
static int code_of(char c)
{
	static const char row[] = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
				  "\0" "\\zxcvbnm,./";

	for (int i = 0; i < (int)sizeof(row) - 1; i++)
		if (row[i] == c)
			return i;
	return 0;
}

static void ctrl(char c)
{
	key(code_of(c), 0, 1, 0, 0, 0);
}

static void ctrl_shift(char c)
{
	key(code_of(c), 1, 1, 0, 0, 0);
}

static int has_hint(const char *keys, const char *label)
{
	struct hintk h[12];
	int n = app_hints(&A, h, 12);

	for (int i = 0; i < n; i++)
		if (!strcmp(h[i].keys, keys) && !strcmp(h[i].label, label))
			return 1;
	return 0;
}

TEST(starts_with_a_new_document)
{
	struct docbar d;

	start(0, 0);
	EQ(A.screen, SC_EDIT);
	EQ(A.ov, OV_NONE);
	EQ(A.e.mode, M_RICH);
	app_docbar(&A, &d);
	STREQ(d.name, "Untitled");
	STREQ(d.info, "not saved yet");
	STREQ(d.count, "0 words");
	STREQ(d.para, "Body");
	EQ(d.md_view, 0);
	OK(has_hint("#1", "options"));
	OK(has_hint("#2", "style"));
	OK(has_hint("#3", "markdown"));
	OK(has_hint("Esc", "close"));
}

TEST(esc_without_changes_closes)
{
	start(0, 0);
	press(K_ESC);
	EQ(A.quit, 1);
}

TEST(esc_with_changes_asks)
{
	start(0, 0);
	type("hello");
	press(K_ESC);
	EQ(A.ov, OV_DISCARD);
	EQ(A.focus, 0);			/* Keep editing */
	press(K_ENTER);
	EQ(A.ov, OV_NONE);
	EQ(A.quit, 0);
	press(K_ESC);
	press(K_DOWN);
	press(K_DOWN);
	press(K_ENTER);			/* Discard changes */
	EQ(A.quit, 1);
}

TEST(save_a_new_document_names_it)
{
	struct docbar d;

	start(0, 0);
	type("# Hacker camp notes\nBring the badge");
	ctrl('s');
	EQ(A.ov, OV_SAVEAS);
	STREQ(A.sa_name, "hacker-camp-notes");
	STREQ(A.sa_ext, ".md");
	STREQ(A.sa_dir, home);
	press(K_ENTER);
	EQ(A.ov, OV_NONE);
	STREQ(slurp("hacker-camp-notes.md"), "# Hacker camp notes\nBring the badge");
	app_docbar(&A, &d);
	STREQ(d.name, "hacker-camp-notes.md");
	EQ(d.edited, 0);
	OK(A.n_log > 0);
	OK(strstr(A.log[A.n_log - 1], "hacker-camp-notes.md") != 0);
	OK(strstr(A.log[A.n_log - 1], "6 words") != 0);
	type("!");
	ctrl('s');			/* named now: saves at once */
	EQ(A.ov, OV_NONE);
	STREQ(slurp("hacker-camp-notes.md"), "# Hacker camp notes\nBring the badge!");
}

TEST(save_as_suggests_v2_and_types_a_name)
{
	start("todo.md", "x\n");
	ctrl_shift('s');
	EQ(A.ov, OV_SAVEAS);
	STREQ(A.sa_name, "todo-v2");
	for (int i = 0; i < 7; i++)
		press(K_BACKSPACE);
	type("list");
	STREQ(A.sa_name, "list");
	press(K_ENTER);
	STREQ(slurp("list.md"), "x\n");
	STREQ(A.name, "list.md");
	STREQ(slurp("todo.md"), "x\n");		/* the old file stays */
}

TEST(save_as_an_existing_name_asks)
{
	start("camp.md", "new\n");
	put("todo.md", "old\n");
	ctrl_shift('s');
	for (int i = 0; i < 10; i++)
		press(K_BACKSPACE);
	type("todo");
	EQ(A.sa_exists, 1);
	OK(strstr(A.sa_err, "todo.md already exists") != 0);
	press(K_ENTER);
	EQ(A.ov, OV_REPLACE_ASK);
	EQ(A.focus, 0);			/* Cancel */
	press(K_ENTER);
	EQ(A.ov, OV_SAVEAS);
	STREQ(slurp("todo.md"), "old\n");
	press(K_ENTER);
	press(K_DOWN);
	press(K_ENTER);			/* Replace */
	EQ(A.ov, OV_NONE);
	STREQ(slurp("todo.md"), "new\n");
}

TEST(save_as_refuses_bad_names)
{
	start(0, 0);
	type("x");
	ctrl('s');
	for (int i = 0; i < 20; i++)
		press(K_BACKSPACE);
	press(K_ENTER);
	EQ(A.ov, OV_SAVEAS);
	STREQ(A.sa_err, "Type a name");
	type("a/b");
	STREQ(A.sa_err, "A name cannot have a /");
	press(K_ENTER);
	EQ(A.ov, OV_SAVEAS);
	press(K_ESC);
	EQ(A.ov, OV_NONE);
}

TEST(opens_files_by_kind)
{
	struct docbar d;

	start("blink.py", "def blink():\n    pass\n");
	EQ(A.e.mode, M_CODE);
	EQ(A.lang, L_PYTHON);
	EQ(A.e.indent, 4);
	app_docbar(&A, &d);
	STREQ(d.lang, "Python");
	STREQ(d.indent, "4 spaces");
	STREQ(d.count, "2 lines");
	EQ(d.md_view, -1);
	OK(has_hint("#2", "run"));
	start("index.html", "<p>x</p>\n");
	OK(has_hint("#2", "preview"));
	start("notes.md", "# A\n");
	EQ(A.e.mode, M_RICH);
	start("plain.txt", "x");
	EQ(A.e.mode, M_CODE);
	EQ(A.lang, L_PLAIN);
	OK(!has_hint("#2", "run"));
}

TEST(cross_switches_rich_and_markdown)
{
	struct docbar d;

	start("n.md", "**b**\n");
	press(K_F3);
	EQ(A.e.mode, M_SOURCE);
	app_docbar(&A, &d);
	EQ(d.md_view, 1);
	STREQ(d.info, "ln 1, col 3");		/* the rich caret was before the b, inside the ** */
	OK(has_hint("#3", "rich text"));
	press(K_F3);
	EQ(A.e.mode, M_RICH);
	start("c.py", "x\n");
	press(K_F3);
	EQ(A.e.mode, M_CODE);
}

TEST(options_menu)
{
	start("n.md", "a\n");
	press(K_F1);
	EQ(A.ov, OV_OPTIONS);
	EQ(A.rows[A.focus].act, A_SAVE);
	press(K_DOWN);
	EQ(A.rows[A.focus].act, A_SAVE_AS);
	press(K_DOWN);			/* past the line */
	EQ(A.rows[A.focus].act, A_NEW);
	press(K_DOWN);
	press(K_DOWN);
	press(K_DOWN);
	press(K_DOWN);			/* Language */
	EQ(A.rows[A.focus].act, A_FULL);
	press(K_ENTER);
	EQ(A.ov, OV_NONE);
	EQ(A.full, 1);
	press(K_ESC);			/* Esc leaves full screen first */
	EQ(A.full, 0);
	EQ(A.quit, 0);
}

TEST(options_in_code_has_language)
{
	start("x.py", "a\n");
	press(K_F1);
	for (int i = 0; i < A.n_rows; i++)
		if (A.rows[i].act == A_LANGUAGE) {
			STREQ(A.rows[i].value, "Python");
			A.focus = i;
		}
	press(K_ENTER);
	EQ(A.ov, OV_LANGUAGE);
	EQ(A.focus, L_PYTHON);		/* on the current one */
	press(K_DOWN);
	press(K_DOWN);
	press(K_DOWN);
	press(K_DOWN);
	press(K_ENTER);
	EQ(A.lang, L_JS);
	EQ(A.ov, OV_NONE);
}

TEST(open_screen)
{
	start("a.md", "first\n");
	put("b.md", "second\n");
	mkdir(strcat(strcpy(tmp, home), "/notes"), 0755);
	ctrl('o');
	EQ(A.screen, SC_OPEN);
	EQ(A.op[0].isnew, 1);		/* New document first */
	STREQ(A.op[1].name, "notes");
	STREQ(A.op[A.op_focus].name, "a.md");	/* on the current document */
	type("b.m");
	EQ(A.op_n, 1);			/* typing filters */
	STREQ(A.op[0].name, "b.md");
	press(K_ENTER);
	EQ(A.screen, SC_EDIT);
	STREQ(A.name, "b.md");
	STREQ(src(), "second\n");
	ctrl('o');
	press(K_ESC);			/* in the home folder Esc closes */
	EQ(A.screen, SC_EDIT);
}

TEST(open_asks_about_changes_first)
{
	start("a.md", "first\n");
	put("b.md", "second\n");
	type("x");
	ctrl('o');
	type("b.md");
	press(K_ENTER);
	EQ(A.ov, OV_DISCARD);
	press(K_DOWN);
	press(K_DOWN);
	press(K_ENTER);			/* discard */
	STREQ(A.name, "b.md");
	STREQ(slurp("a.md"), "first\n");
}

TEST(open_goes_into_folders_and_up)
{
	start(0, 0);
	mkdir(strcat(strcpy(tmp, home), "/notes"), 0755);
	put("notes/inner.md", "in\n");
	ctrl('o');
	A.op_focus = 1;
	STREQ(A.op[1].name, "notes");
	press(K_ENTER);
	OK(strstr(A.op_dir, "/notes") != 0);
	STREQ(A.op[A.op_focus].name, "inner.md");
	press(K_ESC);			/* up, onto the folder it came from */
	STREQ(A.op_dir, home);
	STREQ(A.op[A.op_focus].name, "notes");
}

TEST(open_refuses_what_it_cannot_edit)
{
	char big[300000];

	start("a.md", "keep\n");
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	put("big.txt", big);
	ctrl('o');
	type("big");
	press(K_ENTER);
	EQ(A.ov, OV_ERROR);
	OK(strstr(A.err_text, "256 KB") != 0);
	press(K_ENTER);
	EQ(A.ov, OV_NONE);
	STREQ(A.name, "a.md");
}

TEST(find_and_replace)
{
	char c[32];

	start("n.md", "# Notes\nBring the badge and the Badge.\n- badge\n");
	ctrl('f');
	EQ(A.find_open, 1);
	EQ(A.find_focus, 0);
	OK(has_hint("Enter", "next"));
	type("badge");
	find_count(&A.f, c, sizeof(c));
	STREQ(c, "1 of 3");
	EQ(A.e.sel, 1);
	press(K_ENTER);
	find_count(&A.f, c, sizeof(c));
	STREQ(c, "2 of 3");
	press(K_DOWN);			/* to Replace */
	EQ(A.find_focus, 1);
	OK(has_hint("#2", "replace all"));
	type("device");
	press(K_F2);			/* replace all */
	EQ(A.replaced, 3);
	STREQ(src(), "# Notes\nBring the device and the Device.\n- device\n");
	ctrl('z');			/* one step */
	STREQ(src(), "# Notes\nBring the badge and the Badge.\n- badge\n");
	press(K_ESC);
	EQ(A.find_open, 0);
}

TEST(find_starts_with_the_selection)
{
	start("n.md", "one two\n");
	key(K_RIGHT, 0, 1, 0, 1, 0);	/* Fn+Ctrl+Right: a word */
	ctrl('f');
	STREQ(A.f.q, "one");
}

TEST(find_menu_toggles)
{
	start("n.md", "Ab ab\n");
	ctrl('f');
	type("ab");
	EQ(A.f.n, 2);
	press(K_F1);
	EQ(A.ov, OV_FIND_MENU);
	press(K_ENTER);			/* Match case */
	EQ(A.f.match_case, 1);
	EQ(A.f.n, 1);
	key(code_of('w'), 0, 0, 1, 0, 0);	/* Alt+W: whole word */
	EQ(A.f.whole_word, 1);
}

TEST(style_popup)
{
	start("n.md", "packing list\n");
	press(K_F2);
	EQ(A.ov, OV_STYLE);
	EQ(A.focus, 3);			/* Body: after the three switches */
	press(K_DOWN);
	press(K_DOWN);
	press(K_ENTER);			/* Heading 2 */
	STREQ(src(), "## packing list\n");
	EQ(A.ov, OV_NONE);
	press(K_F2);
	EQ(A.focus, 5);			/* on Heading 2 now */
	press(K_UP);
	press(K_UP);
	press(K_UP);
	EQ(A.focus, 0);			/* up from Body goes to the switches */
	press(K_RIGHT);
	EQ(A.focus, 1);
	press(K_LEFT);
	press(K_LEFT);
	EQ(A.focus, 0);
	press(K_ENTER);			/* Bold on the word at the caret */
	STREQ(src(), "## **packing** list\n");
}

TEST(styles_by_keys)
{
	start("n.md", "");
	ctrl('b');
	type("bold");
	ctrl('b');
	type(" plain");
	STREQ(src(), "**Bold** plain");
	key(code_of('2'), 0, 1, 0, 0, 0);	/* Ctrl+2 */
	STREQ(src(), "## **Bold** plain");
	key(K_8, 1, 1, 0, 0, 0);	/* Ctrl+Shift+8 */
	STREQ(src(), "- **Bold** plain");
}

TEST(styles_in_the_markdown_view)
{
	start("n.md", "word\n");
	press(K_F3);
	key(K_RIGHT, 1, 1, 0, 0, 0);	/* Shift+Ctrl+Right */
	ctrl('b');
	STREQ(src(), "**word**\n");
	EQ(A.e.mode, M_SOURCE);
}

TEST(clipboard)
{
	struct docbar d;

	start("n.md", "Bring **fully charged** now\n");
	for (int i = 0; i < 6; i++)
		press(K_RIGHT);
	key(K_RIGHT, 0, 1, 0, 1, 0);
	key(K_RIGHT, 0, 1, 0, 1, 0);	/* Fn+Ctrl+Right twice: "fully charged" */
	app_docbar(&A, &d);
	STREQ(d.info, "2 selected");
	EQ(d.info_kind, 1);
	OK(has_hint("Ctrl+C", "copy"));
	OK(has_hint("Esc", "cancel"));
	ctrl('c');
	STREQ(slurp(getenv("WRITE_CLIP")), "**fully charged**");
	app_docbar(&A, &d);
	STREQ(d.info, "Copied 2 words");	/* over "2 selected", until the selection changes */
	press(K_ESC);			/* Esc ends the selection first */
	EQ(A.e.sel, 0);
	EQ(A.quit, 0);
	app_docbar(&A, &d);
	STREQ(d.info, "Copied 2 words");
	EQ(d.info_kind, 2);
	press(K_DOWN);			/* the end */
	ctrl('v');
	STREQ(src(), "Bring **fully charged** now**fully charged**\n");
}

TEST(fn_shows_while_held)
{
	start("n.md", "ab\n");
	key(K_RIGHT, 0, 0, 0, 1, 0);
	app_fn(&A, 1);
	OK(has_hint("Fn", ""));
	app_fn(&A, 0);
	OK(!has_hint("Fn", ""));
}

TEST(full_screen_hint)
{
	start("n.md", "x\n");
	A.now = 1000;
	ctrl_shift('f');
	EQ(A.full, 1);
	OK(A.hint_until > 1000);
	EQ(app_tick(&A, 2000), 0);
	EQ(app_tick(&A, 4100), 1);	/* gone after 3 s */
	EQ(A.hint_until, 0);
	ctrl_shift('f');
	EQ(A.full, 0);
}

TEST(run_a_script)
{
	start("x.sh", "echo hi\n");
	press(K_F2);
	EQ(A.run, 1);
	start(0, 0);
	press(K_F1);
	for (int i = 0; i < A.n_rows; i++)
		if (A.rows[i].act == A_LANGUAGE)
			A.focus = i;
	press(K_ENTER);
	A.focus = L_SHELL;
	press(K_ENTER);
	type("echo hi");
	press(K_F2);			/* a script without a name is saved first */
	EQ(A.ov, OV_SAVEAS);
	STREQ(A.sa_ext, ".sh");
	press(K_ENTER);
	EQ(A.run, 1);
}

TEST(settings)
{
	start("n.md", "x\n");
	press(K_F5);
	EQ(A.ov, OV_SETTINGS);
	EQ(A.rows[0].act, A_TEXT_SIZE);
	STREQ(A.rows[0].value, "Normal");
	press(K_ENTER);
	EQ(A.small, 1);
	STREQ(A.rows[0].value, "Small");
	press(K_DOWN);
	press(K_ENTER);
	EQ(A.lineno, 0);
	press(K_ESC);
	{
		char p[512];

		snprintf(p, sizeof(p), "%s/settings", getenv("APP_CONFIG"));
		STREQ(slurp(p), "small=1\nlineno=0\ncaps=1\n");
	}
	app_free(&A);
	app_init(&A, 0);		/* read back at the next start */
	EQ(A.small, 1);
	EQ(A.lineno, 0);
}

TEST(help_and_log)
{
	start("n.md", "x\n");
	press(K_F6);
	EQ(A.ov, OV_HELP);
	press(K_F6);
	EQ(A.ov, OV_NONE);
	press(K_F4);
	EQ(A.ov, OV_LOG);
	press(K_ESC);
	EQ(A.ov, OV_NONE);
}

TEST(the_logo_key_saves)
{
	start("n.md", "x\n");
	type("y");
	app_term(&A);
	EQ(A.quit, 1);
	STREQ(slurp("n.md"), "Yx\n");
	start(0, 0);
	type("lost?");
	app_term(&A);
	STREQ(slurp("untitled.md"), "Lost?");
	start(0, 0);
	app_term(&A);
	EQ(access(strcat(strcpy(tmp, home), "/untitled.md"), F_OK), -1);	/* nothing typed: no file */
}

TEST(new_document)
{
	start("n.md", "x\n");
	ctrl('n');
	STREQ(A.name, "Untitled");
	STREQ(src(), "");
	type("a");
	ctrl('n');
	EQ(A.ov, OV_DISCARD);
}

TEST(undo_redo_keys)
{
	start(0, 0);
	type("one two");			/* a capital: a new sentence */
	ctrl('z');
	STREQ(src(), "One ");
	ctrl_shift('z');
	STREQ(src(), "One two");
}

TEST(moving_keys)
{
	start("n.md", "one two\nthree\n");
	key(K_RIGHT, 0, 0, 1, 0, 0);	/* Alt+Right: the end of the line */
	EQ(A.e.caret.col, 7);
	key(K_DOWN, 0, 0, 1, 0, 0);	/* Alt+Down: the end of the document */
	EQ(A.e.caret.line, 1);
	key(K_UP, 0, 0, 1, 0, 0);
	EQ(A.e.caret.line, 0);
	EQ(A.e.caret.col, 0);
	key(K_RIGHT, 0, 1, 0, 0, 0);	/* Ctrl+Right: a word */
	EQ(A.e.caret.col, 3);
	press(K_DOWN);
	EQ(A.e.caret.line, 1);
	key(K_BACKSPACE, 0, 0, 0, 1, 0);	/* Fn+Backspace deletes forward */
	EQ(A.e.t.l[1].len, 4);
}

TEST(geometry)
{
	int x, y, w, h;

	start("n.md", "x\n");
	app_geometry(&A, &x, &y, &w, &h);
	EQ(x, 48);
	EQ(y, 106);
	EQ(w, 608);
	EQ(h, 664 - 106);
	A.full = 1;
	app_geometry(&A, &x, &y, &w, &h);
	EQ(x, 56);
	EQ(y, 36);
	EQ(w, 591);
}

TEST(capitals_setting)
{
	start(0, 0);
	EQ(A.caps, 1);				/* on unless switched off */
	type("hi. ok");
	STREQ(src(), "Hi. Ok");
	press(K_F5);
	EQ(A.rows[2].act, A_CAPITALS);
	EQ(A.rows[2].toggle, 1);
	EQ(A.rows[2].on, 1);
	A.focus = 2;
	press(K_ENTER);
	EQ(A.caps, 0);
	EQ(A.rows[2].on, 0);
	press(K_ESC);
	type(" no");
	STREQ(src(), "Hi. Ok no");
	app_free(&A);
	app_init(&A, 0);
	EQ(A.caps, 0);
	EQ(A.e.autocap, 0);
}

TEST(html_closes_the_open_element)
{
	start("p.html", "<p>a");
	key(K_DOWN, 0, 0, 1, 0, 0);		/* the end of the document */
	type("</");
	STREQ(src(), "<p>a</p>");
	start("p.php", "<p>a");
	key(K_DOWN, 0, 0, 1, 0, 0);
	type("</");
	STREQ(src(), "<p>a</p>");
	start("p.c", "<p>a");			/* other languages: as typed */
	key(K_DOWN, 0, 0, 1, 0, 0);
	type("</");
	STREQ(src(), "<p>a</");
	start("p.html", "<p>a");		/* a new language from Options */
	A.lang = L_PLAIN;
	for (int i = 0; i < 2; i++) {
		press(K_F1);
		for (int r = 0; r < A.n_rows; r++)
			if (A.rows[r].act == A_LANGUAGE)
				A.focus = r;
		press(K_ENTER);
		A.focus = i ? L_HTML : L_C;
		press(K_ENTER);
		EQ(A.e.autoclose, i);
	}
}
