/* SPDX-License-Identifier: MIT */
/*
 * whyos/test.h: the test channel. When the app's /tmp/app-<id>/test exists
 * (the test framework makes it before the start; WOS_TEST=dir names
 * another), the SDK takes keys from the FIFO "keys" in it and writes what
 * the screen shows to "screen" after every frame, as JSON. Without it
 * nothing is opened. An app has no test code of its own: what a test
 * needs to see comes from here. Part of the WhyOS SDK: include whyos.h.
 *
 * keys: one per line, a key code (pressed and let go), "+code" held down,
 * "-code" let go (for Shift and Fn).
 */
#pragma once

static void wos_test_open(void)
{
	char path[160];
	struct stat st;
	const char *dir = getenv("WOS_TEST");

	if (dir && dir[0])
		wos_copy(wos.test_dir, dir, sizeof(wos.test_dir));
	else if (!wos_path(WOS_TMP, "test", wos.test_dir, sizeof(wos.test_dir)) || stat(wos.test_dir, &st) ||
		 !S_ISDIR(st.st_mode))
		wos.test_dir[0] = 0;
	if (!wos.test_dir[0])
		return;
	wos_copy(path, wos.test_dir, sizeof(path));
	wos_add(path, "/keys", sizeof(path));
	unlink(path);
	if (!mkfifo(path, 0600))
		wos.fifo = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* keys from the FIFO; 1 when one changed something */
static int wos_test_keys(void)
{
	static char part[16];
	static int np;
	char b[64];
	int n = (int)read(wos.fifo, b, sizeof(b)), draw = 0;

	for (int i = 0; i < n; i++) {
		if (b[i] != '\n') {
			if (np < (int)sizeof(part) - 1)
				part[np++] = b[i];
			continue;
		}
		part[np] = 0;
		np = 0;
		if (part[0] == '+' || part[0] == '-') {
			int code = (int)wos_int(part + 1, -1);

			if (code >= 0)
				draw |= wos_event(code, part[0] == '+');
		} else if (wos_int(part, -1) >= 0) {
			draw |= wos_event((int)wos_int(part, 0), 1);
			draw |= wos_event((int)wos_int(part, 0), 0);
		}
	}
	return draw;
}

/* ---- the screen as JSON ------------------------------------------------- */

#define WOS_JSON	12288

static char *wos_json;
static int wos_jn;

static void wos_j(const char *s)
{
	int n = (int)strlen(s);

	if (wos_jn + n < WOS_JSON - 1) {
		memcpy(wos_json + wos_jn, s, (size_t)n);
		wos_jn += n;
	}
}

/* a JSON string, with what JSON needs escaped */
static void wos_js(const char *s)
{
	char e[8];

	if (!s) {
		wos_j("null");
		return;
	}
	wos_j("\"");
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			e[0] = '\\';
			e[1] = (char)c;
			e[2] = 0;
		} else if (c < 0x20) {
			static const char hex[] = "0123456789abcdef";

			memcpy(e, "\\u00", 4);
			e[4] = hex[c >> 4];
			e[5] = hex[c & 15];
			e[6] = 0;
		} else {
			e[0] = (char)c;
			e[1] = 0;
		}
		wos_j(e);
	}
	wos_j("\"");
}

static void wos_jkv(const char *k, const char *v)
{
	wos_js(k);
	wos_j(":");
	wos_js(v);
	wos_j(",");
}

static void wos_jki(const char *k, long v)
{
	char b[24] = "";

	wos_js(k);
	wos_j(":");
	wos_j(wos_add_int(b, v, sizeof(b)));
	wos_j(",");
}

/* the last "," of an object or list goes */
static void wos_jend(const char *close)
{
	if (wos_jn && wos_json[wos_jn - 1] == ',')
		wos_jn--;
	wos_j(close);
	wos_j(",");
}

static const char *const wos_kind_names[] = { "", "colour", "entry", "list", "canvas", "log" };

