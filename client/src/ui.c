/* SPDX-License-Identifier: MIT */
/*
 * store: the full-screen UI (docs/client.md §4), drawn with libwb
 * (why2025-linux libwb/: wb.h, wbstatus.h) in the menu's style: the
 * frontpanel's scene behind every screen, lists of glass rows (an icon, the
 * name, a second line, a value or state on the right), the eyebrow saying
 * where the screen lies, frosted pop-ups with their keys inside.
 *
 *   Discover    ("App store") updates, featured, recently updated, the types
 *   Apps/Games  All, then categories with counts, then their apps (#1: a menu to sort)
 *   Search      typing filters the cached index, offline
 *   App page    cover (or icon), facts, state and actions, then description,
 *               screenshots, commands, requirements, licences, versions
 *   My apps     the sections of client.md §4 (`store --my-apps` opens it)
 *   Settings    sources (on/off, a mirror), refresh, keep packages,
 *               clear cache, storage per app
 *
 * The same functions as the CLI do the work; questions they ask come back
 * as a dialog (opts.ask), their messages as the screen's notice (say_hook),
 * downloads as a progress dialog where Esc cancels.
 *
 * Memory: no 1 MB frame buffer (client.md §3: 1.5 MB in all). Every
 * screen is drawn band by band, 40 rows at a time, as appterm does, and
 * only the bands that changed since the frame before (libwb wb_bands); the
 * scene is kept per rotation (~80 KB), and a pop-up is glass over the band,
 * not blurred (that needs the whole picture).
 *
 * "Open" writes the app's id to /tmp/store-open (P.tmp) and ends: the menu, back
 * from apprun, starts it (why2025-linux home.c).
 *
 * Keys as the WhyBadge design has them (Docs/06-design-system.md 6.6-6.8):
 * Enter does the row's thing, Esc one layer back (in Search it first
 * clears what was typed), Backspace only erases text; the square key is
 * a pop-up menu where a screen has one (a category: sort), the circle the
 * store's log (apprun's /tmp/app-store.log), the cloud its settings, the
 * diamond a help pop-up with this screen's keys.
 *
 * Built only for the badge (build.sh: -DSTORE_UI and libwb) and, with -DSTORE_UI_HOST, as a host program that takes its keys
 * from a script and writes screens as PPM (tests/test_ui.py).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "store.h"

#ifndef STORE_UI

int ui_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	warn("this build has no screens: use the commands (store --help)");
	return 2;
}

#else

#include <poll.h>

#include "wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#ifdef STORE_UI_HOST
#define WB_HOST		/* no panel to turn: rot_read() keeps 0 */
#endif

#include "wb.h"
#include "wbstatus.h"
#include "keyrep.h"

#define S_(x)		((x) ? (x) : "")
#define ARRAY_N(a)	((int)(sizeof(a) / sizeof((a)[0])))

#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_TAB		15
#define KEY_ENTER	28
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_SPACE	57
#define KEY_F1		59
#define KEY_F2		60
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108

#define UP_S		"\xe2\x86\x91\xe2\x86\x93"
#define LR_S		"\xe2\x86\x90 \xe2\x86\x92"
#define ARROW_S		" \xe2\x86\x92 "
#define DOT_S		" \xe2\x80\xa2 "	/* the fonts have no middle dot */

/* ---- small text helpers -------------------------------------------------- */

static char *cat(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	*p = 0;
	return p;
}

static void human(char *out, size_t n, long long b)
{
	if (b < 1024)
		snprintf(out, n, "%lld B", b);
	else if (b < 1024 * 1024)
		snprintf(out, n, "%lld KB", (b + 1023) / 1024);
	else
		snprintf(out, n, "%lld.%lld MB", b / (1024 * 1024), b % (1024 * 1024) * 10 / (1024 * 1024));
}

/* Text cut to max_w px with an ellipsis, into buf. */
#define fit		wb_fit

static int ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; hay && *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

/* ---- categories (repository.md §4), from categories.json ------------------ */

#define MAX_TYPES	6
#define MAX_CATS	24

struct ctype {
	char id[16], name[32], icon[16];
	int hidden, n;
	struct { char id[24], name[40], icon[16]; } c[MAX_CATS];
};
static struct ctype CT[MAX_TYPES];
static int n_ctypes;

static void categories_load(void)
{
	char *data = 0, err[200];
	size_t len;
	struct json j;
	int types;

	n_ctypes = 0;
	for (int i = 0; i < n_indexes && !data; i++)
		if (categories_get(&IX[i], &data, &len, err, sizeof(err)))
			data = 0;
	if (data && !json_parse(&j, data, len)) {
		types = json_get(&j, 0, "types");
		for (int t = 0; t < json_len(&j, types) && n_ctypes < MAX_TYPES; t++) {
			int o = json_item(&j, types, t), cs = json_get(&j, o, "categories");
			struct ctype *c = &CT[n_ctypes++];

			memset(c, 0, sizeof(*c));
			json_str(&j, json_get(&j, o, "id"), c->id, sizeof(c->id));
			json_str(&j, json_get(&j, o, "name"), c->name, sizeof(c->name));
			json_str(&j, json_get(&j, o, "icon"), c->icon, sizeof(c->icon));
			c->hidden = json_bool(&j, json_get(&j, o, "hidden"), 0);
			for (int k = 0; k < json_len(&j, cs) && c->n < MAX_CATS; k++) {
				int co = json_item(&j, cs, k);

				json_str(&j, json_get(&j, co, "id"), c->c[c->n].id, sizeof(c->c[0].id));
				json_str(&j, json_get(&j, co, "name"), c->c[c->n].name, sizeof(c->c[0].name));
				json_str(&j, json_get(&j, co, "icon"), c->c[c->n].icon, sizeof(c->c[0].icon));
				c->n++;
			}
		}
		json_free(&j);
	}
	free(data);
	if (!n_ctypes) {	/* no network yet: the two types every badge knows */
		static const char *const t[2][3] = { { "app", "Apps", "info" }, { "game", "Games", "games" } };

		for (int i = 0; i < 2; i++) {
			memset(&CT[i], 0, sizeof(CT[i]));
			snprintf(CT[i].id, sizeof(CT[i].id), "%s", t[i][0]);
			snprintf(CT[i].name, sizeof(CT[i].name), "%s", t[i][1]);
			snprintf(CT[i].icon, sizeof(CT[i].icon), "%s", t[i][2]);
		}
		n_ctypes = 2;
	}
}

static const struct ctype *ctype_find(const char *id)
{
	for (int i = 0; i < n_ctypes; i++)
		if (id && !strcmp(CT[i].id, id))
			return &CT[i];
	return 0;
}

static const char *cat_name(const char *type, const char *cat_id)
{
	const struct ctype *t = ctype_find(type);

	for (int i = 0; t && i < t->n; i++)
		if (cat_id && !strcmp(t->c[i].id, cat_id))
			return t->c[i].name;
	return cat_id && *cat_id ? cat_id : "Other";
}

/* a menu icon name (launch.icon, categories.json) as one of the row icons */
static const struct wb_icon *icon_named(const char *n)
{
	static const struct { const char *n; const struct wb_icon *ic; } icons[] = {
		{ "globe", &r_globe }, { "gameboy", &r_gameboy }, { "games", &r_games },
		{ "terminal", &r_ip }, { "sensors", &r_sensors }, { "bluetooth", &r_bluetooth },
		{ "usb", &r_usb }, { "chip", &r_chip }, { "wifi", &r_wifi }, { "info", &r_info },
		{ "led", &r_led }, { "disk", &r_drive }, { "sdcard", &r_sdcard },
		{ "ethernet", &r_ethernet }, { "palette", &r_palette }, { "power", &r_power },
		{ "bolt", &r_bolt }, { "keyboard", &r_keyboard }, { "sun", &r_sun },
		{ "store", &r_store }, { "image", &r_image }, { "text", &r_text },
	};

	for (int i = 0; n && i < ARRAY_N(icons); i++)
		if (!strcmp(n, icons[i].n))
			return icons[i].ic;
	return &r_count;
}

static const struct wb_icon *app_icon(const struct app *a)
{
	if (a && a->menu_icon)
		return icon_named(a->menu_icon);
	return a && a->type && !strcmp(a->type, "game") ? &r_games : &r_count;
}

/* ---- the screen model: lists of rows, as the menu's ---------------------- */

enum kind { R_GROUP, R_ACTION, R_INFO, R_TOGGLE };
enum act {
	A_NONE, A_SEARCH, A_TYPE, A_CAT, A_APP, A_MYAPPS, A_SETTINGS,
	A_INSTALL, A_OPEN, A_UPDATE, A_REMOVE, A_REINSTALL, A_REPAIR, A_ADOPT, A_DELETE,
	A_REPLACE, A_DESC, A_SHOTS, A_LICENSES, A_LICENSE, A_VERSIONS, A_VERSION, A_DETAILS,
	A_CHANGES, A_UPDATE_ALL, A_REINSTALL_ALL, A_REFRESH, A_SOURCE, A_SRC_ON, A_MIRROR,
	A_KEEP, A_CLEAR, A_STORAGE,
};

struct row {
	enum kind kind;
	enum act act;
	const struct wb_icon *ic;
	char label[64];
	char sub[96];		/* the second line (a summary, what is in it) */
	char value[64];
	uint32_t value_c;	/* 0: text-mid */
	char arg[48];
	int on;
};

#define LIST_MAX	128

enum view { V_DISCOVER, V_TYPE, V_CAT, V_SEARCH, V_APP, V_TEXT, V_MYAPPS, V_SETTINGS,
	    V_SOURCE, V_STORAGE, V_LICENSES, V_VERSIONS, V_SHOTS };

struct frame {
	enum view v;
	char arg[48], arg2[48];
	char title[64];		/* its title, for the eyebrows of the screens it opens */
	int focus, scroll;
	int sort;		/* V_CAT: 0 by name, 1 recently updated */
};

static struct {
	char eyebrow[72];	/* where the screen lies (crumb()) */
	char title[64];
	struct row rows[LIST_MAX];
	int n;
	int head_h;		/* V_APP, V_SEARCH: their header above the rows */
} L;

static struct frame stack[10];
static int depth;
#define TOP		(&stack[depth - 1])

static char notice[200];
static int notice_bad;
static char query[48];		/* Search */
static char textbuf[24 * 1024];	/* V_TEXT */
#define TL_MAX		2000
static int tl_off[TL_MAX], tl_len[TL_MAX], n_tl;	/* its lines, laid out once */
static char text_title[64];
static int keep_packages;

static struct {			/* the app on the app page */
	char id[33];
	struct app *a;
	struct reg r;
	enum state st;
	char *data;		/* its details (repository.md §5.2), or NULL */
	struct json j;
	int have_j;
	struct picture icon;
	struct picture pic;	/* the screenshot shown */
	struct picture thumb;	/* its cover (180 px, repository.md §5.3), in the header */
	int n_shots, shot;
	/* the header, copied: installing reloads the index under AP.a */
	char name[64], author[96], meta[96], summary[200];
	const struct wb_icon *ic;
	int terminal;
	int files_only;		/* CATEGORY="none": started by opening a file */
	int system;		/* type "system": comes with WhyOS, not removed */
} AP;

/* the overlays: a question, a download, a line while something runs */
enum ov { OV_NONE, OV_ASK, OV_PROGRESS, OV_BUSY, OV_INPUT,
	  OV_HELP,		/* the diamond key */
	  OV_MENU };		/* the square key: a category's sort */
static struct {
	enum ov kind;
	char eyebrow[32], title[96], body[200];
	const char *labels[2];
	const struct wb_icon *yes_ic;
	int focus;
	long long done, total;
	char input[200];
} O;

static struct {
	char hhmm[6];
	int clock_ok, wifi_bars, lora;
} SB;

static struct row *add(enum kind k, const char *label, const struct wb_icon *ic, enum act a)
{
	static struct row spare;
	struct row *r = L.n < LIST_MAX ? &L.rows[L.n++] : &spare;

	memset(r, 0, sizeof(*r));
	r->kind = k;
	r->act = a;
	r->ic = ic;
	snprintf(r->label, sizeof(r->label), "%s", label);
	return r;
}

static void say_notice(int bad, const char *s)
{
	snprintf(notice, sizeof(notice), "%s", s);
	notice_bad = bad;
}

/* messages from the store's functions: the last warning wins over chatter */
static void on_say(int is_warn, const char *text)
{
	if (is_warn || !notice_bad) {
		char *p;

		snprintf(notice, sizeof(notice), "%s", text);
		if ((p = strstr(notice, ": run again with --yes")))
			*p = 0;
		notice[0] = (char)(notice[0] >= 'a' && notice[0] <= 'z' ? notice[0] - 32 : notice[0]);
		notice_bad = is_warn;
	}
}

