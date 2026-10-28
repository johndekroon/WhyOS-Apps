// SPDX-License-Identifier: MIT
/*
 * barbarious (BARbarious) — texts as barcodes and QR Codes on the screen,
 * control characters included, for a scanner to read off it: HIDeous'
 * twin the other way round (sdapps/README.md). Laid out after the design
 * canvas "BARbarious" (2026-10-25), drawn like the menu with libwb in the
 * style of the canvas "Menu's met frontpaneel" (the frontpanel's figures
 * behind glass cards, frosted pop-ups); a code itself stays black on
 * white, for a scanner.
 *
 * A list of codes, each a name, a barcode type and a text of up to
 * TEXT_MAX units: bytes, control characters included, and FNC1
 * (symbol.h, which makes the symbols). Enter shows one as big as the
 * screen allows, at full brightness (a setting), and keeps the screen on
 * while it does, for up to KEEP_ON_S after the last key. The list's first
 * row, Quick code, shows the code of a text while it is typed; it is not
 * saved, and Enter makes it a code of the list.
 *
 * Kept in APP_CONFIG (flash: it survives a new card) as codes.txt, one
 * code a line: "name<TAB>type<TAB>text". In the text \\ is a backslash,
 * \[CR] a control character by its name (\[NUL] .. \[US], \[DEL]), \[FNC1]
 * GS1's FNC1, \xHH any byte; \n, \t and \r read as LF, TAB and CR. Lines
 * starting with # are comments. The settings are in APP_CONFIG/settings.
 *
 * Keys (Docs/06-design-system.md 6.6): Enter shows a code, the square
 * opens its options, the triangle makes a new one (on a code shown: its
 * type), the cross edits it; the circle lists the codes shown this
 * session, the cloud the settings, the diamond the help. In a text Tab
 * puts in a TAB, Shift+Enter a CR (a setting), Ctrl and a letter ^A..^Z,
 * and the square any control character or FNC1 (Insert).
 *
 * The panel is drawn into a whole picture (1 MB, malloc'd at the start),
 * so that pop-ups can blur what is under them; without room for it, one
 * band of BAND_H rows at a time, with plain pop-ups. Either way a frame is
 * recorded first and only the bands that changed are drawn (libwb's
 * wb_frame() and wb_bands(): present()).
 * Built with -DBB_HOST it takes keys from its first argument and writes
 * the screen as PPM to its second; "--sym" writes one symbol
 * (test_barbarious.py). On the badge a test may send key codes to
 * /tmp/barbarious.keys and read what is on screen in /tmp/barbarious.where.
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

#include "wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wb.h"
#include "lora_state.h"
#include "wbstatus.h"
#include "keyrep.h"
#include "power.h"
#include "symbol.h"

#ifndef BB_HOST
#include <poll.h>
#endif

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_TAB		15
#define KEY_ENTER	28
#define KEY_LEFTCTRL	29
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_FN		0x1d0

/* ---- the codes ---------------------------------------------------------- */

#define CODE_MAX	64
#define NM_MAX		40		/* bytes in a name */
#define TEXT_MAX	1000		/* units in a text */
#define LOG_MAX		20
#define KEEP_ON_S	600		/* a code keeps the screen on this long after a key */

struct code {
	char name[NM_MAX + 1];
	int type, n;
	unsigned short t[TEXT_MAX];
};

static struct code S[CODE_MAX];
static int n_code;

/* a text being typed: the editor's, or Quick code's */
struct field {
	unsigned short t[TEXT_MAX];
	int n, cur, top;
};

static struct field ET, QT;

enum { SC_LIST, SC_SHOW, SC_EDIT, SC_QUICK, SC_LOG, SC_SETTINGS };
enum { POP_NONE, POP_HELP, POP_MENU, POP_DELETE, POP_TYPE, POP_INSERT };
enum { NOTE_OK, NOTE_WARN, NOTE_BAD };
enum { M_SHOW, M_EDIT, M_TYPE, M_UP, M_DOWN, M_DELETE };
enum { TF_EDIT, TF_QUICK, TF_CODE };		/* whose type the type pop-up sets */
enum { SE_CR, SE_LF, SE_CRLF };
#define N_SET		6

static const char *const type_ids[N_TYPES] = {
	"qr", "datamatrix", "aztec", "pdf417", "code128", "code39", "ean13", "upca", "itf", "codabar",
};

static struct {
	int sc, back;			/* the screen; where the log and settings go back to */
	int focus;			/* the list's row: 0 Quick code, then code focus - 1 */
	int shown;			/* the code on the show screen */
	int pop, pf;			/* the pop-up on top, and its focus */
	int menu[6], n_menu, menu_for;	/* the square key's items (M_...), for code menu_for */
	int set_focus;			/* the settings' row */
	int def_type, ecc, quiet, shift_enter, under, bright;	/* the settings */
	char notice[120];
	int note;			/* NOTE_... */
	/* the editor */
	int edit;			/* the code edited, or -1 for a new one */
	int edit_back;			/* the screen it goes back to */
	int field;			/* 0 the name, 1 the text, 2 the type */
	char name[NM_MAX + 1];
	int name_cur, etype;
	const char *err;
	int qtype;			/* Quick code's type */
	/* the type pop-up */
	int type_for;			/* TF_... */
	int need[N_TYPES];
	const char *need_note[N_TYPES];
	/* the insert pop-up */
	char filter[8];
	int imap[34], n_imap;
	char log[LOG_MAX][96];
	int n_log;
	int lit;			/* full brightness is on */
} C;

static struct { int shift, fn, ctrl; } K;

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

/* "1 code", "2 codes" */
static char *cat_count(char *d, int n, const char *word)
{
	cat(cat(cat_int(d, n), " "), word);
	return n == 1 ? d + strlen(d) : cat(d, "s");
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* does s start with p, ignoring case? */
static int prefix_ci(const char *s, const char *p)
{
	for (; *p; s++, p++)
		if (lower((unsigned char)*s) != lower((unsigned char)*p))
			return 0;
	return 1;
}

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

/*
 * The control characters, as the insert pop-up lists them: ASCII 0-31,
 * DEL, and FNC1, by their names ("TAB", "GS"). In a text they show as
 * chips with that name; bytes 128-255 as chips with their hex ("xC3").
 */
#define N_CTL		34

static const char *const ctl_name[N_CTL] = {
	"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS", "TAB", "LF", "VT", "FF", "CR", "SO", "SI",
	"DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US",
	"DEL", "FNC1",
};

static const char *const ctl_desc[N_CTL] = {
	"Null", "Start of heading", "Start of text", "End of text", "End of transmission", "Enquiry",
	"Acknowledge", "Bell", "Backspace", "Horizontal tab", "Line feed", "Vertical tab", "Form feed",
	"Carriage return", "Shift out", "Shift in", "Data link escape", "Device control 1",
	"Device control 2", "Device control 3", "Device control 4", "Negative acknowledge",
	"Synchronous idle", "End of transmission block", "Cancel", "End of medium", "Substitute",
	"Escape", "File separator", "Group separator", "Record separator", "Unit separator", "Delete",
	"Function 1: GS1 data",
};

static int ctl_index(int u)
{
	return u < 32 ? u : u == 127 ? 32 : u == SYM_FNC1 ? 33 : -1;
}

static int ctl_unit(int i)
{
	return i < 32 ? i : i == 32 ? 127 : SYM_FNC1;
}

/* "^I"; "" for FNC1 */
static void ctl_caret(int i, char *out)
{
	out[0] = 0;
	if (i < 33) {
		out[0] = '^';
		out[1] = i < 32 ? (char)('@' + i) : '?';
		out[2] = 0;
	}
}

static const char hexd[] = "0123456789ABCDEF";

/* how unit u shows: a character, or (*chip set) a chip's label */
static const char *unit_label(int u, char *buf, int *chip)
{
	int i = ctl_index(u);

	*chip = 1;
	if (i >= 0)
		return ctl_name[i];
	if (u >= 0x80) {
		buf[0] = 'x';
		buf[1] = hexd[u >> 4 & 15];
		buf[2] = hexd[u & 15];
		buf[3] = 0;
		return buf;
	}
	*chip = 0;
	buf[0] = (char)u;
	buf[1] = 0;
	return buf;
}

static int has_ctl(const unsigned short *t, int n)
{
	for (int i = 0; i < n; i++)
		if (ctl_index(t[i]) >= 0)
			return 1;
	return 0;
}

/* ---- files -------------------------------------------------------------- */

static char dir[200], data_path[240], set_path[240];

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
		cat(dir, "/.whyos/config/barbarious");
	}
	mkdirs(dir);
	cat(cat(data_path, dir), "/codes.txt");
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

/* a file written through a small buffer, then fsync and rename */
static struct {
	int fd, n, bad;
	char b[4096];
	char tmp[260];
} WR;

static int wr_open(const char *path)
{
	WR.tmp[0] = 0;
	cat(cat(WR.tmp, path), ".new");
	WR.n = WR.bad = 0;
	WR.fd = open(WR.tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	return WR.fd < 0 ? -1 : 0;
}

static void wr_flush(void)
{
	if (WR.n && write(WR.fd, WR.b, (size_t)WR.n) != WR.n)
		WR.bad = 1;
	WR.n = 0;
}

static void wr(const char *s, int n)
{
	while (n > 0) {
		int k = (int)sizeof(WR.b) - WR.n < n ? (int)sizeof(WR.b) - WR.n : n;

		memcpy(WR.b + WR.n, s, (size_t)k);
		WR.n += k;
		s += k;
		n -= k;
		if (WR.n == (int)sizeof(WR.b))
			wr_flush();
	}
}

static void wrs(const char *s)
{
	wr(s, (int)strlen(s));
}

static int wr_close(const char *path)
{
	int ok;

	wr_flush();
	ok = !WR.bad;
	ok &= fsync(WR.fd) == 0;
	ok &= close(WR.fd) == 0;
	if (!ok || rename(WR.tmp, path) < 0) {
		unlink(WR.tmp);
		return -1;
	}
	return 0;
}

/* a name as codes.txt has it: \t, \n and \\ escaped */
static void wr_name(const char *s)
{
	for (; *s; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\\') {
			char e[3] = { '\\', *s == '\t' ? 't' : *s == '\n' ? 'n' : '\\', 0 };

			wrs(e);
		} else {
			wr(s, 1);
		}
	}
}

/* a text: printable ASCII as it is, \\, \[NAME] for the control characters
 * and FNC1, bytes from 128 up as they are (UTF-8 stays readable) */
static void wr_text(const unsigned short *t, int n)
{
	for (int i = 0; i < n; i++) {
		int c = ctl_index(t[i]);
		char ch = (char)t[i];

		if (c >= 0) {
			wrs("\\[");
			wrs(ctl_name[c]);
			wrs("]");
		} else if (t[i] == '\\') {
			wrs("\\\\");
		} else {
			wr(&ch, 1);
		}
	}
}

static int save(void)
{
	if (wr_open(data_path) < 0)
		goto bad;
	wrs("# Codes for BARbarious: name<TAB>type<TAB>text per line. In the text\n"
	    "# \\\\ is a backslash, \\[CR] a control character by its name (\\[NUL] to\n"
	    "# \\[US], \\[DEL]), \\[FNC1] GS1's FNC1, \\xHH any byte. Types: qr,\n"
	    "# datamatrix, aztec, pdf417, code128, code39, ean13, upca, itf, codabar.\n");
	for (int i = 0; i < n_code; i++) {
		wr_name(S[i].name);
		wrs("\t");
		wrs(type_ids[S[i].type]);
		wrs("\t");
		wr_text(S[i].t, S[i].n);
		wrs("\n");
	}
	if (wr_close(data_path) == 0)
		return 0;
bad:
	strcpy(C.notice, "Could not save the codes");
	C.note = NOTE_BAD;
	return -1;
}

static int hexv(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10 : -1;
}

/* a text from codes.txt (escapes above), into t; how many units */
static int unescape(const char *s, unsigned short *t)
{
	int n = 0;

	while (*s && n < TEXT_MAX) {
		if (s[0] == '\\' && s[1] == '[') {
			const char *e = strchr(s + 2, ']');
			int k = -1;

			for (int i = 0; e && i < N_CTL && k < 0; i++)
				if ((int)strlen(ctl_name[i]) == e - s - 2 && prefix_ci(s + 2, ctl_name[i]))
					k = i;
			if (k >= 0) {
				t[n++] = (unsigned short)ctl_unit(k);
				s = e + 1;
				continue;
			}
		}
		if (s[0] == '\\' && s[1] == 'x' && hexv(s[2]) >= 0 && hexv(s[3]) >= 0) {
			t[n++] = (unsigned short)(hexv(s[2]) * 16 + hexv(s[3]));
			s += 4;
			continue;
		}
		if (s[0] == '\\' && s[1] && strchr("\\ntr", s[1])) {
			t[n++] = s[1] == 'n' ? 10 : s[1] == 't' ? 9 : s[1] == 'r' ? 13 : '\\';
			s += 2;
			continue;
		}
		t[n++] = (unsigned char)*s++;
	}
	return n;
}

static void unescape_name(const char *s, char *out)
{
	int n = 0;

	for (; *s && n < NM_MAX; s++) {
		if (s[0] == '\\' && s[1] && strchr("\\nt", s[1])) {
			s++;
			out[n++] = *s == 'n' ? ' ' : *s == 't' ? ' ' : '\\';
		} else {
			out[n++] = *s;
		}
	}
	out[n] = 0;
}

static int type_of(const char *s, int n)
{
	for (int i = 0; i < N_TYPES; i++)
		if ((int)strlen(type_ids[i]) == n && prefix_ci(s, type_ids[i]))
			return i;
	return -1;
}