static void wos_test_screen(void)
{
	static char *json;	/* only with the channel open: no 12 KB in every app */
	wos_screen *s;
	struct hint hs[10];
	char path[160], b[48];
	int n;

	if (!wos.test_dir[0] || (!json && !(json = malloc(WOS_JSON))))
		return;
	s = wos_top();
	wos_json = json;
	wos_jn = 0;
	wos_j("{");
	wos_jki("t", wos_ms());
	wos_jkv("app", wos.app->name);
	wos_jki("depth", wos.depth);
	wos_jkv("kind", wos_kind_names[s->kind]);
	wos_jkv("eyebrow", s->kind == WOS_COLOUR ? 0 : wos_eyebrow(s, b, sizeof(b)));
	wos_jkv("title", wos_title(s));
	if (s->title_right)
		wos_jkv("title_right", s->title_right);
	wos_jki("backlight", wos_duty);
	if (s->kind == WOS_COLOUR) {
		char c[8] = "#";
		static const char hex[] = "0123456789abcdef";

		for (int i = 0; i < 6; i++)
			c[1 + i] = hex[(s->colour >> (20 - 4 * i)) & 15];
		c[7] = 0;
		wos_jkv("colour", c);
		wos_jki("brightness", s->brightness);
	}
	if (s->kind == WOS_ENTRY) {
		wos_jkv("line", wos.line);
		wos_jki("cursor", wos.cur);
		wos_jkv("result", wos.result);
		wos_jkv("error", wos.err);
		wos_jkv("detail", s->detail);
		wos_js("history");
		wos_j(":[");
		for (int i = 0; i < wos.n_hist; i++) {
			wos_j("[");
			wos_js(wos.hist[i].line);
			wos_j(",");
			wos_js(wos.hist[i].res);
			wos_j("],");
		}
		wos_jend("]");
	}
	if (s->kind == WOS_LIST) {
		wos_jki("focus", s->focus);
		wos_js("rows");
		wos_j(":[");
		for (int i = 0; i < s->n && i < 64; i++) {
			wos_row r = { 0 };

			s->row(i, &r);
			wos_j("{");
			wos_jkv("label", r.label);
			if (r.value)
				wos_jkv("value", r.value);
			if (r.sub)
				wos_jkv("sub", r.sub);
			if (r.kind == WOS_ROW_SWITCH)
				wos_jki("on", r.on);
			wos_jend("}");
		}
		wos_jend("]");
	}
	wos_js("log");
	wos_j(":[");
	for (int i = 0; wos_log_line(i); i++) {
		wos_js(wos_log_line(i));
		wos_j(",");
	}
	wos_jend("]");
	wos_js("keybar");
	wos_j(":");
	if (wos_keys_showing(s)) {
		n = wos_bar_items(s, hs, 10);
		wos_j("[");
		for (int i = 0; i < n; i++) {
			wos_j("[");
			wos_js(hs[i].keys);
			wos_j(",");
			wos_js(hs[i].label);
			wos_j("],");
		}
		wos_j("[\"Esc\",");
		wos_js(wos_esc_word(s));
		wos_j("]],");
	} else {
		wos_j("null,");
	}
	wos_js("popup");
	wos_j(":");
	if (wos.pop == WOS_POP_HELP) {
		struct help_key k[14];

		n = wos_help_keys(s, k, 14);
		wos_j("{");
		wos_jkv("kind", "help");
		wos_jkv("title", wos_title(s));
		wos_jkv("about", s->about ? s->about : s == wos.stack[0] ? wos.app->about : 0);
		wos_js("keys");
		wos_j(":[");
		for (int i = 0; i < n; i++) {
			wos_j("[");
			wos_js(k[i].keys);
			wos_j(",");
			wos_js(k[i].what);
			wos_j("],");
		}
		wos_jend("]");
		wos_jend("}");
	} else if (wos.pop) {
		wos_j("{");
		wos_jkv("kind", wos.pop == WOS_POP_CHOICE ? "choice" : "menu");
		wos_jki("focus", wos.pop_focus);
		wos_jki("current", wos.pop == WOS_POP_CHOICE ? wos.pop_cur : -1);
		wos_js("items");
		wos_j(":[");
		for (int i = 0; i < wos.pop_n; i++) {
			wos_js(wos.pop_items[i].label);
			wos_j(",");
		}
		wos_jend("]");
		wos_jend("}");
	} else {
		wos_j("null,");
	}
	wos_js("settings");
	wos_j(":{");
	for (int i = 0; i < wos_n_kv; i++)
		wos_jkv(wos_kv[i].key, wos_kv[i].val);
	wos_jend("}");
	wos_jend("}");
	wos_jn--;		/* the "," after the whole */
	wos_j("\n");
	wos_copy(path, wos.test_dir, sizeof(path));
	wos_add(path, "/screen", sizeof(path));
	wos_write_file(path, json, wos_jn);
}