static int net_up(void)
{
	char st[16];
	int fd, n, up = 0;
	const char *ifs[2] = { "/sys/class/net/wlan0/operstate", "/sys/class/net/usb0/operstate" };

	for (int i = 0; i < 2 && !up; i++) {
		fd = open(ifs[i], O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		n = (int)read(fd, st, sizeof(st) - 1);
		close(fd);
		up = n >= 2 && !strncmp(st, "up", 2);
	}
	return up;
}

/* an error from the network, said so that the user knows what to do */
static void net_notice(const char *err)
{
#ifdef STORE_UI_HOST
	fprintf(stderr, "store-ui: %s\n", err);	/* the host has no wlan0 */
	if (0)
#else
	if (!net_up())
#endif
		say_notice(1, "No network: connect in Settings > Wi-Fi");
	else
		say_notice(1, err);
}

/* ---- drawing ------------------------------------------------------------- */

/* WhyOS on the left; LoRa, Wi-Fi and the clock on the right (wbstatus.h),
 * as the menu draws them */
static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
	     text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
	status_right(SB.hhmm, SB.clock_ok, SB.wifi_bars, SB.lora);
}

/* The time; on the host STORE_UI_TIME (seconds since 1970) when set, so that
 * the tests' screens do not depend on the minute they ran in */
static time_t ui_time(time_t t)
{
#ifdef STORE_UI_HOST
	const char *s = getenv("STORE_UI_TIME");

	if (s)
		return (time_t)atol(s);
#endif
	return t;
}

static int status_read(void)
{
	time_t t = ui_time(time(0));
	struct tm tm;
	char old[6];
	int bars = SB.wifi_bars, lora = SB.lora, ok = SB.clock_ok;

	memcpy(old, SB.hhmm, sizeof(old));
	localtime_r(&t, &tm);
	SB.clock_ok = tm.tm_year >= 124;
	snprintf(SB.hhmm, sizeof(SB.hhmm), "%02d:%02d", tm.tm_hour % 24, tm.tm_min % 60);
	SB.wifi_bars = wifi_bars();
	SB.lora = lora_on();
	return bars != SB.wifi_bars || lora != SB.lora || ok != SB.clock_ok || memcmp(old, SB.hhmm, sizeof(old));
}

static int focusable(const struct row *r)
{
	return r->kind == R_ACTION || r->kind == R_TOGGLE;
}

static int row_h(const struct row *r)
{
	return r->kind == R_GROUP ? 17 : 68;
}

/*
 * A list row: wb_row() (the menu's Settings and Connection rows: a glass
 * card, the icon, the label with the second line under it, a value in mono
 * and a chevron or the switch on the right), but with the value in its own
 * colour: an app's state (installed green, an update yellow).
 */
