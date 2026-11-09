/* SPDX-License-Identifier: MIT */
/*
 * whyos/app.h: the app framework. An app describes its screens, the keys
 * that do something on them (actions) and its settings; wos_run() runs the
 * loop, draws, and keeps the design system's rules: the key bar, the help
 * (diamond), the log (circle) and the settings (clover) screens, Esc one
 * layer back at a time, the screen kept on, the end on the logo key. Part
 * of the WhyOS SDK: include whyos.h; sdk/README.md has a first app.
 */
#pragma once

/* ---- what an app describes ----------------------------------------------- */

/*
 * A key that does something on a screen: its word in the key bar (0: only
 * in the help), its line in the help, and what it does. fn gets arg, or
 * for WOS_KEY_ARROWS and the like the step (-1 or +1). An action without
 * fn only describes a key the screen handles itself (Enter in a list or an
 * entry: its word and its help line).
 */
typedef struct wos_action {
	int key;
	const char *word;
	const char *help;
	void (*fn)(int arg);
	int arg;
} wos_action;

struct wos_setting;

/* A list's row, as row() fills it in (the design's list row, 6.5). */
enum { WOS_ROW_OPEN, WOS_ROW_PLAIN, WOS_ROW_SWITCH, WOS_ROW_INFO };

typedef struct wos_row {
	const char *label, *sub, *value;
	const struct wb_icon *icon;
	int kind;		/* WOS_ROW_*: a chevron, nothing, a switch, no focus */
	int on;			/* the switch */
} wos_row;

/* the kinds of ready-made screen */
enum { WOS_COLOUR = 1, WOS_ENTRY, WOS_LIST, WOS_CANVAS, WOS_LOGVIEW, WOS_EDITOR };

typedef struct wos_screen {
	int kind;
	const char *eyebrow;	/* where it lies; "APP" on the first screen when 0 */
	const char *title;	/* the app's name when 0 */
	const char *about;	/* the help's sentence; the app's when 0 */
	const char *title_right;	/* right of the title, in text mid */
	const wos_action *actions;	/* ended by { 0 } */
	const char *esc;	/* Esc's word when it is not the SDK's (close, back, clear) */
	void (*update)(struct wos_screen *s);	/* before each frame: the data into the fields */

	/* WOS_COLOUR: the whole screen one colour, at a brightness */
	uint32_t colour;
	int brightness;		/* the backlight's duty, 10..80; 0: the system's */
	int keys_ms;		/* the key bar shows this long first, at the system's brightness */

	/* WOS_ENTRY: a line typed, worked out on Enter, the earlier ones above */
	const char *(*evaluate)(const char *line, char *result, int size);	/* 0 or what is wrong */
	const char *(*preview)(const char *line, char *result, int size);	/* while typing */
	const char *(*typed)(const char *line, int cursor, int ch);	/* what a character inserts */
	const char *placeholder;
	const char *detail;	/* a small line in the field's corner */
	int right;		/* the line right-aligned, as on a calculator */

	/* WOS_LIST: rows */
	int n;
	void (*row)(int i, wos_row *r);
	void (*choose)(int i);	/* Enter on a row */
	int (*adjust)(int i, int step);	/* left and right on a row: 1 if it did */
	const wos_action *menu;	/* the square key's pop-up: fn gets the row */
	const struct wos_setting *fields;	/* a form: these rows, kept as settings are */

	/* WOS_EDITOR: text over several lines, edited in place in the app's
	 * buffer (max bytes with its 0); a change marks the screen dirty */
	char *text;
	int max;

	/* WOS_CANVAS: the app draws (libwb) in the content area */
	void (*paint)(int x, int y, int w, int h);
	int (*key)(int code, int ch);	/* 1: it used the key */
	void (*release)(int code);	/* a key let go (a held key: a mouse button, a fast move) */
	int scene;		/* the frontpanel's figures behind it */

	int dirty;		/* unsaved work: Esc asks "Discard changes?" first (6.7) */
	const char *busy;	/* an action running ("Typing"): a progress pop-up, Esc aborts it */
	int progress;		/* of busy, 0..1000; -1: no bar */
	void (*abort)(void);	/* Esc while busy (6.7 step 1); busy goes when the app clears it */

	/* the SDK's own */
	int focus, scroll;
	int from;		/* WOS_LOGVIEW: lines back from the newest */
} wos_screen;

/* A setting the clover's screen shows and APP_CONFIG/settings keeps. */
enum { WOS_SWITCH = 1, WOS_CHOICE, WOS_SLIDER, WOS_TEXT };

typedef struct wos_setting {
	const char *key, *label, *sub;
	int kind;
	const char *def;	/* the value when none was set: "1"/"0", a choice's value, a number */
	const char *choices;	/* WOS_CHOICE: "value=Label|value=Label", or "Label|Label" */
	int min, max, step;	/* WOS_SLIDER */
	void (*changed)(void);
} wos_setting;

typedef struct wos_app {
	const char *name;	/* the title, the help's */
	const char *about;	/* one or two sentences for the help */
	const wos_setting *settings;	/* ended by { 0 } */
	int keep_on;		/* the screen stays on while the app runs */
	void (*tick)(void);	/* every tick_ms, from the start */
	int tick_ms;
	void (*quit)(void);	/* before the end: save what needs saving */
} wos_app;

static int wos_run(const wos_app *app, wos_screen *first);
static void wos_push(wos_screen *s);
static void wos_pop(void);
static void wos_quit(void);
static void wos_ask(const char *title, const char *body, const char *choices, int safe, int danger,
		    void (*done)(int choice));
static void wos_ask_text(const char *title, const char *explain, const char *text,
			 const char *(*done)(const char *text));
static void wos_message(const char *text);

/* ---- the SDK's state ------------------------------------------------------ */

#define WOS_DEPTH	8
#define WOS_LINE	120
#define WOS_HIST	50
#define WOS_RES		64
#define WOS_BAND_H	40
#define WOS_POP_ITEMS	24

enum { WOS_POP_NONE, WOS_POP_HELP, WOS_POP_CHOICE, WOS_POP_MENU, WOS_POP_ASK, WOS_POP_TEXT };

static struct {
	const wos_app *app;
	wos_screen *stack[WOS_DEPTH];
	int depth;
	int fb, ev, fifo;
	int mods;
	int quit;
	long t0;		/* when the screen on top came */
	int keys_gone;		/* WOS_COLOUR: its keys_ms passed */
	/* the pop-up over the screen */
	int pop, pop_focus, pop_cur, pop_n, pop_setting;
	struct menu_item pop_items[WOS_POP_ITEMS];
	/* WOS_ENTRY: the line and the earlier ones */
	char line[WOS_LINE + 1], draft[WOS_LINE + 1];
	int len, cur, back;
	const char *err;
	char result[WOS_RES];
	struct { char line[WOS_LINE + 1], res[WOS_RES]; } hist[WOS_HIST];
	int n_hist;
	/* a question (WOS_POP_ASK) or a text being typed (WOS_POP_TEXT) */
	char ask_title[64], ask_body[160];
	int ask_danger;
	void (*ask_done)(int choice);
	char text[WOS_LINE + 1];
	int text_len, text_cur;
	const char *text_err;
	const char *(*text_done)(const char *text);
	/* WOS_EDITOR: the cursor (bytes in) and the first line shown */
	int ed_cur, ed_top;
	/* a short message over the key bar */
	char msg[64];
	long msg_until;
	/* the test channel (test.h) */
	char test_dir[128];
} wos;

static wos_screen wos_log_screen = { .kind = WOS_LOGVIEW, .title = "Log" };
static wos_screen wos_settings_screen = { .kind = WOS_LIST, .title = "Settings" };

static wos_screen *wos_top(void)
{
	return wos.stack[wos.depth - 1];
}

