/* SPDX-License-Identifier: MIT */
/*
 * Files: the program. On the badge it reads the keypad (evdev, not grabbed:
 * apprun needs the logo key) and writes the panel band by band.
 *
 *   files [PATH]        start in PATH's folder, on PATH (a file or folder);
 *                       "" for Drives; none for /
 *
 * Files does not start apps: to open a file it writes the app's folder and
 * the file to /tmp/files-open ("<app dir>\n<file>\n") and ends; the menu
 * starts the app and, when it ends, Files again on that file
 * (why2025-linux home.c, "Files, an app"). A note for the screen (the app
 * did not start) comes in /tmp/files-notice, read once.
 *
 * While Files is open the SD card's FAT is mounted at /media/sd
 * (disks_fat_follow); it is unmounted when Files closes, not when it opens
 * a file, which may be on it.
 *
 * /tmp/files.keys takes key codes from a test, as /tmp/write.keys does:
 * decimal codes, each optionally after s (Shift) and h (held: a repeat).
 * After every frame drawn while a test drives it, /tmp/files.where says
 * what is on screen: "<title>\n<folder>\n<focused row>\n<right of the
 * title>\n<screen>\n" (the menu's /tmp/home.where, and the screen: list,
 * settings, log, pick, input, confirm, help).
 *
 * With -DFILES_UI_HOST it is a host program for the tests: keys from the
 * script in FILES_UI_SCRIPT, screens as PPM (tests/test_ui.py).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "ui.h"

#define W	720
#define H	720

static struct files F;

static const char *const screen_names[] = {
	[SC_LIST] = "list", [SC_SETTINGS] = "settings", [SC_LOG] = "log", [SC_PICK] = "pick",
	[SC_INPUT] = "input", [SC_CONFIRM] = "confirm", [SC_HELP] = "help",
};

/* what is on screen, for tests (see the top) */
static void where_write(const char *path)
{
	char buf[512], tmp[300];
	const struct list *l = &F.l;
	const struct row *r = l->focus >= 0 && l->focus < l->n ? &l->rows[l->focus] : 0;
	int fd, n;

	n = snprintf(buf, sizeof(buf), "%s\n%s\n%s\n%s\n%s\n", l->title ? l->title : "", F.b.dir,
		     r ? r->label : "", l->title_r, screen_names[F.sc]);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) == n)
		rename(tmp, path);
	close(fd);
}

/* the file to open and its app, for the menu */
static void open_write(const char *path)
{
	char buf[512], tmp[300];
	int fd, n = snprintf(buf, sizeof(buf), "%s\n%s\n", F.open_dir, F.open_file);

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) == n)
		rename(tmp, path);
	close(fd);
}

/* a key from the test FIFO or the script: "108", "h108" (held), "s30" */
static void key_word(const char *w)
{
	int shift = 0, value = 1;

	for (; *w && (*w < '0' || *w > '9'); w++) {
		shift |= *w == 's';
		if (*w == 'h')
			value = 2;
	}
	if (!*w)
		return;
	if (shift)
		files_key(&F, K_LEFTSHIFT, 1);
	files_key(&F, atoi(w), value);
	if (shift)
		files_key(&F, K_LEFTSHIFT, 0);
	files_build(&F);
}

#ifdef FILES_UI_HOST

/*
 * The script (FILES_UI_SCRIPT): one action per line --
 *   KEY                  up down left right enter esc bs del tab f1..f6
 *   held KEY             a repeat of it, as a key held down sends
 *   type TEXT            keys for TEXT
 *   code WORD            a key as the test FIFO takes it ("h108")
 *   shot PATH            the screen as PPM
 * FILES_APPS: the app folders to read (":" between), FILES_LOG the log,
 * FILES_OPEN where a file to open goes, FILES_WHERE the where file after
 * every line, FILES_MSG_SHOT the message shown while a copy, move or
 * delete runs (as PPM); FILES_BANDS_ONLY draws pop-ups in bands too (ui.c:
 * as when there is no room for a whole frame).
 */