static void store_row(int x, int y, int w, int h, const struct wb_icon *ic, const char *label,
		      const char *sub, const char *value, uint32_t value_c, int right, int sw, int on)
{
	int top = y + h / 2, rx = x + w - 18, lx = x + 20 + (ic ? 26 + 16 : 0);
	int ly = sub && sub[0] ? y + h / 2 - 11 : top;
	uint32_t ic_c = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	char buf[96];

	if (!value_c) {
		wb_row(x, y, w, h, ic, label, sub, value, right, sw, on);
		return;
	}
	wb_card(x, y, w, h, WB_RADIUS_MD, on, &GLOW_FOCUS);
	if (ic)
		icon(ic, x + 20, top - ic->h / 2, ic_c, 255);
	if (right == WB_ROW_OPEN) {
		icon(&r_chevron, rx - 20, top - 10, ic_c, 255);
		rx -= 20 + 16;
	} else if (right == WB_ROW_SWITCH) {
		wb_switch(rx - 52, top, sw);
		rx -= 52 + 16;
	}
	if (value && value[0]) {
		const char *v = wb_fit(&f_mono14, value, (rx - lx) / 2, buf, sizeof(buf));

		rx -= text_w(&f_mono14, v, 0);
		text(&f_mono14, rx, text_top(&f_mono14, top), v, value_c, 0);
		rx -= 16;
	}
	text(&f_ui22, lx, text_top(&f_ui22, ly), wb_fit(&f_ui22, label, rx - lx, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, 0);
	if (sub && sub[0])
		text(&f_ui16, lx, text_top(&f_ui16, ly + 14 + 2 + 10), wb_fit(&f_ui16, sub, rx - lx, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
}

static void draw_row(const struct row *r, int y, int on)
{
	if (r->kind == R_GROUP) {	/* a group's label, as the menu's */
		text(&f_mono13, WB_CONTENT_PAD, y, r->label, WB_COLOR_TEXT_LOW, 21);
		return;
	}
	store_row(WB_CONTENT_PAD, y, W - 2 * WB_CONTENT_PAD, row_h(r), r->ic, r->label, r->sub, r->value,
		  r->value_c, r->kind == R_ACTION ? WB_ROW_OPEN : r->kind == R_TOGGLE ? WB_ROW_SWITCH : WB_ROW_NONE,
		  r->on, on);
}

/* lines of s wrapped to w px in font f; calls out(line, len, i) per line; returns the count */
static int wrap(const struct wb_font *f, const char *s, int w,
		void (*out)(const char *line, int len, int i, void *arg), void *arg)
{
	int lines = 0;

	while (*s) {
		const char *p = s, *brk = 0;
		char tmp[256];
		int n;

		if (*s == '\n') {	/* an empty line */
			if (out)
				out(s, 0, lines, arg);
			lines++;
			s++;
			continue;
		}
		/* the longest piece up to a space that fits */
		while (*p && *p != '\n') {
			const char *q = p;

			while (*q && *q != ' ' && *q != '\n')
				q++;
			n = (int)(q - s);
			if (n >= (int)sizeof(tmp))
				n = (int)sizeof(tmp) - 1;
			memcpy(tmp, s, (size_t)n);
			tmp[n] = 0;
			if (text_w(f, tmp, 0) > w && brk)
				break;
			brk = q;
			p = *q == ' ' ? q + 1 : q;
			if (text_w(f, tmp, 0) > w)
				break;	/* one word wider than the line */
		}
		if (!brk)
			brk = p;
		if (out)
			out(s, (int)(brk - s), lines, arg);
		lines++;
		s = brk;
		while (*s == ' ')
			s++;
		if (*s == '\n')
			s++;
	}
	return lines;
}

struct textpos { const struct wb_font *f; int x, y, lh, first, max; uint32_t c; };

static void text_line(const char *line, int len, int i, void *arg)
{
	struct textpos *t = arg;
	char buf[256];

	if (i < t->first || i >= t->first + t->max || len <= 0)
		return;
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;
	memcpy(buf, line, (size_t)len);
	buf[len] = 0;
	text(t->f, t->x, t->y + (i - t->first) * t->lh, buf, t->c, 0);
}

/* Bumped whenever a picture of the app page is loaded or freed: what a
 * picture draws, for the frame's recording (its pixels are not hashed) */
static unsigned pic_gen;

static void picture_draw(const struct picture *p, int x0, int y0, uint32_t bg)
{
	if (wb_rec) {		/* it writes fb itself: what decides its pixels */
		int k[] = { 0x51c7, x0, p->w, (int)pic_gen, WB_P(p->rgb), WB_P(p->alpha) };

		wb_rec_rows(y0, y0 + p->h, k, sizeof(k));
		return;
	}
	for (int y = 0; y < p->h; y++) {
		int sy = y0 + y;

		if (sy < fb_y0 || sy >= fb_y1)
			continue;
		for (int x = 0; x < p->w; x++) {
			int sx = x0 + x;
			unsigned a = p->alpha ? p->alpha[y * p->w + x] : 255;
			uint16_t v = p->rgb[y * p->w + x];

			if ((unsigned)sx >= W || !a)
				continue;
			if (a == 255) {
				fb[sy * W + sx] = v;
			} else {
				uint32_t c = (uint32_t)(((v >> 11) << 3) << 16 | (((v >> 5) & 63) << 2) << 8 |
						       ((v & 31) << 3));

				put(sx, sy, c, a);
			}
		}
	}
	(void)bg;
}

/*
 * The app page's header: the cover (THUMB px, a screenshot or a crop of
 * one) with name, author, facts and summary beside it; without one the
 * icon (on a 96 px glass tile), with the summary below.
 */
#define THUMB		180

static void draw_app_head(int y)
{
	char buf[160];
	int side = AP.thumb.rgb ? THUMB : 96;
	int tx = WB_CONTENT_PAD + side + 24, tw = W - WB_CONTENT_PAD - tx;
	struct textpos tp = { &f_ui18, WB_CONTENT_PAD, y + 96 + 20, 26, 0, 2, WB_COLOR_TEXT_MID };

	if (AP.thumb.rgb) {
		edge_rect(WB_CONTENT_PAD - 1, y - 1, AP.thumb.w + 2, AP.thumb.h + 2, 0xFFFFFF, 66);
		picture_draw(&AP.thumb, WB_CONTENT_PAD, y, WB_COLOR_VOID);
		tp.x = tx;
		tp.y = y + 108;
		tp.max = 3;
	} else {
		wb_card(WB_CONTENT_PAD, y, 96, 96, WB_RADIUS_LG, 0, &GLOW_FOCUS);
		if (AP.icon.rgb)
			picture_draw(&AP.icon, WB_CONTENT_PAD + (96 - AP.icon.w) / 2, y + (96 - AP.icon.h) / 2,
				     WB_COLOR_VOID);
		else
			icon_scaled(AP.ic, WB_CONTENT_PAD + 22, y + 22, 52, WB_COLOR_CYAN);
	}
	text(&f_ui28b, tx, y + 4, fit(&f_ui28b, AP.name, tw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, -8);
	text(&f_ui18, tx, y + 44, fit(&f_ui18, AP.author, tw, buf, sizeof(buf)), WB_COLOR_TEXT_MID, 0);
	text(&f_mono14, tx, y + 74, fit(&f_mono14, AP.meta, tw, buf, sizeof(buf)),
	     AP.terminal ? WB_COLOR_YELLOW : WB_COLOR_PLACEHOLDER, 0);
	if (AP.summary[0])
		wrap(&f_ui18, AP.summary, AP.thumb.rgb ? tw : W - 2 * WB_CONTENT_PAD, text_line, &tp);
}

/* Search's field: a glass card with the loupe, what was typed (or what to
 * type, as a field's hint) and the cursor */
static void draw_search_head(int y)
{
	int fx = WB_CONTENT_PAD, fw = W - 2 * WB_CONTENT_PAD, tx = fx + 18 + 18 + 12;
	const char *t = query;

	wb_card(fx, y, fw, 56, WB_RADIUS_MD, 0, &GLOW_FOCUS);
	loupe(fx + 18, y + 19, WB_COLOR_TEXT_MID);
	while (*t && text_w(&f_ui20, t, 0) > fw - (tx - fx) - 24)
		next_cp(&t);
	if (!query[0])
		text(&f_ui20, tx + 6, text_top(&f_ui20, y + 28), "Type a name, a tag or a command",
		     WB_COLOR_PLACEHOLDER, 0);
	else
		tx = text(&f_ui20, tx, text_top(&f_ui20, y + 28), t, WB_COLOR_TEXT_HI, 0) + 2;
	fill(tx, y + 17, 2, 22, WB_COLOR_CYAN);
}

/*
 * The hint bar (6.8): only what differs per screen. Enter with its word,
 * the square key where the screen has a menu, Esc on the right; no Up and
 * Down, and no circle, cloud or diamond (the help lists those).
 */
static void hints(void)
{
	static const struct hint nav[] = { { "Enter", "open", 0 }, { "Esc", "back", 1 } };
	static const struct hint nav_sort[] = {
		{ "Enter", "open", 0 }, { "#1", "sort", 0 }, { "Esc", "back", 1 },
	};
	static const struct hint adj[] = { { "Enter", "change", 0 }, { "Esc", "back", 1 } };
	static const struct hint search_clear[] = { { "Enter", "open", 0 }, { "Esc", "clear", 1 } };
	static const struct hint discover[] = { { "Enter", "open", 0 }, { "Esc", "close", 1 } };
	static const struct hint back[] = { { "Esc", "back", 1 } };
	static const struct hint shots[] = { { LR_S, "previous / next", 0 }, { "Esc", "back", 1 } };
	const struct frame *f = TOP;
	const struct row *r = f->focus >= 0 && f->focus < L.n ? &L.rows[f->focus] : 0;

	if (f->v == V_TEXT)
		hint_bar(back, 1);
	else if (f->v == V_SHOTS)
		hint_bar(shots, 2);
	else if (f->v == V_SEARCH && query[0])
		hint_bar(search_clear, 2);
	else if (depth == 1)
		hint_bar(discover, 2);
	else if (f->v == V_CAT)
		hint_bar(nav_sort, 3);
	else if (r && r->kind == R_TOGGLE)
		hint_bar(adj, 2);
	else
		hint_bar(nav, 2);
}

/* The diamond key's pop-up: what the screen is for and its keys. */
static void draw_help(void)
{
	static const struct help_key k_list[] = {
		{ "Enter", "Open the row, or do what it says" },
		{ "Esc", "Back one screen; on the first, close the store" },
		{ "#4", "The store's log" }, { "#5", "Settings: sources, cache, storage" },
		{ "#6", "This help" }, { "Logo", "Back to the menu" },
	};
	static const struct help_key k_cat[] = {
		{ "Enter", "Open the app's page" }, { "#1", "Sort: by name or recently updated" },
		{ "Esc", "Back" }, { "#4", "The store's log" }, { "#5", "Settings" },
		{ "#6", "This help" }, { "Logo", "Back to the menu" },
	};
	static const struct help_key k_search[] = {
		{ "Enter", "Open the app's page" }, { "Backspace", "Erase what was typed" },
		{ "Esc", "Clear what was typed; then back" }, { "#4", "The store's log" },
		{ "#5", "Settings" }, { "#6", "This help" }, { "Logo", "Back to the menu" },
	};
	static const struct help_key k_text[] = {
		{ UP_S, "Scroll" }, { "Esc", "Back" }, { "#4", "The store's log" }, { "#5", "Settings" },
		{ "#6", "This help" }, { "Logo", "Back to the menu" },
	};
	static const struct help_key k_shots[] = {
		{ LR_S, "Previous or next screenshot" }, { "Esc", "Back" }, { "#4", "The store's log" },
		{ "#5", "Settings" }, { "#6", "This help" }, { "Logo", "Back to the menu" },
	};
	const struct frame *f = TOP;
	const char *title = f->v == V_TEXT ? text_title : f->v == V_SHOTS ? "Screenshots" :
			    L.title[0] ? L.title : "App store";
	const char *about =
		f->v == V_DISCOVER ? "Apps and games for the badge, installed on the SD card: updates, "
				     "what is new, and every type." :
		f->v == V_CAT ? "The apps in this category." :
		f->v == V_SEARCH ? "Type a name, a tag or a command: the list filters as you type, "
				   "offline." :
		f->v == V_APP ? "An app: install, open, update or remove it, and what it needs." :
		f->v == V_MYAPPS ? "The apps you installed: updates first." :
		f->v == V_SETTINGS || f->v == V_SOURCE || f->v == V_STORAGE ? "Where the store gets its "
				     "apps, and the room they take." :
		f->v == V_TEXT && !strcmp(f->arg, "log") ? "What the store printed while it ran." : 0;

	if (f->v == V_CAT)
		help_popup(title, about, k_cat, ARRAY_N(k_cat));
	else if (f->v == V_SEARCH)
		help_popup(title, about, k_search, ARRAY_N(k_search));
	else if (f->v == V_TEXT)
		help_popup(title, about, k_text, ARRAY_N(k_text));
	else if (f->v == V_SHOTS)
		help_popup(title, about, k_shots, ARRAY_N(k_shots));
	else
		help_popup(title, about, k_list, ARRAY_N(k_list));
}

/* Where the content ends: above the hint bar */
#define CONTENT_BOTTOM	(H - WB_HINT_BAR_H - 8)

static void draw_list(void)
{
	struct frame *f = TOP;
	int ys[LIST_MAX], hs[LIST_MAX], y = 0, top, bottom = CONTENT_BOTTOM, scroll, cut, keep, show;

	top = WB_STATUS_BAR_H + 20;
	if (f->v != V_APP) {
		char n[24] = "";

		if (f->v == V_CAT)	/* the category's size, right of its name */
			snprintf(n, sizeof(n), "%d app%s", L.n - 1, L.n == 2 ? "" : "s");
		wb_header(L.eyebrow, L.title, n);
		top += 18 + 6 + 44;
	} else {		/* the app's name is in its header */
		eyebrow(WB_CONTENT_PAD + 6, 72, L.eyebrow);
		top = 72 + 18 + 12;
	}
	if (L.head_h) {
		if (f->v == V_APP)
			draw_app_head(top);
		else
			draw_search_head(top + 8);
		top += L.head_h;
	}
	if (notice[0]) {
		char buf[200];

		text(&f_ui18, WB_CONTENT_PAD + 6, top + 8,
		     fit(&f_ui18, notice, W - 2 * WB_CONTENT_PAD - 6, buf, sizeof(buf)),
		     notice_bad ? WB_COLOR_ERROR : WB_COLOR_OK, 0);
		top += 8 + 24;
	}
	top += 24;
	for (int i = 0; i < L.n; i++) {
		if (i && L.rows[i].kind == R_GROUP)
			y += 24 - 10;
		ys[i] = y;
		hs[i] = row_h(&L.rows[i]);
		y += hs[i] + 10;
	}
	/* a group label stays with the row under it */
	keep = f->focus > 0 && f->focus < L.n && L.rows[f->focus - 1].kind == R_GROUP ? f->focus - 1 : f->focus;
	/* on the last row that takes the focus, the rows under it show too */
	show = f->focus < 0 || f->focus >= L.n ? -1 : L.n - 1;
	for (int i = f->focus + 1; show >= 0 && i < L.n; i++)
		if (focusable(&L.rows[i]))
			show = f->focus;
	scroll = list_scroll(ys, hs, L.n, show, keep, bottom - top, &cut);
	/* cards cut off at either end show only their part inside the list */
	clip_rows(scroll ? top : 0, L.n && y - 10 - scroll > bottom - top ? top + cut : H);
	for (int i = 0; i < L.n; i++) {
		int ry = top + ys[i] - scroll;

		if (ry + hs[i] <= top && scroll)
			continue;
		if (ry >= top + cut)
			break;
		if (ry + hs[i] + 40 <= fb_y0 || ry - 40 >= fb_y1)
			continue;	/* not in this band, glow included */
		draw_row(&L.rows[i], ry, i == f->focus);
	}
	clip_end();
	/* a narrow scroll bar right of a list that does not fit (the menu's) */
	if (L.n)
		scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, y - 10, bottom - top, scroll);
	if (!L.n && f->v == V_SEARCH && query[0])
		text(&f_ui18, WB_CONTENT_PAD + 6, top, "Nothing found", WB_COLOR_TEXT_MID, 0);
}

/* A page of text (a description, a licence, the log): on a glass card
 * under the header, TEXT_PAD inside it */
#define TEXT_PAD	24
#define TEXT_LH		26
#define TEXT_W		(W - 2 * WB_CONTENT_PAD - 2 * TEXT_PAD)

static void tl_add(const char *line, int len, int i, void *arg)
{
	(void)arg;
	if (i < TL_MAX) {
		tl_off[i] = (int)(line - textbuf);
		tl_len[i] = len;
		n_tl = i + 1;
	}
}

static void text_layout(void)
{
	n_tl = 0;
	wrap(&f_ui18, textbuf, TEXT_W, tl_add, 0);
}

static int text_rows(void)
{
	return (CONTENT_BOTTOM - WB_ROWS_TOP - 2 * 20) / TEXT_LH;
}

static void draw_text(void)
{
	struct frame *f = TOP;
	int top = WB_ROWS_TOP + 20, rows = text_rows();

	wb_header(L.eyebrow, text_title, 0);
	wb_card(WB_CONTENT_PAD, WB_ROWS_TOP, W - 2 * WB_CONTENT_PAD, CONTENT_BOTTOM - WB_ROWS_TOP,
		WB_RADIUS_MD, 0, &GLOW_FOCUS);
	for (int i = f->scroll; i < n_tl && i < f->scroll + rows; i++) {
		char buf[256];
		int y = top + (i - f->scroll) * TEXT_LH, len = tl_len[i] < 255 ? tl_len[i] : 255;

		if (y + TEXT_LH <= fb_y0 || y >= fb_y1 || len <= 0)
			continue;
		memcpy(buf, textbuf + tl_off[i], (size_t)len);
		buf[len] = 0;
		text(&f_ui18, WB_CONTENT_PAD + TEXT_PAD, y, buf, WB_COLOR_TEXT_HI, 0);
	}
	scrollbar(W - WB_CONTENT_PAD + 18, WB_ROWS_TOP, CONTENT_BOTTOM - WB_ROWS_TOP, n_tl, rows, f->scroll);
}

static void draw_shots(void)
{
	char b[48];

	snprintf(b, sizeof(b), "%d of %d", AP.shot + 1, AP.n_shots);
	wb_header(L.eyebrow, "Screenshots", b);
	if (AP.pic.rgb) {
		int x = (W - AP.pic.w) / 2, y = WB_ROWS_TOP + (CONTENT_BOTTOM - WB_ROWS_TOP - AP.pic.h) / 2;

		edge_rect(x - 1, y - 1, AP.pic.w + 2, AP.pic.h + 2, 0xFFFFFF, 66);
		picture_draw(&AP.pic, x, y, WB_COLOR_VOID);
	} else {
		text(&f_ui18, WB_CONTENT_PAD + 6, WB_ROWS_TOP, notice[0] ? notice : "Loading\xe2\x80\xa6",
		     notice_bad ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	}
}

/* The overlays: libwb's dialog (wbui.h), frosted over the dimmed screen,
 * with its keys inside at the bottom */
static void draw_overlay(void)
{
	static const struct hint ask_h[] = {
		{ UP_S, "select", 0 }, { "Enter", "confirm", 0 }, { "Esc", "cancel", 1 },
	};
	static const struct hint prog_h[] = { { "Esc", "cancel", 1 } };
	static const struct hint input_h[] = { { "Enter", "save", 0 }, { "Esc", "cancel", 1 } };
	int fx = DLG_X + 32, fw = DLG_W - 64, lines, y;
	struct textpos tp = { &f_ui20, fx, 0, 30, 0, 4, WB_COLOR_TEXT_MID };

	switch (O.kind) {
	case OV_ASK: {
		const struct wb_icon *ics[2] = { &r_chevron, O.yes_ic };

		lines = wrap(&f_ui20, O.body, fw, 0, 0);
		if (lines > 4)
			lines = 4;
		/* removing, deleting, clearing: the choice in red */
		y = dialog(O.eyebrow, O.title, lines * 30, O.labels, ics, 2, O.focus,
			   O.yes_ic == &r_trash ? 1 : -1);
		tp.y = y;
		wrap(&f_ui20, O.body, fw, text_line, &tp);
		dlg_hints(ask_h, 3);
		break;
	}
	case OV_PROGRESS: {
		char b[64], d[24], t[24];
		int pct = O.total > 0 ? (int)(O.done * 100 / O.total) : 0;

		y = dialog(O.eyebrow, O.title, 8 + 24 + 18, 0, 0, 0, 0, -1);
		box(fx, y + 8, fw, 8, 4, 0xFFFFFF, 46, 0, 0, 0);
		if (pct)
			box(fx, y + 8, fw * pct / 100 < 8 ? 8 : fw * pct / 100, 8, 4, WB_COLOR_CYAN, 255, 0, 0, 0);
		human(d, sizeof(d), O.done);
		human(t, sizeof(t), O.total);
		if (O.body[0])
			snprintf(b, sizeof(b), "%s", O.body);
		else if (O.total > 0)
			snprintf(b, sizeof(b), "%s of %s", d, t);
		else
			snprintf(b, sizeof(b), "%s", d);
		text(&f_mono14, fx, y + 8 + 24, b, WB_COLOR_TEXT_MID, 0);
		dlg_hints(prog_h, O.body[0] ? 0 : 1);
		break;
	}
	case OV_BUSY:		/* no keys: it ends by itself */
		dialog(O.eyebrow, O.title, 0, 0, 0, 0, 0, -1);
		break;
	case OV_INPUT: {	/* the menu's text field (home.c draw_input) */
		const char *t = O.input;
		char buf[200];
		int tx;

		y = dialog(O.eyebrow, O.title, 56 + 34, 0, 0, 0, 0, -1);
		box_focus(fx, y, fw, 56, WB_RADIUS_MD, &GLOW_FOCUS);
		while (*t && text_w(&f_mono18, t, 0) > fw - 40)
			next_cp(&t);
		tx = text(&f_mono18, fx + 20, text_top(&f_mono18, y + 28), t, WB_COLOR_TEXT_HI, 0);
		fill(tx + 1, y + 16, 2, 24, WB_COLOR_CYAN);
		text(&f_ui18, fx, y + 56 + 10, fit(&f_ui18, O.body, fw, buf, sizeof(buf)),
		     WB_COLOR_PLACEHOLDER, 0);
		dlg_hints(input_h, 2);
		break;
	}
	case OV_HELP:
		draw_help();
		break;
	case OV_MENU: {		/* a choice: the present order ticked */
		static const struct hint menu_h[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
		struct menu_item it[2] = { { .label = "By name" }, { .label = "Recently updated first" } };
		char eb[48];
		int j = 0;

		for (const char *p = L.title[0] ? L.title : "Apps"; *p && j < (int)sizeof(eb) - 1; p++)
			eb[j++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
		eb[j] = 0;
		menu_popup(eb, "Sort", 0, it, 0, 2, O.focus, TOP->sort, 0, 0, menu_h, 2);
		break;
	}
	default:
		break;
	}
}

/* The screen. It only draws (present() runs it once more to record the
 * frame): wb_scene() reads the rotation while recording and keeps it for
 * the bands drawn after. */
static void draw(void)
{
	wb_scene();
	status_bar();
	if (TOP->v == V_TEXT)
		draw_text();
	else if (TOP->v == V_SHOTS)
		draw_shots();
	else
		draw_list();
	hints();		/* under an overlay too: dimmed, as the menu's */
	if (O.kind != OV_NONE)
		draw_overlay();
}

/* ---- output: the panel band by band, or a PPM on the host --------------- */

#define BAND_H	40
static uint16_t band[W * BAND_H];
#ifdef STORE_UI_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static long tm_out, tm_rec;	/* us writing bands out, recording (STORE_UI_TIMING) */

static void out_band(const uint16_t *b, int y0, int y1)
{
	long t = wb_now_us();

#ifdef STORE_UI_HOST
	memcpy(screen + y0 * W, b, (size_t)(y1 - y0) * W * 2);
#else
	if (pwrite(fb_fd, b, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2) < 0)
		return;	/* the panel is gone: nothing to show it on */
#endif
	tm_out += wb_now_us() - t;
}

static void paint(void *arg)
{
	long t = wb_rec ? wb_now_us() : 0;

	(void)arg;
	draw();
	if (t)
		tm_rec += wb_now_us() - t;
}

/*
 * The screen to the panel: recorded first, then only the bands that changed
 * since the frame before drawn and written (libwb wb_bands). A focus that
 * moves draws its two rows' bands and the hint bar; a download's progress
 * the bar's and its numbers' bands; a pop-up's focus its choices' bands.
 * STORE_UI_TIMING=1: a line per frame on stderr (the store's log).
 */
static void present(void)
{
	static int timing = -1;
	long t0 = wb_now_us();
	int n;

	n = wb_bands(paint, 0, band, BAND_H, out_band);
	if (timing < 0)
		timing = getenv("STORE_UI_TIMING") != 0;
	if (timing) {
		char b[128];
		int l = snprintf(b, sizeof(b), "store-ui: frame %ld us, %d bands, record %ld, scene %ld, "
				 "glass %ld, out %ld\n", wb_now_us() - t0, n, tm_rec, tm_scene, tm_glass, tm_out);

		if (write(2, b, (size_t)l) < 0)
			timing = 0;
	}
	tm_out = tm_rec = tm_scene = tm_glass = 0;
}

/* ---- keys ---------------------------------------------------------------- */

struct ev {			/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static int shift;

/* US layout, as home's key_char */
static char key_char(int code)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == KEY_SPACE)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo) - 1)
		return 0;
	return shift ? hi[code] : lo[code];
}

#ifdef STORE_UI_HOST
#include <stdio.h>

/*
 * The script (STORE_UI_SCRIPT): one action per line --
 *   up down left right enter esc bs f1..f6   a key
 *   type TEXT                                 keys for TEXT
 *   shot PATH                                 the screen as PPM
 */
static FILE *script;
static char typing[128];
static int typed;

static void save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];
		unsigned char px[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2),
					(unsigned char)((p & 31) << 3) };

		fwrite(px, 3, 1, f);
	}
	fclose(f);
}

