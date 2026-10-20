// SPDX-License-Identifier: MIT
/*
 * snippets — texts kept on the badge that it types on a computer, as a USB
 * keyboard on the bottom port (J8; docs/USB.md, "Device mode"). Drawn like
 * the menu (wbdraw.h and the design's fonts), as an app from the store
 * (sdapps/README.md).
 *
 * A list of snippets, each a name and a text of up to TEXT_MAX bytes (new
 * lines and tabs included). Enter on one types it: every character becomes
 * a key press and release on /dev/hidg0, the boot keyboard usbd sets up,
 * as long as usbd says a computer has configured it (/var/run/usb.state).
 * The menu itself is not running then, so its keyboard mode cannot type
 * over it.
 *
 * A USB keyboard sends keys, not characters: what comes out depends on the
 * computer's keyboard layout. The settings say which one it uses: US
 * (ASCII only), US International (its dead keys ' " ` ~ ^ get a space
 * after them; accents and € through them and AltGr), or US on a Mac
 * (accents and € through Option). A character the layout cannot type is
 * skipped, and the notice says how many were.
 *
 * Kept in APP_CONFIG (flash: it survives a new card), as snippets.txt: one
 * line per snippet, "name<TAB>text", with \n, \t and \\ for a new line, a
 * tab and a backslash. Lines starting with # are comments. The file may be
 * edited elsewhere (over SSH); accented letters only come in that way, as
 * the badge's keyboard has none, and the fonts draw them as "?".
 * The settings are in APP_CONFIG/settings.
 *
 * Keys (Docs/06-design-system.md 6.6): Enter types, the square opens the
 * snippet's options, the triangle adds one, the cross edits it; the
 * circle shows what was typed this session, the cloud the settings, the
 * diamond the help. Esc closes, or goes back one layer.
 *
 * The panel is drawn one band of BAND_H rows at a time, as calc does.
 * Built with -DSN_HOST, it takes keys from its first argument, writes the
 * screen as PPM to its second and the keyboard reports as hex lines to
 * $SN_HID: the host test (snippets/test_snippets.py).
 *
 * FLAT/NOMMU constraints (overlay-src/Makefile): no stdio.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>		/* rename() only: no stdio streams in the FLAT build */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"
#include "lora_state.h"
#include "wbstatus.h"
#include "usbkbd.h"

#ifndef SN_HOST
#include <poll.h>
#endif

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_TAB		15
#define KEY_ENTER	28
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_FN		0x1d0

/* ---- the snippets ------------------------------------------------------- */

#define SN_MAX		64
#define NM_MAX		40		/* bytes in a name */
#define TEXT_MAX	1000		/* bytes in a text */
#define LOG_MAX		20
#define SLOW_US		15000		/* after each report, at the slow speed */

struct snip { char name[NM_MAX + 1], text[TEXT_MAX + 1]; };

static struct snip S[SN_MAX];
static int n_snip;

enum { SC_LIST, SC_EDIT, SC_LOG, SC_SETTINGS };
enum { POP_NONE, POP_HELP, POP_MENU, POP_DELETE, POP_LAYOUT, POP_SPEED, POP_TYPING };
enum { LAY_US, LAY_INTL, LAY_MAC, N_LAY };
enum { NOTE_OK, NOTE_WARN, NOTE_BAD };
enum { M_TYPE, M_EDIT, M_UP, M_DOWN, M_DELETE };

static const char *const lay_names[N_LAY] = { "US", "US International", "US on a Mac" };
static const char *const lay_subs[N_LAY] = { "no accents", "dead keys", "Option for accents" };
static const char *const speed_names[2] = { "Fast", "Slow" };

static struct {
	int sc, back;			/* the screen; where the log and settings go back to */
	int focus;			/* the list's row */
	int pop, pf;			/* the pop-up on top, and its focus */
	int menu[6], n_menu;		/* the square key's items (M_...) */
	int set_focus;			/* the settings' row */
	int layout, slow;
	int computer;			/* usbd: a computer has configured the keyboard */
	char notice[120];
	int note;			/* NOTE_... */
	/* the editor */
	int edit;			/* the snippet edited, or -1 for a new one */
	int field;			/* 0 the name, 1 the text */
	char name[NM_MAX + 1], text[TEXT_MAX + 1];
	int name_cur, text_cur, text_top;
	const char *err;
	/* typing */
	int typed, total;
	char typing[NM_MAX + 40];
	char log[LOG_MAX][96];
	int n_log;
} C;

static struct { int shift, fn; } K;

/* ---- small text helpers ------------------------------------------------- */

static char *cat(char *d, const char *s)
{
	while (*d)
		d++;
	while ((*d = *s++))
		d++;
	return d;
}

static char *cat_int(char *d, int v)
{
	char t[12];
	int n = 0;

	while (*d)
		d++;
	if (v < 0) {
		*d++ = '-';
		v = -v;
	}
	do
		t[n++] = (char)('0' + v % 10);
	while ((v /= 10));
	while (n)
		*d++ = t[--n];
	*d = 0;
	return d;
}

/* "1 character", "2 characters" */
static char *cat_count(char *d, int n, const char *word)
{
	cat(cat(cat_int(d, n), " "), word);
	return n == 1 ? d + strlen(d) : cat(d, "s");
}

static int cp_next(const char *s, int i)
{
	if (!s[i])
		return i;
	for (i++; ((unsigned char)s[i] & 0xc0) == 0x80; i++)
		;
	return i;
}

static int cp_prev(const char *s, int i)
{
	if (i <= 0)
		return 0;
	for (i--; i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80; i--)
		;
	return i;
}

static int cp_count(const char *s)
{
	int n = 0;

	for (; *s; s++)
		n += ((unsigned char)*s & 0xc0) != 0x80;
	return n;
}

/* s cut to at most max bytes, without half a UTF-8 sequence at the end */
static void utf8_cut(char *s, int max)
{
	int n = (int)strlen(s), i;

	if (n > max)
		s[n = max] = 0;
	for (i = n; i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80; i--)
		;
	if (i > 0 && (unsigned char)s[i - 1] >= 0xc0) {
		int need = (unsigned char)s[i - 1] >= 0xf0 ? 4 : (unsigned char)s[i - 1] >= 0xe0 ? 3 : 2;

		if (n - (i - 1) < need)
			s[i - 1] = 0;
	}
}

/* The text on one line: new lines as " ↓ ", tabs as " → ". */
static void one_line(char *out, int size, const char *s)
{
	int n = 0;

	for (; *s && n < size - 6; s++) {
		const char *r = *s == '\n' ? " \xe2\x86\x93 " : *s == '\t' ? " \xe2\x86\x92 " : 0;

		if (r) {
			memcpy(out + n, r, 5);
			n += 5;
		} else {
			out[n++] = *s;
		}
	}
	out[n] = 0;
	utf8_cut(out, n);
}

/* What the list and the pop-ups call snippet i: its name, or its text. */
static void label_of(int i, char *out, int size)
{
	if (S[i].name[0]) {
		strncpy(out, S[i].name, (size_t)size - 1);
		out[size - 1] = 0;
		utf8_cut(out, size - 1);
	} else {
		one_line(out, size, S[i].text);
	}
}

/* ---- files -------------------------------------------------------------- */

static char dir[200], data_path[240], set_path[240];
static char filebuf[SN_MAX * (2 * (NM_MAX + TEXT_MAX) + 2) + 128];

static void mkdirs(char *p)
{
	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = 0;
		mkdir(p, 0700);
		*s = '/';
	}
	mkdir(p, 0700);
}