/* ---- settings from the schema --------------------------------------------- */

static int wos_n_settings(void)
{
	int n = 0;

	while (wos.app && wos.app->settings && wos.app->settings[n].key)
		n++;
	return n;
}

/* the rows of the form on top: the app's settings on the clover's screen */
static const wos_setting *wos_fields(void)
{
	return wos.stack[wos.depth - 1]->fields;
}

static const char *wos_schema_default(const char *key)
{
	/* the forms on the stack, then the app's settings */
	for (int d = wos.depth - 1; d >= 0; d--)
		for (const wos_setting *f = wos.stack[d]->fields; f && f->key; f++)
			if (!strcmp(f->key, key))
				return f->def;
	for (int i = 0; i < wos_n_settings(); i++)
		if (!strcmp(wos.app->settings[i].key, key))
			return wos.app->settings[i].def;
	return 0;
}

/*
 * A choice's i-th item of "value=Label|value=Label" (or "Label|Label"):
 * its value into v and its label into l; 0 past the last.
 */
static int wos_choice(const char *choices, int i, char *v, int vn, char *l, int ln)
{
	const char *p = choices;

	while (p && i-- > 0)
		p = (p = strchr(p, '|')) ? p + 1 : 0;
	if (!p || !*p)
		return 0;
	{
		int len = (int)(strchr(p, '|') ? strchr(p, '|') - p : (int)strlen(p));
		const char *eq = memchr(p, '=', (size_t)len);
		char item[WOS_VAL_LEN * 2];

		wos_copy(item, p, len + 1 < (int)sizeof(item) ? len + 1 : (int)sizeof(item));
		if (eq) {
			item[eq - p] = 0;
			wos_copy(v, item, vn);
			wos_copy(l, item + (eq - p) + 1, ln);
		} else {
			wos_copy(v, item, vn);
			wos_copy(l, item, ln);
		}
	}
	return 1;
}

/* the label of a choice setting's present value */
static const char *wos_choice_label(const wos_setting *s, char *l, int ln)
{
	char v[WOS_VAL_LEN];
	const char *now = wos_get(s->key, 0);

	for (int i = 0; wos_choice(s->choices, i, v, sizeof(v), l, ln); i++)
		if (!strcmp(v, now))
			return l;
	return wos_copy(l, now, ln);
}

static void wos_settings_row(int i, wos_row *r)
{
	static char value[WOS_VAL_LEN];
	const wos_setting *s = &wos_fields()[i];

	r->label = s->label;
	r->sub = s->sub;
	if (s->kind == WOS_SWITCH) {
		r->kind = WOS_ROW_SWITCH;
		r->on = wos_get_int(s->key, 0) != 0;
	} else if (s->kind == WOS_CHOICE) {
		r->value = wos_choice_label(s, value, sizeof(value));
	} else if (s->kind == WOS_TEXT) {
		r->value = wos_copy(value, wos_get(s->key, 0), sizeof(value));
	} else {
		r->kind = WOS_ROW_PLAIN;
		r->value = wos_copy(value, wos_get(s->key, 0), sizeof(value));
	}
}

static void wos_open_choice(int i);
static int wos_text_setting;

/* a text setting typed in the text pop-up: kept on Enter */
static const char *wos_text_set(const char *text)
{
	const wos_setting *s = &wos_fields()[wos_text_setting];

	if (strchr(text, '\n') || (int)strlen(text) >= WOS_VAL_LEN)
		return "Too long";
	wos_set(s->key, text);
	if (s->changed)
		s->changed();
	return 0;
}

static void wos_settings_open(int i)
{
	const wos_setting *s = &wos_fields()[i];

	if (s->kind == WOS_TEXT) {
		wos_text_setting = i;
		wos_ask_text(s->label, s->sub, wos_get(s->key, 0), wos_text_set);
	} else if (s->kind == WOS_SWITCH) {
		wos_set(s->key, wos_get_int(s->key, 0) ? "0" : "1");
		if (s->changed)
			s->changed();
	} else if (s->kind == WOS_CHOICE) {
		wos_open_choice(i);
	}
}

static int wos_settings_adjust(int i, int step)
{
	const wos_setting *s = &wos_fields()[i];
	long v;

	if (s->kind != WOS_SLIDER)
		return 0;
	v = wos_get_int(s->key, s->min) + step * (s->step ? s->step : 1);
	wos_set_int(s->key, v < s->min ? s->min : v > s->max ? s->max : v);
	if (s->changed)
		s->changed();
	return 1;
}

/* a choice's pop-up list, the present one ticked and focused */
static void wos_open_choice(int i)
{
	const wos_setting *s = &wos_fields()[i];
	const char *now = wos_get(s->key, 0);
	char v[WOS_VAL_LEN];
	static char labels[WOS_POP_ITEMS][48];

	wos.pop_n = 0;
	wos.pop_cur = 0;
	while (wos.pop_n < WOS_POP_ITEMS &&
	       wos_choice(s->choices, wos.pop_n, v, sizeof(v), labels[wos.pop_n], sizeof(labels[0]))) {
		memset(&wos.pop_items[wos.pop_n], 0, sizeof(wos.pop_items[0]));
		wos.pop_items[wos.pop_n].label = labels[wos.pop_n];
		if (!strcmp(v, now))
			wos.pop_cur = wos.pop_n;
		wos.pop_n++;
	}
	wos.pop_setting = i;
	wos.pop_focus = wos.pop_cur;
	wos.pop = WOS_POP_CHOICE;
}

static void wos_pick_choice(void)
{
	const wos_setting *s = &wos_fields()[wos.pop_setting];
	char v[WOS_VAL_LEN], l[48];

	if (wos_choice(s->choices, wos.pop_focus, v, sizeof(v), l, sizeof(l))) {
		wos_set(s->key, v);
		if (s->changed)
			s->changed();
	}
}

/* ---- what the screen on top shows ----------------------------------------- */

static const char *wos_title(const wos_screen *s)
{
	return s->title ? s->title : wos.app->name;
}

/* s in upper case into buf: an eyebrow */
static const char *wos_upper(const char *s, char *buf, int size)
{
	wos_copy(buf, s, size);
	for (char *p = buf; *p; p++)
		if (*p >= 'a' && *p <= 'z')
			*p = (char)(*p - 32);
	return buf;
}

static const char *wos_eyebrow(const wos_screen *s, char *buf, int size)
{
	if (s->eyebrow)
		return s->eyebrow;
	if (s == wos.stack[0])
		return "APP";
	/* the app's name, upper case, and a chevron: where the screen lies */
	wos_upper(wos.app->name, buf, size - 4);
	return wos_add(buf, " \xe2\x80\xba", size);
}

/* Does Esc clear the entry's line (6.7 step 3)? */
static int wos_esc_clears(const wos_screen *s)
{
	return s->kind == WOS_ENTRY && (wos.len || wos.err);
}

/* Esc's word on the screen on top (6.8) */
static const char *wos_esc_word(const wos_screen *s)
{
	if (s->busy)
		return "cancel";
	if (wos_esc_clears(s))
		return "clear";
	if (s->esc)
		return s->esc;
	return wos.depth > 1 ? "back" : "close";
}

static const char *wos_esc_help(const wos_screen *s, char *buf, int size)
{
	if (s->kind == WOS_ENTRY && wos.depth == 1)
		return "Clear the line; on an empty line, close";
	if (s->kind == WOS_ENTRY)
		return "Clear the line; on an empty line, back";
	if (wos.depth > 1)
		return "Back";
	wos_copy(buf, "Close ", size);
	return wos_add(buf, wos.app->name, size);
}

