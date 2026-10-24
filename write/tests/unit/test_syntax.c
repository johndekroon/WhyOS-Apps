/* SPDX-License-Identifier: MIT */
/* Languages and highlighting (src/syntax.c; design Code.dc.html, Language.dc.html). */
#include "t.h"
#include "syntax.h"

static unsigned char C[512];
static int state;

/* the classes of a line as letters: k s n c f p . (text) */
static const char *hl(enum lang l, const char *s)
{
	static char out[512];
	int n = (int)strlen(s);

	state = syn_line(l, s, n, state, C);
	for (int i = 0; i < n; i++)
		out[i] = ".ksncfp"[C[i]];
	out[n] = 0;
	return out;
}

TEST(languages_by_extension)
{
	EQ(lang_of_path("/root/blink.py"), L_PYTHON);
	EQ(lang_of_path("index.HTML"), L_HTML);
	EQ(lang_of_path("a.htm"), L_HTML);
	EQ(lang_of_path("notes.md"), L_MARKDOWN);
	EQ(lang_of_path("x.markdown"), L_MARKDOWN);
	EQ(lang_of_path("todo.txt"), L_PLAIN);
	EQ(lang_of_path("boot.log"), L_PLAIN);
	EQ(lang_of_path("backup.sh"), L_SHELL);
	EQ(lang_of_path("site.php"), L_PHP);
	EQ(lang_of_path("s.css"), L_CSS);
	EQ(lang_of_path("app.js"), L_JS);
	EQ(lang_of_path("data.json"), L_JSON);
	EQ(lang_of_path("c.yml"), L_YAML);
	EQ(lang_of_path("c.yaml"), L_YAML);
	EQ(lang_of_path("netd.conf"), L_CONF);
	EQ(lang_of_path("x.ini"), L_CONF);
	EQ(lang_of_path("main.c"), L_C);
	EQ(lang_of_path("main.h"), L_C);
	EQ(lang_of_path("README"), L_PLAIN);
	EQ(lang_of_path("photo.jpg"), L_NONE);
	STREQ(lang_name(L_PYTHON), "Python");
	STREQ(lang_exts(L_CONF), ".conf .ini");
	EQ(L_COUNT, 12);
}

TEST(python)
{
	state = 0;
	STREQ(hl(L_PYTHON, "def blink(times=3):"),
		 "kkk.fffffp.....pnpp");	/* the parameter list: as the design, names are text */
	STREQ(hl(L_PYTHON, "    print(\"blink\", x[i % 2])"),
		 "....fffffpsssssssp..p..p.npp");
	STREQ(hl(L_PYTHON, "# a comment"), "ccccccccccc");
	STREQ(hl(L_PYTHON, "return True  # done"), "kkkkkk.nnnn..cccccc");
	STREQ(hl(L_PYTHON, "s = 'it\\'s'"), "..p.sssssss");
	STREQ(hl(L_PYTHON, "x = \"\"\"start"), "..p.ssssssss");
	EQ(state != 0, 1);		/* a string goes on */
	STREQ(hl(L_PYTHON, "still\"\"\" + 1"), "ssssssss.p.n");
	EQ(state, 0);
}

TEST(python_names_after_def_and_calls)
{
	state = 0;
	/* "times" is text in the design: only the name after def and a call are functions */
	STREQ(hl(L_PYTHON, "def f(a):"), "kkk.fp.pp");
	STREQ(hl(L_PYTHON, "time.sleep(0.5)"), "....pfffffpnnnp");
}

TEST(html)
{
	state = 0;
	STREQ(hl(L_HTML, "<!doctype html>"), "ppkkkkkkk.....p");
	STREQ(hl(L_HTML, "<html lang=\"en\">"), "pkkkk.ffffpssssp");
	STREQ(hl(L_HTML, "  <p class=\"note\">Bring</p>"), "..pk.fffffpssssssp.....ppkp");
	STREQ(hl(L_HTML, "<!-- Shown -->"), "cccccccccccccc");
	STREQ(hl(L_HTML, "<!-- open"), "ccccccccc");
	STREQ(hl(L_HTML, "still -->x"), "ccccccccc.");
}

TEST(shell)
{
	state = 0;
	STREQ(hl(L_SHELL, "#!/bin/sh"), "ccccccccc");
	STREQ(hl(L_SHELL, "if [ -f \"$x\" ]; then"), "kk.p....ssss.pp.kkkk");
	STREQ(hl(L_SHELL, "echo 'hi' # c"), ".....ssss.ccc");
}

TEST(c_and_js_and_css)
{
	state = 0;
	STREQ(hl(L_C, "#include <stdio.h>"), "kkkkkkkk.sssssssss");
	STREQ(hl(L_C, "int x = 42; /* a"), "kkk...p.nnp.cccc");
	STREQ(hl(L_C, "b */ return 0;"), "cccc.kkkkkk.np");
	state = 0;
	STREQ(hl(L_JS, "const f = () => `t`; // c"), "kkkkk...p.pp.pp.sssp.cccc");
	state = 0;
	STREQ(hl(L_CSS, "h1 { color: #fff; }"), "...p.fffffp.nnnnp.p");
}

TEST(json_yaml_conf)
{
	state = 0;
	STREQ(hl(L_JSON, "{\"a\": [1, true, null]}"), "pfffp.pnp.kkkkp.kkkkpp");
	STREQ(hl(L_YAML, "name: \"x\" # c"), "ffffp.sss.ccc");
	STREQ(hl(L_YAML, "- item"), "p.....");
	STREQ(hl(L_CONF, "[wifi]"), "kkkkkk");
	STREQ(hl(L_CONF, "ssid=\"Camp\""), "ffffpssssss");
	STREQ(hl(L_CONF, "; comment"), "ccccccccc");
}

TEST(plain_is_plain)
{
	state = 0;
	STREQ(hl(L_PLAIN, "def x = \"y\" # z"), "...............");
}

TEST(indent_of_a_language)
{
	EQ(lang_indent(L_PYTHON), 4);
	EQ(lang_indent(L_HTML), 2);
	EQ(lang_indent(L_C), 4);
	EQ(lang_indent(L_YAML), 2);
}

TEST(runner)
{
	EQ(lang_runs(L_PYTHON), 1);
	EQ(lang_runs(L_SHELL), 1);
	EQ(lang_runs(L_HTML), 2);	/* preview */
	EQ(lang_runs(L_CSS), 0);
}
