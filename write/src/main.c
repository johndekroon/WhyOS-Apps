/* SPDX-License-Identifier: MIT */
/*
 * Write: the program. On the badge it reads the keypad (evdev, not
 * grabbed: apprun needs the logo key), keeps Shift, Ctrl, Alt and Fn,
 * writes the panel band by band and ends on SIGTERM after saving (the logo
 * key, design/write/README.md §10). /tmp/write.keys takes key codes from a
 * test, as /tmp/meshchat.keys does (docs/HOME.md of why2025-linux):
 * decimal codes, each optionally after s (Shift), c (Ctrl), a (Alt) and f
 * (Fn): "c31" is Ctrl+S, "f106" Fn+Right.
 *
 *   write [FILE]
 *
 * With -DWRITE_UI_HOST it is a host program for the tests: keys from the
 * script in WRITE_UI_SCRIPT, screens as PPM (tests/test_ui.py).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "ui.h"

#define W	720
#define H	720

static struct app A;

/* US layout, as the menu's key_char */
static char key_char(int code, int shift)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == K_SPACE)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo) - 1)
		return 0;
	return shift ? hi[code] : lo[code];
}

static void key_send(int code, int shift, int ctrl, int alt, int fn)
{
	struct key k = { code, shift, ctrl, alt, fn, "" };
	char c = key_char(code, shift);

	if (c && !ctrl && !alt) {
		k.text[0] = c;
		k.text[1] = 0;
	}
	app_key(&A, &k);
}

/* "c31", "sf106": a key code from the test FIFO */
static void key_word(const char *w)
{
	int s = 0, c = 0, a = 0, f = 0;

	for (; *w && (*w < '0' || *w > '9'); w++) {
		s |= *w == 's';
		c |= *w == 'c';
		a |= *w == 'a';
		f |= *w == 'f';
	}
	if (*w)
		key_send(atoi(w), s, c, a, f);
}

#ifdef WRITE_UI_HOST

/*
 * The script (WRITE_UI_SCRIPT): one action per line --
 *   KEY or MODS KEY      up down left right enter esc bs tab f1..f6, a letter
 *                        or digit; MODS: shift ctrl alt fn ("ctrl s", "fn right")
 *   type TEXT            keys for TEXT
 *   shot PATH            the screen as PPM
 *   fn down / fn up      Fn held / let go
 *   tick MS              time passes
 *   term                 the logo key (SIGTERM)
 *   code WORD            a key as the test FIFO writes it ("c31": Ctrl+S)
 */
static uint16_t screen[W * H];
static long clock_ms = 1000;
static int check_full;

static void out_band(const uint16_t *b, int y0, int y1)
{
	memcpy(screen + y0 * W, b, (size_t)(y1 - y0) * W * 2);
}

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

/* WRITE_UI_CHECK: every frame drawn in bands must equal one drawn whole */
static void present(void)
{
	static uint16_t inc[W * H];

	ui_present(&A, 0, out_band);
	if (!check_full)
		return;
	memcpy(inc, screen, sizeof(screen));
	ui_present(&A, 1, out_band);
	for (int y = 0; y < H; y++)
		if (memcmp(inc + y * W, screen + y * W, W * 2)) {
			fprintf(stderr, "write-ui: band %d (row %d) was not redrawn but changed\n", y / BAND_H, y);
			exit(4);
		}
}

static int code_of(const char *n)
{
	static const struct { const char *n; int c; } k[] = {
		{ "up", K_UP }, { "down", K_DOWN }, { "left", K_LEFT }, { "right", K_RIGHT },
		{ "enter", K_ENTER }, { "esc", K_ESC }, { "bs", K_BACKSPACE }, { "tab", K_TAB },
		{ "space", K_SPACE }, { "f1", K_F1 }, { "f2", K_F2 }, { "f3", K_F3 }, { "f4", K_F4 },
		{ "f5", K_F5 }, { "f6", K_F6 },
	};
	static const char row[] = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
				  "\0" "\\zxcvbnm,./";

	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
		if (!strcmp(n, k[i].n))
			return k[i].c;
	if (n[0] && !n[1])
		for (int i = 0; i < (int)sizeof(row) - 1; i++)
			if (row[i] == n[0])
				return i;
	return 0;
}