static int code_of(char c)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";

	if (c == ' ')
		return KEY_SPACE;
	for (int i = 0; i < (int)sizeof(lo) - 1; i++)
		if (lo[i] == c)
			return i;
	return 0;
}

/* the next key from the script; -1 at its end; 0 with nothing yet (wait).
 * "held <key>" is a repeat of it (value 2), as a key held down sends. */
static int next_key(int wait_ms)
{
	char line[256];

	(void)wait_ms;
	if (typing[typed])
		return code_of(typing[typed++]);
	while (script && fgets(line, sizeof(line), script)) {
		static const struct { const char *n; int c; } k[] = {
			{ "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
			{ "enter", KEY_ENTER }, { "esc", KEY_ESC }, { "bs", KEY_BACKSPACE },
			{ "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_SYM_SECOND }, { "f4", KEY_SYM_LOG },
			{ "f5", KEY_SYM_SETTINGS }, { "f6", KEY_SYM_HELP }, { "tab", KEY_TAB },
		};

		const char *name = line;
		int value = 1;

		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "held ", 5)) {
			name = line + 5;
			value = 2;
		}
		if (!strncmp(line, "shot ", 5)) {
			present();
			save_ppm(line + 5);
			continue;
		}
		if (!strncmp(line, "type ", 5)) {
			snprintf(typing, sizeof(typing), "%s", line + 5);
			typed = 0;
			return typing[0] ? code_of(typing[typed++]) : 0;
		}
		for (int i = 0; i < ARRAY_N(k); i++)
			if (!strcmp(name, k[i].n) && key_acts(k[i].c, value))
				return k[i].c;
	}
	return -1;
}

static void input_open(void)
{
	const char *s = getenv("STORE_UI_SCRIPT");

	script = s ? fopen(s, "r") : 0;
}

#else

static int ev_fd = -1;

static void input_open(void)
{
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
}

/* the next key press (code), 0 after wait_ms without one, -1 when input is gone */
static int next_key(int wait_ms)
{
	struct pollfd p = { ev_fd, POLLIN, 0 };
	struct ev e;

	if (ev_fd < 0)
		return -1;
	for (;;) {
		int n = (int)read(ev_fd, &e, sizeof(e));

		if (n == (int)sizeof(e)) {
			if (e.type != 1)
				continue;
			if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT) {
				shift = e.value != 0;
				continue;
			}
			/* a press; a repeat of a key that repeats, unless it
			 * waited while the screen fell behind */
			if (key_acts(e.code, e.value) && !(e.value == 2 && key_stale(e.sec, e.usec)))
				return e.code;
			continue;
		}
		if (n < 0 && errno != EAGAIN)
			return -1;
		if (wait_ms <= 0 || poll(&p, 1, wait_ms) <= 0)
			return 0;
		wait_ms = 0;	/* one wait only: the caller loops */
	}
}

#endif

/* The next key (-1 when input is gone); meanwhile the screen turns with the
 * badge (the scene's figures are drawn for each rotation). */
static int wait_key(void)
{
	for (;;) {
		int k = next_key(250);

		if (k)
			return k;
		if (wb_scene_turned())
			present();
	}
}

/* ---- questions and progress for the store's functions ------------------- */

static int ask(const char *eb, const char *title, const char *body, const char *yes)
{
	O.yes_ic = !strcmp(yes, "Remove") || !strcmp(yes, "Delete") || !strcmp(yes, "Clear") ? &r_trash : &r_check;
	O.kind = OV_ASK;
	snprintf(O.eyebrow, sizeof(O.eyebrow), "%s", eb);
	snprintf(O.title, sizeof(O.title), "%s", title);
	snprintf(O.body, sizeof(O.body), "%s", body);
	O.labels[0] = "Cancel";
	O.labels[1] = yes;
	O.focus = 0;	/* the safe choice (Dialog.dc.html) */
	for (;;) {
		int k;

		present();
		k = wait_key();
		if (k < 0 || k == KEY_ESC) {
			O.kind = OV_NONE;
			return 0;
		}
		if (k == KEY_UP || k == KEY_DOWN || k == KEY_TAB)
			O.focus = !O.focus;
		else if (k == KEY_ENTER) {
			O.kind = OV_NONE;
			return O.focus;
		}
	}
}

/* lifecycle's confirm(): its question, as a dialog */
static int ask_cb(const char *question, void *arg)
{
	enum ov was = O.kind;
	char title[96];
	int r;

	(void)arg;
	snprintf(title, sizeof(title), "%s", (const char *)arg ? (const char *)arg : "Go ahead?");
	{
		char q[200];

		snprintf(q, sizeof(q), "%s", question);
		if (q[0] >= 'a' && q[0] <= 'z')
			q[0] = (char)(q[0] - 32);
		r = ask("APP STORE", title, q, "Go ahead");
	}
	O.kind = was;
	return r;
}

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int cancelled;

static int progress_cb(long long done, long long total, void *arg)
{
	static long last;
	long t = now_ms();
	int k;

	(void)arg;
	if (O.kind != OV_PROGRESS) {
		O.kind = OV_PROGRESS;
		last = 0;
	}
	O.done = done;
	O.total = total;
	O.body[0] = 0;
	if (!last) {
		last = t;
		present();
	} else if (t - last >= 250 || (total > 0 && done >= total)) {
		last = t;
		present();	/* the bar's and its numbers' bands only */
	}
#ifndef STORE_UI_HOST		/* the host's keys are the script's, not for this */
	while ((k = next_key(0)) > 0)
		if (k == KEY_ESC)
			cancelled = 1;
#else
	(void)k;
#endif
	return cancelled;
}

static void busy(const char *eb, const char *title)
{
	O.kind = OV_BUSY;
	snprintf(O.eyebrow, sizeof(O.eyebrow), "%s", eb);
	snprintf(O.title, sizeof(O.title), "%s", title);
	present();
}

/* ---- the screens' contents ----------------------------------------------- */

static void app_page_load(const char *id);

static int is_hidden_type(const char *t)
{
	const struct ctype *c = ctype_find(t);

	return !t || (c && c->hidden) || !strcmp(t, "system");
}

static void state_value(struct row *r, enum state s, const struct app *a, const struct reg *rg)
{
	switch (s) {
	case ST_INSTALLED:
		snprintf(r->value, sizeof(r->value), "Installed");
		r->value_c = WB_COLOR_OK;
		break;
	case ST_UPDATE:
		snprintf(r->value, sizeof(r->value), "%s" ARROW_S "%s", rg->version, S_(a->version));
		r->value_c = WB_COLOR_YELLOW;
		break;
	case ST_MISSING:
		snprintf(r->value, sizeof(r->value), "Not on this card");
		r->value_c = WB_COLOR_WARN;
		break;
	case ST_DAMAGED:
		snprintf(r->value, sizeof(r->value), "Needs repair");
		r->value_c = WB_COLOR_ERROR;
		break;
	case ST_LEGACY:		/* on the badge, put there by hand: installed all the same */
		snprintf(r->value, sizeof(r->value), "Installed by hand");
		r->value_c = WB_COLOR_OK;
		break;
	case ST_UNREGISTERED:
		snprintf(r->value, sizeof(r->value), "Found on this card");
		break;
	default:		/* not installed: nothing on the right */
		break;
	}
	if (a && !r->sub[0])
		snprintf(r->sub, sizeof(r->sub), "%s", S_(a->summary));
}

static struct row *app_row(struct app *a)
{
	struct reg rg;
	enum state s = app_state(a->id, &rg, 0);
	struct row *r = add(R_ACTION, a->name, app_icon(a), A_APP);

	snprintf(r->arg, sizeof(r->arg), "%s", a->id);
	state_value(r, s, a, &rg);
	return r;
}

static int by_updated(const void *x, const void *y)
{
	const struct app *a = *(struct app *const *)x, *b = *(struct app *const *)y;

	return strcmp(S_(b->updated), S_(a->updated));
}

static int by_name(const void *x, const void *y)
{
	const struct app *a = *(struct app *const *)x, *b = *(struct app *const *)y;

	return strcasecmp(S_(a->name), S_(b->name));
}

/* the apps of the index that a list may show, n of them */
static struct app **visible_apps(int *n)
{
	static struct app *v[512];

	*n = 0;
	for (int i = 0; i < n_indexes; i++)
		for (int k = 0; k < IX[i].n && *n < ARRAY_N(v); k++) {
			struct app *a = &IX[i].apps[k];

			if (!a->withdrawn && !is_hidden_type(a->type) && index_find(a->id, 0) == a)
				v[(*n)++] = a;
		}
	return v;
}

static int count_updates(void)
{
	char ids[64][33];
	int n = reg_list(ids, 64), u = 0;

	for (int i = 0; i < n; i++)
		u += app_state(ids[i], 0, 0) == ST_UPDATE;
	return u;
}

/* name added to a row's second line, as a list: "Flappy, Marble" */
static void sub_add(struct row *r, const char *name)
{
	size_t l = strlen(r->sub);

	if (l + 8 < sizeof(r->sub))
		snprintf(r->sub + l, sizeof(r->sub) - l, "%s%s", l ? ", " : "", S_(name));
}

/* A type's second line on Discover: its categories that have apps */
static void type_sub(struct row *r, const struct ctype *t, struct app **v, int n)
{
	for (int k = 0; k < t->n; k++)
		for (int i = 0; i < n; i++)
			if (v[i]->type && !strcmp(v[i]->type, t->id) && v[i]->category &&
			    !strcmp(v[i]->category, t->c[k].id)) {
				sub_add(r, t->c[k].name);
				break;
			}
}