static void paths(void)
{
	const char *d = getenv("APP_CONFIG"), *home = getenv("HOME");

	if (d && *d && strlen(d) < sizeof(dir)) {
		strcpy(dir, d);
	} else {
		strncpy(dir, home && *home && strlen(home) < 120 ? home : "/root", sizeof(dir) - 40);
		cat(dir, "/.whyos/config/snippets");
	}
	mkdirs(dir);
	cat(cat(data_path, dir), "/snippets.txt");
	cat(cat(set_path, dir), "/settings");
}

static int read_file(const char *path, char *buf, int size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n = 0, r;

	if (fd < 0)
		return -1;
	while (n < size - 1 && (r = (int)read(fd, buf + n, (size_t)(size - 1 - n))) > 0)
		n += r;
	close(fd);
	buf[n] = 0;
	return n;
}

/* the whole of buf, through path.new and a rename */
static int write_file(const char *path, const char *buf, int n)
{
	char tmp[260];
	int fd, ok;

	tmp[0] = 0;
	cat(cat(tmp, path), ".new");
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	ok = write(fd, buf, (size_t)n) == n;
	ok &= fsync(fd) == 0;
	ok &= close(fd) == 0;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static void unescape(char *out, const char *s, int max)
{
	int n = 0;

	for (; *s && n < max; s++) {
		if (*s == '\\' && (s[1] == 'n' || s[1] == 't' || s[1] == '\\')) {
			s++;
			out[n++] = *s == 'n' ? '\n' : *s == 't' ? '\t' : '\\';
		} else {
			out[n++] = *s;
		}
	}
	out[n] = 0;
	utf8_cut(out, max);
}

static char *escape(char *o, const char *s)
{
	for (; *s; s++) {
		if (*s == '\n' || *s == '\t' || *s == '\\') {
			*o++ = '\\';
			*o++ = *s == '\n' ? 'n' : *s == '\t' ? 't' : '\\';
		} else {
			*o++ = *s;
		}
	}
	return o;
}

static void load(void)
{
	char *p = filebuf;

	if (read_file(data_path, filebuf, sizeof(filebuf)) < 0)
		return;
	while (*p && n_snip < SN_MAX) {
		char *e = strchr(p, '\n'), *tab;

		if (e)
			*e = 0;
		if (e && e > p && e[-1] == '\r')
			e[-1] = 0;
		if (*p && *p != '#') {
			struct snip *s = &S[n_snip];

			if ((tab = strchr(p, '\t'))) {
				*tab = 0;
				unescape(s->name, p, NM_MAX);
				unescape(s->text, tab + 1, TEXT_MAX);
			} else {
				s->name[0] = 0;
				unescape(s->text, p, TEXT_MAX);
			}
			if (s->text[0])
				n_snip++;
		}
		if (!e)
			break;
		p = e + 1;
	}
}

static int save(void)
{
	static const char head[] =
		"# Snippets for the badge to type: name<TAB>text per line;\n"
		"# \\n a new line, \\t a tab, \\\\ a backslash.\n";
	char *o = filebuf;

	memcpy(o, head, sizeof(head) - 1);
	o += sizeof(head) - 1;
	for (int i = 0; i < n_snip; i++) {
		o = escape(o, S[i].name);
		*o++ = '\t';
		o = escape(o, S[i].text);
		*o++ = '\n';
	}
	if (write_file(data_path, filebuf, (int)(o - filebuf)) < 0) {
		strcpy(C.notice, "Could not save the snippets");
		C.note = NOTE_BAD;
		return -1;
	}
	return 0;
}

static void load_settings(void)
{
	char b[128], *p;

	if (read_file(set_path, b, sizeof(b)) < 0)
		return;
	if ((p = strstr(b, "layout=")) && p[7] >= '0' && p[7] < '0' + N_LAY)
		C.layout = p[7] - '0';
	if ((p = strstr(b, "slow=")))
		C.slow = p[5] == '1';
}

static void save_settings(void)
{
	char b[32] = "layout=";

	cat_int(b, C.layout);
	cat(cat(b, "\nslow="), C.slow ? "1\n" : "0\n");
	write_file(set_path, b, (int)strlen(b));
}

/* the session's log, newest last */
static void log_add(const char *s)
{
	char t[96];
	time_t now = time(0);
	struct tm tm;

	localtime_r(&now, &tm);
	t[0] = (char)('0' + tm.tm_hour / 10);
	t[1] = (char)('0' + tm.tm_hour % 10);
	t[2] = ':';
	t[3] = (char)('0' + tm.tm_min / 10);
	t[4] = (char)('0' + tm.tm_min % 10);
	t[5] = ' ';
	t[6] = ' ';
	t[7] = 0;
	strncat(t, s, sizeof(t) - 8);
	if (C.n_log == LOG_MAX) {
		memmove(C.log, C.log + 1, sizeof(C.log[0]) * (LOG_MAX - 1));
		C.n_log--;
	}
	strcpy(C.log[C.n_log++], t);
}

/* ---- characters as key presses ------------------------------------------ */

/* US layout, as home's key_char. */
static char key_char(int code, int shift)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == 57)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo))
		return 0;
	return shift ? hi[code] : lo[code];
}

#define MOD_SHIFT	0x02
#define MOD_ALT		0x04		/* left Alt: Option on a Mac */
#define MOD_ALTGR	0x40		/* right Alt: AltGr on US International */

struct tap { unsigned char mods, key; };

/* An ASCII character (or new line, tab) on a US keyboard; 0 if none. */
static int ascii_tap(int c, struct tap *t)
{
	t->mods = 0;
	if (c == '\n' || c == '\t' || c == ' ') {
		t->key = c == '\n' ? 0x28 : c == '\t' ? 0x2b : 0x2c;
		return 1;
	}
	for (int code = 1; code < 64; code++)
		for (int sh = 0; sh < 2; sh++)
			if (c && key_char(code, sh) == c) {
				t->key = (unsigned char)kb_usage(code, 0);
				t->mods = sh ? MOD_SHIFT : 0;
				return 1;
			}
	return 0;
}

/*
 * Accented letters: the accent (acute, grave, diaeresis, circumflex, tilde)
 * and the letter. US International types the accent with a dead key
 * (' ` " ^ ~), a Mac with Option+e ` u i n; then the letter.
 */
static const struct { unsigned short cp; char acc, base; } accents[] = {
	{ 0xe1, 0, 'a' }, { 0xe9, 0, 'e' }, { 0xed, 0, 'i' }, { 0xf3, 0, 'o' }, { 0xfa, 0, 'u' },
	{ 0xfd, 0, 'y' }, { 0xc1, 0, 'A' }, { 0xc9, 0, 'E' }, { 0xcd, 0, 'I' }, { 0xd3, 0, 'O' },
	{ 0xda, 0, 'U' }, { 0xdd, 0, 'Y' },
	{ 0xe0, 1, 'a' }, { 0xe8, 1, 'e' }, { 0xec, 1, 'i' }, { 0xf2, 1, 'o' }, { 0xf9, 1, 'u' },
	{ 0xc0, 1, 'A' }, { 0xc8, 1, 'E' }, { 0xcc, 1, 'I' }, { 0xd2, 1, 'O' }, { 0xd9, 1, 'U' },
	{ 0xe4, 2, 'a' }, { 0xeb, 2, 'e' }, { 0xef, 2, 'i' }, { 0xf6, 2, 'o' }, { 0xfc, 2, 'u' },
	{ 0xff, 2, 'y' }, { 0xc4, 2, 'A' }, { 0xcb, 2, 'E' }, { 0xcf, 2, 'I' }, { 0xd6, 2, 'O' },
	{ 0xdc, 2, 'U' },
	{ 0xe2, 3, 'a' }, { 0xea, 3, 'e' }, { 0xee, 3, 'i' }, { 0xf4, 3, 'o' }, { 0xfb, 3, 'u' },
	{ 0xc2, 3, 'A' }, { 0xca, 3, 'E' }, { 0xce, 3, 'I' }, { 0xd4, 3, 'O' }, { 0xdb, 3, 'U' },
	{ 0xe3, 4, 'a' }, { 0xf5, 4, 'o' }, { 0xf1, 4, 'n' }, { 0xc3, 4, 'A' }, { 0xd5, 4, 'O' },
	{ 0xd1, 4, 'N' },
};