/* the action of Enter that only describes it, if there is one */
static const wos_action *wos_enter_action(const wos_screen *s)
{
	for (const wos_action *a = s->actions; a && a->key; a++)
		if (a->key == WOS_KEY_ENTER)
			return a;
	return 0;
}

/* The key bar's items, left of the circle, clover and diamond: Enter,
 * then the square, triangle and cross, then other keys with a word. */
static int wos_bar_items(const wos_screen *s, struct hint *hs, int max)
{
	const wos_action *en = wos_enter_action(s);
	int n = 0;

	if (en && en->word)
		hs[n++] = (struct hint){ "Enter", en->word, 0 };
	else if (s->kind == WOS_LIST && s->n && (s->choose || s->fields))
		hs[n++] = (struct hint){ "Enter", s->fields ? "change" : "open", 0 };
	else if (s->kind == WOS_ENTRY)
		hs[n++] = (struct hint){ "Enter", "enter", 0 };
	if (s->menu && s->n && n < max)
		hs[n++] = (struct hint){ "#1", "options", 0 };
	for (int k = WOS_KEY_OPTIONS; k <= WOS_KEY_SECOND; k++)
		for (const wos_action *a = s->actions; a && a->key && n < max; a++)
			if (a->key == k && a->word && !(k == WOS_KEY_OPTIONS && s->menu))
				hs[n++] = (struct hint){ wos_key_cap(a->key), a->word, 0 };
	for (const wos_action *a = s->actions; a && a->key && n < max; a++)
		if (a->word && a->key != WOS_KEY_ENTER && (a->key < WOS_KEY_OPTIONS || a->key > WOS_KEY_SECOND))
			hs[n++] = (struct hint){ wos_key_cap(a->key), a->word, 0 };
	return n;
}

/* The help's keys: Enter, the actions, the screen's own keys, Esc, the
 * circle, clover and diamond, the logo key. */
static int wos_help_keys(const wos_screen *s, struct help_key *k, int max)
{
	static char esc[64];
	const wos_action *en = wos_enter_action(s);
	int n = 0;

	if (en && en->help)
		k[n++] = (struct help_key){ "Enter", en->help };
	else if (s->kind == WOS_LIST && s->fields && s->n)
		k[n++] = (struct help_key){ "Enter", "Change the setting" };
	else if (s->kind == WOS_LIST && s->choose && s->n)
		k[n++] = (struct help_key){ "Enter", "Open the row" };
	if (s->menu && s->n)
		k[n++] = (struct help_key){ "#1", "Options for the row" };
	for (const wos_action *a = s->actions; a && a->key && n < max; a++)
		if (a->help && a->key != WOS_KEY_ENTER)
			k[n++] = (struct help_key){ wos_key_cap(a->key), a->help };
	if (s->kind == WOS_LIST && s->n > 1 && n < max)
		k[n++] = (struct help_key){ wos_key_cap(WOS_KEY_UPDOWN), "Move the focus" };
	if (s->kind == WOS_LOGVIEW && n < max)
		k[n++] = (struct help_key){ wos_key_cap(WOS_KEY_UPDOWN), "Scroll" };
	if (s->kind == WOS_EDITOR && n + 2 < max) {
		k[n++] = (struct help_key){ wos_key_cap(WOS_KEY_ARROWS), "Move the cursor (with Fn: to a line's end)" };
		k[n++] = (struct help_key){ "Enter", "A new line" };
	}
	if (s->kind == WOS_ENTRY && n + 3 < max) {
		k[n++] = (struct help_key){ wos_key_cap(WOS_KEY_UPDOWN), "Bring back earlier lines" };
		k[n++] = (struct help_key){ wos_key_cap(WOS_KEY_LEFTRIGHT), "Move the cursor (with Fn: to an end)" };
		k[n++] = (struct help_key){ "Backspace", "Erase before the cursor" };
	}
	if (n < max)
		k[n++] = (struct help_key){ "Esc", wos_esc_help(s, esc, sizeof(esc)) };
	if (n + 4 <= max) {
		k[n++] = (struct help_key){ "#4", "The log" };
		k[n++] = (struct help_key){ "#5", "Settings" };
		k[n++] = (struct help_key){ "#6", "This help" };
		k[n++] = (struct help_key){ "Logo", "Back to the menu" };
	}
	return n;
}

/* ---- drawing -------------------------------------------------------------- */

#define WOS_FIELD_Y	448
#define WOS_FIELD_H	(H - WB_HINT_BAR_H - 24 - WOS_FIELD_Y)
#define WOS_HIST_ROW	44

static int wos_keys_showing(const wos_screen *s)
{
	return s->kind != WOS_COLOUR || !wos.keys_gone;
}

static void wos_paint_list(wos_screen *s)
{
	int ys[64], hs[64], n = s->n < 64 ? s->n : 64, cut, scroll, top = WB_ROWS_TOP;
	int avail = H - WB_HINT_BAR_H - 16 - top;

	for (int i = 0; i < n; i++) {
		ys[i] = i * (WB_LIST_ROW_H + WB_SPACE_2);
		hs[i] = WB_LIST_ROW_H;
	}
	scroll = list_scroll(ys, hs, n, s->focus, -1, avail, &cut);
	clip_rows(top, top + cut);
	for (int i = 0; i < n; i++) {
		wos_row r = { 0 };
		int y = top + ys[i] - scroll;

		if (y + hs[i] < top || y >= top + cut)
			continue;
		s->row(i, &r);
		wb_row(WB_CONTENT_PAD, y, W - 2 * WB_CONTENT_PAD, WB_LIST_ROW_H, r.icon, r.label, r.sub, r.value,
		       r.kind == WOS_ROW_OPEN ? WB_ROW_OPEN : r.kind == WOS_ROW_SWITCH ? WB_ROW_SWITCH : WB_ROW_NONE,
		       r.on, i == s->focus && r.kind != WOS_ROW_INFO);
	}
	clip_end();
	if (n)
		scrollbar(W - WB_CONTENT_PAD + 9, top, cut, ys[n - 1] + hs[n - 1], cut, scroll);
}

static void wos_paint_entry(wos_screen *s)
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, fy = WOS_FIELD_Y, fh = WOS_FIELD_H;
	int rows = (fy - 16 - WB_ROWS_TOP) / WOS_HIST_ROW, first = wos.n_hist - rows;
	const struct wb_font *fr = &f_ui36b;
	char buf[WOS_LINE + WOS_RES + 8];

	/* the earlier lines, the newest just above the field: the line on
	 * the left, its result on the right */
	if (first < 0)
		first = 0;
	for (int i = first; i < wos.n_hist; i++) {
		int cy = fy - 16 - (wos.n_hist - i) * WOS_HIST_ROW + WOS_HIST_ROW / 2, ty = text_top(&f_mono16b, cy);
		int on = wos.back && i == wos.n_hist - wos.back, rw = text_w(&f_mono16b, wos.hist[i].res, 0);

		text(&f_mono16b, x + w - 12 - rw, ty, wos.hist[i].res, WB_COLOR_TEXT_HI, 0);
		text(&f_mono16b, x + 12, ty, wb_fit(&f_mono16b, wos.hist[i].line, w - 48 - rw, buf, sizeof(buf)),
		     on ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER, 0);
	}
	/* the field: a corner line, the line typed, its result or what is wrong */
	wb_card(x, fy, w, fh, WB_SQUARE, 1, &GLOW_FOCUS);
	if (s->detail && s->detail[0])
		text(&f_mono13, x + 22, fy + 14, wb_fit(&f_mono13, s->detail, w - 44, buf, sizeof(buf)),
		     WB_COLOR_PLACEHOLDER, 0);
	wos_line(&f_ui22, x + 22, w - 44, fy + 64, wos.line, wos.cur, s->right, s->placeholder,
		 wos.err ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI);
	if (wos.err) {
		const char *e = wb_fit(&f_ui22, wos.err, w - 44, buf, sizeof(buf));

		text(&f_ui22, s->right ? x + w - 22 - text_w(&f_ui22, e, 0) : x + 22, text_top(&f_ui22, fy + fh - 44),
		     e, WB_COLOR_ERROR, 0);
	} else if (wos.result[0]) {
		const char *r = wos.result;

		if (text_w(fr, r, -5) > w - 44)
			fr = &f_ui28b;
		r = wb_fit(fr, r, w - 44, buf, sizeof(buf));
		text(fr, s->right ? x + w - 22 - text_w(fr, r, -5) : x + 22, text_top(fr, fy + fh - 44), r,
		     wos.len ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, -5);
	}
}