static void build_discover(void)
{
	int n, ups = count_updates(), shown = 0;
	struct app **v = visible_apps(&n);
	struct row *r;

	snprintf(L.title, sizeof(L.title), "App store");
	r = add(R_ACTION, "Search", &r_keyboard, A_SEARCH);
	snprintf(r->sub, sizeof(r->sub), "A name, a tag or a command");
	if (ups) {
		r = add(R_ACTION, "Updates", &r_refresh, A_MYAPPS);
		snprintf(r->sub, sizeof(r->sub), "Update all in My apps");
		snprintf(r->value, sizeof(r->value), "%d waiting", ups);
		r->value_c = WB_COLOR_YELLOW;
	}
	add(R_GROUP, "BROWSE", 0, A_NONE);
	for (int t = 0; t < n_ctypes; t++) {
		int c = 0;

		if (CT[t].hidden)
			continue;
		for (int i = 0; i < n; i++)
			c += v[i]->type && !strcmp(v[i]->type, CT[t].id);
		r = add(R_ACTION, CT[t].name, icon_named(CT[t].icon), A_TYPE);
		snprintf(r->arg, sizeof(r->arg), "%s", CT[t].id);
		snprintf(r->value, sizeof(r->value), "%d", c);
		type_sub(r, &CT[t], v, n);
	}
	for (int i = 0; i < n_indexes; i++) {
		char f[512], *w[16];
		int k;

		snprintf(f, sizeof(f), "%s", S_(IX[i].featured));
		k = split_words(f, w, 16);
		for (int j = 0; j < k; j++) {
			struct app *a = index_find(w[j], IX[i].src->id);

			if (!a || a->withdrawn)
				continue;
			if (!shown++)
				add(R_GROUP, "FEATURED", 0, A_NONE);
			app_row(a);
		}
	}
	if (n) {
		qsort(v, (size_t)n, sizeof(v[0]), by_updated);
		add(R_GROUP, "RECENTLY UPDATED", 0, A_NONE);
		for (int i = 0; i < n && i < 5; i++)
			app_row(v[i]);
	}
	add(R_GROUP, "YOUR BADGE", 0, A_NONE);
	r = add(R_ACTION, "My apps", &r_store, A_MYAPPS);
	snprintf(r->sub, sizeof(r->sub), "Installed, updates, repairs");
	r = add(R_ACTION, "Store settings", &r_chip, A_SETTINGS);
	snprintf(r->sub, sizeof(r->sub), "Sources, cache, storage");
}

static void build_type(const char *type)
{
	const struct ctype *t = ctype_find(type);
	int n;
	struct app **v = visible_apps(&n);

	snprintf(L.title, sizeof(L.title), "%s", t ? t->name : type);
	{			/* first, every app of the type in one list ("*") */
		int c = 0;
		struct row *r;

		for (int i = 0; i < n; i++)
			c += v[i]->type && !strcmp(v[i]->type, type);
		if (t && c) {
			r = add(R_ACTION, "All", &r_folder, A_CAT);
			snprintf(r->arg, sizeof(r->arg), "*");
			snprintf(r->value, sizeof(r->value), "%d", c);
			snprintf(r->sub, sizeof(r->sub), "Every one in one list");
		}
	}
	for (int k = 0; t && k <= t->n; k++) {
		/* categories.json's order; apps in a category it does not name go last */
		int c = 0;
		struct row *r;

		for (int i = 0; i < n; i++) {
			int known = 0;

			if (!v[i]->type || strcmp(v[i]->type, type))
				continue;
			for (int q = 0; q < t->n; q++)
				known |= v[i]->category && !strcmp(v[i]->category, t->c[q].id);
			c += k < t->n ? v[i]->category && !strcmp(v[i]->category, t->c[k].id) : !known;
		}
		if (!c)
			continue;
		r = add(R_ACTION, k < t->n ? t->c[k].name : "Other", icon_named(k < t->n ? t->c[k].icon : 0),
			A_CAT);
		snprintf(r->arg, sizeof(r->arg), "%s", k < t->n ? t->c[k].id : "");
		snprintf(r->value, sizeof(r->value), "%d", c);
		/* the second line: the apps in it */
		for (int i = 0; i < n; i++) {
			int known = 0;

			if (!v[i]->type || strcmp(v[i]->type, type))
				continue;
			for (int q = 0; q < t->n; q++)
				known |= v[i]->category && !strcmp(v[i]->category, t->c[q].id);
			if (k < t->n ? v[i]->category && !strcmp(v[i]->category, t->c[k].id) : !known)
				sub_add(r, v[i]->name);
		}
	}
	if (!L.n)
		add(R_INFO, "Nothing here yet", 0, A_NONE);
}

static void build_cat(const char *type, const char *catid, int sort)
{
	const struct ctype *t = ctype_find(type);
	int n, m = 0;
	struct app **v = visible_apps(&n), *sel[512];

	if (!strcmp(catid, "*")) {		/* "All apps", "All games" */
		snprintf(L.title, sizeof(L.title), "All %s", t ? t->name : type);
		for (char *p = L.title + 4; *p; p++)
			*p = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
	} else {
		snprintf(L.title, sizeof(L.title), "%s", catid[0] ? cat_name(type, catid) : "Other");
	}
	for (int i = 0; i < n; i++) {
		int known = 0;

		if (!v[i]->type || strcmp(v[i]->type, type))
			continue;
		for (int q = 0; t && q < t->n; q++)
			known |= v[i]->category && !strcmp(v[i]->category, t->c[q].id);
		if (!strcmp(catid, "*") || (catid[0] ? v[i]->category && !strcmp(v[i]->category, catid) : !known))
			sel[m++] = v[i];
	}
	qsort(sel, (size_t)m, sizeof(sel[0]), sort ? by_updated : by_name);
	add(R_GROUP, sort ? "RECENTLY UPDATED FIRST" : "BY NAME", 0, A_NONE);
	for (int i = 0; i < m; i++)
		app_row(sel[i]);
}