static void type_text(const char *t)
{
	static const char hi[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
	static const char lo[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";

	for (; *t; t++) {
		const char *p = strchr(hi, *t);
		char c[2] = { p ? lo[p - hi] : *t, 0 };

		key_send(code_of(*t == ' ' ? "space" : c), p != 0, 0, 0, 0);
		present();
	}
}

int main(int argc, char **argv)
{
	const char *s = getenv("WRITE_UI_SCRIPT");
	FILE *script = s ? fopen(s, "r") : 0;
	char line[512];

	check_full = getenv("WRITE_UI_CHECK") != 0;
	ui_init();
	app_init(&A, argc > 1 ? argv[1] : 0);
	A.now = clock_ms;
	present();
	while (script && !A.quit && fgets(line, sizeof(line), script)) {
		char *w[4] = { 0 }, *p = line;
		int n = 0, sh = 0, ct = 0, al = 0, fn = 0;

		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "shot ", 5)) {
			present();
			save_ppm(line + 5);
			continue;
		}
		if (!strncmp(line, "type ", 5)) {
			type_text(line + 5);
			continue;
		}
		if (!strncmp(line, "tick ", 5)) {
			clock_ms += atol(line + 5);
			if (app_tick(&A, clock_ms))
				present();
			continue;
		}
		if (!strncmp(line, "code ", 5)) {
			A.now = clock_ms;
			key_word(line + 5);
			present();
			continue;
		}
		if (!strcmp(line, "term")) {
			app_term(&A);
			break;
		}
		if (!strcmp(line, "fn down") || !strcmp(line, "fn up")) {
			app_fn(&A, line[3] == 'd');
			present();
			continue;
		}
		while (n < 4 && (w[n] = strtok(n ? 0 : p, " ")))
			n++;
		for (int i = 0; i < n - 1; i++) {
			sh |= !strcmp(w[i], "shift");
			ct |= !strcmp(w[i], "ctrl");
			al |= !strcmp(w[i], "alt");
			fn |= !strcmp(w[i], "fn");
		}
		if (n) {
			A.now = clock_ms;
			key_send(code_of(w[n - 1]), sh, ct, al, fn);
			present();
		}
	}
	if (A.run) {
		FILE *f = fopen(getenv("WRITE_RUN_OUT") ? getenv("WRITE_RUN_OUT") : "/dev/null", "w");

		if (f) {
			fprintf(f, "%d %s\n", A.run, A.path);
			fclose(f);
		}
	}
	app_free(&A);
	return 0;
}

#else /* the badge */

#define FIFO	"/tmp/write.keys"

static volatile sig_atomic_t term;
static struct { int shift, ctrl, alt, fn; } M;

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