/* The key presses for character cp in the chosen layout: how many (0: it
 * cannot be typed). */
static int char_taps(unsigned cp, struct tap *t)
{
	int lay = C.layout;

	if (cp < 0x80) {
		if (!ascii_tap((int)cp, &t[0]))
			return 0;
		if (lay == LAY_INTL && strchr("'\"`~^", (int)cp)) {	/* a dead key alone */
			ascii_tap(' ', &t[1]);
			return 2;
		}
		return 1;
	}
	if (lay == LAY_US)
		return 0;
	if (cp == 0x20ac) {		/* € */
		ascii_tap(lay == LAY_MAC ? '2' : '5', &t[0]);
		t[0].mods = lay == LAY_MAC ? MOD_ALT | MOD_SHIFT : MOD_ALTGR;
		return 1;
	}
	if (cp == 0xe7 || cp == 0xc7) {	/* ç Ç */
		if (lay == LAY_MAC) {
			ascii_tap('c', &t[0]);
			t[0].mods = MOD_ALT | (cp == 0xc7 ? MOD_SHIFT : 0);
			return 1;
		}
		ascii_tap('\'', &t[0]);
		ascii_tap(cp == 0xc7 ? 'C' : 'c', &t[1]);
		return 2;
	}
	for (unsigned i = 0; i < sizeof(accents) / sizeof(accents[0]); i++) {
		if (accents[i].cp != cp)
			continue;
		if (lay == LAY_MAC) {
			ascii_tap("e`uin"[(int)accents[i].acc], &t[0]);
			t[0].mods = MOD_ALT;
		} else {
			ascii_tap("'`\"^~"[(int)accents[i].acc], &t[0]);
		}
		ascii_tap(accents[i].base, &t[1]);
		return 2;
	}
	return 0;
}

/* ---- layout ------------------------------------------------------------- */

#define TOP		(WB_STATUS_BAR_H + 20)	/* the eyebrow */
#define TITLE_Y		(TOP + 18 + 6)
#define BODY_Y		(TOP + 18 + 6 + 44)
#define BOTTOM		(H - WB_HINT_BAR_H - 8)
#define ROW_H		76
#define ROW_GAP		10
#define X0		WB_CONTENT_PAD
#define XW		(W - 2 * WB_CONTENT_PAD)

/* the editor */
#define NAME_LY		(BODY_Y + 16)		/* "NAME" */
#define NAME_Y		(NAME_LY + 24)		/* its field */
#define NAME_H		56
#define TEXT_LY		(NAME_Y + NAME_H + 22)
#define TEXT_Y		(TEXT_LY + 24)
#define LINE_H		28
#define VIS_LINES	11
#define TEXT_H		(16 * 2 + VIS_LINES * LINE_H)
#define TEXT_PAD	20
#define TEXT_W		(XW - 2 * TEXT_PAD - 16)	/* room for the ↓ of a new line */
#define COUNT_Y		(TEXT_Y + TEXT_H + 10)
#define TF		(&f_mono18)

/* ---- the status bar ----------------------------------------------------- */

static struct {
	char hhmm[6];
	int clock_ok, wifi_up /* bars, -1 none: wbstatus.h */, lora;
} U;

/* as home's: WhyOS left; LoRa, Wi-Fi and the clock right */
static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
	     text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
	status_right(U.hhmm, U.clock_ok, U.wifi_up, U.lora);
}