static void build_search(void)
{
	int n;
	struct app **v = visible_apps(&n);
	char q[48], *w[8];
	int nw;

	snprintf(L.title, sizeof(L.title), "Search");
	L.head_h = 8 + 56 + 8;
	if (!query[0])
		return;
	snprintf(q, sizeof(q), "%s", query);
	nw = split_words(q, w, 8);
	qsort(v, (size_t)n, sizeof(v[0]), by_name);
	for (int t = 0; t < n_ctypes; t++) {
		int shown = 0;

		for (int i = 0; i < n; i++) {
			struct app *a = v[i];
			int ok = a->type && !strcmp(a->type, CT[t].id);

			for (int k = 0; ok && k < nw; k++)
				ok = ci_has(a->id, w[k]) || ci_has(a->name, w[k]) || ci_has(a->summary, w[k]) ||
				     ci_has(a->tags, w[k]) || ci_has(a->author, w[k]) || ci_has(a->commands, w[k]);
			if (!ok)
				continue;
			if (!shown++) {
				char g[40];
				int j = 0;

				for (const char *p = CT[t].name; *p && j < 38; p++)
					g[j++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
				g[j] = 0;
				add(R_GROUP, g, 0, A_NONE);
			}
			app_row(a);
		}
	}
}

/* A key from an installed app's app.conf; out keeps @def when it is not there */
static void conf_value(const char *id, const char *key, const char *def, char *out, size_t n)
{
	char dir[256], p[512], *data;
	size_t kl = strlen(key);

	snprintf(out, n, "%s", def);
	for (int loc = 0; loc < 2; loc++) {
		if (!loc && !P.sd_present)
			continue;
		app_dir(dir, sizeof(dir), id, loc ? "flash" : "sd");
		if (!read_file(path_join(p, sizeof(p), dir, "app.conf"), &data, 0, 8192)) {
			for (char *l = data; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0)
				if (!strncmp(l, key, kl) && l[kl] == '=') {
					l += kl + 1;
					if (*l == '"')
						l++;
					snprintf(out, n, "%.*s", (int)strcspn(l, "\"\n"), l);
					break;
				}
			free(data);
			return;
		}
	}
}

/* app.conf's NAME of an app the store did not install */
static void conf_name(const char *id, char *out, size_t n)
{
	conf_value(id, "NAME", id, out, n);
}

static void build_myapps(void)
{
	char ids[64][33], other[64][33], found[64][33];
	int n = reg_list(ids, 64), no = legacy_list(other, 64, 0), nf = legacy_list(found, 64, 1);
	static const enum state order[] = { ST_UPDATE, ST_MISSING, ST_DAMAGED };
	static const char *const groups[] = { "UPDATES", "NOT ON THIS CARD", "NEEDS REPAIR" };
	struct row *r;

	snprintf(L.title, sizeof(L.title), "My apps");
	for (int g = 0; g < 3; g++) {
		int shown = 0;

		for (int i = 0; i < n; i++) {
			struct reg rg;
			struct app *a;
			enum state s = app_state(ids[i], &rg, &a);

			if (s != order[g])
				continue;
			if (!shown++) {
				add(R_GROUP, groups[g], 0, A_NONE);
				if (g == 0)
					add(R_ACTION, "Update all", &r_refresh, A_UPDATE_ALL);
				if (g == 1)
					add(R_ACTION, "Reinstall all", &r_refresh, A_REINSTALL_ALL);
			}
			r = add(R_ACTION, rg.name[0] ? rg.name : ids[i], app_icon(a), A_APP);
			snprintf(r->arg, sizeof(r->arg), "%s", ids[i]);
			state_value(r, s, a, &rg);
		}
	}
	for (int i = 0; i < nf; i++) {
		if (!i)
			add(R_GROUP, "FOUND ON THIS CARD", 0, A_NONE);
		r = add(R_ACTION, found[i], &r_sdcard, A_APP);
		conf_name(found[i], r->label, sizeof(r->label));
		snprintf(r->arg, sizeof(r->arg), "%s", found[i]);
		snprintf(r->sub, sizeof(r->sub), "badge/apps/%s", found[i]);
		snprintf(r->value, sizeof(r->value), "Adopt or delete");
	}
	{
		int shown = 0;

		for (int i = 0; i < n; i++) {
			struct reg rg;
			struct app *a;
			enum state s = app_state(ids[i], &rg, &a);

			if (s != ST_INSTALLED && s != ST_WITHDRAWN && s != ST_ELSEWHERE)
				continue;
			if (!shown++)
				add(R_GROUP, "INSTALLED", 0, A_NONE);
			r = add(R_ACTION, rg.name[0] ? rg.name : ids[i], app_icon(a), A_APP);
			snprintf(r->arg, sizeof(r->arg), "%s", ids[i]);
			if (a)
				snprintf(r->sub, sizeof(r->sub), "%s", S_(a->summary));
			snprintf(r->value, sizeof(r->value), "%s%s", rg.version,
				 s == ST_WITHDRAWN ? DOT_S "withdrawn" : !strcmp(rg.location, "flash") ? DOT_S "flash" : "");
			if (s == ST_WITHDRAWN)
				r->value_c = WB_COLOR_WARN;
		}
	}
	{
		int shown = 0;

		for (int i = 0; i < no; i++) {
			if (!strcmp(other[i], "store"))
				continue;	/* this program, put there by hand */
			if (!shown++)
				add(R_GROUP, "OTHER APPS", 0, A_NONE);
			r = add(R_ACTION, other[i], &r_folder, A_APP);
			conf_name(other[i], r->label, sizeof(r->label));
			snprintf(r->arg, sizeof(r->arg), "%s", other[i]);
			snprintf(r->value, sizeof(r->value), "Installed by hand");
			r->value_c = WB_COLOR_OK;
		}
	}
	{
		FILE *nul = fopen("/dev/null", "w");
		int c = nul ? cleanup_list(nul, 0) : 0;

		if (nul)
			fclose(nul);
		if (c) {
			add(R_GROUP, "PENDING CLEANUPS", 0, A_NONE);
			r = add(R_INFO, "Apps removed from another card", &r_trash, A_NONE);
			snprintf(r->value, sizeof(r->value), "%d to delete there", c);
		}
	}
	if (!L.n)
		add(R_INFO, "No apps yet", 0, A_NONE);
}

static void keep_path(char *out, size_t n)
{
	snprintf(out, n, "%s/store/keep-packages", P.whyos);
}

static long long cache_size(void)
{
	return (dir_size(P.cache_dir, 0) > 0 ? dir_size(P.cache_dir, 0) : 0) +
	       (P.sd_present && dir_size(P.dl_dir, 0) > 0 ? dir_size(P.dl_dir, 0) : 0);
}

static void build_settings(void)
{
	struct row *r;
	char p[512];
	struct stat st;

	snprintf(L.title, sizeof(L.title), "Store settings");
	add(R_GROUP, "SOURCES", 0, A_NONE);
	for (int i = 0; i < n_sources; i++) {
		int apps = -1;

		for (int k = 0; k < n_indexes; k++)
			if (IX[k].src == &S[i])
				apps = IX[k].n;
		r = add(R_ACTION, S[i].id, &r_globe, A_SOURCE);
		snprintf(r->arg, sizeof(r->arg), "%s", S[i].id);
		snprintf(r->sub, sizeof(r->sub), "%s", S[i].url);
		if (!S[i].enabled)
			snprintf(r->value, sizeof(r->value), "Off");
		else if (apps >= 0)
			snprintf(r->value, sizeof(r->value), "%d apps", apps);
		else
			snprintf(r->value, sizeof(r->value), "Not fetched yet");
	}
	r = add(R_ACTION, "Refresh now", &r_refresh, A_REFRESH);
	snprintf(p, sizeof(p), "%s/%s/index.json", P.cache_dir, S[0].id);
	if (!stat(p, &st)) {
		struct tm tm;
		time_t t = ui_time(st.st_mtime);

		localtime_r(&t, &tm);
		snprintf(r->value, sizeof(r->value), "Fetched %02d:%02d", tm.tm_hour, tm.tm_min);
	}
	add(R_GROUP, "STORAGE", 0, A_NONE);
	r = add(R_TOGGLE, "Keep downloaded packages", &r_drive, A_KEEP);
	r->on = keep_packages;
	r = add(R_ACTION, "Clear cache", &r_trash, A_CLEAR);
	human(r->value, sizeof(r->value), cache_size());
	add(R_ACTION, "Storage per app", &r_sdcard, A_STORAGE);
	add(R_GROUP, "ABOUT", 0, A_NONE);
	r = add(R_INFO, "Store version", &r_info, A_NONE);
	snprintf(r->value, sizeof(r->value), "%s" DOT_S "WhyOS API %d", STORE_VERSION, P.api);
}

static void build_source(const char *id)
{
	struct source *s = source_find(id);
	struct row *r;

	snprintf(L.title, sizeof(L.title), "%s", id);
	if (!s)
		return;
	r = add(R_TOGGLE, "Enabled", &r_check, A_SRC_ON);
	r->on = s->enabled;
	r = add(R_ACTION, "Address", &r_globe, A_MIRROR);
	snprintf(r->value, sizeof(r->value), "%s", s->url);
	for (int k = 0; k < n_indexes; k++)
		if (IX[k].src == s) {
			r = add(R_INFO, "Index", &r_info, A_NONE);
			snprintf(r->value, sizeof(r->value), "%d apps" DOT_S "serial %d", IX[k].n, IX[k].serial);
			r = add(R_INFO, "Published", &r_refresh, A_NONE);
			snprintf(r->value, sizeof(r->value), "%.16s UTC", S_(IX[k].generated));
			for (char *c = r->value; *c; c++)
				if (*c == 'T')
					*c = ' ';
		}
}

static void build_storage(void)
{
	char ids[64][33];
	int n = reg_list(ids, 64);

	snprintf(L.title, sizeof(L.title), "Storage per app");
	add(R_GROUP, "APP" DOT_S "ITS DATA", 0, A_NONE);
	for (int i = 0; i < n; i++) {
		struct reg rg;
		char d[256], a[24], b[24];
		long long as, ds;
		struct row *r;

		if (reg_read(ids[i], &rg))
			continue;
		as = dir_size(app_dir(d, sizeof(d), ids[i], rg.location), 0);
		ds = P.sd_present ? dir_size(appdata_dir(d, sizeof(d), ids[i]), 0) : -1;
		human(a, sizeof(a), as > 0 ? as : 0);
		human(b, sizeof(b), ds > 0 ? ds : 0);
		r = add(R_ACTION, rg.name[0] ? rg.name : ids[i], &r_drive, A_APP);
		snprintf(r->arg, sizeof(r->arg), "%s", ids[i]);
		snprintf(r->value, sizeof(r->value), "%s" DOT_S "%s", as >= 0 ? a : "not here", ds >= 0 ? b : "-");
	}
	if (!n)
		add(R_INFO, "No apps from the store", 0, A_NONE);
}

/* ---- the app page -------------------------------------------------------- */

static void app_page_free(void)
{
	if (AP.have_j)
		json_free(&AP.j);
	free(AP.data);
	picture_free(&AP.icon);
	picture_free(&AP.pic);
	picture_free(&AP.thumb);
	memset(&AP, 0, sizeof(AP));
	pic_gen++;
}

static void app_page_load(const char *id)
{
	char err[300];
	size_t len;

	app_page_free();
	snprintf(AP.id, sizeof(AP.id), "%s", id);
	AP.st = app_state(id, &AP.r, &AP.a);
	if (AP.a) {
		char sz[24];

		human(sz, sizeof(sz), AP.a->size);
		snprintf(AP.name, sizeof(AP.name), "%s", AP.a->name);
		snprintf(AP.author, sizeof(AP.author), "%s", S_(AP.a->author));
		snprintf(AP.meta, sizeof(AP.meta), "%s" DOT_S "%s%s", S_(AP.a->version), sz,
			 AP.a->terminal ? DOT_S "COMMAND-LINE" : "");
		snprintf(AP.summary, sizeof(AP.summary), "%s", S_(AP.a->summary));
		AP.terminal = AP.a->terminal;
	} else {
		conf_name(id, AP.name, sizeof(AP.name));
		snprintf(AP.author, sizeof(AP.author), "Not from the store");
		snprintf(AP.meta, sizeof(AP.meta), "%s", AP.r.version);
	}
	AP.ic = app_icon(AP.a);
	AP.system = is_system_app(id);
	if (AP.st != ST_ELSEWHERE && AP.st != ST_MISSING) {
		char c[16];

		conf_value(id, "CATEGORY", "", c, sizeof(c));
		AP.files_only = !strcmp(c, "none");
	}
	if (AP.a && !AP.a->withdrawn) {
		busy("APP STORE", "Loading\xe2\x80\xa6");
		if (details_get(AP.a, &AP.data, &len, err, sizeof(err)))
			net_notice(err);
		else if (!json_parse(&AP.j, AP.data, len))
			AP.have_j = 1;
		O.kind = OV_NONE;
	}
	if (AP.have_j)
		AP.n_shots = json_len(&AP.j, json_get(&AP.j, 0, "screenshots"));
	if (AP.have_j) {
		char rel[256], path[512];

		if (!json_str(&AP.j, json_get(&AP.j, 0, "cover"), rel, sizeof(rel)) &&
		    !asset_get(AP.a->src, rel, path, sizeof(path), err, sizeof(err)) &&
		    qoi_decode(path, &AP.thumb) == 0 && (AP.thumb.w > THUMB || AP.thumb.h > THUMB))
			picture_free(&AP.thumb);
	}
	if (AP.a && AP.a->icon) {
		char path[512];

		if (!asset_get(AP.a->src, AP.a->icon, path, sizeof(path), err, sizeof(err)) &&
		    qoi_decode(path, &AP.icon) == 0 && (AP.icon.w > 96 || AP.icon.h > 96))
			picture_free(&AP.icon);
	}	pic_gen++;
}

static const char *jstr(int obj, const char *key, char *out, size_t n)
{
	if (!AP.have_j || json_str(&AP.j, json_path(&AP.j, obj, key), out, n))
		out[0] = 0;
	return out;
}

/* "Open", except for an app that only opens files (Picture viewer): started
 * without one it quits at once */
static struct row *add_open(void)
{
	return AP.files_only ? 0 : add(R_ACTION, "Open", &r_store, A_OPEN);
}

/* Remove or Delete; an app WhyOS brings (the store, Files, Write) stays:
 * the menu starts it, and only a new image would bring it back */
static void add_remove(const char *label, enum act act)
{
	struct row *r;

	if (!AP.system) {
		add(R_ACTION, label, &r_trash, act);
		return;
	}
	r = add(R_INFO, "Comes with WhyOS", &r_info, A_NONE);
	snprintf(r->value, sizeof(r->value), "Not removable");
}

static void build_app(void)
{
	struct app *a = AP.a;
	struct row *r;
	char sz[24], b[64];
	int cmds = AP.have_j ? json_get(&AP.j, 0, "commands") : -1;
	int vers = AP.have_j ? json_get(&AP.j, 0, "versions") : -1;

	snprintf(L.title, sizeof(L.title), "%s", a ? a->name : AP.name);
	L.head_h = AP.thumb.rgb ? THUMB + 24 : 96 + 20 + (AP.summary[0] ? 2 * 26 : 0);
	if (a)
		human(sz, sizeof(sz), a->size);
	switch (AP.st) {
	case ST_NONE:
	case ST_ELSEWHERE:
		if (a) {
			r = add(R_ACTION, "Install", &r_check, A_INSTALL);
			snprintf(r->value, sizeof(r->value), "%s", sz);
		}
		break;
	case ST_UPDATE:
		r = add(R_ACTION, "Update", &r_refresh, A_UPDATE);
		snprintf(r->value, sizeof(r->value), "%s" ARROW_S "%s", AP.r.version, S_(a->version));
		r->value_c = WB_COLOR_YELLOW;
		/* fall through */
	case ST_INSTALLED:
		if (a && a->terminal && !(AP.have_j && json_get(&AP.j, 0, "launch") >= 0)) {
			r = add(R_INFO, "Run it from a terminal", &r_ip, A_NONE);
		} else {
			if ((r = add_open()))
				snprintf(r->value, sizeof(r->value), "%s", AP.r.version);
		}
		add_remove("Remove", A_REMOVE);
		break;
	case ST_MISSING:
		add(R_ACTION, "Reinstall on this card", &r_refresh, A_REINSTALL);
		if (AP.system) {
			add_remove(0, A_REMOVE);
			break;
		}
		r = add(R_ACTION, "Forget it", &r_trash, A_REMOVE);
		snprintf(r->value, sizeof(r->value), "Its data stays where it is");
		break;
	case ST_DAMAGED:
		add(R_ACTION, "Repair", &r_refresh, A_REPAIR);
		add_remove("Remove", A_REMOVE);
		break;
	case ST_UNREGISTERED:
		add(R_ACTION, "Adopt", &r_check, A_ADOPT);
		add_remove("Delete", A_DELETE);
		break;
	case ST_LEGACY:
		r = add(R_INFO, "Installed by hand", &r_check, A_NONE);
		snprintf(r->value, sizeof(r->value), "%s", a ? "Not updated by the store" : "Not from the store");
		add_open();
		if (a) {
			r = add(R_ACTION, "Replace with the store version", &r_refresh, A_REPLACE);
			snprintf(r->value, sizeof(r->value), "Gets updates");
		}
		add_remove("Delete", A_DELETE);
		break;
	case ST_WITHDRAWN:
		r = add(R_INFO, "Withdrawn from the store", &r_info, A_NONE);
		snprintf(r->value, sizeof(r->value), "%s", S_(a ? a->withdrawn : 0));
		r->value_c = WB_COLOR_WARN;
		add_open();
		add_remove("Remove", A_REMOVE);
		break;
	}
	if (!AP.have_j)
		return;
	add(R_GROUP, "ABOUT", 0, A_NONE);
	add(R_ACTION, "Description", &r_text, A_DESC);
	if (AP.n_shots) {
		r = add(R_ACTION, "Screenshots", &r_image, A_SHOTS);
		snprintf(r->value, sizeof(r->value), "%d", AP.n_shots);
	}
	for (int i = 0; i < json_len(&AP.j, cmds) && json_is_arr(&AP.j, cmds); i++) {
		int c = json_item(&AP.j, cmds, i);

		if (!i)
			add(R_GROUP, "COMMAND-LINE", 0, A_NONE);
		r = add(R_INFO, jstr(c, "name", b, sizeof(b)), 0, A_NONE);
		jstr(c, "summary", r->value, sizeof(r->value));
	}
	add(R_GROUP, "DETAILS", 0, A_NONE);
	r = add(R_ACTION, "Requirements and storage", &r_chip, A_DETAILS);
	snprintf(r->value, sizeof(r->value), "%d KB memory", a ? a->memory_kb : 0);
	r = add(R_ACTION, "Licences", &r_doc, A_LICENSES);
	jstr(0, "license", r->value, sizeof(r->value));
	r = add(R_ACTION, "Versions", &r_count, A_VERSIONS);
	snprintf(r->value, sizeof(r->value), "%d", json_len(&AP.j, vers));
	add(R_ACTION, "What changed", &r_text, A_CHANGES);
}

static void build_licenses(void)
{
	int b = json_get(&AP.j, 0, "bundled");
	struct row *r;
	char n[64], v[32];

	snprintf(L.title, sizeof(L.title), "Licences");
	add(R_GROUP, "THE APP", 0, A_NONE);
	r = add(R_ACTION, AP.a ? AP.a->name : AP.id, &r_doc, A_LICENSE);
	jstr(0, "license", r->value, sizeof(r->value));
	jstr(0, "license_url", r->arg, sizeof(r->arg));
	if (!r->value[0]) {
		snprintf(r->value, sizeof(r->value), "Not open source");
		r->value_c = WB_COLOR_WARN;
	}
	for (int i = 0; i < json_len(&AP.j, b) && json_is_arr(&AP.j, b); i++) {
		int o = json_item(&AP.j, b, i);

		if (!i)
			add(R_GROUP, "BUNDLED", 0, A_NONE);
		jstr(o, "name", n, sizeof(n));
		jstr(o, "version", v, sizeof(v));
		r = add(R_ACTION, n, &r_doc, A_LICENSE);
		if (v[0])
			snprintf(r->label, sizeof(r->label), "%s %s", n, v);
		jstr(o, "license", r->value, sizeof(r->value));
		jstr(o, "license_url", r->arg, sizeof(r->arg));
		if (!r->value[0]) {
			snprintf(r->value, sizeof(r->value), "Not open source");
			r->value_c = WB_COLOR_WARN;
		}
	}
}

static void build_versions(void)
{
	int vs = json_get(&AP.j, 0, "versions");
	char b[32];

	snprintf(L.title, sizeof(L.title), "Versions");
	for (int i = json_len(&AP.j, vs) - 1; i >= 0 && json_is_arr(&AP.j, vs); i--) {
		int o = json_item(&AP.j, vs, i);
		struct row *r = add(R_ACTION, jstr(o, "version", b, sizeof(b)), &r_count, A_VERSION);

		snprintf(r->arg, sizeof(r->arg), "%s", b);
		if (AP.r.id[0] && !strcmp(b, AP.r.version)) {
			snprintf(r->value, sizeof(r->value), "Installed");
			r->value_c = WB_COLOR_OK;
		} else {
			jstr(o, "published", r->value, 11);
		}
	}
}

/* textbuf: the description, details or changes of the app page */
static void text_of(enum act a)
{
	char *p = textbuf, *end = textbuf + sizeof(textbuf) - 256, b[600];

	textbuf[0] = 0;
	if (a == A_DESC) {
		snprintf(text_title, sizeof(text_title), "Description");
		jstr(0, "description", textbuf, sizeof(textbuf));
		if (!textbuf[0])
			snprintf(textbuf, sizeof(textbuf), "%s", AP.a ? S_(AP.a->summary) : "");
		if (jstr(0, "homepage", b, sizeof(b))[0])
			p = cat(cat(cat(textbuf + strlen(textbuf), "\n\nHomepage: "), b), "");
	} else if (a == A_DETAILS) {
		int st = json_path(&AP.j, 0, "storage.data.user_folders"), perm = json_get(&AP.j, 0, "permissions");

		snprintf(text_title, sizeof(text_title), "Requirements");
		p += snprintf(p, (size_t)(end - p), "WhyOS API %d or newer.\n", AP.a ? AP.a->os_api : 1);
		if (AP.a && AP.a->memory_kb)
			p += snprintf(p, (size_t)(end - p), "About %d KB of memory while it runs.\n", AP.a->memory_kb);
		if (AP.a && AP.a->features && AP.a->features[0])
			p += snprintf(p, (size_t)(end - p), "Uses: %s.\n", AP.a->features);
		if (AP.a && AP.a->requires_apps && AP.a->requires_apps[0])
			p += snprintf(p, (size_t)(end - p), "Needs the apps: %s (installed with it).\n",
				      AP.a->requires_apps);
		for (int i = 0; i < json_len(&AP.j, perm) && json_is_arr(&AP.j, perm) && p < end; i++) {
			if (!i)
				p += snprintf(p, (size_t)(end - p), "\nPermissions:\n");
			if (!json_str(&AP.j, json_item(&AP.j, perm, i), b, sizeof(b)))
				p += snprintf(p, (size_t)(end - p), "- %s\n", b);
		}
		p += snprintf(p, (size_t)(end - p), "\nStorage:\n");
		if (AP.a) {
			char s1[24], s2[24];

			human(s1, sizeof(s1), AP.a->installed_size);
			human(s2, sizeof(s2), AP.a->size);
			p += snprintf(p, (size_t)(end - p), "%s installed (a %s download), %s.\n", s1, s2,
				      AP.a->install_location && !strcmp(AP.a->install_location, "flash") ?
				      "in the badge's flash" : "on the SD card in badge/apps/" );
		}
		p += snprintf(p, (size_t)(end - p), "Its data: badge/appdata/%s on the card.\n", AP.id);
		for (int i = 0; i < json_len(&AP.j, st) && json_is_arr(&AP.j, st) && p < end; i++)
			if (!json_str(&AP.j, json_item(&AP.j, st, i), b, sizeof(b)))
				p += snprintf(p, (size_t)(end - p), "Put your files in badge/appdata/%s/%s.\n", AP.id, b);
		if (jstr(0, "source", b, sizeof(b))[0])
			p += snprintf(p, (size_t)(end - p), "\nSource: %s\n", b);
	} else if (a == A_CHANGES) {
		int vs = json_get(&AP.j, 0, "versions");

		snprintf(text_title, sizeof(text_title), "What changed");
		for (int i = json_len(&AP.j, vs) - 1; i >= 0 && json_is_arr(&AP.j, vs) && p < end; i--) {
			int o = json_item(&AP.j, vs, i);
			char v[32], d[32];

			jstr(o, "version", v, sizeof(v));
			jstr(o, "published", d, 11);
			jstr(o, "changelog", b, sizeof(b));
			p += snprintf(p, (size_t)(end - p), "%s, %s\n%s\n\n", v, d, b[0] ? b : "No notes.");
		}
	}
	(void)p;
}

/* a licence text (license_url, relative to the source) into textbuf */
static int license_text(const char *rel, const char *title)
{
	char path[512], err[300], *data;
	size_t len;

	snprintf(text_title, sizeof(text_title), "%s", title);
	if (!rel[0] || !AP.a) {
		snprintf(textbuf, sizeof(textbuf), "There is no licence text for this.");
		return 0;
	}
	busy("LICENCES", "Loading\xe2\x80\xa6");
	if (asset_get(AP.a->src, rel, path, sizeof(path), err, sizeof(err)) ||
	    read_file(path, &data, &len, sizeof(textbuf) - 1)) {
		O.kind = OV_NONE;
		net_notice(err);
		return -1;
	}
	O.kind = OV_NONE;
	snprintf(textbuf, sizeof(textbuf), "%s", data);
	free(data);
	return 0;
}

static void shot_load(void)
{
	int s = json_get(&AP.j, 0, "screenshots");
	char rel[256], path[512], err[300];

	picture_free(&AP.pic);
	pic_gen++;
	notice[0] = 0;
	if (json_str(&AP.j, json_item(&AP.j, s, AP.shot), rel, sizeof(rel)))
		return;
	present();	/* "Loading..." */
	if (asset_get(AP.a->src, rel, path, sizeof(path), err, sizeof(err)))
		net_notice(err);
	else if (qoi_decode(path, &AP.pic) || AP.pic.w > 480 || AP.pic.h > 480) {
		picture_free(&AP.pic);
		say_notice(1, "The screenshot is damaged");
	}
	pic_gen++;
}

/* ---- navigation ---------------------------------------------------------- */

/* The eyebrow: "APP" on the first screen, else the screen this one opens
 * from ("APP STORE ›", "HELLO ›") */
static void crumb(void)
{
	const char *t = depth > 1 ? stack[depth - 2].title : "";
	int j = 0;

	if (!t[0]) {
		snprintf(L.eyebrow, sizeof(L.eyebrow), "APP");
		return;
	}
	for (; *t && j < (int)sizeof(L.eyebrow) - 5; t++)
		L.eyebrow[j++] = (char)(*t >= 'a' && *t <= 'z' ? *t - 32 : *t);
	cat(L.eyebrow + j, " \xe2\x80\xba");
}

static void rebuild(void)
{
	struct frame *f = TOP;
	int keep = f->focus;

	L.n = 0;
	L.head_h = 0;
	L.title[0] = 0;
	switch (f->v) {
	case V_DISCOVER: build_discover(); break;
	case V_TYPE: build_type(f->arg); break;
	case V_CAT: build_cat(f->arg, f->arg2, f->sort); break;
	case V_SEARCH: build_search(); break;
	case V_APP: build_app(); break;
	case V_MYAPPS: build_myapps(); break;
	case V_SETTINGS: build_settings(); break;
	case V_SOURCE: build_source(f->arg); break;
	case V_STORAGE: build_storage(); break;
	case V_LICENSES: build_licenses(); break;
	case V_VERSIONS: build_versions(); break;
	case V_TEXT: case V_SHOTS:
		snprintf(L.title, sizeof(L.title), "%s", f->v == V_TEXT ? text_title : "Screenshots");
		break;
	}
	snprintf(f->title, sizeof(f->title), "%s", L.title);
	crumb();
	if (f->v == V_TEXT || f->v == V_SHOTS)
		return;
	f->focus = keep;
	if (f->focus >= L.n || f->focus < 0 || !focusable(&L.rows[f->focus])) {
		f->focus = -1;
		for (int i = 0; i < L.n && f->focus < 0; i++)
			if (focusable(&L.rows[i]))
				f->focus = i;
	}
}

static void push(enum view v, const char *arg, const char *arg2)
{
	struct frame *f;

	if (depth >= ARRAY_N(stack))
		return;
	f = &stack[depth++];
	memset(f, 0, sizeof(*f));
	f->v = v;
	snprintf(f->arg, sizeof(f->arg), "%s", S_(arg));
	snprintf(f->arg2, sizeof(f->arg2), "%s", S_(arg2));
	notice[0] = 0;
	if (v == V_APP)
		app_page_load(f->arg);
	if (v == V_TEXT)
		text_layout();
	rebuild();
}

static void pop(void)
{
	if (depth > 1)
		depth--;
	notice[0] = 0;
	/* back on an app page from its sub-screens: its data is still loaded */
	if (TOP->v == V_APP && strcmp(AP.id, TOP->arg))
		app_page_load(TOP->arg);
	rebuild();
}

/* ---- actions --------------------------------------------------------------- */

static struct opts O_ui;

static void after_op(int ret, const char *ok_text)
{
	O.kind = OV_NONE;
	if (cancelled) {
		say_notice(1, "Cancelled: nothing was changed");
	} else if (!ret && ok_text) {
		say_notice(0, ok_text);
	} else if (ret && !notice_bad) {
		say_notice(1, "That did not work");
	}
	cancelled = 0;
	index_load_all();
	if (TOP->v == V_APP) {
		char keep[200];
		int bad = notice_bad;

		snprintf(keep, sizeof(keep), "%s", notice);
		app_page_load(TOP->arg);
		snprintf(notice, sizeof(notice), "%s", keep);
		notice_bad = bad;
	}
	rebuild();
}

static int run_install(const char *spec, const char *verb, const char *name)
{
	int r;

	notice[0] = 0;
	notice_bad = 0;
	cancelled = 0;
	O_ui.keep = keep_packages;
	O_ui.progress = progress_cb;
	O_ui.ask = ask_cb;
	O_ui.ask_arg = (void *)name;
	snprintf(O.eyebrow, sizeof(O.eyebrow), "%s", verb);
	snprintf(O.title, sizeof(O.title), "%s", name);
	O.done = O.total = 0;
	O.body[0] = 0;
	busy(verb, name);
	r = store_install(spec, &O_ui, 0);
	return r;
}

static void do_open(const char *id)
{
	char p[512];
	int fd;

	snprintf(p, sizeof(p), "%s/store-open", P.tmp);
	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd >= 0) {
		if (write(fd, id, strlen(id)) < 0)
			say_notice(1, "Cannot ask the menu to open it");
		close(fd);
	}
}