static uint16_t screen[W * H];

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

static int code_of(const char *n)
{
	static const struct { const char *n; int c; } k[] = {
		{ "up", K_UP }, { "down", K_DOWN }, { "left", K_LEFT }, { "right", K_RIGHT },
		{ "enter", K_ENTER }, { "esc", K_ESC }, { "bs", K_BACKSPACE }, { "del", K_DELETE },
		{ "tab", K_TAB }, { "space", K_SPACE }, { "f1", K_F1 }, { "f2", K_F2 }, { "f3", K_F3 },
		{ "f4", K_F4 }, { "f5", K_F5 }, { "f6", K_F6 },
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

		if (p)
			files_key(&F, K_LEFTSHIFT, 1);
		files_key(&F, code_of(*t == ' ' ? "space" : c), 1);
		if (p)
			files_key(&F, K_LEFTSHIFT, 0);
		files_build(&F);
	}
}

static void ui_run(struct browse *b, const char *what)
{
	(void)b;
	ui_message("FILES", what, out_band);
	if (getenv("FILES_MSG_SHOT"))	/* the message while it runs, as PPM */
		save_ppm(getenv("FILES_MSG_SHOT"));
}

static int run(struct browse *b, const char *what, char *const argv[])
{
	ui_run(b, what);
	return br_run(b, what, argv);
}

int main(int argc, char **argv)
{
	const char *s = getenv("FILES_UI_SCRIPT"), *where = getenv("FILES_WHERE");
	FILE *script = s ? fopen(s, "r") : 0;
	char line[512];

	memcpy(ui_st.hhmm, "12:34", 6);
	ui_st.clock_ok = 1;
	ui_st.bars = -1;
	F.b.log = getenv("FILES_LOG");
	F.b.run = run;
	files_init(&F, argc > 1 ? argv[1] : 0);
	if (getenv("FILES_APPS")) {
		char roots[512], *save = 0;
		const char *r[8];
		int n = 0;

		snprintf(roots, sizeof(roots), "%s", getenv("FILES_APPS"));
		for (char *t = strtok_r(roots, ":", &save); t && n < 8; t = strtok_r(0, ":", &save))
			r[n++] = t;
		apps_read(&F.apps, r, n);
	}
	files_build(&F);
	while (script && !F.quit && fgets(line, sizeof(line), script)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "shot ", 5)) {
			ui_present(&F, out_band);
			save_ppm(line + 5);
		} else if (!strncmp(line, "type ", 5)) {
			type_text(line + 5);
		} else if (!strncmp(line, "code ", 5)) {
			key_word(line + 5);
		} else if (!strncmp(line, "held ", 5)) {
			files_key(&F, code_of(line + 5), 2);
			files_build(&F);
		} else if (line[0]) {
			files_key(&F, code_of(line), 1);
			files_build(&F);
		}
		if (where)
			where_write(where);
	}
	if (F.open_file[0])
		open_write(getenv("FILES_OPEN") ? getenv("FILES_OPEN") : "/tmp/files-open");
	return 0;
}

#else /* the badge */

#include "keyrep.h"

#define FIFO	"/tmp/files.keys"
#define WHERE	"/tmp/files.where"
#define OPEN	"/tmp/files-open"
#define NOTICE	"/tmp/files-notice"