/* one line of codes.txt: name<TAB>type<TAB>text (or name<TAB>text, or text) */
static void load_line(char *p)
{
	struct code *c = &S[n_code];
	char *t1, *t2;
	int ty;

	if (!*p || *p == '#' || n_code == CODE_MAX)
		return;
	c->name[0] = 0;
	c->type = T_QR;
	if ((t1 = strchr(p, '\t'))) {
		*t1 = 0;
		unescape_name(p, c->name);
		p = t1 + 1;
		if ((t2 = strchr(p, '\t')) && (ty = type_of(p, (int)(t2 - p))) >= 0) {
			c->type = ty;
			p = t2 + 1;
		}
	}
	c->n = unescape(p, c->t);
	if (c->n)
		n_code++;
}

static void load(void)
{
	static char buf[4096], line[8192];
	int fd = open(data_path, O_RDONLY | O_CLOEXEC), len = 0, r;

	if (fd < 0)
		return;
	while ((r = (int)read(fd, buf, sizeof(buf))) > 0)
		for (int i = 0; i < r; i++) {
			if (buf[i] == '\n') {
				line[len] = 0;
				if (len && line[len - 1] == '\r')
					line[len - 1] = 0;
				load_line(line);
				len = 0;
			} else if (len < (int)sizeof(line) - 1) {
				line[len++] = buf[i];
			}
		}
	close(fd);
	line[len] = 0;
	load_line(line);
}

static int num_after(const char *b, const char *key, int dflt, int lo, int hi)
{
	const char *p = strstr(b, key);
	int v = 0;

	if (!p || (p > b && p[-1] != '\n'))
		return dflt;
	p += strlen(key);
	if (*p < '0' || *p > '9')
		return dflt;
	while (*p >= '0' && *p <= '9' && v < 1000)
		v = v * 10 + *p++ - '0';
	return v < lo || v > hi ? dflt : v;
}

static void load_settings(void)
{
	char b[256];

	if (read_file(set_path, b, sizeof(b)) < 0)
		b[0] = 0;
	C.def_type = num_after(b, "type=", T_QR, 0, N_TYPES - 1);
	C.ecc = num_after(b, "ecc=", ECC_M, ECC_L, ECC_H);
	C.quiet = num_after(b, "quiet=", 4, 1, 16);
	C.shift_enter = num_after(b, "shift=", SE_CR, SE_CR, SE_CRLF);
	C.under = num_after(b, "under=", 1, 0, 1);
	C.bright = num_after(b, "bright=", 1, 0, 1);
}