static void write_source_conf(const struct source *s, const char *url, int enabled)
{
	char p[512], b[600];
	int n;

	mkdir_p(P.user_sources);
	snprintf(p, sizeof(p), "%s/%s.conf", P.user_sources, s->id);
	n = snprintf(b, sizeof(b), "# written by the store's settings\nID=%s\n%s%s%sENABLED=%d\n", s->id,
		     url ? "URL=" : "", url ? url : "", url ? "\n" : "", enabled);
	if (write_atomic(p, b, (size_t)n))
		say_notice(1, "Cannot save the source");
	sources_load();
	index_load_all();
}

static int text_input(const char *eb, const char *title, const char *hint, char *buf, size_t n)
{
	O.kind = OV_INPUT;
	snprintf(O.eyebrow, sizeof(O.eyebrow), "%s", eb);
	snprintf(O.title, sizeof(O.title), "%s", title);
	snprintf(O.body, sizeof(O.body), "%s", hint);
	snprintf(O.input, sizeof(O.input), "%s", buf);
	for (;;) {
		int k;
		size_t l = strlen(O.input);
		char c;

		present();
		k = wait_key();
		if (k < 0 || k == KEY_ESC) {
			O.kind = OV_NONE;
			return -1;
		}
		if (k == KEY_ENTER) {
			snprintf(buf, n, "%s", O.input);
			O.kind = OV_NONE;
			return 0;
		}
		if (k == KEY_BACKSPACE && l)
			O.input[l - 1] = 0;
		else if ((c = key_char(k)) && l + 1 < sizeof(O.input)) {
			O.input[l] = c;
			O.input[l + 1] = 0;
		}
	}
}