static void header(const char *eb, const char *title)
{
	char buf[96];

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	eyebrow(X0, TOP, eb);
	text(&f_ui36b, X0, TITLE_Y, wb_fit(&f_ui36b, title, XW - 220, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, -11);
}

/* The computer on J8, as a chip right of the title. */
static void computer_chip(void)
{
	const char *s = C.computer ? "COMPUTER" : "NO COMPUTER";
	uint32_t c = C.computer ? WB_COLOR_OK : WB_COLOR_TEXT_LOW;
	int w = text_w(&f_mono14, s, 0) + 24, x = W - X0 - w, y = TITLE_Y + 8;

	box(x, y, w, 28, 14, WB_COLOR_VOID, 255, C.computer ? WB_COLOR_OK : WB_COLOR_LINE, 1, 0);
	text(&f_mono14, x + 12, text_top(&f_mono14, y + 14), s, c, 0);
}

/* ---- the list ----------------------------------------------------------- */

static int list_top(void)
{
	return BODY_Y + (C.notice[0] ? 8 + 24 : 0) + 24;
}

static void draw_row(int i, int y, int on)
{
	char buf[128], line[160];
	int x = X0, lx = x + 20 + 26 + 16, rx = x + XW - 20;

	if (on)
		box_focus(x, y, XW, ROW_H, WB_RADIUS_MD, &GLOW_ROW);
	else
		box_plain(x, y, XW, ROW_H, WB_RADIUS_MD, WB_COLOR_SURFACE1);
	icon(&r_text, x + 20, y + ROW_H / 2 - 13, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
	label_of(i, line, sizeof(line));
	text(&f_ui22, lx, y + 12, wb_fit(&f_ui22, line, rx - lx, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	if (S[i].name[0]) {
		one_line(line, sizeof(line), S[i].text);
	} else {
		line[0] = 0;
		cat_count(line, cp_count(S[i].text), "character");
	}
	text(&f_mono14, lx, y + 46, wb_fit(&f_mono14, line, rx - lx, buf, sizeof(buf)),
	     WB_COLOR_TEXT_LOW, 0);
}

static void list_hints(void)
{
	struct hint h[5];
	int n = 0;

	if (n_snip) {
		h[n++] = (struct hint){ "Enter", "type", 0 };
		h[n++] = (struct hint){ "#1", "options", 0 };
	}
	h[n++] = (struct hint){ "#2", "new", 0 };
	if (n_snip)
		h[n++] = (struct hint){ "#3", "edit", 0 };
	h[n++] = (struct hint){ "Esc", "close", 1 };
	hint_bar(h, n);
}

static void draw_list(void)
{
	static const uint32_t note_c[3] = { WB_COLOR_OK, WB_COLOR_WARN, WB_COLOR_ERROR };
	int ys[SN_MAX], hs[SN_MAX], top = list_top(), scroll, cut, y = 0;
	char buf[128];

	header("APPS", "Snippets");
	computer_chip();
	if (C.notice[0])
		text(&f_ui18, X0, BODY_Y + 8, wb_fit(&f_ui18, C.notice, XW, buf, sizeof(buf)),
		     note_c[C.note], 0);
	if (!n_snip) {
		int ty = top + 8;

		text(&f_ui22, X0, ty, "No snippets yet.", WB_COLOR_TEXT_HI, 0);
		ty += 44;
		ty += 28 * text_wrap(&f_ui20, X0, ty, XW, 28,
				     "Save texts you type often: an address, a long command, "
				     "a greeting. Enter on one types it on the computer.",
				     WB_COLOR_TEXT_MID, 4, 1);
		ty += 24;
		eyebrow(X0, ty, "TO START");
		ty += 30;
		text(&f_ui20, X0, ty, "1. Connect the computer to the bottom USB-C port.", WB_COLOR_TEXT_MID, 0);
		ty += 30;
		text(&f_ui20, X0, ty, "2. Press the triangle key to add a snippet.", WB_COLOR_TEXT_MID, 0);
		ty += 30;
		text(&f_ui20, X0, ty, "3. Click where the text goes, then Enter here.", WB_COLOR_TEXT_MID, 0);
		list_hints();
		return;
	}
	for (int i = 0; i < n_snip; i++) {
		ys[i] = y;
		hs[i] = ROW_H;
		y += ROW_H + ROW_GAP;
	}
	scroll = list_scroll(ys, hs, n_snip, C.focus, C.focus, BOTTOM - top, &cut);
	clip_rows(scroll ? top : 0, y - ROW_GAP - scroll > BOTTOM - top ? top + cut : H);
	for (int i = 0; i < n_snip; i++) {
		int ry = top + ys[i] - scroll;

		if (ry + ROW_H <= top && scroll)
			continue;
		if (ry >= top + cut)
			break;
		draw_row(i, ry, i == C.focus);
	}
	clip_end();
	scrollbar(W - X0 + 18, top, BOTTOM - top, y - ROW_GAP, BOTTOM - top, scroll);
	list_hints();
}

/* ---- the editor --------------------------------------------------------- */

/* the text's lines as shown: where each starts (new lines and wrapping) */
static int lay_start[TEXT_MAX + 2], lay_n;

static int cp_w(const char *s, int a, int b)
{
	char t[8];

	if (s[a] == '\t')
		return text_w(TF, "\xe2\x86\x92", 0);
	if (s[a] == '\n')
		return 0;
	memcpy(t, s + a, (size_t)(b - a));
	t[b - a] = 0;
	return text_w(TF, t, 0);
}

/* Wrapped at the last space that fits, or inside a word longer than a line. */
static void layout(void)
{
	int x = 0, i = 0, sp = 0, xsp = 0;	/* after the line's last space, and its x */

	lay_n = 1;
	lay_start[0] = 0;
	while (C.text[i]) {
		int j = cp_next(C.text, i), w;

		if (C.text[i] == '\n') {
			lay_start[lay_n++] = j;
			x = sp = 0;
			i = j;
			continue;
		}
		w = cp_w(C.text, i, j);
		if (x + w > TEXT_W && x > 0) {
			if (sp > lay_start[lay_n - 1] && C.text[i] != ' ') {
				lay_start[lay_n++] = sp;
				x -= xsp;
			} else {
				lay_start[lay_n++] = i;
				x = 0;
			}
			sp = 0;
		}
		x += w;
		i = j;
		if (C.text[i - 1] == ' ' || C.text[i - 1] == '\t') {
			sp = i;
			xsp = x;
		}
	}
}

static int line_end(int l)
{
	return l + 1 < lay_n ? lay_start[l + 1] : (int)strlen(C.text);
}

static int line_of(int cur)
{
	int l = 0;

	while (l + 1 < lay_n && lay_start[l + 1] <= cur)
		l++;
	return l;
}

static int x_of(int cur)
{
	int x = 0;

	for (int i = lay_start[line_of(cur)]; i < cur; i = cp_next(C.text, i))
		x += cp_w(C.text, i, cp_next(C.text, i));
	return x;
}

/* keep the cursor's line in view */
static void text_scroll(void)
{
	int l;

	layout();
	l = line_of(C.text_cur);
	if (l < C.text_top)
		C.text_top = l;
	if (l >= C.text_top + VIS_LINES)
		C.text_top = l - VIS_LINES + 1;
}

/* Up (-1) and Down (1) in the text: a line, to the nearest place */
static void text_vert(int dir)
{
	int l, x, best, bx, e;

	layout();
	l = line_of(C.text_cur);
	if (dir < 0 && l == 0) {
		C.field = 0;
		return;
	}
	if (dir > 0 && l == lay_n - 1) {
		C.text_cur = (int)strlen(C.text);
		return;
	}
	x = x_of(C.text_cur);
	l += dir;
	e = line_end(l);
	if (e > lay_start[l] && C.text[cp_prev(C.text, e)] == '\n')
		e = cp_prev(C.text, e);
	best = lay_start[l];
	bx = 0;
	for (int i = lay_start[l], ix = 0; i <= e; ) {
		if (abs(ix - x) < abs(bx - x)) {
			best = i;
			bx = ix;
		}
		if (i == e)
			break;
		ix += cp_w(C.text, i, cp_next(C.text, i));
		i = cp_next(C.text, i);
	}
	C.text_cur = best;
}

static void draw_name(void)
{
	int fy = NAME_Y, fx = X0, max = XW - 40, from = 0, cx, on = C.field == 0;
	char t[NM_MAX + 1];

	if (on)
		box_focus(fx, fy, XW, NAME_H, WB_RADIUS_MD, &GLOW_ROW);
	else
		box_plain(fx, fy, XW, NAME_H, WB_RADIUS_MD, WB_COLOR_SURFACE1);
	/* drop characters off the front until the cursor shows */
	for (;;) {
		memcpy(t, C.name + from, (size_t)(C.name_cur - from));
		t[C.name_cur - from] = 0;
		cx = text_w(TF, t, 0);
		if (cx <= max || from >= C.name_cur)
			break;
		from = cp_next(C.name, from);
	}
	if (!C.name[0] && !on)
		text(&f_ui18, fx + 20, text_top(&f_ui18, fy + 28), "Optional: shown in the list",
		     WB_COLOR_TEXT_LOW, 0);
	text(TF, fx + 20, text_top(TF, fy + 28), C.name + from, WB_COLOR_TEXT_HI, 0);
	if (on)
		fill(fx + 20 + cx + 1, fy + 16, 2, 24, WB_COLOR_CYAN);
}

static void draw_text_field(void)
{
	int on = C.field == 1, n = cp_count(C.text);
	char t[48];

	if (on)
		box_focus(X0, TEXT_Y, XW, TEXT_H, WB_RADIUS_MD, &GLOW_ROW);
	else
		box_plain(X0, TEXT_Y, XW, TEXT_H, WB_RADIUS_MD, WB_COLOR_SURFACE1);
	layout();
	for (int l = C.text_top; l < lay_n && l < C.text_top + VIS_LINES; l++) {
		int x = X0 + TEXT_PAD, y = TEXT_Y + 16 + (l - C.text_top) * LINE_H;
		int ty = y + (LINE_H - (TF->ascent + TF->descent)) / 2, e = line_end(l);

		for (int i = lay_start[l]; i < e; ) {
			int j = cp_next(C.text, i);

			if (on && i == C.text_cur)
				fill(x, y + 2, 2, LINE_H - 4, WB_COLOR_CYAN);
			if (C.text[i] == '\t')
				text(TF, x, ty, "\xe2\x86\x92", WB_COLOR_TEXT_LOW, 0);
			else if (C.text[i] == '\n')
				text(TF, x + 2, ty, "\xe2\x86\x93", WB_COLOR_TEXT_LOW, 0);
			else {
				memcpy(t, C.text + i, (size_t)(j - i));
				t[j - i] = 0;
				text(TF, x, ty, t, WB_COLOR_TEXT_HI, 0);
			}
			x += cp_w(C.text, i, j);
			i = j;
		}
		if (on && e == C.text_cur && (l == lay_n - 1))
			fill(x, y + 2, 2, LINE_H - 4, WB_COLOR_CYAN);
	}
	if (!C.text[0] && !on)
		text(&f_ui18, X0 + TEXT_PAD, TEXT_Y + 16 + 4, "What the badge types", WB_COLOR_TEXT_LOW, 0);
	if (C.err)
		text(&f_ui18, X0, COUNT_Y, C.err, WB_COLOR_ERROR, 0);
	else if (on)
		text(&f_ui16, X0, COUNT_Y, "Shift+Enter: a new line.  Tab: a tab.", WB_COLOR_TEXT_LOW, 0);
	t[0] = 0;
	cat(cat_int(cat(cat_count(t, n, "character"), ", "), (int)strlen(C.text)), " of 1000 bytes");
	text(&f_mono13, X0 + XW - text_w(&f_mono13, t, 0), COUNT_Y + 2, t, WB_COLOR_TEXT_LOW, 0);
}

static void draw_edit(void)
{
	static const struct hint hints[] = { { "Enter", "save", 0 }, { "Esc", "cancel", 1 } };

	header("SNIPPETS", C.edit < 0 ? "New snippet" : "Edit snippet");
	text(&f_mono13, X0, NAME_LY, "NAME", WB_COLOR_TEXT_LOW, 21);
	text(&f_mono13, X0, TEXT_LY, "TEXT", WB_COLOR_TEXT_LOW, 21);
	draw_name();
	draw_text_field();
	hint_bar(hints, 2);
}

/* ---- log, settings, help, pop-ups --------------------------------------- */

static void draw_log(void)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int y = BODY_Y + 24;
	char buf[128];

	if (!C.n_log) {
		empty_screen("SNIPPETS", "Log", "No log yet. What is typed shows here until the app closes.");
		status_bar();
		return;
	}
	header("SNIPPETS", "Log");
	for (int i = C.n_log - 1; i >= 0 && y + 30 < BOTTOM; i--, y += 30)
		text(&f_mono14, X0, y, wb_fit(&f_mono14, C.log[i], XW, buf, sizeof(buf)),
		     i == C.n_log - 1 ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	hint_bar(hints, 1);
}

static void draw_settings(void)
{
	static const struct hint hints[] = { { "Enter", "change", 0 }, { "Esc", "back", 1 } };
	static const char *const labels[2] = { "Computer's layout", "Typing speed" };
	static const char *const notes[2] = {
		"The keyboard layout the computer uses: otherwise symbols come out wrong.",
		"Slow is for remote desktops and virtual machines that drop keys.",
	};
	int y = BODY_Y + 24;

	header("SNIPPETS", "Settings");
	for (int i = 0; i < 2; i++, y += 68 + 12 + 24 + 20) {
		const char *v = i ? speed_names[C.slow] : lay_names[C.layout];
		int cy = y + 34, rx = X0 + XW - 20 - 20;

		if (C.set_focus == i)
			box_focus(X0, y, XW, 68, WB_RADIUS_MD, &GLOW_ROW);
		else
			box_plain(X0, y, XW, 68, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		icon(&r_chevron, rx, cy - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 16 + text_w(&f_ui18, v, 0);
		text(&f_ui18, rx, text_top(&f_ui18, cy), v, C.set_focus == i ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
		text(&f_ui22, X0 + 20, text_top(&f_ui22, cy), labels[i], WB_COLOR_TEXT_HI, 0);
		text(&f_ui18, X0, y + 68 + 12, notes[i], WB_COLOR_TEXT_LOW, 0);
	}
	hint_bar(hints, 2);
}

static void draw_help(void)
{
	static const struct help_key list_keys[] = {
		{ "Enter", "Type the snippet on the computer" },
		{ "#1", "Options: type, edit, move, delete" },
		{ "#2", "A new snippet" },
		{ "#3", "Edit this snippet" },
		{ "Esc", "Close (also while typing: stop)" },
		{ "#4", "Log: what was typed" },
		{ "#5", "Settings: the computer's layout, speed" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key edit_keys[] = {
		{ "Enter", "Save" },
		{ "Shift+Enter", "A new line in the text" },
		{ "Tab", "In the text: a tab; in the name: to the text" },
		{ "\xe2\x86\x91\xe2\x86\x93", "A line up or down; between name and text" },
		{ "\xe2\x86\x90\xe2\x86\x92", "Move the cursor (with Fn: to the line's end)" },
		{ "Backspace", "Erase before the cursor (Fn: after it)" },
		{ "Esc", "Cancel: the snippet stays as it was" },
		{ "#6", "This help" },
	};
	static const struct help_key other_keys[] = {
		{ "Enter", "Change the setting" },
		{ "Esc", "Back" },
		{ "#4", "Log" },
		{ "#5", "Settings" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};

	if (C.sc == SC_EDIT)
		help_popup("Edit snippet", "The name is only for the list. The text is what the badge "
			   "types, new lines and tabs included.", edit_keys, 8);
	else if (C.sc == SC_LIST)
		help_popup("Snippets", "Connect a computer to the bottom USB-C port (J8), click where "
			   "the text should go, and press Enter on a snippet.", list_keys, 9);
	else
		help_popup(C.sc == SC_LOG ? "Log" : "Settings",
			   C.sc == SC_LOG ? "What was typed since the app started." :
			   "Kept on the badge, in flash.", other_keys, 6);
}

static void draw_pop(void)
{
	static const struct hint pick[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
	static const struct hint menu_h[] = { { "Enter", "do", 0 }, { "Esc", "cancel", 1 } };
	static const struct hint stop[] = { { "Esc", "stop", 1 } };
	static const char *const mlabel[] = { "Type on the computer", "Edit", "Move up", "Move down", "Delete" };
	struct menu_item it[6];
	char name[128];

	memset(it, 0, sizeof(it));
	switch (C.pop) {
	case POP_HELP:
		draw_help();
		break;
	case POP_MENU:
		label_of(C.focus, name, sizeof(name));
		for (int i = 0; i < C.n_menu; i++) {
			it[i].label = mlabel[C.menu[i]];
			it[i].danger = C.menu[i] == M_DELETE;
			it[i].sep = C.menu[i] == M_DELETE;
		}
		menu_popup("SNIPPET", name, 0, it, 0, C.n_menu, C.pf, -1, 0, 0, menu_h, 2);
		break;
	case POP_DELETE:
		label_of(C.focus, name, sizeof(name));
		it[0].label = "Keep it";
		it[1].label = "Delete";
		it[1].danger = it[1].sep = 1;
		menu_popup("DELETE THIS SNIPPET?", name, 0, it, 0, 2, C.pf, -1, 0, 0, menu_h, 2);
		break;
	case POP_LAYOUT:
		for (int i = 0; i < N_LAY; i++) {
			it[i].label = lay_names[i];
			it[i].sub = lay_subs[i];
		}
		menu_popup("SETTINGS", "Computer's layout", "3 options", it, 0, N_LAY, C.pf, C.layout,
			   0, 0, pick, 2);
		break;
	case POP_SPEED:
		it[0].label = speed_names[0];
		it[1].label = speed_names[1];
		it[1].sub = "for remote desktops";
		menu_popup("SETTINGS", "Typing speed", "2 options", it, 0, 2, C.pf, C.slow, 0, 0, pick, 2);
		break;
	case POP_TYPING: {
		int h = 32 + 18 + 6 + 36 + 24 + 12 + 20 + 26 + 32, y = WB_STATUS_BAR_H +
			(H - WB_STATUS_BAR_H - WB_HINT_BAR_H - h) / 2, x = POP_X + 32, w = POP_W - 64;
		char t[48];

		pop_frame(y, h);
		eyebrow(x, y + 32, "TYPING ON THE COMPUTER");
		text(&f_ui28b, x, y + 32 + 18 + 6 + 2, wb_fit(&f_ui28b, C.typing, w, name, sizeof(name)),
		     WB_COLOR_TEXT_HI, 0);
		y += 32 + 18 + 6 + 36 + 24;
		box(x, y, w, 12, 6, WB_COLOR_LINE, 255, 0, 0, 0);
		if (C.total && C.typed)
			box(x, y, w * C.typed / C.total < 12 ? 12 : w * C.typed / C.total, 12, 6,
			    WB_COLOR_CYAN, 255, 0, 0, 0);
		t[0] = 0;
		cat(cat_int(cat(cat_int(t, C.typed), " of "), C.total), " characters");
		text(&f_ui18, x, y + 12 + 20, t, WB_COLOR_TEXT_MID, 0);
		hint_bar(stop, 1);
		break;
	}
	}
}

static void draw_band(int y0, int y1)
{
	(void)y0;
	(void)y1;
	switch (C.sc) {
	case SC_LIST:
		draw_list();
		break;
	case SC_EDIT:
		draw_edit();
		break;
	case SC_LOG:
		draw_log();
		break;
	case SC_SETTINGS:
		draw_settings();
		break;
	}
	draw_pop();
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

#define BAND_H	40
static uint16_t band[W * BAND_H];
#ifdef SN_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void flush_band(int y0, int y1)
{
	fb = band - (long)y0 * W;
	fb_y0 = y0;
	fb_y1 = y1;
	draw_band(y0, y1);
#ifdef SN_HOST
	memcpy(screen + y0 * W, band, (size_t)(y1 - y0) * W * 2);
#else
	pwrite(fb_fd, band, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
}

static void flush_rows(int y0, int y1)
{
	for (int y = y0; y < y1; y += BAND_H)
		flush_band(y, y + BAND_H < y1 ? y + BAND_H : y1);
}

/* ---- the keyboard on the computer --------------------------------------- */

static int hid_fd = -1;
#ifndef SN_HOST
static int ev_fd = -1;
static volatile sig_atomic_t stop_sig;
#endif

/* /var/run/usb.state (usbd.c): has a computer configured the keyboard? */
static int computer_read(void)
{
#ifdef SN_HOST
	const char *c = getenv("SN_COMPUTER");

	return c && *c == '1';
#else
	char b[160];

	return read_file("/var/run/usb.state", b, sizeof(b)) > 0 && strstr(b, "computer=1") != 0;
#endif
}

/* One report; -1 when the computer stopped taking them. */
static int hid_rep(unsigned char mods, unsigned char key)
{
	unsigned char rep[8] = { mods, 0, key, 0, 0, 0, 0, 0 };
#ifdef SN_HOST
	static const char hx[] = "0123456789abcdef";
	char line[17];

	for (int i = 0; i < 8; i++) {
		line[2 * i] = hx[rep[i] >> 4];
		line[2 * i + 1] = hx[rep[i] & 15];
	}
	line[16] = '\n';
	if (hid_fd >= 0)
		write(hid_fd, line, 17);
	return 0;
#else
	struct pollfd w = { .fd = hid_fd, .events = POLLOUT };

	/* the computer polls every few ms; one that stops for a second is gone */
	if (poll(&w, 1, 1000) <= 0 || write(hid_fd, rep, 8) != 8)
		return -1;
	if (C.slow)
		usleep(SLOW_US);
	return 0;
#endif
}

/* A key pressed and let go; a modifier goes down first, on its own. */
static int tap(const struct tap *t)
{
	if (t->mods && hid_rep(t->mods, 0))
		return -1;
	if (hid_rep(t->mods, t->key) || hid_rep(t->mods, 0))
		return -1;
	return t->mods ? hid_rep(0, 0) : 0;
}

/* While typing: Esc stops it, and the progress shows. 1 to stop. */
static int typing_check(void)
{
#ifdef SN_HOST
	return 0;
#else
	static struct timespec last;
	struct timespec now;
	struct pollfd p = { .fd = ev_fd, .events = POLLIN };
	struct { unsigned int sec, usec; unsigned short type, code; int value; } e;

	while (poll(&p, 1, 0) > 0 && read(ev_fd, &e, sizeof(e)) == sizeof(e))
		if (e.type == 1 && e.value == 1 && e.code == KEY_ESC)
			return 1;
	if (stop_sig)
		return 1;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if ((now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000 >= 150) {
		flush_rows(WB_STATUS_BAR_H, H);
		last = now;
	}
	return 0;
#endif
}

static void type_snip(int i)
{
	const char *s = S[i].text;
	char t[NM_MAX + 80];
	int skipped = 0, failed = 0, stopped = 0;

	C.computer = computer_read();
	if (!C.computer) {
		strcpy(C.notice, "No computer: connect one to the bottom USB-C port (J8)");
		C.note = NOTE_BAD;
		return;
	}
#ifndef SN_HOST
	hid_fd = open("/dev/hidg0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (hid_fd < 0) {
		strcpy(C.notice, "The badge's USB keyboard is not there (/dev/hidg0)");
		C.note = NOTE_BAD;
		return;
	}
#endif
	label_of(i, C.typing, sizeof(C.typing));
	C.total = cp_count(s);
	C.typed = 0;
	C.pop = POP_TYPING;
	flush_rows(WB_STATUS_BAR_H, H);
	while (*s) {
		struct tap tp[2];
		int n = char_taps(next_cp(&s), tp);

		if (!n)
			skipped++;
		for (int k = 0; k < n && !failed; k++)
			failed = tap(&tp[k]) < 0;
		if (failed)
			break;
		C.typed++;
		if ((stopped = typing_check()))
			break;
	}
	hid_rep(0, 0);		/* nothing left held down */
#ifndef SN_HOST
	close(hid_fd);
	hid_fd = -1;
#endif
	C.pop = POP_NONE;
	t[0] = 0;
	if (failed) {
		cat(t, "The computer stopped taking keys");
		C.note = NOTE_BAD;
	} else if (stopped) {
		cat_count(cat(t, "Stopped after "), C.typed, "character");
		C.note = NOTE_WARN;
	} else {
		cat_count(cat(t, "Typed "), C.typed - skipped, "character");
		C.note = NOTE_OK;
	}
	if (skipped) {
		cat(cat_int(cat(t, "; skipped "), skipped), " the layout cannot type");
		C.note = C.note == NOTE_OK ? NOTE_WARN : C.note;
	}
	strcpy(C.notice, t);
	label_of(i, t, 32);
	cat(cat(t, ": "), C.notice);
	log_add(t);
}

/* ---- keys --------------------------------------------------------------- */

static void open_edit(int i)
{
	C.edit = i;
	C.field = i < 0 ? 0 : 1;
	strcpy(C.name, i < 0 ? "" : S[i].name);
	strcpy(C.text, i < 0 ? "" : S[i].text);
	C.name_cur = (int)strlen(C.name);
	C.text_cur = (int)strlen(C.text);
	C.text_top = 0;
	C.err = 0;
	C.sc = SC_EDIT;
	text_scroll();
}

static void save_edit(void)
{
	struct snip *s;

	if (!C.text[0]) {
		C.err = "Type a text first";
		C.field = 1;
		return;
	}
	if (C.edit < 0) {
		if (n_snip == SN_MAX) {
			C.err = "The list is full (64 snippets)";
			return;
		}
		C.edit = n_snip++;
	}
	s = &S[C.edit];
	strcpy(s->name, C.name);
	strcpy(s->text, C.text);
	C.focus = C.edit;
	C.notice[0] = 0;
	C.sc = SC_LIST;
	if (!save()) {
		strcpy(C.notice, "Saved");
		C.note = NOTE_OK;
	}
}

static void delete_snip(int i)
{
	memmove(S + i, S + i + 1, sizeof(S[0]) * (size_t)(n_snip - i - 1));
	n_snip--;
	if (C.focus >= n_snip)
		C.focus = n_snip - 1;
	if (C.focus < 0)
		C.focus = 0;
	if (!save()) {
		strcpy(C.notice, "Deleted");
		C.note = NOTE_OK;
	}
}

static void move_snip(int i, int dir)
{
	struct snip t;
	int j = i + dir;

	if (j < 0 || j >= n_snip)
		return;
	t = S[i];
	S[i] = S[j];
	S[j] = t;
	C.focus = j;
	save();
}

static void insert_text(const char *ins)
{
	char *buf = C.field ? C.text : C.name;
	int *cur = C.field ? &C.text_cur : &C.name_cur;
	int max = C.field ? TEXT_MAX : NM_MAX, len = (int)strlen(buf), n = (int)strlen(ins);

	if (len + n > max) {
		C.err = C.field ? "The text is full (1000 bytes)" : 0;
		return;
	}
	memmove(buf + *cur + n, buf + *cur, (size_t)(len - *cur + 1));
	memcpy(buf + *cur, ins, (size_t)n);
	*cur += n;
	C.err = 0;
}

static void erase(int fwd)
{
	char *buf = C.field ? C.text : C.name;
	int *cur = C.field ? &C.text_cur : &C.name_cur;
	int a = fwd ? *cur : cp_prev(buf, *cur), b = fwd ? cp_next(buf, *cur) : *cur;

	if (a == b)
		return;
	memmove(buf + a, buf + b, strlen(buf + b) + 1);
	*cur = a;
	C.err = 0;
}

/* The editor's keys: 2 when only the field changed, 3 for the screen. */
static int edit_key(int code)
{
	char *buf = C.field ? C.text : C.name;
	int *cur = C.field ? &C.text_cur : &C.name_cur, r = 2;
	char ch, s[2] = "";

	switch (code) {
	case KEY_ESC:
		C.sc = SC_LIST;
		return 3;
	case KEY_ENTER:
		if (K.shift && C.field)
			insert_text("\n");
		else if (K.shift)
			C.field = 1, r = 3;
		else
			save_edit(), r = 3;
		break;
	case KEY_TAB:
		if (C.field)
			insert_text("\t");
		else
			C.field = 1, r = 3;
		break;
	case KEY_BACKSPACE:
		erase(K.fn);
		break;
	case KEY_LEFT:
		if (K.fn && C.field) {
			layout();
			*cur = lay_start[line_of(*cur)];
		} else {
			*cur = K.fn ? 0 : cp_prev(buf, *cur);
		}
		break;
	case KEY_RIGHT:
		if (K.fn && C.field) {
			int e;

			layout();
			e = line_end(line_of(*cur));
			if (e > *cur && C.text[cp_prev(C.text, e)] == '\n')
				e = cp_prev(C.text, e);
			*cur = e;
		} else {
			*cur = K.fn ? (int)strlen(buf) : cp_next(buf, *cur);
		}
		break;
	case KEY_UP:
		if (C.field) {
			text_vert(-1);
			r = C.field ? 2 : 3;
		}
		break;
	case KEY_DOWN:
		if (C.field)
			text_vert(1);
		else
			C.field = 1, r = 3;
		break;
	default:
		if (!(ch = key_char(code, K.shift)))
			return 0;
		s[0] = ch;
		insert_text(s);
	}
	if (C.field)
		text_scroll();
	return r;
}

static void open_menu(void)
{
	C.n_menu = 0;
	C.menu[C.n_menu++] = M_TYPE;
	C.menu[C.n_menu++] = M_EDIT;
	if (C.focus > 0)
		C.menu[C.n_menu++] = M_UP;
	if (C.focus < n_snip - 1)
		C.menu[C.n_menu++] = M_DOWN;
	C.menu[C.n_menu++] = M_DELETE;
	C.pop = POP_MENU;
	C.pf = 0;
}

/* The pop-ups' keys. */
static int pop_key(int code)
{
	int n = C.pop == POP_MENU ? C.n_menu : C.pop == POP_LAYOUT ? N_LAY : 2, pick = C.pf;

	if (C.pop == POP_HELP) {	/* only an explanation: Esc or the diamond closes it */
		if (code == KEY_ESC || code == KEY_SYM_HELP)
			C.pop = POP_NONE;
		return 3;
	}
	if (code == KEY_UP && C.pf > 0)
		C.pf--;
	else if (code == KEY_DOWN && C.pf < n - 1)
		C.pf++;
	else if (code == KEY_ESC)
		C.pop = POP_NONE;
	if (code != KEY_ENTER)
		return 3;
	switch (C.pop) {
	case POP_MENU:
		C.pop = POP_NONE;
		switch (C.menu[pick]) {
		case M_TYPE:
			C.notice[0] = 0;
			type_snip(C.focus);
			break;
		case M_EDIT:
			open_edit(C.focus);
			break;
		case M_UP:
			move_snip(C.focus, -1);
			break;
		case M_DOWN:
			move_snip(C.focus, 1);
			break;
		case M_DELETE:
			C.pop = POP_DELETE;
			C.pf = 0;
			break;
		}
		break;
	case POP_DELETE:
		C.pop = POP_NONE;
		if (pick == 1)
			delete_snip(C.focus);
		break;
	case POP_LAYOUT:
		C.layout = pick;
		C.pop = POP_NONE;
		save_settings();
		break;
	case POP_SPEED:
		C.slow = pick;
		C.pop = POP_NONE;
		save_settings();
		break;
	}
	return 3;
}

/* One key; 0 nothing to draw, 1 to close, 2 the editor's field, 3 all. */
static int key(int code)
{
	if (C.pop)
		return pop_key(code);
	switch (code) {
	case KEY_SYM_HELP:
		C.pop = POP_HELP;
		return 3;
	case KEY_SYM_LOG:
	case KEY_SYM_SETTINGS:
		if (C.sc == SC_LIST || C.sc == SC_EDIT)
			C.back = C.sc;
		C.sc = code == KEY_SYM_LOG ? SC_LOG : SC_SETTINGS;
		return 3;
	}
	switch (C.sc) {
	case SC_EDIT:
		return edit_key(code);
	case SC_LOG:
		if (code == KEY_ESC)
			C.sc = C.back;
		return 3;
	case SC_SETTINGS:
		if (code == KEY_ESC)
			C.sc = C.back;
		else if (code == KEY_UP || code == KEY_DOWN)
			C.set_focus = code == KEY_DOWN;
		else if (code == KEY_ENTER) {
			C.pop = C.set_focus ? POP_SPEED : POP_LAYOUT;
			C.pf = C.set_focus ? C.slow : C.layout;
		}
		return 3;
	}
	/* the list */
	switch (code) {
	case KEY_ESC:
		return 1;
	case KEY_UP:
		if (C.focus > 0)
			C.focus--;
		break;
	case KEY_DOWN:
		if (C.focus < n_snip - 1)
			C.focus++;
		break;
	case KEY_ENTER:
		if (n_snip) {
			C.notice[0] = 0;
			type_snip(C.focus);
		}
		return 3;
	case KEY_SYM_MENU:
		if (n_snip)
			open_menu();
		return 3;
	case KEY_SYM_MAIN:
		open_edit(-1);
		return 3;
	case KEY_SYM_SECOND:
		if (n_snip)
			open_edit(C.focus);
		return 3;
	default:
		return 0;
	}
	C.notice[0] = 0;
	return 3;
}

/* ---- the host test ------------------------------------------------------ */

#ifdef SN_HOST
static void save_ppm(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	static uint8_t rgb_[W * H * 3];
	static const char hdr[] = "P6\n720 720\n255\n";

	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];

		rgb_[i * 3] = (uint8_t)((p >> 11) << 3);
		rgb_[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) << 2);
		rgb_[i * 3 + 2] = (uint8_t)((p & 31) << 3);
	}
	if (fd < 0 || write(fd, hdr, sizeof(hdr) - 1) < 0 || write(fd, rgb_, sizeof(rgb_)) < 0)
		_exit(1);
	close(fd);
}

/*
 * snippets-host KEYS [OUT.ppm], with APP_CONFIG, SN_COMPUTER=1 (a computer
 * on J8) and SN_HID (where the reports go, as hex). KEYS is typed; \n
 * Enter, \N Shift+Enter, \e Esc, \t Tab, \b Backspace, \u \d \l \r the
 * arrows, \f Fn held for the next key, \1..\6 the symbol keys. Prints the
 * screen, the notice, the editor's fields and the snippets.
 */
int main(int argc, char **argv)
{
	const char *k = argc > 1 ? argv[1] : "", *hid = getenv("SN_HID");
	char out[8192];

	paths();
	load();
	load_settings();
	C.computer = computer_read();
	if (hid)
		hid_fd = open(hid, O_WRONLY | O_CREAT | O_APPEND, 0644);
	memcpy(U.hhmm, "12:34", 6);
	U.clock_ok = 1;
	for (; *k; k++) {
		int code = -1, fn = 0;

		if (*k == '\\' && k[1] == 'f') {
			fn = 1;
			k += 2;
		}
		if (*k == '\\' && k[1] && k[1] != '\\') {	/* \\ is a backslash */
			k++;
			K.shift = *k == 'N';
			code = *k == 'n' || *k == 'N' ? KEY_ENTER : *k == 'e' ? KEY_ESC : *k == 't' ? KEY_TAB :
			       *k == 'b' ? KEY_BACKSPACE : *k == 'u' ? KEY_UP : *k == 'd' ? KEY_DOWN :
			       *k == 'l' ? KEY_LEFT : *k == 'r' ? KEY_RIGHT :
			       *k >= '1' && *k <= '6' ? KEY_SYM_MENU + (*k - '1') : -1;
		} else {
			k += *k == '\\' && k[1] == '\\';
			for (int c = 0; c < 64 && code < 0; c++) {
				if (key_char(c, 0) == *k)
					code = c, K.shift = 0;
				else if (key_char(c, 1) == *k)
					code = c, K.shift = 1;
			}
			if (*k == ' ')
				code = 57, K.shift = 0;
		}
		K.fn = fn;
		if (code >= 0 && key(code) == 1) {
			write(1, "closed\n", 7);
			break;
		}
		K.shift = K.fn = 0;
	}
	flush_rows(0, H);
	if (argc > 2 && argv[2][0])
		save_ppm(argv[2]);
	out[0] = 0;
	cat(out, "screen ");
	cat(out, C.sc == SC_EDIT ? "edit" : C.sc == SC_LOG ? "log" : C.sc == SC_SETTINGS ? "settings" : "list");
	cat(out, C.pop == POP_HELP ? " help" : C.pop == POP_MENU ? " menu" : C.pop == POP_DELETE ? " delete" :
	    C.pop == POP_LAYOUT ? " layout" : C.pop == POP_SPEED ? " speed" : "");
	cat(cat(out, "\nnotice "), C.notice);
	cat(cat(out, "\nerror "), C.err ? C.err : "");
	cat(cat_int(cat(out, "\nfocus "), C.focus), "\nname ");
	escape(out + strlen(out), C.name)[0] = 0;
	cat(out, "\ntext ");
	escape(out + strlen(out), C.text)[0] = 0;
	cat(cat_int(cat(out, "\ncursor "), C.text_cur), "\n");
	for (int i = 0; i < n_snip; i++) {
		char *o = cat(out, "snip ");

		o = escape(o, S[i].name);
		*o++ = '|';
		o = escape(o, S[i].text);
		*o++ = '\n';
		*o = 0;
	}
	write(1, out, strlen(out));
	return 0;
}

#else

/* ---- the badge ---------------------------------------------------------- */

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static void on_signal(int s)
{
	stop_sig = s;
}

/* the status bar; 1 if it changed */
static int status_read(void)
{
	time_t t = time(0);
	struct tm tm;
	char old[6];
	int wifi = U.wifi_up, ok = U.clock_ok, lora = U.lora;

	memcpy(old, U.hhmm, sizeof(old));
	localtime_r(&t, &tm);
	U.clock_ok = tm.tm_year >= 124;
	U.hhmm[0] = (char)('0' + tm.tm_hour / 10);
	U.hhmm[1] = (char)('0' + tm.tm_hour % 10);
	U.hhmm[2] = ':';
	U.hhmm[3] = (char)('0' + tm.tm_min / 10);
	U.hhmm[4] = (char)('0' + tm.tm_min % 10);
	U.hhmm[5] = 0;
	U.wifi_up = wifi_bars();	/* -1: not connected, else the lit arcs */
	U.lora = lora_on();
	return wifi != U.wifi_up || ok != U.clock_ok || lora != U.lora || memcmp(old, U.hhmm, sizeof(old));
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p;
	int ticks = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev_fd < 0)
		return 1;
	paths();
	load();
	load_settings();
	C.computer = computer_read();
	status_read();
	flush_rows(0, H);
	p.fd = ev_fd;
	p.events = POLLIN;
	while (!stop_sig) {
		struct ev evs[16];
		int k = poll(&p, 1, 1000), got, draw = 0, quit = 0;

		if (k <= 0) {
			int pc = C.computer;

			C.computer = computer_read();
			if (pc != C.computer && C.sc == SC_LIST && !C.pop)
				flush_rows(TITLE_Y, TITLE_Y + 44);
			if (++ticks % 5 == 0 && status_read())
				flush_rows(0, WB_STATUS_BAR_H);
			continue;
		}
		got = (int)read(ev_fd, evs, sizeof(evs)) / (int)sizeof(evs[0]);
		for (int q = 0; q < got && !quit; q++) {
			const struct ev e = evs[q];
			int down = e.value != 0, r;

			if (e.type != 1)
				continue;
			if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT)
				K.shift = down;
			else if (e.code == KEY_FN)
				K.fn = down;
			else if (e.value == 1 || (e.value == 2 && C.sc == SC_EDIT && !C.pop)) {
				r = key(e.code);
				if (r == 1)
					quit = 1;
				else if (r > draw)
					draw = r;
			}
		}
		if (quit)
			break;
		/* typing redraws the field only: a whole screen is ~18 bands */
		if (draw == 3)
			flush_rows(0, H);
		else if (draw == 2 && C.field)
			flush_rows(TEXT_Y - 20, COUNT_Y + 24);
		else if (draw == 2)
			flush_rows(NAME_Y - 20, NAME_Y + NAME_H + 20);
	}
	return 0;
}
#endif