/* ---- WOS_EDITOR ---------------------------------------------------------- */

#define WOS_ED_LINES	200
#define WOS_ED_ROW	30
#define WOS_ED_X	(WB_CONTENT_PAD + 20)
#define WOS_ED_W	(W - 2 * WB_CONTENT_PAD - 40)

/* the width of the character at s (a tab as an arrow) */
static int wos_ed_cw(const char *s, int *len)
{
	char one[5];
	int n = 1;

	if (*s == '\t') {
		*len = 1;
		return text_w(&f_ui20, "\xe2\x86\x92", 0);
	}
	while (n < 4 && ((unsigned char)s[n] & 0xc0) == 0x80)
		n++;
	memcpy(one, s, (size_t)n);
	one[n] = 0;
	*len = n;
	return text_w(&f_ui20, one, 0);
}

/* where each line starts, wrapped at word ends to WOS_ED_W; the count */
static int wos_ed_layout(const char *t, int *starts)
{
	int n = 0, i = 0;

	starts[n++] = 0;
	while (t[i] && n < WOS_ED_LINES) {
		int x = 0, brk = -1, j = i, len;

		while (t[j] && t[j] != '\n') {
			int w = wos_ed_cw(t + j, &len);

			if (x + w > WOS_ED_W && j > i)
				break;
			x += w;
			if (t[j] == ' ')
				brk = j + 1;
			j += len;
		}
		if (t[j] == '\n') {
			i = j + 1;
		} else if (!t[j]) {
			break;
		} else {
			i = brk > i ? brk : j;
		}
		starts[n++] = i;
	}
	return n;
}

static int wos_ed_line_of(const int *starts, int n, int cur)
{
	int l = 0;

	while (l + 1 < n && starts[l + 1] <= cur)
		l++;
	return l;
}

/* the x of byte cur in its line */
static int wos_ed_x(const char *t, int from, int cur)
{
	int x = 0, len;

	for (int i = from; i < cur && t[i] && t[i] != '\n'; i += len)
		x += wos_ed_cw(t + i, &len);
	return x;
}

static void wos_paint_editor(wos_screen *s)
{
	int starts[WOS_ED_LINES], n, top = WB_ROWS_TOP, h = H - WB_HINT_BAR_H - 16 - top;
	int rows = (h - 24) / WOS_ED_ROW, cl;
	const char *t = s->text ? s->text : "";

	n = wos_ed_layout(t, starts);
	cl = wos_ed_line_of(starts, n, wos.ed_cur);
	wb_card(WB_CONTENT_PAD, top, W - 2 * WB_CONTENT_PAD, h, WB_SQUARE, 1, &GLOW_FOCUS);
	if (!t[0] && s->placeholder)
		text(&f_ui20, WOS_ED_X, top + 12, s->placeholder, WB_COLOR_PLACEHOLDER, 0);
	for (int l = wos.ed_top; l < n && l < wos.ed_top + rows; l++) {
		int y = top + 12 + (l - wos.ed_top) * WOS_ED_ROW, x = WOS_ED_X, end = l + 1 < n ? starts[l + 1] : (int)strlen(t);
		char run[256];
		int k = 0, len;

		for (int i = starts[l]; i < end && t[i] && t[i] != '\n'; i += len) {
			wos_ed_cw(t + i, &len);
			if (t[i] == '\t') {
				run[k] = 0;
				x = text(&f_ui20, x, y, run, WB_COLOR_TEXT_HI, 0);
				x = text(&f_ui20, x, y, "\xe2\x86\x92", WB_COLOR_TEXT_LOW, 0);
				k = 0;
			} else if (k + len < (int)sizeof(run)) {
				memcpy(run + k, t + i, (size_t)len);
				k += len;
			}
		}
		run[k] = 0;
		text(&f_ui20, x, y, run, WB_COLOR_TEXT_HI, 0);
		if (l == cl)
			fill(WOS_ED_X + wos_ed_x(t, starts[l], wos.ed_cur), y + 2, 2, f_ui20.ascent + f_ui20.descent - 2,
			     WB_COLOR_CYAN);
	}
	if (n > rows)
		scrollbar(W - WB_CONTENT_PAD + 9, top, h, n * WOS_ED_ROW, rows * WOS_ED_ROW, wos.ed_top * WOS_ED_ROW);
}

/* 1 when the editor used the key */
static int wos_editor_key(wos_screen *s, int code)
{
	int starts[WOS_ED_LINES], n, l, len = s->text ? (int)strlen(s->text) : 0, fn = wos.mods & WOS_FN, ch, at = -1;
	int rows = (H - WB_HINT_BAR_H - 16 - WB_ROWS_TOP - 24) / WOS_ED_ROW;
	char *t = s->text;

	if (!t)
		return 0;
	n = wos_ed_layout(t, starts);
	l = wos_ed_line_of(starts, n, wos.ed_cur);
	switch (code) {
	case WOS_KEY_LEFT:
		if (fn)
			wos.ed_cur = starts[l];
		else
			while (wos.ed_cur > 0 && ((unsigned char)t[--wos.ed_cur] & 0xc0) == 0x80)
				;
		break;
	case WOS_KEY_RIGHT:
		if (fn)
			wos.ed_cur = l + 1 < n ? starts[l + 1] - (t[starts[l + 1] - 1] == '\n' || t[starts[l + 1] - 1] == ' ') : len;
		else if (wos.ed_cur < len)
			for (wos.ed_cur++; ((unsigned char)t[wos.ed_cur] & 0xc0) == 0x80; wos.ed_cur++)
				;
		break;
	case WOS_KEY_UP:
	case WOS_KEY_DOWN: {
		int to = code == WOS_KEY_UP ? l - 1 : l + 1, x = wos_ed_x(t, starts[l], wos.ed_cur), i, cw;

		if (to < 0 || to >= n)
			break;
		i = starts[to];
		while (t[i] && t[i] != '\n' && (to + 1 >= n || i < starts[to + 1]) && (cw = wos_ed_cw(t + i, &len)) / 2 <= x) {
			x -= cw;
			i += len;
		}
		wos.ed_cur = i;
		break;
	}
	case WOS_KEY_HOME:
		wos.ed_cur = starts[l];
		break;
	case WOS_KEY_END:
		wos.ed_cur = l + 1 < n ? starts[l + 1] - 1 : len;
		break;
	case WOS_KEY_BACKSPACE:
		if (wos.ed_cur > 0) {
			at = wos.ed_cur;
			while (at > 0 && ((unsigned char)t[--at] & 0xc0) == 0x80)
				;
		}
		break;
	case WOS_KEY_DELETE:
		if (wos.ed_cur < len)
			at = wos.ed_cur;
		break;
	default:
		ch = code == WOS_KEY_ENTER ? '\n' : code == WOS_KEY_TAB ? '\t' : wos_key_char(code, wos.mods);
		if (!ch)
			return 0;
		if (len + 1 < s->max) {
			memmove(t + wos.ed_cur + 1, t + wos.ed_cur, (size_t)(len - wos.ed_cur + 1));
			t[wos.ed_cur++] = (char)ch;
			s->dirty = 1;
		}
	}
	if (at >= 0) {
		int k = at + 1;

		while (((unsigned char)t[k] & 0xc0) == 0x80)
			k++;
		memmove(t + at, t + k, (size_t)(len - k + 1));
		wos.ed_cur = at;
		s->dirty = 1;
	}
	/* the cursor's line in view */
	n = wos_ed_layout(t, starts);
	l = wos_ed_line_of(starts, n, wos.ed_cur);
	if (l < wos.ed_top)
		wos.ed_top = l;
	if (l >= wos.ed_top + rows)
		wos.ed_top = l - rows + 1;
	return 1;
}