static void activate(struct row *r)
{
	char q[200], name[64];
	int ret;

	/* a copy: installing reloads the index that AP.a points into */
	snprintf(name, sizeof(name), "%s", AP.name[0] ? AP.name : AP.id);

	switch (r->act) {
	case A_SEARCH:
		push(V_SEARCH, 0, 0);
		break;
	case A_TYPE:
		push(V_TYPE, r->arg, 0);
		break;
	case A_CAT:
		push(V_CAT, TOP->arg, r->arg);
		break;
	case A_APP:
		push(V_APP, r->arg, 0);
		break;
	case A_MYAPPS:
		push(V_MYAPPS, 0, 0);
		break;
	case A_SETTINGS:
		push(V_SETTINGS, 0, 0);
		break;
	case A_INSTALL:
	case A_REINSTALL:
	case A_REPLACE:
		ret = run_install(AP.id, r->act == A_INSTALL ? "INSTALLING" : "REINSTALLING", name);
		snprintf(q, sizeof(q), "%s is installed", name);
		after_op(ret, q);
		break;
	case A_UPDATE:
		ret = run_install(AP.id, "UPDATING", name);
		snprintf(q, sizeof(q), "%s is up to date", name);
		after_op(ret, q);
		break;
	case A_VERSION:
		snprintf(q, sizeof(q), "Install version %s?", r->arg);
		if (!ask("VERSIONS", q, "An older version may not read the data of a newer one.", "Install")) {
			rebuild();
			break;
		}
		snprintf(q, sizeof(q), "%s@%s", AP.id, r->arg);
		ret = run_install(q, "INSTALLING", name);
		snprintf(q, sizeof(q), "%s %s is installed", name, r->arg);
		pop();
		after_op(ret, q);
		break;
	case A_OPEN:
		do_open(AP.id);
		if (!notice_bad) {
			O.kind = OV_BUSY;	/* the caller ends us */
			depth = 0;
		}
		break;
	case A_REMOVE:
		snprintf(q, sizeof(q), "Remove %s?", name);
		if (ask("MY APPS", q, AP.st == ST_MISSING ?
			"The store forgets it. Its files stay on the card that has them." :
			"Its files and its data on the card are deleted.", "Remove")) {
			O_ui.ask = ask_cb;
			O_ui.ask_arg = (void *)name;
			O_ui.yes = 1;
			notice[0] = 0;
			notice_bad = 0;
			busy("MY APPS", "Removing\xe2\x80\xa6");
			ret = store_remove(AP.id, &O_ui);
			O_ui.yes = 0;
			snprintf(q, sizeof(q), "%s is removed", name);
			after_op(ret, q);
		}
		break;
	case A_DELETE:
		snprintf(q, sizeof(q), "Delete %s?", name);
		if (ask("MY APPS", q, "Its folder in badge/apps is deleted. Files it wrote elsewhere stay.",
			"Delete")) {
			notice[0] = 0;
			notice_bad = 0;
			ret = store_delete_legacy(AP.id);
			snprintf(q, sizeof(q), "%s is deleted", name);
			after_op(ret, q);
		}
		break;
	case A_ADOPT:
		notice[0] = 0;
		notice_bad = 0;
		ret = store_adopt(AP.id);
		snprintf(q, sizeof(q), "%s is now one of your apps", name);
		after_op(ret, q);
		break;
	case A_REPAIR:
		notice[0] = 0;
		notice_bad = 0;
		O_ui.progress = progress_cb;
		O_ui.ask = ask_cb;
		O_ui.ask_arg = (void *)name;
		busy("REPAIRING", name);
		ret = store_repair(AP.id, &O_ui);
		snprintf(q, sizeof(q), "%s is repaired", name);
		after_op(ret, q);
		break;
	case A_UPDATE_ALL:
	case A_REINSTALL_ALL: {
		char ids[64][33];
		int n = reg_list(ids, 64), done = 0, failed = 0;
		enum state want = r->act == A_UPDATE_ALL ? ST_UPDATE : ST_MISSING;

		for (int i = 0; i < n && !cancelled; i++) {
			struct reg rg;

			if (app_state(ids[i], &rg, 0) != want)
				continue;
			if (run_install(ids[i], want == ST_UPDATE ? "UPDATING" : "REINSTALLING",
					rg.name[0] ? rg.name : ids[i]))
				failed++;
			else
				done++;
		}
		if (failed || cancelled) {
			after_op(1, 0);
		} else {
			snprintf(q, sizeof(q), "%d app%s %s", done, done == 1 ? "" : "s",
				 want == ST_UPDATE ? "updated" : "reinstalled");
			after_op(0, q);
		}
		break;
	}
	case A_DESC:
	case A_DETAILS:
	case A_CHANGES:
		text_of(r->act);
		push(V_TEXT, 0, 0);
		break;
	case A_LICENSES:
		push(V_LICENSES, 0, 0);
		break;
	case A_LICENSE:
		if (!license_text(r->arg, r->label))
			push(V_TEXT, 0, 0);
		break;
	case A_VERSIONS:
		push(V_VERSIONS, 0, 0);
		break;
	case A_SHOTS:
		AP.shot = 0;
		push(V_SHOTS, 0, 0);
		shot_load();
		break;
	case A_REFRESH:
		busy("APP STORE", "Refreshing\xe2\x80\xa6");
		{
			char err[300];

			if (index_update(1, err, sizeof(err)))
				net_notice(err);
			else
				say_notice(0, "The store is up to date");
		}
		O.kind = OV_NONE;
		index_load_all();
		categories_load();
		rebuild();
		break;
	case A_SOURCE:
		push(V_SOURCE, r->arg, 0);
		break;
	case A_SRC_ON: {
		struct source *s = source_find(TOP->arg);

		if (s)
			write_source_conf(s, 0, !s->enabled);
		rebuild();
		break;
	}
	case A_MIRROR: {
		struct source *s = source_find(TOP->arg);
		char url[200];

		if (!s)
			break;
		snprintf(url, sizeof(url), "%s", s->url);
		if (text_input("SOURCES", "A mirror of this source",
			       "The same files under another https:// address", url, sizeof(url))) {
			rebuild();
			break;
		}
		if (strncmp(url, "https://", 8)) {
			say_notice(1, "A source needs an https:// address");
		} else {
			write_source_conf(s, url, s->enabled);
			say_notice(0, "Saved: refresh to use it");
		}
		rebuild();
		break;
	}
	case A_KEEP: {
		char p[512];

		keep_packages = !keep_packages;
		keep_path(p, sizeof(p));
		*strrchr(p, '/') = 0;
		mkdir_p(p);
		keep_path(p, sizeof(p));
		if (keep_packages && write_atomic(p, "1\n", 2)) {
			keep_packages = 0;
			say_notice(1, "Cannot save that setting");
		}
		if (!keep_packages)
			unlink(p);
		rebuild();
		break;
	}
	case A_CLEAR:
		if (ask("APP STORE", "Clear the cache?",
			"Pictures, details and downloaded packages are fetched again when needed.", "Clear")) {
			char keep[512], p[512];

			/* the indexes stay: the store works offline with them */
			for (int i = 0; i < n_sources; i++) {
				const char *sub[] = { "apps", "icons", "screens", "licenses" };

				for (int k = 0; k < ARRAY_N(sub); k++) {
					snprintf(p, sizeof(p), "%s/%s/%s", P.cache_dir, S[i].id, sub[k]);
					rm_rf(p);
				}
			}
			if (P.sd_present)
				rm_rf(P.dl_dir);
			snprintf(keep, sizeof(keep), "Cache cleared");
			say_notice(0, keep);
		}
		rebuild();
		break;
	case A_STORAGE:
		push(V_STORAGE, 0, 0);
		break;
	default:
		break;
	}
}

/* The circle key: the store's log, as apprun keeps it (its end). */
static void log_open(void)
{
	char path[256];
	int fd, n = 0;
	off_t size;

	snprintf(text_title, sizeof(text_title), "Log");
	snprintf(path, sizeof(path), "%s/app-store.log", P.tmp);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		size = lseek(fd, 0, SEEK_END);
		if (size > (off_t)sizeof(textbuf) - 1)
			lseek(fd, size - (off_t)sizeof(textbuf) + 1, SEEK_SET);
		else
			lseek(fd, 0, SEEK_SET);
		n = (int)read(fd, textbuf, sizeof(textbuf) - 1);
		close(fd);
	}
	textbuf[n > 0 ? n : 0] = 0;
	if (n <= 0)
		snprintf(textbuf, sizeof(textbuf), "No log yet.");
	push(V_TEXT, "log", 0);
	TOP->scroll = n_tl > text_rows() ? n_tl - text_rows() : 0;	/* the newest */
}

/* An overlay that waits for its own keys: help, or the sort menu. */
static void overlay_keys(enum ov kind)
{
	O.kind = kind;
	O.focus = kind == OV_MENU ? TOP->sort : 0;
	for (;;) {
		int k;

		present();
		k = wait_key();
		if (k < 0 || k == KEY_ESC || (kind == OV_HELP && k == KEY_SYM_HELP))
			break;
		if (kind == OV_MENU && (k == KEY_UP || k == KEY_DOWN)) {
			O.focus = !O.focus;
		} else if (kind == OV_MENU && k == KEY_ENTER) {
			TOP->sort = O.focus;
			rebuild();
			break;
		}
	}
	O.kind = OV_NONE;
}

static void key(int k)
{
	struct frame *f = TOP;

	/* the right three symbol keys, the same on every screen */
	if (k == KEY_SYM_HELP) {
		overlay_keys(OV_HELP);
		return;
	}
	if (k == KEY_SYM_LOG) {
		if (!(f->v == V_TEXT && !strcmp(f->arg, "log")))
			log_open();
		return;
	}
	if (k == KEY_SYM_SETTINGS) {
		if (f->v != V_SETTINGS)
			push(V_SETTINGS, 0, 0);
		return;
	}
	if (f->v == V_TEXT) {
		if (k == KEY_DOWN && f->scroll + text_rows() < n_tl)
			f->scroll++;
		else if (k == KEY_UP && f->scroll > 0)
			f->scroll--;
		else if (k == KEY_ESC)
			pop();
		return;
	}
	if (f->v == V_SHOTS) {
		if (k == KEY_ESC) {
			picture_free(&AP.pic);
			pic_gen++;
			pop();
		} else if ((k == KEY_RIGHT || k == KEY_DOWN) && AP.shot + 1 < AP.n_shots) {
			AP.shot++;
			shot_load();
		} else if ((k == KEY_LEFT || k == KEY_UP) && AP.shot > 0) {
			AP.shot--;
			shot_load();
		}
		return;
	}
	if (k == KEY_UP || k == KEY_DOWN) {
		for (int i = f->focus + (k == KEY_DOWN ? 1 : -1); i >= 0 && i < L.n; i += k == KEY_DOWN ? 1 : -1)
			if (focusable(&L.rows[i])) {
				f->focus = i;
				break;
			}
		return;
	}
	if (k == KEY_ESC) {
		if (f->v == V_SEARCH && query[0]) {	/* typed text first (6.7) */
			query[0] = 0;
			f->focus = -1;
			rebuild();
		} else if (depth == 1) {
			depth = 0;	/* close the store */
		} else {
			pop();
		}
		return;
	}
	if (k == KEY_ENTER) {	/* a toggle too: the arrows never change one */
		if (f->focus >= 0)
			activate(&L.rows[f->focus]);
		return;
	}
	if (f->v == V_CAT && k == KEY_F1) {
		overlay_keys(OV_MENU);
		return;
	}
	if (f->v == V_SEARCH) {
		size_t l = strlen(query);
		char c;

		if (k == KEY_BACKSPACE && l)
			query[l - 1] = 0;
		else if ((c = key_char(k)) && l + 1 < sizeof(query) && (c != ' ' || l))
			query[l] = c, query[l + 1] = 0;
		else
			return;
		f->focus = -1;
		rebuild();
	}
}

int ui_main(int argc, char **argv)
{
	char err[300], p[512];
	int my_apps = argc > 1 && !strcmp(argv[1], "--my-apps");
	long last_status = 0;

	(void)argv;
	paths_init();
	say_hook = on_say;
#ifndef STORE_UI_HOST
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd < 0) {
		say_hook = 0;
		warn("no panel (/dev/fb0): use the commands (store --help)");
		return 2;
	}
#endif
	input_open();
	keep_path(p, sizeof(p));
	keep_packages = is_file(p);
	status_read();
	sources_load();
	push(V_DISCOVER, 0, 0);	/* drawn under "Loading" */
	busy("APP STORE", "Loading the store\xe2\x80\xa6");
	store_maintain();
	if (index_update(0, err, sizeof(err))) {
		index_load_all();
		if (!n_indexes)
			net_notice(err);
	}
	index_load_all();
	categories_load();
	O.kind = OV_NONE;
	{
		char keep[200];
		int bad = notice_bad;

		snprintf(keep, sizeof(keep), "%s", notice);
		if (my_apps) {
			stack[0].v = V_MYAPPS;
			stack[0].focus = 0;
		}
		rebuild();
		snprintf(notice, sizeof(notice), "%s", bad ? keep : "");
		notice_bad = bad;
	}
	while (depth > 0) {
		int k;
		long t;

		present();
		do {
			k = next_key(250);
			t = now_ms();
			if (!k && wb_scene_turned()) {
				present();
			} else if (t - last_status >= 5000) {
				last_status = t;
				if (status_read() && !k)
					present();
			}
		} while (k == 0);
		if (k < 0)
			break;
		key(k);
	}
	app_page_free();
	return 0;
}

#endif