struct ev {			/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static int fb_fd = -1, ev_fd = -1, fifo_fd = -1;
static int timing;

static void out_band(const uint16_t *b, int y0, int y1)
{
	if (pwrite(fb_fd, b, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2) < 0)
		return;
}

static void present(int force)
{
	long t0 = now_ms();
	int n = ui_present(&A, force, out_band);

	if (timing && n) {
		char b[80];
		int len = snprintf(b, sizeof(b), "write: %d bands, %ld ms\n", n, now_ms() - t0);

		if (write(2, b, (size_t)len) < 0)
			timing = 0;
	}
}

static void on_term(int sig)
{
	(void)sig;
	term = 1;
}

static void status_read(void)
{
	time_t t = time(0);
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(ui_st.hhmm, sizeof(ui_st.hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
	ui_st.clock_ok = tm.tm_year >= 125;
}

/* the keys in the evdev queue */
static void read_keys(void)
{
	struct ev evs[16];
	int got;

	while ((got = (int)read(ev_fd, evs, sizeof(evs))) > 0) {
		for (int i = 0; i < got / (int)sizeof(evs[0]); i++) {
			const struct ev *e = &evs[i];
			int down = e->value != 0;

			if (e->type != 1)
				continue;
			if (e->code == 42 || e->code == 54)
				M.shift = down;
			else if (e->code == 29 || e->code == 97)
				M.ctrl = down;
			else if (e->code == 56 || e->code == 100)
				M.alt = down;
			else if (e->code == K_FN) {
				M.fn = down;
				app_fn(&A, down);
			} else if (down && e->code != 125) {	/* 125: the logo key is apprun's */
				A.now = now_ms();
				key_send(e->code, M.shift, M.ctrl, M.alt, M.fn);
			}
		}
	}
}

static void read_fifo(void)
{
	char buf[256], *save = 0;
	int n = (int)read(fifo_fd, buf, sizeof(buf) - 1);

	if (n <= 0)
		return;
	buf[n] = 0;
	for (char *w = strtok_r(buf, " \n", &save); w; w = strtok_r(0, " \n", &save)) {
		A.now = now_ms();
		key_word(w);
		present(0);
	}
}

/* ▲ in the code view: the script in appterm's window, or a page in NetSurf */
static void run_program(void)
{
	char micro[512], ns[512], url[600];
	const char *home = getenv("HOME") ? getenv("HOME") : "/root";
	char *argv[10];
	int n = 0, status;
	pid_t pid;

	if (A.run == 1) {
		argv[n++] = "/usr/bin/appterm";
		argv[n++] = "--title";
		argv[n++] = A.name;
		argv[n++] = "--eyebrow";
		argv[n++] = "WRITE";
		if (A.lang == L_PYTHON) {
			snprintf(micro, sizeof(micro), "%s/.whyos/bin/micropython", home);
			if (access(micro, X_OK)) {
				A.ov = OV_ERROR;
				snprintf(A.err_title, sizeof(A.err_title), "Python is not installed");
				snprintf(A.err_text, sizeof(A.err_text),
					 "Install MicroPython from the app store to run Python scripts.");
				return;
			}
			argv[n++] = micro;
		} else {
			argv[n++] = "/bin/sh";
		}
		argv[n++] = A.path;
	} else {
		snprintf(ns, sizeof(ns), "/mnt/sd/badge/apps/netsurf/netsurf");
		if (access(ns, X_OK)) {
			A.ov = OV_ERROR;
			snprintf(A.err_title, sizeof(A.err_title), "NetSurf is not installed");
			snprintf(A.err_text, sizeof(A.err_text), "Install NetSurf from the app store to preview pages.");
			return;
		}
		snprintf(url, sizeof(url), "file://%s", A.path);
		argv[n++] = ns;
		argv[n++] = url;
	}
	argv[n] = 0;
	app_log(&A, "run %s", A.path);
	pid = vfork();
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid > 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			if (term) {
				kill(pid, SIGTERM);
				term = 1;
			}
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
			app_log(&A, "error: could not start %s", argv[0]);
	}
	/* what was typed meanwhile went to the program */
	{
		struct ev junk[16];

		while (read(ev_fd, junk, sizeof(junk)) > 0)
			;
	}
	M.shift = M.ctrl = M.alt = M.fn = 0;
	A.redraw = 1;
}

int main(int argc, char **argv)
{
	long last_status = 0, last_blink = 0, last_key = 0;
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd < 0) {
		fprintf(stderr, "write: no panel (/dev/fb0)\n");
		return 2;
	}
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	unlink(FIFO);
	if (!mkfifo(FIFO, 0600))
		fifo_fd = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	timing = getenv("WRITE_UI_TIMING") != 0;
	ui_init();
	status_read();
	ui_st.bars = -1;
	app_init(&A, argc > 1 ? argv[1] : 0);
	A.now = now_ms();
	present(1);
	while (!A.quit) {
		struct pollfd p[2] = { { ev_fd, POLLIN, 0 }, { fifo_fd, POLLIN, 0 } };
		long t;
		int r = poll(p, fifo_fd >= 0 ? 2 : 1, 530);

		if (term) {
			/* keys typed before the logo key belong in the document */
			if (ev_fd >= 0)
				read_keys();
			if (fifo_fd >= 0)
				read_fifo();
			fprintf(stderr, "write: the logo key: %s\n", text_dirty(&A.e.t) ? "saving first" : "nothing to save");
			app_term(&A);
			break;
		}
		t = now_ms();
		if (r > 0 && (p[0].revents & POLLIN)) {
			read_keys();
			last_key = t;
			ui_st.blink = 1;
			last_blink = t;
		}
		if (r > 0 && fifo_fd >= 0 && (p[1].revents & POLLIN)) {
			read_fifo();
			last_key = t;
		}
		if (A.run) {
			run_program();
			A.run = 0;
		}
		if (t - last_status >= 5000 && t - last_key >= 3000) {
			last_status = t;
			status_read();
			ui_st.bars = ui_wifi_bars();
			ui_st.lora = ui_lora_on();
		}
		/* the caret blinks, but not while typing, and it stops after a minute */
		if (t - last_blink >= 530 && t - last_key < 60000) {
			ui_st.blink = !ui_st.blink;
			last_blink = t;
		} else if (t - last_key >= 60000) {
			ui_st.blink = 1;
		}
		app_tick(&A, t);
		present(0);
	}
	if (fifo_fd >= 0)
		unlink(FIFO);
	app_free(&A);
	return 0;
}

#endif