struct ev {			/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t term;
static int fb_fd = -1, ev_fd = -1, fifo_fd = -1, fifo_used;

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void on_term(int sig)
{
	(void)sig;
	term = 1;
}

static void out_band(const uint16_t *b, int y0, int y1)
{
	if (pwrite(fb_fd, b, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2) < 0)
		return;
}

static int fifo_more;		/* keys from the test FIFO still to come */

static void present(void)
{
	ui_present(&F, out_band);
	if (fifo_used && !fifo_more)	/* after the last of them: the tests wait for it */
		where_write(WHERE);
}

/* the status bar: 1 if it changed */
static int status_read(void)
{
	time_t t = time(0);
	struct tm tm;
	struct ui_status was = ui_st;

	localtime_r(&t, &tm);
	snprintf(ui_st.hhmm, sizeof(ui_st.hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
	ui_st.clock_ok = tm.tm_year >= 124;
	ui_st.bars = ui_wifi_bars();
	ui_st.lora = ui_lora_on();
	return memcmp(&was, &ui_st, sizeof(was)) != 0;
}

/* a copy, move or delete: its message while it runs */
static int run(struct browse *b, const char *what, char *const argv[])
{
	ui_message("FILES", what, out_band);
	return br_run(b, what, argv);
}

static void read_keys(void)
{
	struct ev evs[16];
	int got;

	while ((got = (int)read(ev_fd, evs, sizeof(evs))) > 0) {
		int n = got / (int)sizeof(evs[0]);

		for (int i = 0; i < n; i++) {
			const struct ev *e = &evs[i];

			if (e->type != 1 || e->code == 125)	/* 125: the logo key is apprun's */
				continue;
			/* fell behind a held key: skip what waited */
			if (e->value == 2 && key_stale(e->sec, e->usec))
				continue;
			files_key(&F, e->code, e->value);
		}
	}
	files_build(&F);
}

static void read_fifo(void)
{
	char buf[256], *save = 0;
	int n = (int)read(fifo_fd, buf, sizeof(buf) - 1);

	if (n <= 0)
		return;
	buf[n] = 0;
	fifo_used = 1;
	for (char *w = strtok_r(buf, " \n", &save); w && !F.quit; w = strtok_r(0, " \n", &save)) {
		key_word(w);
		fifo_more = save && strspn(save, " \n") < strlen(save);
		present();
	}
	fifo_more = 0;
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	long last_status = 0, last_disks = 0;
	char notice[160];
	int fd;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb_fd < 0) {
		fprintf(stderr, "files: no panel (/dev/fb0)\n");
		return 2;
	}
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	unlink(FIFO);
	unlink(OPEN);
	if (!mkfifo(FIFO, 0600))
		fifo_fd = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	disks_fat_follow(1);
	F.b.run = run;
	files_init(&F, argc > 1 ? argv[1] : 0);
	if ((fd = open(NOTICE, O_RDONLY | O_CLOEXEC)) >= 0) {
		int n = (int)read(fd, notice, sizeof(notice) - 1);

		close(fd);
		unlink(NOTICE);
		while (n > 0 && notice[n - 1] == '\n')
			n--;
		if (n > 0) {
			notice[n] = 0;
			br_say(&F.b, notice, "", 1);
		}
	}
	status_read();
	files_build(&F);
	present();
	while (!F.quit && !term) {
		struct pollfd p[2] = { { ev_fd, POLLIN, 0 }, { fifo_fd, POLLIN, 0 } };
		int drives = F.sc == SC_LIST && br_drives(&F.b);
		/* four times a second: has the badge turned (the scene with it)? */
		int r = poll(p, fifo_fd >= 0 ? 2 : 1, 250);
		long t = now_ms();
		int dirty = 0;

		if (term)
			break;
		if (r > 0 && (p[0].revents & POLLIN)) {
			read_keys();
			dirty = 1;
		}
		if (r > 0 && fifo_fd >= 0 && (p[1].revents & POLLIN))
			read_fifo();
		if (F.quit)
			break;
		if (drives && t - last_disks >= 1500) {		/* disks come and go */
			last_disks = t;
			files_refresh(&F);
			files_build(&F);
			dirty = 1;
		}
		if (t - last_status >= 5000) {
			last_status = t;
			dirty |= status_read();
		}
		if (dirty || ui_turned())
			present();
	}
	if (fifo_fd >= 0)
		unlink(FIFO);
	if (F.open_file[0] && !term) {
		open_write(OPEN);
		return 0;
	}
	disks_fat_follow(0);
	return 0;
}

#endif