static void save_settings(void)
{
	char b[128] = "";

	cat_int(cat(b, "type="), C.def_type);
	cat_int(cat(b, "\necc="), C.ecc);
	cat_int(cat(b, "\nquiet="), C.quiet);
	cat_int(cat(b, "\nshift="), C.shift_enter);
	cat_int(cat(b, "\nunder="), C.under);
	cat_int(cat(b, "\nbright="), C.bright);
	cat(b, "\n");
	if (wr_open(set_path) == 0) {
		wrs(b);
		wr_close(set_path);
	}
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

/* What the pop-ups and the show screen call code i: its name, or the
 * characters of its text (without the control characters). */
static void label_of(int i, char *out, int size)
{
	int k = 0;

	if (S[i].name[0]) {
		strncpy(out, S[i].name, (size_t)size - 1);
		out[size - 1] = 0;
		return;
	}
	for (int j = 0; j < S[i].n && k < size - 1; j++)
		if (S[i].t[j] >= 32 && S[i].t[j] < 127)
			out[k++] = (char)S[i].t[j];
	out[k] = 0;
	if (!k)
		strncpy(out, "Code", (size_t)size);
}

/* ---- the screen's light ------------------------------------------------- */

#define BL_PATH		"/sys/class/backlight/backlight/brightness"
#define BL_MAX_PATH	"/sys/class/backlight/backlight/max_brightness"

#ifndef BB_HOST
static int bl_saved = -1;

static int read_int(const char *path)
{
	char b[16];

	return read_file(path, b, sizeof(b)) > 0 ? atoi(b) : -1;
}

static void write_int(const char *path, int v)
{
	char b[16] = "";
	int fd = open(path, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return;
	cat(cat_int(b, v), "\n");
	if (write(fd, b, strlen(b)) < 0)
		b[0] = 0;
	close(fd);
}
#endif

/* Full brightness while a code shows (the setting), and back after. */
static void lights(int on)
{
	on = on && C.bright;
	if (on == C.lit)
		return;
	C.lit = on;
#ifndef BB_HOST
	if (on) {
		int max = read_int(BL_MAX_PATH);

		bl_saved = read_int(BL_PATH);
		if (max > 0 && bl_saved >= 0 && bl_saved < max)
			write_int(BL_PATH, max);
	} else if (bl_saved > 0) {
		write_int(BL_PATH, bl_saved);
	}
#endif
}

static void go(int sc)
{
	C.sc = sc;
	lights(sc == SC_SHOW || sc == SC_QUICK);
}

/* ---- layout ------------------------------------------------------------- */

#define TOP		72			/* the eyebrow (wb_header) */
#define BODY_Y		WB_ROWS_TOP		/* under the title */
#define X0		WB_CONTENT_PAD
#define XW		(W - 2 * WB_CONTENT_PAD)
#define HX		(X0 + 6)		/* the header, a little in */
#define BOTTOM		(H - WB_HINT_BAR_H - 8)
#define ROW_H		WB_LIST_ROW_H		/* 68, as wb_row's */
#define ROW_GAP		10
#define RW		(XW - 16)		/* the list's rows: its scroll bar right of them */
#define LIST_Y		BODY_Y

#define CHIP_PAD	6			/* inside a chip, either side */
#define CHIP_GAP	3			/* outside */
#define CHIP_EDGE	0x959360		/* yellow at 55 % on a field */
#define DARK_EDGE	0x5A5A70		/* a chip under a barcode */
#define DARK_INK	0x3A3A50
#define WHITE		0xFFFFFF
#define BLACK		0x000000

/* the text fields: the editor's and Quick code's, three lines */
#define TF		(&f_mono18)
#define TF_PAD		18
#define TF_W		(XW - 2 * TF_PAD)
#define LINE_H		28
#define TF_LINES	3
#define TF_H		(2 * 10 + TF_LINES * LINE_H)

/* the editor */
#define E_NAME_LY	BODY_Y
#define E_NAME_Y	(E_NAME_LY + 19 + 6)
#define E_NAME_H	52
#define E_TEXT_LY	(E_NAME_Y + E_NAME_H + 16)
#define E_TEXT_Y	(E_TEXT_LY + 19 + 6)
#define E_HELP_Y	(E_TEXT_Y + TF_H + 8)
#define E_TYPE_Y	(E_HELP_Y + 19 + 16)
#define E_TYPE_H	56
#define E_PREV_Y	(E_TYPE_Y + E_TYPE_H + 16)
#define E_PREV_H	64

/* the show screen; Tools/usb/hil/test_hil_barbarious.py reads the code
 * from these places */
#define SQ_Y		148			/* a square code's area */
#define SQ		407
#define WIDE_Y		154			/* a wide code's box */
#define BAR_H		200
#define PDF_H		300			/* PDF417's most height */

/* Quick code */
#define Q_TEXT_Y	BODY_Y
#define Q_HELP_Y	(Q_TEXT_Y + TF_H + 8)
#define Q_AREA_Y	(Q_HELP_Y + 19 + 16)
#define Q_AREA_H	(H - WB_HINT_BAR_H - 16 - 25 - Q_AREA_Y)

/*
 * The whole picture (1 MB), when there was room for it: pop-ups are
 * frosted glass then, which blurs what lies around them. Without it the
 * panel is drawn a band at a time and pop-ups are plain.
 */
static uint16_t *whole;

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

/* The frontpanel's figures (libwb's wb_scene(), which reads the rotation
 * while a frame is recorded and keeps it while it is drawn), the status
 * bar, the eyebrow ("APP", or where the screen lies), the title, and a
 * word right of the title ("6 codes"). */
static void header(const char *eb, const char *title, const char *right)
{
	wb_scene();
	status_bar();
	wb_header(eb, title, right);
}

/* a chevron pointing right, or (flip) left */
static void chevron(int x, int y, uint32_t c, int flip)
{
	const struct wb_icon *ic = &r_chevron;
	int j0 = fb_y0 - y > 0 ? fb_y0 - y : 0, j1 = fb_y1 - y < ic->h ? fb_y1 - y : ic->h;

	if (wb_rec) {		/* recorded as one call, as libwb's icon() */
		int k[] = { 0xbb01, x, (int)c, flip };

		wb_rec_rows(y, y + ic->h, k, sizeof(k));
		return;
	}
	for (int j = j0; j < j1; j++)
		for (int i = 0; i < ic->w; i++)
			put(x + i, y + j, c, ic->a[j * ic->w + (flip ? ic->w - 1 - i : i)]);
}

/* ---- texts with chips ---------------------------------------------------- */

enum { CHIP_YELLOW, CHIP_DARK };

static int unit_w(const struct wb_font *f, const struct wb_font *cf, int u)
{
	char b[8];
	int chip;
	const char *l = unit_label(u, b, &chip);

	return chip ? text_w(cf, l, 0) + 2 * CHIP_PAD + 2 * CHIP_GAP : text_w(f, l, 0);
}

/* one unit at x, centred on cy; returns the x after it */
static int draw_unit(int u, int x, int cy, const struct wb_font *f, const struct wb_font *cf,
		     uint32_t c, int chip_h, int style)
{
	char b[8];
	int chip, w;
	const char *l = unit_label(u, b, &chip);

	if (!chip)
		return text(f, x, text_top(f, cy), l, c, 0);
	w = text_w(cf, l, 0) + 2 * CHIP_PAD;
	x += CHIP_GAP;
	if (style == CHIP_YELLOW)
		box(x, cy - chip_h / 2, w, chip_h, 4, WB_COLOR_YELLOW, 31, CHIP_EDGE, 1, 0);
	else
		box(x, cy - chip_h / 2, w, chip_h, 4, 0, 0, DARK_EDGE, 1, 0);
	text(cf, x + CHIP_PAD, text_top(cf, cy), l, style == CHIP_YELLOW ? WB_COLOR_YELLOW : DARK_INK, 0);
	return x + w + CHIP_GAP;
}

static int units_w(const unsigned short *t, int n, const struct wb_font *f, const struct wb_font *cf)
{
	int w = 0;

	for (int i = 0; i < n && w < 4 * W; i++)
		w += unit_w(f, cf, t[i]);
	return w;
}

/* t on one line from x, cut with an ellipsis at max_w */
static void draw_units(const unsigned short *t, int n, int x, int cy, int max_w, const struct wb_font *f,
		       const struct wb_font *cf, uint32_t c, int chip_h, int style)
{
	int end = x + max_w, ell = text_w(f, "\xe2\x80\xa6", 0);

	for (int i = 0; i < n; i++) {
		int w = unit_w(f, cf, t[i]);

		if (x + w > end - (i + 1 < n ? ell : 0)) {
			text(f, x, text_top(f, cy), "\xe2\x80\xa6", c, 0);
			return;
		}
		x = draw_unit(t[i], x, cy, f, cf, c, chip_h, style);
	}
}

/* ---- the text fields ----------------------------------------------------- */

/* the lines as shown: where each starts (wrapped at a space, or in a word
 * longer than a line) */
static int lay_start[TEXT_MAX + 2], lay_n;

static void layout(const struct field *fd)
{
	int x = 0, sp = 0, xsp = 0;	/* after the line's last space, and its x */

	lay_n = 1;
	lay_start[0] = 0;
	for (int i = 0; i < fd->n; i++) {
		int w = unit_w(TF, &f_mono13, fd->t[i]);

		if (x + w > TF_W && x > 0) {
			if (sp > lay_start[lay_n - 1] && fd->t[i] != ' ') {
				lay_start[lay_n++] = sp;
				x -= xsp;
			} else {
				lay_start[lay_n++] = i;
				x = 0;
			}
			sp = 0;
		}
		x += w;
		if (fd->t[i] == ' ') {
			sp = i + 1;
			xsp = x;
		}
	}
}

static int line_end(const struct field *fd, int l)
{
	return l + 1 < lay_n ? lay_start[l + 1] : fd->n;
}

static int line_of(int cur)
{
	int l = 0;

	while (l + 1 < lay_n && lay_start[l + 1] <= cur)
		l++;
	return l;
}

static int x_of(const struct field *fd, int cur)
{
	int x = 0;

	for (int i = lay_start[line_of(cur)]; i < cur; i++)
		x += unit_w(TF, &f_mono13, fd->t[i]);
	return x;
}

/* keep the cursor's line in view */
static void field_scroll(struct field *fd)
{
	int l;

	layout(fd);
	l = line_of(fd->cur);
	if (l < fd->top)
		fd->top = l;
	if (l >= fd->top + TF_LINES)
		fd->top = l - TF_LINES + 1;
	if (fd->top > lay_n - 1)
		fd->top = lay_n - 1 < 0 ? 0 : lay_n - 1;
}

static void draw_field(const struct field *fd, int y, int on, const char *placeholder)
{
	wb_card(X0, y, XW, TF_H, WB_RADIUS_MD, on, &GLOW_FOCUS);
	layout(fd);
	for (int l = fd->top; l < lay_n && l < fd->top + TF_LINES; l++) {
		int x = X0 + TF_PAD, ly = y + 10 + (l - fd->top) * LINE_H, cy = ly + LINE_H / 2;
		int e = line_end(fd, l);

		for (int i = lay_start[l]; i < e; i++) {
			if (on && i == fd->cur)
				fill(x, ly + 1, 2, LINE_H - 2, WB_COLOR_CYAN);
			x = draw_unit(fd->t[i], x, cy, TF, &f_mono13, WB_COLOR_TEXT_HI, 20, CHIP_YELLOW);
		}
		if (on && fd->cur == e && l == lay_n - 1)
			fill(x + 1, ly + 1, 2, LINE_H - 2, WB_COLOR_CYAN);
	}
	if (!fd->n && !on)
		text(&f_ui18, X0 + TF_PAD, text_top(&f_ui18, y + 10 + LINE_H / 2), placeholder, WB_COLOR_PLACEHOLDER, 0);
}

/* "Tab types TAB, Shift+Enter CR, Ctrl+letter ^A to ^Z. More with ■." */
static void field_help(int y)
{
	static const char *const se[3] = { "CR", "LF", "CR LF" };
	char t[96] = "Tab types TAB, Shift+Enter ";
	int x;

	if (C.err) {
		text(&f_ui16, HX, y, C.err, WB_COLOR_ERROR, 0);
		return;
	}
	cat(cat(t, se[C.shift_enter]), ", Ctrl+letter ^A to ^Z. More with ");
	x = text(&f_ui16, HX, y, t, WB_COLOR_PLACEHOLDER, 0);
	icon(&kc_square, x + 2, y + 4, WB_COLOR_KEY_SQUARE, 255);
	text(&f_ui16, x + 2 + 12 + 2, y, ".", WB_COLOR_TEXT_LOW, 0);
}

static void field_insert(struct field *fd, const unsigned short *u, int n)
{
	if (fd->n + n > TEXT_MAX) {
		C.err = "The text is full (1000 characters)";
		return;
	}
	memmove(fd->t + fd->cur + n, fd->t + fd->cur, sizeof(fd->t[0]) * (size_t)(fd->n - fd->cur));
	memcpy(fd->t + fd->cur, u, sizeof(u[0]) * (size_t)n);
	fd->n += n;
	fd->cur += n;
	C.err = 0;
}

static void field_put(struct field *fd, int u)
{
	unsigned short v = (unsigned short)u;

	field_insert(fd, &v, 1);
}

/* Up (-1) or Down (1) a line, to the nearest place; 0 when there is no
 * line there (the caller moves to the field above or below) */
static int field_vert(struct field *fd, int dir)
{
	int l, x, best, bx, e;

	layout(fd);
	l = line_of(fd->cur);
	if ((dir < 0 && l == 0) || (dir > 0 && l == lay_n - 1))
		return 0;
	x = x_of(fd, fd->cur);
	l += dir;
	e = line_end(fd, l);
	if (l + 1 < lay_n && e > lay_start[l])
		e--;			/* the last place of a wrapped line is the next line's first */
	best = lay_start[l];
	bx = 0;
	for (int i = lay_start[l], ix = 0; i <= e; i++) {
		if (abs(ix - x) < abs(bx - x)) {
			best = i;
			bx = ix;
		}
		if (i < fd->n)
			ix += unit_w(TF, &f_mono13, fd->t[i]);
	}
	fd->cur = best;
	return 1;
}

/*
 * A key in a text field: characters, Tab (TAB), Shift+Enter (the
 * setting's CR, LF or both), Ctrl with a letter (^A..^Z; [ \ ] ^ _ for
 * ESC, FS, GS, RS, US; @ or Space NUL; ? DEL), Backspace (Fn: after the
 * cursor), the arrows (Fn+Left/Right: the line's ends). 1 when it took
 * the key, -1 for Up on the first line or Down on the last.
 */
static int field_key(struct field *fd, int code)
{
	char ch = key_char(code, K.shift);

	if (K.ctrl) {
		int u = -1;

		if (lower(ch) >= 'a' && lower(ch) <= 'z')
			u = lower(ch) - 'a' + 1;
		else if (ch && strchr("[\\]^_", ch))
			u = 27 + (int)(strchr("[\\]^_", ch) - "[\\]^_");
		else if (ch == '@' || ch == ' ' || ch == '2')
			u = 0;
		else if (ch == '?')
			u = 127;
		if (u < 0)
			return 0;
		field_put(fd, u);
		return 1;
	}
	switch (code) {
	case KEY_TAB:
		field_put(fd, 9);
		return 1;
	case KEY_ENTER:
		if (!K.shift)
			return 0;
		if (C.shift_enter == SE_CRLF) {
			static const unsigned short crlf[2] = { 13, 10 };

			field_insert(fd, crlf, 2);
		} else {
			field_put(fd, C.shift_enter == SE_LF ? 10 : 13);
		}
		return 1;
	case KEY_BACKSPACE: {
		int a = K.fn ? fd->cur : fd->cur - 1;

		if (a < 0 || a >= fd->n)
			return 1;
		memmove(fd->t + a, fd->t + a + 1, sizeof(fd->t[0]) * (size_t)(fd->n - a - 1));
		fd->n--;
		fd->cur = a;
		C.err = 0;
		return 1;
	}
	case KEY_LEFT:
		layout(fd);
		fd->cur = K.fn ? lay_start[line_of(fd->cur)] : fd->cur - (fd->cur > 0);
		return 1;
	case KEY_RIGHT:
		layout(fd);
		if (K.fn) {
			int l = line_of(fd->cur), e = line_end(fd, l);

			fd->cur = l + 1 < lay_n && e > lay_start[l] ? e - 1 : e;
		} else {
			fd->cur += fd->cur < fd->n;
		}
		return 1;
	case KEY_UP:
	case KEY_DOWN:
		return field_vert(fd, code == KEY_UP ? -1 : 1) ? 1 : -1;
	}
	if (!ch)
		return 0;
	field_put(fd, ch);
	return 1;
}

/* ---- the symbols on screen ------------------------------------------------ */

/* the code on screen, made once for each change */
static struct sym SY, SCRATCH;
static struct {
	int ok, type, ecc, n, bw, bh;
	uint32_t sum;
	const char *err;
} SYK = { -1, 0, 0, 0, 0, 0, 0, 0 };

static const struct sym *code_sym(int type, const unsigned short *t, int n, int bw, int bh, const char **err)
{
	uint32_t sum = 2166136261u;

	if (type != T_PDF417)		/* only PDF417's shape depends on the box */
		bw = bh = 0;
	for (int i = 0; i < n; i++)
		sum = (sum ^ t[i]) * 16777619u;
	if (SYK.ok < 0 || SYK.type != type || SYK.ecc != C.ecc || SYK.n != n || SYK.sum != sum ||
	    SYK.bw != bw || SYK.bh != bh) {
		SYK.type = type;
		SYK.ecc = C.ecc;
		SYK.n = n;
		SYK.sum = sum;
		SYK.bw = bw;
		SYK.bh = bh;
		SYK.err = 0;
		SYK.ok = sym_make(&SY, type, t, n, C.ecc, bw, bh, &SYK.err) == 0;
	}
	*err = SYK.err;
	return SYK.ok ? &SY : 0;
}

static int is_wide(int type)
{
	return !sym_is_2d(type) || type == T_PDF417;
}

/* the quiet zone the screen gives a code */
static int quiet_of(const struct sym *s)
{
	return s->quiet > C.quiet ? s->quiet : C.quiet;
}

/*
 * Recording a frame (libwb's wb_rec): a symbol drawn at x in rows y0..y1
 * as one call, its modules and how it is drawn (tag and a, b: px and the
 * bars' height, or the box), instead of a fill() for every run. 1 when
 * recorded: then draw nothing.
 */
static int rec_sym(const struct sym *s, int tag, int x, int y0, int y1, int a, int b)
{
	if (!wb_rec)
		return 0;
	{
		int k[] = { tag, x, a, b, s->type, s->w, s->h, s->row_h };

		wb_rec_rows(y0, y1, k, sizeof(k));
		wb_rec_rows(y0, y1, s->bits, (size_t)(s->w * s->h + 7) / 8);
	}
	return 1;
}

/* the modules from (x, y), px each; a 1D code bar_h high */
static void draw_modules(const struct sym *s, int x, int y, int px, int bar_h)
{
	int rh = px * s->row_h, r0, r1;

	if (rec_sym(s, 0xbb02, x, y, y + (s->h == 1 && s->row_h == 1 && !sym_is_2d(s->type) ? bar_h : s->h * rh),
		    px, bar_h))
		return;
	if (s->h == 1 && s->row_h == 1 && !sym_is_2d(s->type)) {
		for (int m = 0; m < s->w; ) {
			int e = m;

			while (e < s->w && sym_dark(s, e, 0))
				e++;
			if (e > m)
				fill(x + m * px, y, (e - m) * px, bar_h, BLACK);
			m = e + 1;
		}
		return;
	}
	r0 = fb_y0 > y ? (fb_y0 - y) / rh : 0;		/* the rows in this band */
	r1 = (fb_y1 - y + rh - 1) / rh;
	r1 = r1 > s->h ? s->h : r1;
	for (int r = r0; r < r1; r++)
		for (int m = 0; m < s->w; ) {
			int e = m;

			while (e < s->w && sym_dark(s, e, r))
				e++;
			if (e > m)
				fill(x + m * px, y + r * rh, (e - m) * px, rh, BLACK);
			m = e + 1;
		}
}

/* s squeezed into bw x bh px from (x, y), module by module: the preview */
static void draw_scaled(const struct sym *s, int x, int y, int bw, int bh)
{
	int rows = s->h * s->row_h;

	if (rec_sym(s, 0xbb03, x, y, y + bh, bw, bh))
		return;
	for (int j = 0; j < bh; j++) {
		int my = sym_is_2d(s->type) || s->type == T_PDF417 ? j * rows / bh / s->row_h : 0;

		if (y + j < fb_y0 || y + j >= fb_y1)
			continue;
		for (int i = 0; i < bw; ) {
			int e = i;

			while (e < bw && sym_dark(s, e * s->w / bw, my))
				e++;
			if (e > i)
				fill(x + i, y + j, e - i, 1, BLACK);
			i = e + 1;
		}
	}
}

/* px for s in a box bw x bh (its quiet zone included) */
static int fit_px(const struct sym *s, int bw, int bh)
{
	int q = quiet_of(s), pw = bw / (s->w + 2 * q), ph;

	if (!is_wide(s->type) || s->type == T_PDF417) {
		ph = bh / (s->h * s->row_h + 2 * q);
		return pw < ph ? pw : ph;
	}
	return pw;
}

/* "QR Code · error correction M · 38 bytes", "Code 128 · 167 modules · 3 px per module · 10 characters" */
static void sym_info(const struct sym *s, int px, char *out)
{
	out[0] = 0;
	cat(out, sym_names[s->type]);
	cat(out, " \xc2\xb7 ");
	switch (s->type) {
	case T_QR: {
		char l[2] = { "LMQH"[s->ecc], 0 };

		cat(cat(out, "error correction "), l);
		break;
	}
	case T_DM:
		cat_int(cat(cat_int(out, s->w), " by "), s->h);
		break;
	case T_AZTEC:
		if (s->size2 < 0)
			cat(out, "compact, ");
		cat_count(out, s->size2 < 0 ? -s->size2 : s->size2, "layer");
		break;
	case T_PDF417:
		cat_count(cat(cat_count(out, s->h, "row"), ", "), s->size2, "column");
		break;
	default:
		cat_count(out, s->w, "module");
		cat(cat_int(cat(out, " \xc2\xb7 "), px), " px per module");
	}
	cat(out, " \xc2\xb7 ");
	cat_count(out, s->bytes, sym_is_2d(s->type) ? "byte" : "character");
}

/* the error in a box where the code would be */
static void draw_sym_error(int x, int y, int w, int h, const char *err)
{
	wb_card(x, y, w, h, WB_RADIUS_MD, 0, 0);
	text_wrap(&f_ui20, x + 24, y + h / 2 - 28, w - 48, 28, err, WB_COLOR_ERROR, 1, 1);
	text(&f_ui16, x + 24, y + h / 2 + 8, "Another type: the triangle key.", WB_COLOR_TEXT_MID, 0);
}

/*
 * The note under a wide code with control characters: what a scanner
 * does with them, and how the type holds them. The first one as a chip.
 */
static void draw_ctl_note(const struct sym *s, const unsigned short *t, int n, int y)
{
	int first = -1, keys = 1, fnc = 1, lines;
	char msg[200] = "";

	for (int i = 0; i < n; i++)
		if (ctl_index(t[i]) >= 0) {
			if (first < 0)
				first = t[i];
			keys &= t[i] == 9 || t[i] == 10 || t[i] == 13;
			fnc &= t[i] == SYM_FNC1;
		}
	if (first < 0)
		return;
	if (fnc)
		cat(msg, "GS1 data: a scanner sends the first FNC1 as its mark and the others as GS.");
	else if (keys)
		cat(msg, "A scanner that types like a keyboard sends Tab and Enter here.");
	else
		cat(msg, "A scanner passes control characters on when it is set to.");
	if (s->type == T_C128 && (s->shifted || s->latched))
		cat(msg, s->shifted && !s->latched ? " Code 128 codes them with a Shift to code set A." :
		    " Code 128 codes them in code set A.");
	else if (s->type == T_C39 && !fnc)
		cat(msg, " Code 39 codes them as pairs: the scanner needs Full ASCII on.");
	else if (s->type == T_PDF417)
		cat(msg, " PDF417 holds them as bytes.");
	lines = text_wrap(&f_ui18, 0, 0, XW - 36 - 60, 24, msg, 0, 3, 0);
	wb_card(X0, y, XW, 28 + lines * 24, WB_RADIUS_MD, 0, 0);
	draw_unit(first, X0 + 18 - CHIP_GAP, y + 14 + 12, &f_mono13, &f_mono13, WB_COLOR_YELLOW, 18, CHIP_YELLOW);
	text_wrap(&f_ui18, X0 + 18 + 60, y + 14, XW - 36 - 60, 24, msg, WB_COLOR_TEXT_MID, 3, 1);
}

/*
 * A code as big as fits: a square one centred in the area from y, side
 * high; a wide one in a white box across the screen from wy, bars bar_h
 * high (the text under it, a setting). Returns the y below what it drew.
 */
static int draw_code(const unsigned short *t, int n, int type, int y, int side, int wy, int bar_h, int under,
		     int *px_out, const struct sym **out)
{
	const char *err;
	const struct sym *s = code_sym(type, t, n, XW, type == T_PDF417 ? bar_h : side, &err);
	int px, q;

	*out = s;
	*px_out = 0;
	if (!s) {
		draw_sym_error(X0, is_wide(type) ? wy : y + side / 2 - 70, XW, 140, err);
		return (is_wide(type) ? wy : y + side / 2 - 70) + 140;
	}
	q = quiet_of(s);
	if (!is_wide(type)) {
		int tot = s->w + 2 * q, d, bx, by;

		px = side / tot;
		if (!px) {
			draw_sym_error(X0, y + side / 2 - 70, XW, 140, "Too big for the screen");
			return y + side / 2 + 70;
		}
		d = tot * px;
		bx = W / 2 - d / 2;
		by = y + (side - d) / 2;
		box(bx, by, d, d, WB_RADIUS_SM, WHITE, 255, 0, 0, 0);
		draw_modules(s, bx + q * px, by + q * px, px, 0);
		*px_out = px;
		return y + side;
	}
	if (type == T_PDF417) {
		int d;

		px = fit_px(s, XW, bar_h);
		if (!px) {
			draw_sym_error(X0, wy, XW, 140, "Too big for the screen");
			return wy + 140;
		}
		d = (s->h * s->row_h + 2 * q) * px;
		box(X0, wy, XW, d, WB_RADIUS_SM, WHITE, 255, 0, 0, 0);
		draw_modules(s, X0 + (XW - s->w * px) / 2, wy + q * px, px, 0);
		*px_out = px;
		return wy + d;
	}
	px = fit_px(s, XW, 0);
	if (!px) {
		char e[80] = "Too long for the screen: ";

		cat(cat_count(e, s->w + 2 * q, "module"), " in 624 px");
		draw_sym_error(X0, wy, XW, 140, e);
		return wy + 140;
	}
	{
		int bh = 36 + bar_h + (under ? 18 + 26 : 0) + 24;

		box(X0, wy, XW, bh, WB_RADIUS_SM, WHITE, 255, 0, 0, 0);
		draw_modules(s, X0 + (XW - s->w * px) / 2, wy + 36, px, bar_h);
		if (under) {
			static unsigned short ht[SYM_TEXT + 1];
			int hn = 0, cy = wy + 36 + bar_h + 18 + 13, w;

			if (type == T_EAN13 || type == T_UPCA || type == T_ITF) {
				for (const char *p = s->text; *p; p++)
					ht[hn++] = (unsigned char)*p;
				w = units_w(ht, hn, TF, &f_mono13);
				draw_units(ht, hn, W / 2 - (w < XW - 48 ? w : XW - 48) / 2, cy, XW - 48, TF, &f_mono13,
					   WB_COLOR_VOID, 20, CHIP_DARK);
			} else {
				w = units_w(t, n, TF, &f_mono13);
				draw_units(t, n, W / 2 - (w < XW - 48 ? w : XW - 48) / 2, cy, XW - 48, TF, &f_mono13,
					   WB_COLOR_VOID, 20, CHIP_DARK);
			}
		}
		*px_out = px;
		return wy + bh;
	}
}

/* ---- the list ------------------------------------------------------------ */

static const struct wb_icon *type_icon(int type)
{
	return type == T_QR || type == T_AZTEC ? &r_qr : type == T_DM ? &r_matrix : &r_barcode;
}

static int list_top(void)
{
	return LIST_Y + (C.notice[0] ? 32 : 0);
}

/* row i: -1 Quick code, else code i */
static void draw_row(int i, int y, int on)
{
	/* laid out as wb_row's: the label's middle 11 px over the row's, the
	 * second line's 15 under it */
	int x = X0, lx = x + 20 + 26 + 16, rx = x + RW - 18, ly = y + ROW_H / 2 - 11, sy = y + ROW_H / 2 + 15;
	uint32_t ink = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	char buf[128], name[96];

	wb_card(x, y, RW, ROW_H, WB_RADIUS_MD, on, &GLOW_FOCUS);
	if (i < 0) {
		icon(&r_keyboard, x + 20, y + ROW_H / 2 - 13, ink, 255);
		chevron(rx - 20, y + ROW_H / 2 - 10, ink, 0);
		text(&f_ui22, lx, text_top(&f_ui22, ly), "Quick code", WB_COLOR_TEXT_HI, 0);
		text(&f_mono14, lx, text_top(&f_mono14, sy), "Type text, see the code. Not saved", WB_COLOR_TEXT_MID, 0);
		return;
	}
	icon(type_icon(S[i].type), x + 20, y + ROW_H / 2 - 13, ink, 255);
	{
		const char *tag = sym_tags[S[i].type];
		int tw = text_w(&f_mono14, tag, 0);

		text(&f_mono14, rx - tw, text_top(&f_mono14, y + ROW_H / 2), tag, ink, 0);
		rx -= tw + 18;
	}
	if (S[i].name[0]) {
		label_of(i, name, sizeof(name));
		text(&f_ui22, lx, text_top(&f_ui22, ly), wb_fit(&f_ui22, name, rx - lx, buf, sizeof(buf)),
		     WB_COLOR_TEXT_HI, 0);
		draw_units(S[i].t, S[i].n, lx, sy, rx - lx, &f_mono14, &f_mono13, WB_COLOR_TEXT_MID, 17, CHIP_YELLOW);
		return;
	}
	/* no name: the text itself, and how long it is */
	draw_units(S[i].t, S[i].n, lx, ly, rx - lx, &f_ui22, &f_mono13, WB_COLOR_TEXT_HI, 20, CHIP_YELLOW);
	name[0] = 0;
	cat_count(name, S[i].n, "character");
	text(&f_mono14, lx, text_top(&f_mono14, sy), name, WB_COLOR_TEXT_MID, 0);
}

static void list_hints(void)
{
	struct hint h[5];
	int n = 0;

	if (C.focus) {
		h[n++] = (struct hint){ "#1", "options", 0 };
		h[n++] = (struct hint){ "#2", "new", 0 };
		h[n++] = (struct hint){ "#3", "edit", 0 };
		h[n++] = (struct hint){ "Enter", "show", 0 };
	} else {
		h[n++] = (struct hint){ "#2", "new", 0 };
		h[n++] = (struct hint){ "Enter", "open", 0 };
	}
	h[n++] = (struct hint){ "Esc", "close", 1 };
	hint_bar(h, n);
}

static void draw_list(void)
{
	static const uint32_t note_c[3] = { WB_COLOR_OK, WB_COLOR_WARN, WB_COLOR_ERROR };
	static const char about[] = "Keep the codes you show often: a Wi-Fi login, a ticket, a product "
				    "number. Enter on one shows it for a scanner.";
	int ys[CODE_MAX + 1], hs[CODE_MAX + 1], top = list_top(), scroll, cut, y = 0;
	char buf[128], count[24] = "";

	cat_count(count, n_code, "code");
	header("APP", "BARbarious", count);
	if (C.notice[0])
		text(&f_ui18, HX, LIST_Y - 8, wb_fit(&f_ui18, C.notice, XW - 6, buf, sizeof(buf)), note_c[C.note], 0);
	if (!n_code) {	/* Quick code, and how to start on a card under it */
		int tx = X0 + 24, tw = RW - 48, lines = text_wrap(&f_ui20, 0, 0, tw, 28, about, 0, 4, 0);
		int ty = top + ROW_H + ROW_GAP;

		draw_row(-1, top, C.focus == 0);
		wb_card(X0, ty, RW, 24 + 30 + 14 + lines * 28 + 22 + 18 + 14 + 3 * 30 + 16, WB_RADIUS_MD, 0, 0);
		ty += 24;
		text(&f_ui22, tx, ty, "No codes yet.", WB_COLOR_TEXT_HI, 0);
		ty += 30 + 14;
		ty += 28 * text_wrap(&f_ui20, tx, ty, tw, 28, about, WB_COLOR_TEXT_MID, 4, 1);
		ty += 22;
		eyebrow(tx, ty, "TO START");
		ty += 18 + 14;
		text(&f_ui20, tx, ty, "1. Press the triangle key for a new code.", WB_COLOR_TEXT_MID, 0);
		ty += 30;
		text(&f_ui20, tx, ty, "2. Type its text; pick the type with the triangle.", WB_COLOR_TEXT_MID, 0);
		ty += 30;
		text(&f_ui20, tx, ty, "3. Enter saves it; Enter in the list shows it.", WB_COLOR_TEXT_MID, 0);
		list_hints();
		return;
	}
	for (int i = 0; i <= n_code; i++) {
		ys[i] = y;
		hs[i] = ROW_H;
		y += ROW_H + ROW_GAP;
	}
	scroll = list_scroll(ys, hs, n_code + 1, C.focus, C.focus, BOTTOM - top, &cut);
	clip_rows(scroll ? top : 0, y - ROW_GAP - scroll > BOTTOM - top ? top + cut : H);
	for (int i = 0; i <= n_code; i++) {
		int ry = top + ys[i] - scroll;

		if (ry + ROW_H <= top && scroll)
			continue;
		if (ry >= top + cut)
			break;
		draw_row(i - 1, ry, i == C.focus);
	}
	clip_end();
	scrollbar(X0 + XW - 4, top, BOTTOM - top, y - ROW_GAP, BOTTOM - top, scroll);
	list_hints();
}

/* ---- the show screen ---------------------------------------------------- */

static void draw_show(void)
{
	static const struct hint hints[] = { { "#1", "options", 0 }, { "#2", "type", 0 }, { "#3", "edit", 0 },
					     { "\xe2\x86\x90 \xe2\x86\x92", "next", 0 }, { "Esc", "back", 1 } };
	const struct code *c = &S[C.shown];
	const struct sym *s;
	char pos[24] = "", name[96], info[120];
	int px, y;

	cat_int(cat(cat_int(pos, C.shown + 1), " / "), n_code);
	label_of(C.shown, name, sizeof(name));
	header("BARBARIOUS \xe2\x80\xba", name, pos);
	y = draw_code(c->t, c->n, c->type, SQ_Y, SQ, WIDE_Y, c->type == T_PDF417 ? PDF_H : BAR_H, C.under, &px, &s);
	if (n_code > 1 && !is_wide(c->type)) {
		chevron(X0, SQ_Y + SQ / 2 - 10, WB_COLOR_TEXT_MID, 1);
		chevron(X0 + XW - 20, SQ_Y + SQ / 2 - 10, WB_COLOR_TEXT_MID, 0);
	}
	if (s) {
		sym_info(s, px, info);
		if (!is_wide(c->type) || c->type == T_PDF417) {
			int w = units_w(c->t, c->n, &f_mono16b, &f_mono13), mw = XW - 24;

			y += 16;
			draw_units(c->t, c->n, W / 2 - (w < mw ? w : mw) / 2, y + 10, mw, &f_mono16b, &f_mono13,
				   WB_COLOR_TEXT_MID, 18, CHIP_YELLOW);
			y += 20 + 6;
			text(&f_ui16, W / 2 - text_w(&f_ui16, info, 0) / 2, y, info, WB_COLOR_TEXT_MID, 0);
			y += 20;
		} else {
			y += 22;
			text(&f_ui16, HX, y, info, WB_COLOR_TEXT_MID, 0);
			y += 20;
		}
		if (is_wide(c->type) && y + 10 + 52 < H - WB_HINT_BAR_H)
			draw_ctl_note(s, c->t, c->n, y + 10);
	}
	hint_bar(hints, 5);
}

/* ---- Quick code ------------------------------------------------------------ */

static void draw_quick(void)
{
	static const struct hint hints[] = { { "#1", "insert", 0 }, { "#2", "type", 0 }, { "Enter", "save", 0 },
					     { "Esc", "back", 1 } };
	const struct sym *s;
	char info[120];
	int px, y;

	header("BARBARIOUS \xe2\x80\xba", "Quick code", sym_names[C.qtype]);
	draw_field(&QT, Q_TEXT_Y, 1, "");
	field_help(Q_HELP_Y);
	if (!QT.n) {
		text(&f_ui20, HX, Q_AREA_Y + 12, "The code shows here as you type.", WB_COLOR_PLACEHOLDER, 0);
	} else {
		y = draw_code(QT.t, QT.n, C.qtype, Q_AREA_Y, Q_AREA_H, Q_AREA_Y, C.qtype == T_PDF417 ? Q_AREA_H : 140,
			      0, &px, &s);
		if (s) {
			sym_info(s, px, info);
			text(&f_ui16, W / 2 - text_w(&f_ui16, info, 0) / 2, y + 12, info, WB_COLOR_TEXT_MID, 0);
		}
	}
	hint_bar(hints, 4);
}

/* ---- the editor ------------------------------------------------------------ */

/* how the code fits on the screen, as the editor's preview says */
static void draw_preview(void)
{
	const char *err;
	const struct sym *s = code_sym(C.etype, ET.t, ET.n, XW, PDF_H, &err);
	int bw = is_wide(C.etype) ? 300 : E_PREV_H, x = X0 + bw + 18, cy = E_PREV_Y + 20;
	char t[96] = "";

	if (!ET.n)
		return;
	box(X0, E_PREV_Y, bw, E_PREV_H, WB_RADIUS_SM, WHITE, 255, 0, 0, 0);
	if (!s) {
		text_wrap(&f_ui18, x, cy - 12, X0 + XW - x, 24, err, WB_COLOR_ERROR, 2, 1);
		return;
	}
	if (is_wide(C.etype))
		draw_scaled(s, X0 + 10, E_PREV_Y + 8, bw - 20, E_PREV_H - 16);
	else
		draw_scaled(s, X0 + 6, E_PREV_Y + 6, bw - 12, E_PREV_H - 12);
	{
		int px = fit_px(s, XW, is_wide(C.etype) ? (C.etype == T_PDF417 ? PDF_H : 1 << 20) : SQ);
		int m = s->w;
		uint32_t c = px >= 2 ? WB_COLOR_OK : px == 1 ? WB_COLOR_WARN : WB_COLOR_ERROR;
		const char *what = px >= 2 ? "Fits on the screen" : px == 1 ? "Small on the screen" :
				   "Too long for the screen";

		if (px >= 2) {
			tick(x, cy, c);
			text(&f_ui18, x + 26, text_top(&f_ui18, cy), what, c, 0);
		} else {
			text(&f_ui18, x, text_top(&f_ui18, cy), what, c, 0);
		}
		if (sym_is_2d(C.etype) && C.etype != T_PDF417)
			cat(cat_int(cat(cat_int(t, m), " by "), s->h), " modules, ");
		else
			cat(cat_count(t, m, "module"), ", ");
		if (px >= 1)
			cat(cat_int(t, px), px == 1 ? " px each: hard to scan" : " px each");
		else
			cat(t, "not one px each");
		text(&f_ui16, x, text_top(&f_ui16, cy + 24), t, WB_COLOR_TEXT_MID, 0);
	}
}

static void draw_edit(void)
{
	static const struct hint hints[] = { { "#1", "insert", 0 }, { "Enter", "save", 0 }, { "Esc", "cancel", 1 } };
	static const struct hint type_hints[] = { { "Enter", "choose", 0 }, { "\xe2\x86\x90 \xe2\x86\x92", "change", 0 },
						  { "Esc", "cancel", 1 } };
	char t[32] = "";

	header("BARBARIOUS \xe2\x80\xba", C.edit < 0 ? "New code" : (C.name[0] ? C.name : "Code"), 0);
	/* the name */
	text(&f_ui16, HX, E_NAME_LY, "Name", C.field == 0 ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER, 0);
	wb_card(X0, E_NAME_Y, XW, E_NAME_H, WB_RADIUS_MD, C.field == 0, &GLOW_FOCUS);
	{
		int from = 0, cx, max = XW - 2 * TF_PAD - 4, cy = E_NAME_Y + E_NAME_H / 2;
		char v[NM_MAX + 1];

		for (;;) {	/* characters off the front until the cursor shows */
			memcpy(v, C.name + from, (size_t)(C.name_cur - from));
			v[C.name_cur - from] = 0;
			cx = text_w(&f_ui20, v, 0);
			if (cx <= max || from >= C.name_cur)
				break;
			from++;
		}
		if (!C.name[0] && C.field != 0)
			text(&f_ui18, X0 + TF_PAD, text_top(&f_ui18, cy), "Optional: shown in the list", WB_COLOR_PLACEHOLDER, 0);
		text(&f_ui20, X0 + TF_PAD, text_top(&f_ui20, cy), C.name + from, WB_COLOR_TEXT_HI, 0);
		if (C.field == 0)
			fill(X0 + TF_PAD + cx + 1, cy - 13, 2, 26, WB_COLOR_CYAN);
	}
	/* the text */
	text(&f_ui16, HX, E_TEXT_LY, "Text", C.field == 1 ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER, 0);
	cat_count(t, ET.n, "character");
	text(&f_mono14, X0 + XW - 6 - text_w(&f_mono14, t, 0), E_TEXT_LY + 2, t, WB_COLOR_TEXT_MID, 0);
	draw_field(&ET, E_TEXT_Y, C.field == 1, "What the code holds");
	field_help(E_HELP_Y);
	/* the type */
	{
		int cy = E_TYPE_Y + E_TYPE_H / 2, rx = X0 + XW - 18 - 20;
		const char *v = sym_names[C.etype];

		wb_card(X0, E_TYPE_Y, XW, E_TYPE_H, WB_RADIUS_MD, C.field == 2, &GLOW_FOCUS);
		chevron(rx, cy - 10, C.field == 2 ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
		rx -= 16 + text_w(&f_mono14, v, 0);
		text(&f_mono14, rx, text_top(&f_mono14, cy), v, C.field == 2 ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
		text(&f_ui20, X0 + TF_PAD, text_top(&f_ui20, cy), "Type", WB_COLOR_TEXT_HI, 0);
	}
	draw_preview();
	hint_bar(C.field == 2 ? type_hints : hints, 3);
}

/* ---- log, settings -------------------------------------------------------- */

static void draw_log(void)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int y = BODY_Y, n = C.n_log;
	char buf[128];

	header("BARBARIOUS \xe2\x80\xba", "Log", 0);
	if (!n) {
		wb_card(X0, y, XW, ROW_H, WB_RADIUS_MD, 0, 0);
		text(&f_ui20, X0 + 20, text_top(&f_ui20, y + ROW_H / 2),
		     wb_fit(&f_ui20, "No codes shown yet. They show here until the app closes.", XW - 40, buf,
			    sizeof(buf)), WB_COLOR_TEXT_MID, 0);
		hint_bar(hints, 1);
		return;
	}
	if (n > (BOTTOM - y - 28) / 30)
		n = (BOTTOM - y - 28) / 30;
	wb_card(X0, y, XW, 14 + n * 30 + 14, WB_RADIUS_MD, 0, 0);
	y += 14;
	for (int i = C.n_log - 1; i >= C.n_log - n; i--, y += 30)
		text(&f_mono14, X0 + 20, text_top(&f_mono14, y + 15), wb_fit(&f_mono14, C.log[i], XW - 40, buf, sizeof(buf)),
		     i == C.n_log - 1 ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	hint_bar(hints, 1);
}

static const char *const ecc_names[4] = { "L \xc2\xb7 7 %", "M \xc2\xb7 15 %", "Q \xc2\xb7 25 %", "H \xc2\xb7 30 %" };
static const char *const se_names[3] = { "CR", "LF", "CR LF" };

static int set_h(int i)
{
	return i == 2 ? 84 : ROW_H;
}

static void draw_settings(void)
{
	static const struct hint hints[] = { { "\xe2\x86\x90 \xe2\x86\x92", "adjust", 0 }, { "Esc", "back", 1 } };
	static const char *const labels[N_SET] = { "Type for a new code", "QR error correction", "Quiet zone",
						   "Shift+Enter types", "Text under the code",
						   "Full brightness while showing" };
	static const char *const notes[N_SET] = {
		"The type a new code starts with; the triangle changes it.",
		"More correction reads through glare and scratches, but makes the code bigger.",
		"White border around a 2D code. 1D codes get at least 10.",
		"What Shift+Enter puts in a text. Tab is always TAB.",
		"The text in letters under a barcode.",
		"A scanner reads a bright screen best. Back to normal when you leave the code.",
	};
	int y = BODY_Y;

	header("BARBARIOUS \xe2\x80\xba", "Settings", 0);
	for (int i = 0; i < N_SET; i++) {
		int h = set_h(i), on = C.set_focus == i;

		if (i >= 4) {		/* a switch */
			wb_row(X0, y, XW, h, 0, labels[i], 0, 0, WB_ROW_SWITCH, i == 4 ? C.under : C.bright, on);
		} else if (i == 2) {	/* the quiet zone: a slider, as the menu's */
			int cy = y + 30, rx = X0 + XW - 20, tx = X0 + 20, tw = XW - 40, ty = y + 60;
			int kx = tx + tw * (C.quiet - 1) / 15;
			uint32_t c = on ? WB_COLOR_CYAN : WB_COLOR_PLACEHOLDER;
			char v[16] = "";

			wb_card(X0, y, XW, h, WB_RADIUS_MD, on, &GLOW_FOCUS);
			text(&f_ui22, X0 + 20, text_top(&f_ui22, cy), labels[i], WB_COLOR_TEXT_HI, 0);
			cat(cat_int(v, C.quiet), C.quiet == 1 ? " module" : " modules");
			text(&f_mono14, rx - text_w(&f_mono14, v, 0), text_top(&f_mono14, cy), v,
			     on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
			box(tx, ty - 4, tw, 8, 4, 0xFFFFFF, 46, 0, 0, 0);
			if (kx > tx)
				box(tx, ty - 4, kx - tx, 8, 4, c, 255, 0, 0, 0);
			box(kx - 9, ty - 9, 18, 18, 4, WB_COLOR_TEXT_HI, 255, c, 3, 0);
		} else {
			const char *v = i == 0 ? sym_names[C.def_type] : i == 1 ? ecc_names[C.ecc] : se_names[C.shift_enter];

			wb_row(X0, y, XW, h, 0, labels[i], 0, v, WB_ROW_OPEN, 0, on);
		}
		y += h + 6;
	}
	text_wrap(&f_ui16, HX, y + 4, XW - 6, 20, notes[C.set_focus], WB_COLOR_TEXT_MID, 2, 1);
	hint_bar(hints, 2);
}

/* ---- pop-ups ------------------------------------------------------------- */

/* a key cap as the design's help has it: raised a little */
static int help_cap(int x, int cy, const char *keys)
{
	int sym = one_sym(keys), w = (sym ? 12 : text_w(&f_mono14, keys, 0)) + 18;

	box(x, cy - 12, w, 25, 6, WB_COLOR_SURFACE1, 255, WB_COLOR_LINE, 1, 0);
	fill(x + 6, cy + 11, w - 12, 1, WB_COLOR_LINE);
	if (sym)
		icon(keycap_syms[sym - 1], x + 9, cy - 6, key_colors[sym - 1], 255);
	else
		text(&f_mono14, x + 9, text_top(&f_mono14, cy), keys, WB_COLOR_TEXT_HI, 0);
	return x + w;
}

/* The help (diamond key), as the design draws it: the screen's name, what
 * it is for, and its keys with a line between them; a frosted pop-up with
 * its own key at its bottom. */
static void help_popup2(const char *title, const char *about, const struct help_key *k, int n)
{
	static const struct hint hints[] = { { "Esc", "close", 1 } };
	int x = POP_X + 32, tw = POP_W - 64, lines = text_wrap(&f_ui18, 0, 0, tw, 25, about, 0, 3, 0);
	int h = 26 + 18 + 6 + 36 + 6 + lines * 25 + 16 + 17 + 8 + n * HELP_ROW + 8 + 48 + 12;
	int y = (H - h) / 2;
	char buf[96];

	dim(WB_DIM);
	wb_panel(POP_X, y, POP_W, h, WB_RADIUS_LG);
	y += 26;
	icon(&kc_diamond, x, y + 1, WB_COLOR_KEY_DIAMOND, 255);
	eyebrow(x + 12 + 8, y, "HELP");
	text(&f_ui28b, x, y + 18 + 6 + 2, wb_fit(&f_ui28b, title, tw, buf, sizeof(buf)), WB_COLOR_TEXT_HI, 0);
	y += 18 + 6 + 36 + 6;
	text_wrap(&f_ui18, x, y, tw, 25, about, WB_COLOR_TEXT_MID, 3, 1);
	y += lines * 25 + 16;
	text(&f_mono13, x, y, "KEYS ON THIS SCREEN", WB_COLOR_PLACEHOLDER, 21);
	y += 17 + 8;
	for (int i = 0; i < n; i++, y += HELP_ROW) {
		int cy = y + HELP_ROW / 2;

		if (i)
			fill(x, y, tw, 1, 0x26262F);
		help_cap(x, cy, k[i].keys);
		text(&f_ui18, x + 80, text_top(&f_ui18, cy), wb_fit(&f_ui18, k[i].what, tw - 80, buf, sizeof(buf)),
		     WB_COLOR_TEXT_HI, 0);
	}
	panel_hints(x, tw, y + 8, hints, 1);
}

static void draw_help(void)
{
	static const struct help_key list_keys[] = {
		{ "Enter", "Show the code full screen" },
		{ "#1", "Options: show, edit, type, move, delete" },
		{ "#2", "New code" },
		{ "#3", "Edit the code" },
		{ "#4", "Codes shown this session" },
		{ "#5", "Settings: type, error correction, quiet zone" },
		{ "\xe2\x86\x90 \xe2\x86\x92", "In a code: the previous or next one" },
		{ "Esc", "Close BARbarious" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key show_keys[] = {
		{ "\xe2\x86\x90 \xe2\x86\x92", "The previous or next code" },
		{ "#1", "Options: edit, type, move, delete" },
		{ "#2", "Barcode type" },
		{ "#3", "Edit the code" },
		{ "#4", "Codes shown this session" },
		{ "#5", "Settings: brightness, quiet zone" },
		{ "Esc", "Back to the list" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key edit_keys[] = {
		{ "Enter", "Save" },
		{ "Tab", "In the text: TAB" },
		{ "Shift", "With Enter: CR (or LF: settings)" },
		{ "Ctrl", "With a letter: ^A to ^Z; [ ] for ESC, GS" },
		{ "#1", "Insert a control character or FNC1" },
		{ "#2", "Barcode type" },
		{ "\xe2\x86\x91\xe2\x86\x93", "A line up or down; the fields" },
		{ "\xe2\x86\x90\xe2\x86\x92", "Move the cursor (Fn: the line's ends)" },
		{ "Esc", "Cancel: the code stays as it was" },
	};
	static const struct help_key quick_keys[] = {
		{ "Enter", "Save it as a code of the list" },
		{ "Tab", "TAB" },
		{ "Shift", "With Enter: CR (or LF: settings)" },
		{ "Ctrl", "With a letter: ^A to ^Z" },
		{ "#1", "Insert a control character or FNC1" },
		{ "#2", "Barcode type" },
		{ "Esc", "Back to the list; the text stays" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key set_keys[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "Choose a setting" },
		{ "\xe2\x86\x90 \xe2\x86\x92", "Change it" },
		{ "Enter", "The next value" },
		{ "Esc", "Back" },
		{ "Logo", "Back to the menu" },
	};
	static const struct help_key log_keys[] = {
		{ "Esc", "Back" },
		{ "Logo", "Back to the menu" },
	};

	switch (C.sc) {
	case SC_SHOW:
		help_popup2("Show code", "As big as the screen allows, at full brightness; the screen stays on "
			    "while it shows, up to 10 minutes after a key.", show_keys, 8);
		break;
	case SC_EDIT:
		help_popup2(C.edit < 0 ? "New code" : "Edit code", "The name is only for the list. The text is "
			    "what the code holds, control characters included.", edit_keys, 9);
		break;
	case SC_QUICK:
		help_popup2("Quick code", "The code of a text as you type it. It is not saved; Enter makes it a "
			    "code of the list.", quick_keys, 8);
		break;
	case SC_SETTINGS:
		help_popup2("Settings", "Kept on the badge, in flash.", set_keys, 5);
		break;
	case SC_LOG:
		help_popup2("Log", "The codes shown since the app started.", log_keys, 2);
		break;
	default:
		help_popup2("BARbarious", "Turns your texts into barcodes and QR Codes, control characters "
			    "included, for a scanner to read off the screen.", list_keys, 9);
	}
}

/*
 * The type pop-up's frame, as menu_popup()'s (wbui.h): a frosted panel
 * POP_X from the sides, the eyebrow (the screen it opens from, with a ›),
 * the title with the count on its line, a line about the list where
 * menu_popup() has its filter, room for rows of rows_h, and the keys at
 * the bottom. The y the rows start at; *x0, *w their box.
 */
#define BP_ROW		PK_ROW
#define BP_GAP		PK_GAP

static int pop_frame2(const char *eb_s, const char *title, const char *count, const char *sub,
		      int rows_h, const struct hint *hs, int nh, int *x0, int *w)
{
	int h = 22 + 55 + 14 + 20 + 14 + rows_h + 14 + 1 + 10 + 23 + 16, y = (H - h) / 2, top;
	char buf[96], eb[48];

	*x0 = POP_X + 22;
	*w = POP_W - 44;
	dim(WB_DIM);
	wb_panel(POP_X, y, POP_W, h, WB_RADIUS_LG);
	strncpy(eb, eb_s, sizeof(eb) - 5);
	eb[sizeof(eb) - 5] = 0;
	strcat(eb, " \xe2\x80\xba");
	text(&f_mono13, *x0, y + 22, eb, WB_COLOR_CYAN, 21);
	text(&f_ui28b, *x0, y + 22 + 17 + 4, wb_fit(&f_ui28b, title, *w - 140, buf, sizeof(buf)), WB_COLOR_TEXT_HI, -4);
	text(&f_mono14, *x0 + *w - text_w(&f_mono14, count, 0), y + 22 + 55 - 20, count, WB_COLOR_TEXT_MID, 0);
	top = y + 22 + 55 + 14;
	text(&f_ui16, *x0, top, sub, WB_COLOR_PLACEHOLDER, 0);
	top += 20 + 14;
	panel_hints(*x0, *w, top + rows_h + 14, hs, nh);
	return top;
}

static void type_target(const unsigned short **t, int *n, int **type)
{
	if (C.type_for == TF_QUICK) {
		*t = QT.t;
		*n = QT.n;
		*type = &C.qtype;
	} else if (C.type_for == TF_EDIT) {
		*t = ET.t;
		*n = ET.n;
		*type = &C.etype;
	} else {
		*t = S[C.menu_for].t;
		*n = S[C.menu_for].n;
		*type = &S[C.menu_for].type;
	}
}

static void draw_type_pop(void)
{
	static const struct hint hs[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };
	const unsigned short *t;
	int n, *type, ys[N_TYPES], scroll, top, x0, w;
	int list_h = PK_VIS * (BP_ROW + BP_GAP) - BP_GAP;
	char eb[NM_MAX + 8];

	type_target(&t, &n, &type);
	if (C.type_for == TF_QUICK)
		strcpy(eb, "QUICK CODE");
	else if (C.type_for == TF_EDIT)
		strcpy(eb, C.name[0] ? C.name : C.edit < 0 ? "NEW CODE" : "CODE");
	else
		label_of(C.menu_for, eb, NM_MAX);
	for (char *p = eb; *p; p++)
		*p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	top = pop_frame2(eb, "Barcode type", "10 options", "Right: what this text needs from the type", list_h, hs,
			 2, &x0, &w);
	for (int i = 0; i < N_TYPES; i++)
		ys[i] = i * (BP_ROW + BP_GAP);
	/* as menu_popup(): by whole rows, only rows that fit whole, so none
	 * is cut off against the note above or the line below; the focus's
	 * box with room for its glow */
	scroll = 0;
	if (C.pf >= 0 && C.pf < N_TYPES) {
		for (int f = 0; f < C.pf && ys[C.pf] + BP_ROW - ys[f] > list_h; f++)
			scroll = ys[f + 1];
		clip_rows(top - 14, top + list_h + 14);
		box_shaped(x0, top + ys[C.pf] - scroll, w, BP_ROW, 10, WB_COLOR_CYAN, 56, WB_COLOR_CYAN, 2, &GLOW_POP);
		clip_end();
	}
	clip_rows(top, top + list_h);
	for (int i = 0; i < N_TYPES; i++) {
		int ry = top + ys[i] - scroll, cy = ry + BP_ROW / 2, on = i == C.pf, x = x0 + 14;
		uint32_t nc = C.need[i] == NEED_OK ? (on ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID) :
			      C.need[i] == NEED_WARN ? WB_COLOR_WARN : WB_COLOR_ERROR;
		const char *note = C.need_note[i];

		if (ry < top || ry + BP_ROW > top + list_h)
			continue;
		if (i == *type) {	/* the present type's box, ticked */
			box(x, cy - 12, 24, 24, 4, WB_COLOR_CYAN, 255, 0, 0, 0);
			tick(x + 4, cy, WB_COLOR_VOID);
		} else {
			edge_rect(x, cy - 12, 24, 24, 0xFFFFFF, 56);
			edge_rect(x + 1, cy - 11, 22, 22, 0xFFFFFF, 56);
		}
		x += 24 + 14;
		x = text(&f_ui20, x, text_top(&f_ui20, cy), sym_names[i], WB_COLOR_TEXT_HI, 0);
		text(&f_ui16, x + 12, text_top(&f_ui16, cy), sym_is_2d(i) ? "2D" : "1D", WB_COLOR_TEXT_MID, 0);
		text(&f_mono14, x0 + w - 14 - text_w(&f_mono14, note, 0), text_top(&f_mono14, cy), note, nc, 0);
	}
	clip_end();
	scrollbar(x0 + w + 9, top, list_h, N_TYPES * (BP_ROW + BP_GAP) - BP_GAP, list_h, scroll);
}

/* The control characters and FNC1 to insert: libwb's pop-up list with
 * its filter field; the caret and the code on the right. */
static void draw_insert_pop(void)
{
	static const struct hint hs[] = { { "Enter", "insert", 0 }, { "A\xe2\x80\x93Z", "filter", 0 },
					  { "Esc", "cancel", 1 } };
	static struct menu_item it[N_CTL];
	char count[24] = "", eb[24];

	memset(it, 0, sizeof(it));
	for (int i = 0; i < N_CTL; i++) {
		char *v = it[i].value;

		it[i].label = ctl_name[i];
		it[i].sub = ctl_desc[i];
		ctl_caret(i, v);
		if (i < 33) {
			int u = ctl_unit(i);

			v[2] = ' ';
			v[3] = ' ';
			v[4] = hexd[u >> 4];
			v[5] = hexd[u & 15];
			v[6] = 0;
		} else {
			strcpy(v, "GS1");
		}
	}
	cat_count(count, C.n_imap, "option");
	strcpy(eb, C.sc == SC_QUICK ? "QUICK CODE" : C.edit < 0 ? "NEW CODE" : "EDIT CODE");
	menu_popup(eb, "Control character", count, it, C.imap, C.n_imap, C.pf, -2, C.filter,
		   "Type a name or ^ and a letter", hs, 3);
}

static void draw_pop(void)
{
	static const struct hint menu_h[] = { { "Enter", "do", 0 }, { "Esc", "cancel", 1 } };
	static const struct hint ask[] = { { "\xe2\x86\x91\xe2\x86\x93", "select", 0 }, { "Enter", "confirm", 0 },
					   { "Esc", "cancel", 1 } };
	static const char *const mlabel[] = { "Show", "Edit", "Barcode type", "Move up", "Move down", "Delete" };
	static const char *const keep_del[2] = { "Keep it", "Delete" };
	static const struct wb_icon *const keep_del_ic[2] = { &r_check, &r_trash };
	struct menu_item it[6];
	char name[96], buf[96];

	memset(it, 0, sizeof(it));
	switch (C.pop) {
	case POP_HELP:
		draw_help();
		break;
	case POP_TYPE:
		draw_type_pop();
		break;
	case POP_INSERT:
		draw_insert_pop();
		break;
	case POP_MENU:
		label_of(C.menu_for, name, sizeof(name));
		for (int i = 0; i < C.n_menu; i++) {
			it[i].label = mlabel[C.menu[i]];
			it[i].danger = it[i].sep = C.menu[i] == M_DELETE;
		}
		menu_popup("BARBARIOUS", name, 0, it, 0, C.n_menu, C.pf, -2, 0, 0, menu_h, 2);
		break;
	case POP_DELETE: {	/* a question: the dialog, the destructive choice red */
		int y;

		label_of(C.menu_for, name, sizeof(name));
		y = dialog("BARBARIOUS", "Delete this code?", 28, keep_del, keep_del_ic, 2, C.pf, 1);
		text(&f_ui20, DLG_X + 32, y, wb_fit(&f_ui20, name, DLG_W - 64, buf, sizeof(buf)), WB_COLOR_TEXT_MID, 0);
		dlg_hints(ask, 3);
		break;
	}
	}
}

/* The screen, and the pop-up over it. It only draws: present() runs it
 * once more than the bands it draws (the first time to record it). */
static void paint(void *unused)
{
	(void)unused;
	switch (C.sc) {
	case SC_LIST:
		draw_list();
		break;
	case SC_SHOW:
		draw_show();
		break;
	case SC_EDIT:
		draw_edit();
		break;
	case SC_QUICK:
		draw_quick();
		break;
	case SC_LOG:
		draw_log();
		break;
	case SC_SETTINGS:
		draw_settings();
		break;
	}
	/* a pop-up is frosted when the frame is whole, else (drawn a band
	 * at a time) libwb draws it as darker unblurred glass */
	draw_pop();
}

/* ---- output: the panel, or a PPM on the host ---------------------------- */

#define BAND_H	40
static uint16_t band[W * BAND_H];
#ifdef BB_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif
static long tm_out;		/* us spent writing to the panel, this frame */

/* rows y0..y1 of the picture in p (whole, or the band) to the panel */
static void put_rows(const uint16_t *p, int y0, int y1)
{
	long t = wb_now_us();

#ifdef BB_HOST
	if (p != screen + y0 * W)
		memcpy(screen + y0 * W, p, (size_t)(y1 - y0) * W * 2);
#else
	ssize_t r = pwrite(fb_fd, p, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);

	(void)r;		/* nothing to do about a frame not written */
#endif
	tm_out += wb_now_us() - t;
}

/*
 * BARBARIOUS_UI_TIMING=1: a line per frame on stderr (the app's log): how
 * it was drawn, the bands drawn, and the time in us, of which libwb's
 * scene and glass and the writing out.
 */
static void timing(long t0, const char *how, int bands)
{
	static int on = -1;
	char line[160];

	if (on < 0)
		on = getenv("BARBARIOUS_UI_TIMING") != 0;
	if (on) {
		line[0] = 0;
		cat(cat_int(cat(cat_int(cat(cat_int(cat(cat_int(cat(cat_int(cat(cat(cat(line,
			"barbarious: frame "), how), ", "), bands), " bands, "), (int)(wb_now_us() - t0)),
			" us, scene "), (int)tm_scene), ", glass "), (int)tm_glass), ", out "), (int)tm_out), "\n");
		if (write(2, line, strlen(line)) < 0)
			on = 0;
	}
	tm_scene = tm_glass = tm_out = 0;
}

/*
 * The screen to the panel: recorded, then only the bands that changed
 * since the last frame drawn (libwb): into the whole picture when there
 * is one (a pop-up's blur kept while only its focus moves), else a band
 * at a time. A focus that moves draws its two rows and the hint bar; a
 * character typed the field's lines and the code, which changed.
 */
static void present(void)
{
	long t0 = wb_now_us();
	int n;

	if (whole)
		n = wb_frame(paint, 0, whole, BAND_H, put_rows);
	else
		n = wb_bands(paint, 0, band, BAND_H, put_rows);
	timing(t0, whole ? "whole" : "bands", n);
}

/* ---- keys --------------------------------------------------------------- */

static void log_shown(void)
{
	char t[96], name[48];

	label_of(C.shown, name, 40);
	t[0] = 0;
	cat(cat(cat(t, name), " \xc2\xb7 "), sym_names[S[C.shown].type]);
	log_add(t);
}

static void show(int i)
{
	C.shown = i;
	C.notice[0] = 0;
	log_shown();
	go(SC_SHOW);
}

static void open_edit(int i, int back)
{
	C.edit = i;
	C.edit_back = back;
	C.field = i < 0 ? 0 : 1;
	strcpy(C.name, i < 0 ? "" : S[i].name);
	C.name_cur = (int)strlen(C.name);
	ET.n = i < 0 ? 0 : S[i].n;
	if (i >= 0)
		memcpy(ET.t, S[i].t, sizeof(ET.t[0]) * (size_t)ET.n);
	ET.cur = ET.n;
	ET.top = 0;
	C.etype = i < 0 ? C.def_type : S[i].type;
	C.err = 0;
	field_scroll(&ET);
	go(SC_EDIT);
}

static void save_edit(void)
{
	struct code *c;

	if (!ET.n) {
		C.err = "Type a text first";
		C.field = 1;
		return;
	}
	if (C.edit < 0) {
		if (n_code == CODE_MAX) {
			C.err = "The list is full (64 codes)";
			return;
		}
		C.edit = n_code++;
	}
	c = &S[C.edit];
	strcpy(c->name, C.name);
	c->type = C.etype;
	c->n = ET.n;
	memcpy(c->t, ET.t, sizeof(c->t[0]) * (size_t)ET.n);
	C.focus = C.edit + 1;
	C.notice[0] = 0;
	if (C.edit_back == SC_SHOW) {
		C.shown = C.edit;
		go(SC_SHOW);
	} else {
		go(SC_LIST);
	}
	if (!save()) {
		strcpy(C.notice, "Saved");
		C.note = NOTE_OK;
	}
}

static void delete_code(int i)
{
	memmove(S + i, S + i + 1, sizeof(S[0]) * (size_t)(n_code - i - 1));
	n_code--;
	if (C.focus > n_code)
		C.focus = n_code;
	if (C.sc == SC_SHOW) {
		if (!n_code)
			go(SC_LIST);
		else if (C.shown >= n_code)
			C.shown = n_code - 1;
	}
	if (!save()) {
		strcpy(C.notice, "Deleted");
		C.note = NOTE_OK;
	}
}

static void move_code(int i, int dir)
{
	struct code t;
	int j = i + dir;

	if (j < 0 || j >= n_code)
		return;
	t = S[i];
	S[i] = S[j];
	S[j] = t;
	C.focus = j + 1;
	if (C.sc == SC_SHOW)
		C.shown = j;
	save();
}

static void open_menu(int i)
{
	C.menu_for = i;
	C.n_menu = 0;
	if (C.sc != SC_SHOW)
		C.menu[C.n_menu++] = M_SHOW;
	C.menu[C.n_menu++] = M_EDIT;
	C.menu[C.n_menu++] = M_TYPE;
	if (i > 0)
		C.menu[C.n_menu++] = M_UP;
	if (i < n_code - 1)
		C.menu[C.n_menu++] = M_DOWN;
	C.menu[C.n_menu++] = M_DELETE;
	C.pop = POP_MENU;
	C.pf = 0;
}

/* The type pop-up, for the editor, Quick code or code menu_for: what its
 * text needs of each type, and whether it fits one at all. */
static void open_type(int what)
{
	const unsigned short *t;
	int n, *type;

	C.type_for = what;
	type_target(&t, &n, &type);
	for (int i = 0; i < N_TYPES; i++) {
		const char *err;

		C.need[i] = sym_needs(i, t, n, &C.need_note[i]);
		if (C.need[i] != NEED_BAD && n && sym_make(&SCRATCH, i, t, n, C.ecc, XW, PDF_H, &err) < 0) {
			C.need[i] = NEED_BAD;
			C.need_note[i] = "too long";
		}
	}
	C.pop = POP_TYPE;
	C.pf = *type;
}

/* The insert pop-up's rows: what matches the filter, as a name ("GS"),
 * a word of its description ("group"), or ^ and a letter ("^]"). */
static void insert_filter(void)
{
	C.n_imap = 0;
	for (int i = 0; i < N_CTL; i++) {
		int ok = !C.filter[0];
		char caret[4];

		ctl_caret(i, caret);
		if (C.filter[0] == '^')
			ok = caret[0] && prefix_ci(caret, C.filter);
		else if (C.filter[0]) {
			ok = prefix_ci(ctl_name[i], C.filter);
			for (const char *d = ctl_desc[i]; *d && !ok; d++)
				if (d == ctl_desc[i] || d[-1] == ' ')
					ok = prefix_ci(d, C.filter);
		}
		if (ok)
			C.imap[C.n_imap++] = i;
	}
	C.pf = 0;
}

static void open_insert(void)
{
	C.filter[0] = 0;
	insert_filter();
	C.pf = 9;			/* TAB */
	C.pop = POP_INSERT;
}

static struct field *cur_field(void)
{
	return C.sc == SC_QUICK ? &QT : &ET;
}

static int insert_key(int code)
{
	int len = (int)strlen(C.filter);
	char ch;

	switch (code) {
	case KEY_ESC:
		C.pop = POP_NONE;
		return 3;
	case KEY_UP:
		C.pf -= C.pf > 0;
		return 3;
	case KEY_DOWN:
		C.pf += C.pf < C.n_imap - 1;
		return 3;
	case KEY_BACKSPACE:
		if (len) {
			C.filter[len - 1] = 0;
			insert_filter();
		}
		return 3;
	case KEY_ENTER:
		if (C.n_imap) {
			field_put(cur_field(), ctl_unit(C.imap[C.pf]));
			field_scroll(cur_field());
			C.pop = POP_NONE;
		}
		return 3;
	}
	ch = key_char(code, K.shift);
	if (!ch || ch == ' ' || len >= (int)sizeof(C.filter) - 1)
		return 0;
	C.filter[len] = ch;
	C.filter[len + 1] = 0;
	insert_filter();
	return 3;
}

/* The pop-ups' keys. */
static int pop_key(int code)
{
	int n = C.pop == POP_MENU ? C.n_menu : C.pop == POP_TYPE ? N_TYPES : 2, pick = C.pf;

	if (C.pop == POP_INSERT)
		return insert_key(code);
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
		case M_SHOW:
			show(C.menu_for);
			break;
		case M_EDIT:
			open_edit(C.menu_for, C.sc);
			break;
		case M_TYPE:
			open_type(TF_CODE);
			break;
		case M_UP:
			move_code(C.menu_for, -1);
			break;
		case M_DOWN:
			move_code(C.menu_for, 1);
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
			delete_code(C.menu_for);
		break;
	case POP_TYPE: {
		const unsigned short *t;
		int nn, *type;

		type_target(&t, &nn, &type);
		*type = pick;
		C.pop = POP_NONE;
		if (C.type_for == TF_CODE) {
			if (C.sc == SC_SHOW)
				log_shown();
			save();
		}
		break;
	}
	}
	return 3;
}

static int list_key(int code)
{
	switch (code) {
	case KEY_ESC:
		return 1;
	case KEY_UP:
		if (C.focus > 0)
			C.focus--;
		break;
	case KEY_DOWN:
		if (C.focus < n_code)
			C.focus++;
		break;
	case KEY_ENTER:
		C.notice[0] = 0;
		if (C.focus) {
			show(C.focus - 1);
		} else {
			QT.cur = QT.n;
			C.err = 0;
			field_scroll(&QT);
			go(SC_QUICK);
		}
		return 3;
	case KEY_SYM_MENU:
		if (C.focus)
			open_menu(C.focus - 1);
		return 3;
	case KEY_SYM_MAIN:
		open_edit(-1, SC_LIST);
		return 3;
	case KEY_SYM_SECOND:
		if (C.focus)
			open_edit(C.focus - 1, SC_LIST);
		return 3;
	default:
		return 0;
	}
	C.notice[0] = 0;
	return 3;
}

static int show_key(int code)
{
	switch (code) {
	case KEY_LEFT:
	case KEY_RIGHT:
		if (n_code < 2)
			return 0;
		C.shown = (C.shown + (code == KEY_LEFT ? n_code - 1 : 1)) % n_code;
		log_shown();
		return 3;
	case KEY_SYM_MENU:
		open_menu(C.shown);
		return 3;
	case KEY_SYM_MAIN:
		C.menu_for = C.shown;
		open_type(TF_CODE);
		return 3;
	case KEY_SYM_SECOND:
		open_edit(C.shown, SC_SHOW);
		return 3;
	case KEY_ESC:
		C.focus = C.shown + 1;
		go(SC_LIST);
		return 3;
	}
	return 0;
}

static int quick_key(int code)
{
	int r;

	switch (code) {
	case KEY_SYM_MENU:
		open_insert();
		return 3;
	case KEY_SYM_MAIN:
		open_type(TF_QUICK);
		return 3;
	case KEY_ESC:
		C.err = 0;
		go(SC_LIST);
		return 3;
	case KEY_ENTER:
		if (K.shift)
			break;
		if (!QT.n) {
			C.err = "Type a text first";
			return 2;
		}
		open_edit(-1, SC_QUICK);
		ET = QT;
		ET.cur = ET.n;
		ET.top = 0;
		C.etype = C.qtype;
		field_scroll(&ET);
		return 3;
	}
	r = field_key(&QT, code);
	if (r > 0)
		field_scroll(&QT);
	return r > 0 ? 2 : 0;
}

/* The editor's keys: 2 when only its body changed, 3 for the screen. */
static int edit_key(int code)
{
	int r;
	char ch;

	switch (code) {
	case KEY_SYM_MENU:
		C.field = 1;
		open_insert();
		return 3;
	case KEY_SYM_MAIN:
		open_type(TF_EDIT);
		return 3;
	case KEY_ESC:
		C.err = 0;
		go(C.edit_back);
		return 3;
	}
	if (C.field == 2) {
		switch (code) {
		case KEY_LEFT:
			C.etype = (C.etype + N_TYPES - 1) % N_TYPES;
			return 2;
		case KEY_RIGHT:
			C.etype = (C.etype + 1) % N_TYPES;
			return 2;
		case KEY_ENTER:
			open_type(TF_EDIT);
			return 3;
		case KEY_UP:
			C.field = 1;
			return 3;
		}
		return 0;
	}
	if (C.field == 1) {
		if (code == KEY_ENTER && !K.shift) {
			save_edit();
			return 3;
		}
		r = field_key(&ET, code);
		if (r < 0) {
			C.field = code == KEY_UP ? 0 : 2;
			return 3;
		}
		if (r)
			field_scroll(&ET);
		return r ? 2 : 0;
	}
	/* the name */
	switch (code) {
	case KEY_ENTER:
		save_edit();
		return 3;
	case KEY_TAB:
	case KEY_DOWN:
		C.field = 1;
		return 3;
	case KEY_BACKSPACE: {
		int a = K.fn ? C.name_cur : C.name_cur - 1, len = (int)strlen(C.name);

		if (a >= 0 && a < len) {
			memmove(C.name + a, C.name + a + 1, (size_t)(len - a));
			C.name_cur = a;
		}
		return 2;
	}
	case KEY_LEFT:
		C.name_cur = K.fn ? 0 : C.name_cur - (C.name_cur > 0);
		return 2;
	case KEY_RIGHT:
		C.name_cur = K.fn ? (int)strlen(C.name) : C.name_cur + (C.name[C.name_cur] != 0);
		return 2;
	}
	ch = key_char(code, K.shift);
	if (!ch || K.ctrl || (int)strlen(C.name) >= NM_MAX)
		return 0;
	memmove(C.name + C.name_cur + 1, C.name + C.name_cur, strlen(C.name + C.name_cur) + 1);
	C.name[C.name_cur++] = ch;
	return 3;		/* the title shows the name too */
}

static int settings_key(int code)
{
	int d = code == KEY_LEFT ? -1 : 1;

	switch (code) {
	case KEY_ESC:
		go(C.back);
		return 3;
	case KEY_UP:
		C.set_focus -= C.set_focus > 0;
		return 3;
	case KEY_DOWN:
		C.set_focus += C.set_focus < N_SET - 1;
		return 3;
	case KEY_LEFT:
	case KEY_RIGHT:
	case KEY_ENTER:
		break;
	default:
		return 0;
	}
	switch (C.set_focus) {
	case 0:
		C.def_type = (C.def_type + d + N_TYPES) % N_TYPES;
		break;
	case 1:
		C.ecc = code == KEY_ENTER ? (C.ecc + 1) % 4 : C.ecc + d < 0 ? 0 : C.ecc + d > 3 ? 3 : C.ecc + d;
		break;
	case 2:
		C.quiet = code == KEY_ENTER ? C.quiet % 16 + 1 : C.quiet + d < 1 ? 1 : C.quiet + d > 16 ? 16 : C.quiet + d;
		break;
	case 3:
		C.shift_enter = (C.shift_enter + d + 3) % 3;
		break;
	case 4:
		C.under = code == KEY_ENTER ? !C.under : code == KEY_RIGHT;
		break;
	case 5:
		C.bright = code == KEY_ENTER ? !C.bright : code == KEY_RIGHT;
		break;
	}
	save_settings();
	return 3;
}

/* One key; 0 nothing to draw, 1 to close, 2 the body, 3 all. */
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
		if (C.sc != SC_LOG && C.sc != SC_SETTINGS)
			C.back = C.sc;
		go(code == KEY_SYM_LOG ? SC_LOG : SC_SETTINGS);
		return 3;
	}
	switch (C.sc) {
	case SC_EDIT:
		return edit_key(code);
	case SC_QUICK:
		return quick_key(code);
	case SC_SHOW:
		return show_key(code);
	case SC_LOG:
		if (code == KEY_ESC)
			go(C.back);
		return 3;
	case SC_SETTINGS:
		return settings_key(code);
	}
	return list_key(code);
}

/* What is on screen, for a test (and the host's output). */
static int where_text(char *out, int size)
{
	static const char *const scn[] = { "list", "show", "edit", "quick", "log", "settings" };
	static const char *const pops[] = { "", "help", "menu", "delete", "type", "insert" };
	const struct field *fd = C.sc == SC_QUICK ? &QT : &ET;
	char *o = out;

	out[0] = 0;		/* cat() appends */
	o = cat(o, scn[C.sc]);
	o = cat(o, " ");
	o = cat(o, pops[C.pop]);
	o = cat_int(cat(o, "\nfocus "), C.focus);
	o = cat_int(cat(o, "\nfield "), C.field);
	o = cat_int(cat(o, "\nshown "), C.shown);
	o = cat(cat(o, "\ntype "), sym_names[C.sc == SC_QUICK ? C.qtype : C.sc == SC_SHOW && n_code ?
					   S[C.shown].type : C.etype]);
	o = cat_int(cat(o, "\nlit "), C.lit);
	o = cat(cat(o, "\nnotice "), C.notice);
	o = cat(cat(o, "\nerror "), C.err ? C.err : "");
	o = cat(cat(o, "\nname "), C.name);
	o = cat(o, "\ntext ");
	for (int i = 0; i < fd->n && o - out < size - 16; i++) {
		int c = ctl_index(fd->t[i]);

		if (c >= 0) {
			o = cat(cat(cat(o, "\\["), ctl_name[c]), "]");
		} else if (fd->t[i] >= 128) {
			*o++ = '\\';
			*o++ = 'x';
			*o++ = hexd[fd->t[i] >> 4];
			*o++ = hexd[fd->t[i] & 15];
		} else {
			*o++ = (char)fd->t[i];
		}
	}
	*o = 0;
	o = cat_int(cat(o, "\ncursor "), fd->cur);
	o = cat(cat(o, "\nfilter "), C.filter);
	o = cat_int(cat(o, "\nlog "), C.n_log);
	o = cat(o, "\nneeds");
	for (int i = 0; i < N_TYPES; i++)
		o = cat_int(cat(o, " "), C.pop == POP_TYPE ? C.need[i] : 0);
	o = cat(o, "\n");
	return (int)(o - out);
}

/* ---- the host test ------------------------------------------------------ */

#ifdef BB_HOST
static void out_s(const char *s)
{
	if (write(1, s, strlen(s)) < 0)
		_exit(1);
}

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

/* the screen as it is now to BB_FRAMES followed by 000.ppm, 001.ppm ... */
static const char *frames;

static void host_shot(void)
{
	static int n;
	char path[300] = "", num[4] = { (char)('0' + n / 100 % 10), (char)('0' + n / 10 % 10), (char)('0' + n % 10), 0 };

	n++;
	if (strlen(frames) < sizeof(path) - 8)
		save_ppm((cat(cat(cat(path, frames), num), ".ppm"), path));
}

/* --sym TYPE HEX OUT.pgm [ECC]: one symbol, 4 px a module with its quiet
 * zone (a 1D code 80 px high); HEX is the text, "XX" for FNC1 */
static int sym_out(int argc, char **argv)
{
	static unsigned short t[4000];
	static uint8_t row[65536];
	static struct sym s;
	const char *h = argv[3], *err = "";
	int n = 0, type = atoi(argv[2]), px = 4, q, w, hh, fd, wide;
	char hdr[48] = "P5\n";

	for (; h[0] && h[1] && n < 4000; h += 2)
		t[n++] = (unsigned short)(h[0] == 'X' ? SYM_FNC1 : hexv(h[0]) * 16 + hexv(h[1]));
	if (sym_make(&s, type, t, n, argc > 5 ? atoi(argv[5]) : ECC_M, 2000, 2000, &err)) {
		char o[96] = "error ";

		cat(cat(o, err), "\n");
		out_s(o);
		return 0;
	}
	wide = !sym_is_2d(type);
	q = s.quiet < 4 ? 4 : s.quiet;
	w = (s.w + 2 * q) * px;
	hh = wide ? 80 + 2 * 8 : (s.h * s.row_h + 2 * q) * px;
	if (w > (int)sizeof(row))
		return 1;
	cat(cat_int(cat(cat_int(hdr, w), " "), hh), "\n255\n");
	fd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, hdr, strlen(hdr)) < 0)
		return 1;
	for (int y = 0; y < hh; y++) {
		int my = wide ? (y >= 8 && y < hh - 8 ? 0 : -1) : (y / px - q >= 0 ? (y / px - q) / s.row_h : -1);

		for (int x = 0; x < w; x++) {
			int mx = x / px - q;

			row[x] = mx >= 0 && mx < s.w && my >= 0 && my < s.h && sym_dark(&s, mx, my) ? 0 : 255;
		}
		if (write(fd, row, (size_t)w) < 0)
			return 1;
	}
	close(fd);
	{
		char o[96] = "ok ";

		cat(cat_int(cat(cat_int(cat(cat_int(cat(cat_int(o, s.w), " "), s.h), " "), s.size2), " "),
			    s.shifted * 2 + s.latched), "\n");
		out_s(o);
	}
	return 0;
}

/*
 * barbarious-host KEYS [OUT.ppm], with APP_CONFIG. KEYS is typed; \n
 * Enter, \N Shift+Enter, \e Esc, \t Tab, \b Backspace, \u \d \l \r the
 * arrows, \1..\6 the symbol keys, and \f Fn, \s Shift, \c Ctrl held for
 * the next key. Prints what is on screen (where_text) and the codes.
 * BB_BANDS draws a band at a time, as the badge does without room for the
 * whole picture. BB_FRAMES=PREFIX draws a frame after every key, as the
 * badge does, and writes each to PREFIX000.ppm on.
 */
int main(int argc, char **argv)
{
	const char *k = argc > 1 ? argv[1] : "";
	static char out[64 * 1024];

	if (argc > 4 && !strcmp(argv[1], "--sym"))
		return sym_out(argc, argv);
	paths();
	load();
	load_settings();
	C.qtype = C.def_type;
	wb_init();
	whole = getenv("BB_BANDS") ? 0 : screen;	/* BB_BANDS: as without room for the whole */
	frames = getenv("BB_FRAMES");
	memcpy(U.hhmm, "12:34", 6);
	U.clock_ok = 1;
	U.wifi_up = 3;
	for (; *k; k++) {
		int code = -1;

		for (; *k == '\\' && k[1] && strchr("fsc", k[1]); k += 2) {
			K.fn |= k[1] == 'f';
			K.shift |= k[1] == 's';
			K.ctrl |= k[1] == 'c';
		}
		if (*k == '\\' && k[1] && k[1] != '\\') {	/* \\ is a backslash */
			k++;
			K.shift |= *k == 'N';
			code = *k == 'n' || *k == 'N' ? KEY_ENTER : *k == 'e' ? KEY_ESC : *k == 't' ? KEY_TAB :
			       *k == 'b' ? KEY_BACKSPACE : *k == 'u' ? KEY_UP : *k == 'd' ? KEY_DOWN :
			       *k == 'l' ? KEY_LEFT : *k == 'r' ? KEY_RIGHT :
			       *k >= '1' && *k <= '6' ? KEY_SYM_MENU + (*k - '1') : -1;
		} else {
			k += *k == '\\' && k[1] == '\\';
			for (int c = 0; c < 64 && code < 0; c++) {
				if (key_char(c, 0) == *k)
					code = c;
				else if (key_char(c, 1) == *k)
					code = c, K.shift = 1;
			}
			if (*k == ' ')
				code = 57;
		}
		if (code >= 0 && key(code) == 1) {
			out_s("closed\n");
			break;
		}
		K.shift = K.fn = K.ctrl = 0;
		if (frames) {
			present();
			host_shot();
		}
	}
	present();
	if (argc > 2 && argv[2][0])
		save_ppm(argv[2]);
	where_text(out, (int)sizeof(out) - 8192);
	for (int i = 0; i < n_code; i++) {
		char *o = out + strlen(out);
		int c;

		o = cat(cat(cat(cat(o, "code "), S[i].name), "|"), type_ids[S[i].type]);
		o = cat(o, "|");
		for (int j = 0; j < S[i].n && o - out < (int)sizeof(out) - 64; j++) {
			if ((c = ctl_index(S[i].t[j])) >= 0) {
				o = cat(cat(cat(o, "\\["), ctl_name[c]), "]");
			} else {
				*o++ = (char)S[i].t[j];
				*o = 0;
			}
		}
		cat(o, "\n");
	}
	out_s(out);
	return 0;
}

#else

/* ---- the badge ---------------------------------------------------------- */

#define FIFO	"/tmp/barbarious.keys"
#define WHERE	"/tmp/barbarious.where"

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t stop_sig;
static int fifo_used;

static void on_signal(int s)
{
	stop_sig = s;
}

/* continued after a stop (the menu's console or screenshot): the panel may
 * show something else meanwhile, so the next frame is drawn whole */
static volatile sig_atomic_t cont_sig;

static void on_cont(int s)
{
	(void)s;
	cont_sig = 1;
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

static void where_write(void)
{
	static char b[4096];
	int n = where_text(b, (int)sizeof(b)), fd;

	fd = open(WHERE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, b, (size_t)n) == n)
		rename(WHERE ".new", WHERE);
	close(fd);
}

/* keys from a test: decimal codes; s30 is Shift+A, c32 Ctrl+D, f105 Fn+Left */
static int fifo_keys(int fd, int *draw)
{
	char buf[1024], *p = buf;
	int n = (int)read(fd, buf, sizeof(buf) - 1);

	if (n <= 0)
		return 0;
	buf[n] = 0;
	fifo_used = 1;
	while (*p) {
		int code = 0, r;

		while (*p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;
		K.shift = *p == 's';
		K.ctrl = *p == 'c';
		K.fn = *p == 'f';
		p += K.shift || K.ctrl || K.fn;
		while (*p >= '0' && *p <= '9')
			code = code * 10 + *p++ - '0';
		while (*p && *p != ' ' && *p != '\n')
			p++;
		r = key(code);
		K.shift = K.ctrl = K.fn = 0;
		if (r == 1)
			return 1;
		*draw = r > *draw ? r : *draw;
	}
	return 0;
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p[2];
	int ev_fd, fifo_fd = -1, ticks = 0;
	long last_key = power_now(), touched = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	sa.sa_handler = on_cont;
	sigaction(SIGCONT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev_fd < 0)
		return 1;
	unlink(FIFO);
	if (!mkfifo(FIFO, 0600))
		fifo_fd = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	paths();
	load();
	load_settings();
	C.qtype = C.def_type;
	wb_init();
	whole = malloc((size_t)W * H * sizeof(*whole));	/* 0: bands, and plain pop-ups */
	wb_rec_invalidate();	/* nothing in it yet: the first frame whole */
	status_read();
	present();
	p[0].fd = ev_fd;
	p[0].events = POLLIN;
	p[1].fd = fifo_fd;
	p[1].events = POLLIN;
	while (!stop_sig) {
		struct ev evs[16];
		int k, got, draw = 0, quit = 0;
		long now;

		/* four times a second: has the badge turned? */
		k = poll(p, fifo_fd >= 0 ? 2 : 1, 250);
		now = power_now();
		if (cont_sig) {
			cont_sig = 0;
			wb_rec_invalidate();
			present();
		}
		/* each draws only what changed (present) */
		if (wb_scene_turned())
			present();
		if (++ticks % 20 == 0 && status_read())
			present();
		/* a code on screen keeps it on, as a key would; not past KEEP_ON_S,
		 * and never out of standby */
		if ((C.sc == SC_SHOW || C.sc == SC_QUICK) && now - last_key < KEEP_ON_S && now - touched >= 20 &&
		    access(POWER_STANDBY, F_OK) != 0) {
			power_touch(now);
			touched = now;
		}
		if (k <= 0)
			continue;
		if (fifo_fd >= 0 && (p[1].revents & POLLIN)) {
			quit = fifo_keys(fifo_fd, &draw);
			last_key = now;
		}
		if (p[0].revents & POLLIN) {
			got = (int)read(ev_fd, evs, sizeof(evs)) / (int)sizeof(evs[0]);
			for (int q = 0; q < got && !quit; q++) {
				const struct ev e = evs[q];
				int down = e.value != 0, r;

				if (e.type != 1)
					continue;
				last_key = now;
				if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT)
					K.shift = down;
				else if (e.code == KEY_FN)
					K.fn = down;
				else if (e.code == KEY_LEFTCTRL)
					K.ctrl = down;
				else if (key_acts(e.code, e.value) && !(e.value == 2 && key_stale(e.sec, e.usec))) {
					r = key(e.code);
					if (r == 1)
						quit = 1;
					else if (r > draw)
						draw = r;
				}
			}
		}
		if (quit)
			break;
		/* 2: from the title's foot down changed, 3: anything; either
		 * way only the bands that changed are drawn */
		if (draw >= 2)
			present();
		if (draw && fifo_used)
			where_write();
	}
	lights(0);
	if (fifo_fd >= 0)
		unlink(FIFO);
	return 0;
}
#endif