static void wos_paint_logview(wos_screen *s)
{
	int n = wos_log_count < WOS_LOG_N ? wos_log_count : WOS_LOG_N;

	if (!n) {
		text(&f_ui20, WB_CONTENT_PAD + 6, WB_ROWS_TOP + 8, "No log yet.", WB_COLOR_TEXT_LOW, 0);
		return;
	}
	wos_lines_card(WB_ROWS_TOP, H - WB_HINT_BAR_H - 16 - WB_ROWS_TOP, n, s->from, wos_log_line);
}

static void wos_paint(void *arg)
{
	wos_screen *s = wos_top();
	struct hint hs[10];
	char eb[48];

	(void)arg;
	if (s->kind == WOS_COLOUR) {
		fill(0, 0, W, H, s->colour);
	} else if (s->kind == WOS_CANVAS && !s->scene) {
		fill(0, 0, W, H, WB_COLOR_VOID);
	} else {
		wb_scene();
	}
	if (s->kind != WOS_COLOUR) {
		wb_header(wos_eyebrow(s, eb, sizeof(eb)), wos_title(s), s->title_right);
		if (s->kind == WOS_LIST && s->n)
			wos_paint_list(s);
		else if (s->kind == WOS_LIST)
			text(&f_ui20, WB_CONTENT_PAD + 6, WB_ROWS_TOP + 8,
			     s == &wos_settings_screen ? "This app has no settings." : "Nothing here yet.",
			     WB_COLOR_TEXT_LOW, 0);
		else if (s->kind == WOS_ENTRY)
			wos_paint_entry(s);
		else if (s->kind == WOS_LOGVIEW)
			wos_paint_logview(s);
		else if (s->kind == WOS_EDITOR)
			wos_paint_editor(s);
		else if (s->kind == WOS_CANVAS && s->paint)
			s->paint(0, WB_ROWS_TOP, W, H - WB_HINT_BAR_H - WB_ROWS_TOP);
	}
	if (wos_keys_showing(s))
		wos_keybar(hs, wos_bar_items(s, hs, 10), wos_esc_word(s), 1);
	if (s->busy) {			/* the running action: its name, a bar, Esc to cancel */
		static const struct hint bh[] = { { "Esc", "cancel", 1 } };
		int x = POP_X + 32, w = POP_W - 64, h = 32 + 18 + 6 + 36 + 24 + 12 + 24 + 48 + 16, y = (H - h) / 2;

		pop_frame(y, h);
		eyebrow(x, y + 32, wos_upper(wos_title(s), eb, sizeof(eb)));
		text(&f_ui28b, x, y + 32 + 18 + 6 + 2, s->busy, WB_COLOR_TEXT_HI, 0);
		if (s->progress >= 0) {
			int p = s->progress > 1000 ? 1000 : s->progress;

			box(x, y + 32 + 18 + 6 + 36 + 24, w, 12, 6, 0xFFFFFF, 26, 0, 0, 0);
			if (p)
				box(x, y + 32 + 18 + 6 + 36 + 24, w * p / 1000 < 12 ? 12 : w * p / 1000, 12, 6, WB_COLOR_CYAN,
				    255, 0, 0, 0);
		}
		panel_hints(x, w, y + h - 16 - 48 + 14, s->abort ? bh : 0, s->abort ? 1 : 0);
	} else if (wos.pop == WOS_POP_HELP) {
		struct help_key k[14];
		const char *about = s->about ? s->about : s == wos.stack[0] ? wos.app->about : 0;

		help_popup(wos_title(s), about, k, wos_help_keys(s, k, 14));
	} else if (wos.pop == WOS_POP_CHOICE) {
		static const struct hint ch[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
		char count[24] = "";

		wos_add_int(count, wos.pop_n, sizeof(count));
		wos_add(count, wos.pop_n == 1 ? " option" : " options", sizeof(count));
		menu_popup(s == &wos_settings_screen ? "SETTINGS" : wos_upper(wos_title(s), eb, sizeof(eb)),
			   wos_fields()[wos.pop_setting].label, count, wos.pop_items, 0, wos.pop_n,
			   wos.pop_focus, wos.pop_cur, 0, 0, ch, 2);
	} else if (wos.pop == WOS_POP_ASK) {
		static const struct hint ah[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
		const char *labels[8];
		const struct wb_icon *ics[8];
		int n = wos.pop_n < 8 ? wos.pop_n : 8, lines = wos.ask_body[0] ?
			text_wrap(&f_ui20, 0, 0, DLG_W - 64, 28, wos.ask_body, 0, 3, 0) : 0, y;

		for (int i = 0; i < n; i++) {
			labels[i] = wos.pop_items[i].label;
			ics[i] = i == wos.ask_danger ? &r_trash : &r_chevron;
		}
		y = dialog(wos_upper(wos_title(s), eb, sizeof(eb)), wos.ask_title, lines * 28, labels, ics, n, wos.pop_focus,
			   wos.ask_danger);
		if (lines)
			text_wrap(&f_ui20, DLG_X + 32, y, DLG_W - 64, 28, wos.ask_body, WB_COLOR_TEXT_MID, 3, 1);
		dlg_hints(ah, 2);
	} else if (wos.pop == WOS_POP_TEXT) {
		static const struct hint th[] = { { "Enter", "save", 0 }, { "Esc", "cancel", 1 } };
		int x = POP_X + 32, w = POP_W - 64, h = 32 + 18 + 6 + 36 + 20 + 56 + 14 + 2 * 26 + 14 + 48 + 16;
		int y = (H - h) / 2, fy;
		const char *under = wos.text_err ? wos.text_err : wos.ask_body;

		pop_frame(y, h);
		eyebrow(x, y + 32, wos_upper(wos_title(s), eb, sizeof(eb)));
		text(&f_ui28b, x, y + 32 + 18 + 6 + 2, wos.ask_title, WB_COLOR_TEXT_HI, 0);
		fy = y + 32 + 18 + 6 + 36 + 20;
		box(x, fy, w, 56, 10, WB_COLOR_VOID, 160, WB_COLOR_CYAN, 2, 0);
		wos_line(&f_ui22, x + 16, w - 32, fy + 28, wos.text, wos.text_cur, 0, 0, WB_COLOR_TEXT_HI);
		if (under[0])
			text_wrap(&f_ui18, x, fy + 56 + 14, w, 26, under, wos.text_err ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID,
				  2, 1);
		panel_hints(x, w, y + h - 16 - 48 + 14, th, 2);
	} else if (wos.pop == WOS_POP_MENU) {
		static const struct hint mh[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
		wos_row r = { 0 };

		s->row(s->focus, &r);
		menu_popup("OPTIONS", r.label ? r.label : wos_title(s), 0, wos.pop_items, 0, wos.pop_n,
			   wos.pop_focus, -2, 0, 0, mh, 2);
	}
	if (wos.msg[0]) {		/* a message: a pill over the key bar */
		int mw = text_w(&f_ui18, wos.msg, 0) + 48, my = H - WB_HINT_BAR_H - 16 - 44;

		box((W - mw) / 2, my, mw, 44, 22, WB_COLOR_SURFACE2, 245, WB_COLOR_CYAN, 1, 0);
		text(&f_ui18, (W - mw) / 2 + 24, text_top(&f_ui18, my + 22), wos.msg, WB_COLOR_TEXT_HI, 0);
	}
}

static uint16_t wos_band[W * WOS_BAND_H];

static void wos_out(const uint16_t *rows, int y0, int y1)
{
	pwrite(wos.fb, rows, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
}

static void wos_test_screen(void);	/* test.h */
static void wos_entry_preview(const wos_screen *s);

/* The screen as it is now, drawn where it changed. */
static void wos_present(void)
{
	wos_screen *s = wos_top();

	if (s->update)
		s->update(s);
	if (s->fields) {		/* a form: its rows are its fields */
		s->n = 0;
		while (s->fields[s->n].key)
			s->n++;
		s->row = wos_settings_row;
		s->choose = wos_settings_open;
		s->adjust = wos_settings_adjust;
	}
	if (s->kind == WOS_ENTRY && !wos.err)	/* a setting may have changed it */
		wos_entry_preview(s);
	if (s->kind == WOS_LIST && s->focus >= s->n)
		s->focus = s->n ? s->n - 1 : 0;
	if (getenv("WOS_TIMING")) {	/* a line per frame in the app's log: bands, ms */
		long t = wos_ms();
		char line[48] = "frame: ";
		int n = wb_bands(wos_paint, 0, wos_band, WOS_BAND_H, wos_out);

		wos_add(wos_add_int(line, n, sizeof(line)), " bands, ", sizeof(line));
		wos_add(wos_add_int(line, wos_ms() - t, sizeof(line)), " ms\n", sizeof(line));
		write(2, line, strlen(line));
	} else {
		wb_bands(wos_paint, 0, wos_band, WOS_BAND_H, wos_out);
	}
	wos_test_screen();
}

/* ---- the screen stack ----------------------------------------------------- */

static void wos_shown(void)
{
	wos_screen *s = wos.stack[wos.depth - 1];

	if (s->kind == WOS_EDITOR && s->text) {
		wos.ed_cur = (int)strlen(s->text);
		wos.ed_top = 0;
	}
	wos.t0 = wos_ms();
	wos.keys_gone = 0;
	wos.pop = WOS_POP_NONE;
}

/* A screen on top of this one; Esc comes back. */
static void wos_push(wos_screen *s)
{
	if (wos.depth == WOS_DEPTH)
		return;
	wos.stack[wos.depth++] = s;
	wos_shown();
}

/* Back to the screen under this one; the last one ends the app. A screen
 * with unsaved work asks first (wos_esc_pop). */
static void wos_pop(void)
{
	if (wos.depth <= 1) {
		wos.quit = 1;
		return;
	}
	wos.depth--;
	wos_shown();
}

static void wos_quit(void)
{
	wos.quit = 1;
}

/* ---- WOS_ENTRY's line ----------------------------------------------------- */

/* the result shown under the line: the app's preview of it (also of an
 * empty line: a calculator shows its last result there) */
static void wos_entry_preview(const wos_screen *s)
{
	wos.result[0] = 0;
	if (s->preview && s->preview(wos.line, wos.result, sizeof(wos.result)))
		wos.result[0] = 0;
}

static void wos_entry_set(const wos_screen *s, const char *t)
{
	wos_copy(wos.line, t, sizeof(wos.line));
	wos.len = wos.cur = (int)strlen(wos.line);
	wos.err = 0;
	wos_entry_preview(s);
}

static void wos_entry_insert(const wos_screen *s, const char *t)
{
	int n = (int)strlen(t);

	if (wos.len + n > WOS_LINE)
		return;
	memmove(wos.line + wos.cur + n, wos.line + wos.cur, (size_t)(wos.len - wos.cur + 1));
	memcpy(wos.line + wos.cur, t, (size_t)n);
	wos.len += n;
	wos.cur += n;
	wos.err = 0;
	wos.back = 0;
	wos_entry_preview(s);
}

static void wos_entry_erase(const wos_screen *s, int at)
{
	if (at < 0 || at >= wos.len)
		return;
	memmove(wos.line + at, wos.line + at + 1, (size_t)(wos.len - at));
	wos.len--;
	if (wos.cur > at)
		wos.cur--;
	wos.err = 0;
	wos.back = 0;
	wos_entry_preview(s);
}

static void wos_entry_enter(const wos_screen *s)
{
	char res[WOS_RES] = "";
	const char *why;

	if (!wos.len || !s->evaluate)
		return;
	why = s->evaluate(wos.line, res, sizeof(res));
	if (why) {
		wos.err = why;
		wos.result[0] = 0;
		return;
	}
	if (wos.n_hist == WOS_HIST) {
		memmove(wos.hist, wos.hist + 1, sizeof(wos.hist[0]) * (WOS_HIST - 1));
		wos.n_hist--;
	}
	wos_copy(wos.hist[wos.n_hist].line, wos.line, sizeof(wos.hist[0].line));
	wos_copy(wos.hist[wos.n_hist].res, res, sizeof(wos.hist[0].res));
	wos.n_hist++;
	wos.back = 0;
	wos_entry_set(s, "");
}

/* Up (1) and Down (-1) through the earlier lines */
static void wos_entry_recall(const wos_screen *s, int dir)
{
	int b = wos.back + dir;

	if (b < 0 || b > wos.n_hist)
		return;
	if (!wos.back)
		wos_copy(wos.draft, wos.line, sizeof(wos.draft));
	wos_entry_set(s, b ? wos.hist[wos.n_hist - b].line : wos.draft);
	wos.back = b;
}

/* 1 when the entry used the key */
static int wos_entry_key(const wos_screen *s, int code)
{
	int fn = wos.mods & WOS_FN, ch;
	const char *t;
	char one[2] = "";

	switch (code) {
	case WOS_KEY_ENTER:
		wos_entry_enter(s);
		return 1;
	case WOS_KEY_BACKSPACE:
		wos_entry_erase(s, fn ? wos.cur : wos.cur - 1);
		return 1;
	case WOS_KEY_DELETE:
		wos_entry_erase(s, wos.cur);
		return 1;
	case WOS_KEY_LEFT:
		wos.cur = fn ? 0 : wos.cur > 0 ? wos.cur - 1 : 0;
		return 1;
	case WOS_KEY_RIGHT:
		wos.cur = fn ? wos.len : wos.cur < wos.len ? wos.cur + 1 : wos.len;
		return 1;
	case WOS_KEY_HOME:
		wos.cur = 0;
		return 1;
	case WOS_KEY_END:
		wos.cur = wos.len;
		return 1;
	case WOS_KEY_UP:
		wos_entry_recall(s, 1);
		return 1;
	case WOS_KEY_DOWN:
		wos_entry_recall(s, -1);
		return 1;
	}
	ch = wos_key_char(code, wos.mods);
	if (!ch)
		return 0;
	one[0] = (char)ch;
	one[1] = 0;
	t = s->typed ? s->typed(wos.line, wos.cur, ch) : one;
	if (t && !strcmp(t, "\n"))
		wos_entry_enter(s);
	else if (t && t[0])
		wos_entry_insert(s, t);
	return 1;
}

/* ---- keys ------------------------------------------------------------------ */

static int wos_text_key(int code);

/* the key on the pop-up over the screen */
static void wos_pop_key(wos_screen *s, int code)
{
	if (wos.pop == WOS_POP_TEXT) {
		wos_text_key(code);
		return;
	}
	if (wos.pop == WOS_POP_ASK && (code == WOS_KEY_ENTER || code == WOS_KEY_ESC)) {
		void (*done)(int) = wos.ask_done;

		wos.pop = WOS_POP_NONE;
		if (done)
			done(code == WOS_KEY_ENTER ? wos.pop_focus : -1);
		return;
	}
	if (wos.pop == WOS_POP_HELP) {	/* only an explanation */
		if (code == WOS_KEY_ESC || code == WOS_KEY_HELP)
			wos.pop = WOS_POP_NONE;
		return;
	}
	if (code == WOS_KEY_UP && wos.pop_focus > 0)
		wos.pop_focus--;
	else if (code == WOS_KEY_DOWN && wos.pop_focus + 1 < wos.pop_n)
		wos.pop_focus++;
	else if (code == WOS_KEY_ESC || (code == WOS_KEY_OPTIONS && wos.pop == WOS_POP_MENU))
		wos.pop = WOS_POP_NONE;
	else if (code == WOS_KEY_ENTER) {
		int pop = wos.pop;

		wos.pop = WOS_POP_NONE;
		if (pop == WOS_POP_CHOICE)
			wos_pick_choice();
		else if (s->menu[wos.pop_focus].fn)
			s->menu[wos.pop_focus].fn(s->focus);
	}
}

static void wos_open_menu(wos_screen *s)
{
	wos.pop_n = 0;
	for (const wos_action *a = s->menu; a->key && wos.pop_n < WOS_POP_ITEMS; a++) {
		memset(&wos.pop_items[wos.pop_n], 0, sizeof(wos.pop_items[0]));
		wos.pop_items[wos.pop_n++].label = a->word;
	}
	wos.pop_focus = 0;
	wos.pop = WOS_POP_MENU;
}

/*
 * A question over the screen (the design's dialog, 6.5): choices as
 * "Keep editing|Discard", the focus on the safe one; done(i) with the one
 * chosen, or -1 on Esc. danger: the destructive choice, in red (-1: none).
 */
static void wos_ask(const char *title, const char *body, const char *choices, int safe, int danger,
		    void (*done)(int choice))
{
	static char labels[8][48];
	char v[48];

	wos_copy(wos.ask_title, title, sizeof(wos.ask_title));
	wos_copy(wos.ask_body, body ? body : "", sizeof(wos.ask_body));
	wos.pop_n = 0;
	while (wos.pop_n < 8 && wos_choice(choices, wos.pop_n, v, sizeof(v), labels[wos.pop_n], sizeof(labels[0]))) {
		memset(&wos.pop_items[wos.pop_n], 0, sizeof(wos.pop_items[0]));
		wos.pop_items[wos.pop_n].label = labels[wos.pop_n];
		wos.pop_n++;
	}
	wos.pop_focus = safe >= 0 && safe < wos.pop_n ? safe : 0;
	wos.ask_danger = danger;
	wos.ask_done = done;
	wos.pop = WOS_POP_ASK;
}

/*
 * Text typed in a pop-up (6.5 Text input): a title, the field with text in
 * it, an explanation under it. Enter calls done(text): 0 closes it, a
 * message keeps it open with the message in red. Esc cancels.
 */
static void wos_ask_text(const char *title, const char *explain, const char *text,
			 const char *(*done)(const char *text))
{
	wos_copy(wos.ask_title, title, sizeof(wos.ask_title));
	wos_copy(wos.ask_body, explain ? explain : "", sizeof(wos.ask_body));
	wos_copy(wos.text, text ? text : "", sizeof(wos.text));
	wos.text_len = wos.text_cur = (int)strlen(wos.text);
	wos.text_err = 0;
	wos.text_done = done;
	wos.pop = WOS_POP_TEXT;
}

/* 1 when the text pop-up used the key */
static int wos_text_key(int code)
{
	int fn = wos.mods & WOS_FN, ch, at = -1;

	switch (code) {
	case WOS_KEY_ESC:
		wos.pop = WOS_POP_NONE;
		return 1;
	case WOS_KEY_ENTER:
		wos.text_err = wos.text_done ? wos.text_done(wos.text) : 0;
		if (!wos.text_err)
			wos.pop = WOS_POP_NONE;
		return 1;
	case WOS_KEY_BACKSPACE:
		at = fn ? wos.text_cur : wos.text_cur - 1;
		break;
	case WOS_KEY_DELETE:
		at = wos.text_cur;
		break;
	case WOS_KEY_LEFT:
		wos.text_cur = fn ? 0 : wos.text_cur > 0 ? wos.text_cur - 1 : 0;
		return 1;
	case WOS_KEY_RIGHT:
		wos.text_cur = fn ? wos.text_len : wos.text_cur < wos.text_len ? wos.text_cur + 1 : wos.text_len;
		return 1;
	case WOS_KEY_HOME:
		wos.text_cur = 0;
		return 1;
	case WOS_KEY_END:
		wos.text_cur = wos.text_len;
		return 1;
	}
	if (at >= 0) {
		if (at < wos.text_len) {
			memmove(wos.text + at, wos.text + at + 1, (size_t)(wos.text_len - at));
			wos.text_len--;
			if (wos.text_cur > at)
				wos.text_cur--;
		}
		wos.text_err = 0;
		return 1;
	}
	ch = wos_key_char(code, wos.mods);
	if (!ch || wos.text_len >= WOS_LINE)
		return 0;
	memmove(wos.text + wos.text_cur + 1, wos.text + wos.text_cur, (size_t)(wos.text_len - wos.text_cur + 1));
	wos.text[wos.text_cur++] = (char)ch;
	wos.text_len++;
	wos.text_err = 0;
	return 1;
}

/* A short line over the key bar for two seconds: "Saved", "Copied". */
static void wos_message(const char *text)
{
	wos_copy(wos.msg, text, sizeof(wos.msg));
	wos.msg_until = wos_ms() + 2000;
}

/* a list's focus to the next row that can take it, i from it, by step */
static void wos_list_move(wos_screen *s, int step)
{
	for (int i = s->focus + step; i >= 0 && i < s->n; i += step) {
		wos_row r = { 0 };

		s->row(i, &r);
		if (r.kind != WOS_ROW_INFO) {
			s->focus = i;
			return;
		}
	}
}

/* 1 when the screen's kind used the key */
static int wos_screen_key(wos_screen *s, int code)
{
	int ch = wos_key_char(code, wos.mods);

	if (s->kind == WOS_ENTRY)
		return wos_entry_key(s, code);
	if (s->kind == WOS_EDITOR)
		return wos_editor_key(s, code);
	if (s->kind == WOS_CANVAS)
		return s->key && s->key(code, ch);
	if (s->kind == WOS_LOGVIEW) {
		int n = wos_log_count < WOS_LOG_N ? wos_log_count : WOS_LOG_N;

		if (code == WOS_KEY_UP && s->from + 1 < n)
			s->from++;
		else if (code == WOS_KEY_DOWN && s->from > 0)
			s->from--;
		return code == WOS_KEY_UP || code == WOS_KEY_DOWN;
	}
	if (s->kind != WOS_LIST || !s->n)
		return 0;
	switch (code) {
	case WOS_KEY_UP:
		wos_list_move(s, -1);
		return 1;
	case WOS_KEY_DOWN:
		wos_list_move(s, 1);
		return 1;
	case WOS_KEY_LEFT:
	case WOS_KEY_RIGHT:
		if (s->adjust && s->adjust(s->focus, code == WOS_KEY_LEFT ? -1 : 1))
			return 1;
		/* a page back or on */
		for (int i = 0; i < 4; i++)
			wos_list_move(s, code == WOS_KEY_LEFT ? -1 : 1);
		return 1;
	case WOS_KEY_ENTER:
		if (s->choose)
			s->choose(s->focus);
		return 1;
	case WOS_KEY_OPTIONS:
		if (!s->menu)
			return 0;
		wos_open_menu(s);
		return 1;
	}
	return 0;
}

static void wos_discard(int choice)
{
	if (choice == 1) {
		wos_top()->dirty = 0;
		wos_pop();
	}
}

static void wos_key(int code)
{
	wos_screen *s = wos_top();

	if (s->busy) {			/* a running action: only Esc, which aborts it (6.7 step 1) */
		if (code == WOS_KEY_ESC && s->abort)
			s->abort();
		return;
	}
	if (wos.pop) {
		wos_pop_key(s, code);
		return;
	}
	/* the circle, clover and diamond are the same in every app (6.6) */
	if (code == WOS_KEY_HELP) {
		wos.pop = WOS_POP_HELP;
		return;
	}
	if (code == WOS_KEY_LOG || code == WOS_KEY_SETTINGS) {
		wos_screen *to = code == WOS_KEY_LOG ? &wos_log_screen : &wos_settings_screen;

		if (s == &wos_log_screen || s == &wos_settings_screen)
			wos.depth--;	/* one goes instead of the other */
		if (to != s) {
			to->from = to->focus = 0;
			wos_push(to);
		} else {
			wos.depth++;
		}
		return;
	}
	if (code == WOS_KEY_ESC) {
		/* 6.7: a pop-up (above), the line's text, one screen back, the end */
		if (wos_esc_clears(s)) {
			wos.back = 0;
			wos_entry_set(s, "");
		} else if (s->dirty) {
			wos_ask("Discard changes?", "What you changed here is not saved.", "Keep editing|Discard", 0, 1,
				wos_discard);
		} else {
			wos_pop();
		}
		return;
	}
	for (const wos_action *a = s->actions; a && a->key; a++) {
		if (!a->fn)
			continue;
		if (a->key == code || (a->key > 0 && (a->key & 0x10000) && (a->key & 0x7f) == wos_key_char(code, wos.mods))) {
			a->fn(a->arg);
			return;
		}
		if (a->key < 0 && wos_key_step(a->key, code)) {
			a->fn(wos_key_step(a->key, code));
			return;
		}
	}
	wos_screen_key(s, code);
}

/* ---- the loop ---------------------------------------------------------------- */

#include <poll.h>

struct wos_ev {				/* the kernel's input_event on RV32 */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static void wos_mod(int code, int down)
{
	int bit = code == WOS_KEY_LEFTSHIFT || code == WOS_KEY_RIGHTSHIFT ? WOS_SHIFT :
		  code == WOS_KEY_LEFTCTRL ? WOS_CTRL : code == WOS_KEY_LEFTALT ? WOS_ALT :
		  code == WOS_KEY_FN ? WOS_FN : 0;

	if (down)
		wos.mods |= bit;
	else
		wos.mods &= ~bit;
}

static int wos_is_mod(int code)
{
	return code == WOS_KEY_LEFTSHIFT || code == WOS_KEY_RIGHTSHIFT || code == WOS_KEY_LEFTCTRL ||
	       code == WOS_KEY_LEFTALT || code == WOS_KEY_FN;
}

/* One key event; 1 when it changed something to draw. */
static int wos_event(int code, int value)
{
	if (wos_is_mod(code)) {
		wos_mod(code, value != 0);
		return 0;
	}
	if (!value) {			/* a release: only a canvas that asks for them */
		wos_screen *s = wos_top();

		if (s->kind == WOS_CANVAS && s->release && !wos.pop && !s->busy) {
			s->release(code);
			return 1;
		}
		return 0;
	}
	if (!key_acts(code, value))
		return 0;
	if (code == WOS_KEY_ESC && (wos.mods & WOS_FN))	/* Fn+Esc is standby: sleepd's */
		return 0;
	wos_key(code);
	return 1;
}

static void wos_test_open(void);	/* test.h */
static int wos_test_keys(void);

/*
 * Runs the app from its first screen until Esc on that screen, the logo
 * key (SIGTERM) or wos_quit(). Returns the exit status: 0, or 1 when the
 * panel or the keys cannot be opened.
 */
static int wos_run(const wos_app *app, wos_screen *first)
{
	long touched = -100000, ticked, turned_check = 0;
	int was_standby = 0;
	struct pollfd p[2];

	wos.app = app;
	wos.stack[0] = first;
	wos.depth = 1;
	wos_kv_default = wos_schema_default;
	if (app->settings)
		wos_settings_screen.fields = app->settings;
	wos_signals();
	wos.fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	wos.ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (wos.fb < 0 || wos.ev < 0)
		return 1;
	wos.fifo = -1;
	wos_test_open();
	wb_init();
	wos_shown();
	ticked = wos.t0;
	wos_present();
	p[0].fd = wos.ev;
	p[0].events = POLLIN;
	p[1].fd = wos.fifo;
	p[1].events = POLLIN;
	while (!wos.quit && !wos_stop_sig) {
		wos_screen *s = wos_top();
		long now = wos_ms(), wait = 250;
		int standby, draw = 0, k;

		if (s->kind == WOS_COLOUR && !wos.keys_gone && s->keys_ms - (now - wos.t0) < wait)
			wait = s->keys_ms - (now - wos.t0);
		if (app->tick && app->tick_ms && app->tick_ms - (now - ticked) < wait)
			wait = app->tick_ms - (now - ticked);
		if (wos.msg[0] && wos.msg_until - now < wait)
			wait = wos.msg_until - now;
		k = poll(p, wos.fifo >= 0 ? 2 : 1, wait < 0 ? 0 : (int)wait);
		now = wos_ms();
		standby = wos_standby();
		/* the panel again after standby or a stop: it showed something else */
		if (wos_cont_sig || (was_standby && !standby)) {
			wos_cont_sig = 0;
			wb_rec_invalidate();
			draw = 1;
		}
		was_standby = standby;
		if (s->kind == WOS_COLOUR && !wos.keys_gone && !standby && now - wos.t0 >= s->keys_ms) {
			wos.keys_gone = 1;
			draw = 1;
		}
		if (wos.msg[0] && now >= wos.msg_until) {
			wos.msg[0] = 0;
			draw = 1;
		}
		if (app->keep_on && !standby && now - touched >= 5000) {
			wos_keep_on();
			touched = now;
		}
		if (app->tick && app->tick_ms && now - ticked >= app->tick_ms) {
			ticked = now;
			app->tick();
			draw = 1;
		}
		/* the badge turned: the scene and the screen again (four times a second) */
		if (now - turned_check >= 250) {
			turned_check = now;
			if (wb_scene_turned()) {
				wb_rec_invalidate();
				draw = 1;
			}
		}
		if (k > 0 && wos.fifo >= 0 && (p[1].revents & POLLIN))
			draw |= wos_test_keys();
		if (k > 0 && (p[0].revents & POLLIN)) {
			struct wos_ev evs[16];
			int got = (int)read(wos.ev, evs, sizeof(evs)) / (int)sizeof(evs[0]);

			for (int q = 0; q < got && !wos.quit; q++) {
				if (evs[q].type != 1)
					continue;
				if (evs[q].value == 2 && key_stale(evs[q].sec, evs[q].usec))
					continue;
				draw |= wos_event(evs[q].code, evs[q].value);
			}
		}
		if (wos.quit || wos_stop_sig)
			break;
		s = wos_top();
		/* a colour's brightness, once its keys are gone, and never on a
		 * screen that standby turned off */
		if (s->kind == WOS_COLOUR && wos.keys_gone && !standby && s->brightness && s->brightness != wos_duty)
			wos_backlight(s->brightness);
		if (draw || wos_log_seen == 0) {
			wos_log_seen = 1;
			wos_present();
		}
	}
	if (app->quit)
		app->quit();
	return 0;
}
