// SPDX-License-Identifier: MIT
/*
 * flashlight — the whole screen as a light, as an app from the store
 * (sdapps/README.md).
 *
 * Three modes: white at full brightness, white at half and red at full.
 * The square, triangle and cross choose one, the arrows step through them,
 * the diamond opens the help, Esc closes. The mode is kept in APP_CONFIG/mode and comes back next time.
 *
 * For the first START_MS the backlight stays as the system has it, so a
 * dark room is not lit at once, and the screen shows the keys. After that
 * the keys go, the screen is one colour and the backlight goes to the mode's
 * brightness. apprun puts the lights back when the app ends. While it runs
 * it keeps the screen on, as a key would (TOUCH_S), but it does not wake the
 * badge from standby.
 *
 * The panel is drawn in bands of BAND_H rows. Built with -DFL_HOST, it
 * takes keys from its first argument and writes the screen as PPM to its
 * second: the host test (flashlight/test_flashlight.py). On the badge,
 * decimal key codes written to /tmp/flashlight.keys act as keys (in the
 * sandbox that is /tmp/app-flashlight/flashlight.keys).
 *
 * FLAT/NOMMU constraints (overlay-src/Makefile): no stdio.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>		/* rename() */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wb.h"
#include "power.h"

#ifndef FL_HOST
#include <poll.h>
#endif

#define START_MS	2000		/* at the system's brightness, with the keys */
#define TOUCH_S		5		/* how often the screen is kept on */

/* the keys' panel */
#define PANEL_X		120
#define PANEL_W		(W - 2 * PANEL_X)
#define PANEL_Y		196
#define PANEL_PAD	32
#define ROW_H		64
#define ROWS_Y		(PANEL_Y + 64)
#define ARROWS_Y	(ROWS_Y + 3 * ROW_H + 12)
#define PANEL_H		(ARROWS_Y + 16 + PANEL_PAD - PANEL_Y)

/* ---- keys (linux/input-event-codes.h) ----------------------------------- */

#define KEY_ESC		1
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_FN		0x1d0

/* ---- the modes ---------------------------------------------------------- */

static const struct mode {
	const char *keys, *name;
	uint32_t color;
	int percent;
} modes[] = {
	{ "#1", "White 100%", 0xFFFFFF, 100 },
	{ "#2", "White 50%", 0xFFFFFF, 50 },
	{ "#3", "Red 100%", 0xFF0000, 100 },
};

#define N_MODES		(int)(sizeof(modes) / sizeof(modes[0]))

static struct {
	int mode;
	int start;			/* the first START_MS: the keys show */
	int help;			/* the diamond's pop-up is open */
	int fn;				/* Fn is down: Fn+Esc is sleepd's */
} S;

/* ---- the mode kept ------------------------------------------------------ */

static int mode_path(char *path, int size, const char *name)
{
	const char *dir = getenv("APP_CONFIG");

	if (!dir || (int)(strlen(dir) + strlen(name)) + 2 > size)
		return -1;
	strcpy(path, dir);
	strcat(path, "/");
	strcat(path, name);
	return 0;
}

static void mode_load(void)
{
	char path[256], c;
	int fd;

	if (mode_path(path, sizeof(path), "mode"))
		return;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (read(fd, &c, 1) == 1 && c >= '0' && c < '0' + N_MODES)
		S.mode = c - '0';
	close(fd);
}

static void mode_save(void)
{
	char path[256], tmp[256], c = (char)('0' + S.mode);
	int fd, ok;

	if (mode_path(path, sizeof(path), "mode") || mode_path(tmp, sizeof(tmp), "mode.new"))
		return;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	ok = write(fd, &c, 1) == 1 && !fsync(fd);
	close(fd);
	if (ok)
		rename(tmp, path);
	else
		unlink(tmp);
}

/* ---- the light ---------------------------------------------------------- */

#define BL_PATH		"/sys/class/backlight/backlight/brightness"
#define BL_MAX_PATH	"/sys/class/backlight/backlight/max_brightness"
#define BL_MAX		255		/* if the panel does not say */

#ifdef FL_HOST
static int bl_level = -1;		/* -1: as the system has it */

static void light(void)
{
	bl_level = BL_MAX * modes[S.mode].percent / 100;
}
#else
static int bl_max(void)
{
	char b[16];
	int fd = open(BL_MAX_PATH, O_RDONLY | O_CLOEXEC), n, max = 0;

	if (fd < 0)
		return BL_MAX;
	n = (int)read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n > 0) {
		b[n] = 0;
		max = atoi(b);
	}
	return max > 0 ? max : BL_MAX;
}

static void light(void)
{
	int v = bl_max() * modes[S.mode].percent / 100, n = 0, fd;
	char d[12], b[12];

	do
		d[n++] = (char)('0' + v % 10);
	while ((v /= 10));
	for (int i = 0; i < n; i++)
		b[i] = d[n - 1 - i];
	b[n++] = '\n';
	fd = open(BL_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, b, (size_t)n) < 0)
		n = 0;
	close(fd);
}
#endif

/* ---- the screen --------------------------------------------------------- */

#define BAND_H		40

static uint16_t band[W * BAND_H];
#ifdef FL_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void out_rows(const uint16_t *rows, int y0, int y1)
{
#ifdef FL_HOST
	memcpy(screen + y0 * W, rows, (size_t)(y1 - y0) * W * 2);
#else
	pwrite(fb_fd, rows, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2);
#endif
}

static void paint(void *arg)
{
	static const struct hint close_hint[] = { { "Esc", "close", 1 } };
	static const struct help_key help_keys[] = {
		{ "#1", "White at full brightness" },
		{ "#2", "White at half brightness" },
		{ "#3", "Red at full brightness" },
		{ "\xe2\x86\x91\xe2\x86\x93\xe2\x86\x90\xe2\x86\x92", "Step through the modes" },
		{ "Esc", "Close the flashlight" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};

	(void)arg;
	fill(0, 0, W, H, modes[S.mode].color);
	if (S.help) {
		help_popup("Flashlight", "The whole screen as a light. It stays on until you close it, "
			   "and the mode you chose comes back next time.",
			   help_keys, sizeof(help_keys) / sizeof(help_keys[0]));
		return;
	}
	if (!S.start)
		return;
	box_plain(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 0, WB_COLOR_SURFACE1);
	eyebrow(PANEL_X + PANEL_PAD, PANEL_Y + PANEL_PAD, "FLASHLIGHT");
	for (int i = 0; i < N_MODES; i++) {
		int cy = ROWS_Y + i * ROW_H + ROW_H / 2, on = i == S.mode;
		int x = keycap(PANEL_X + PANEL_PAD, cy, modes[i].keys);

		text(&f_ui28b, x + 20, text_top(&f_ui28b, cy), modes[i].name,
		     on ? WB_COLOR_CYAN : WB_COLOR_TEXT_HI, 0);
	}
	text(&f_ui16, PANEL_X + PANEL_PAD, ARROWS_Y, "The arrow keys step through them",
	     WB_COLOR_TEXT_MID, 0);
	hint_bar(close_hint, 1);
}

static void present(void)
{
	wb_bands(paint, 0, band, BAND_H, out_rows);
}

/* the keys go, the light comes */
static void start_over(void)
{
	S.start = 0;
	present();
	light();
}

/* 1: close */
static int key(int code)
{
	int m = S.mode;

	if (S.help) {		/* only an explanation: Esc or the diamond closes it */
		if ((code == KEY_ESC && !S.fn) || code == KEY_SYM_HELP) {
			S.help = 0;
			present();
		}
		return 0;
	}
	switch (code) {
	case KEY_ESC:
		return !S.fn;
	case KEY_SYM_MENU:
	case KEY_SYM_MAIN:
	case KEY_SYM_SECOND:
		m = code - KEY_SYM_MENU;
		break;
	case KEY_RIGHT:
	case KEY_DOWN:
		m = (m + 1) % N_MODES;
		break;
	case KEY_LEFT:
	case KEY_UP:
		m = (m + N_MODES - 1) % N_MODES;
		break;
	case KEY_SYM_HELP:
		S.help = 1;
		present();
		return 0;
	default:
		return 0;
	}
	if (m != S.mode) {
		S.mode = m;
		present();
		if (!S.start)
			light();
		mode_save();
	}
	return 0;
}

#ifdef FL_HOST

/* ---- the host test ------------------------------------------------------ */

static void save_ppm(const char *path)
{
	static uint8_t row[W * 3];
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	const char *head = "P6\n720 720\n255\n";

	if (fd < 0)
		_exit(1);
	write(fd, head, strlen(head));
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint16_t c = screen[y * W + x];

			row[x * 3] = (uint8_t)((c >> 11) * 255 / 31);
			row[x * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
			row[x * 3 + 2] = (uint8_t)((c & 31) * 255 / 31);
		}
		write(fd, row, sizeof(row));
	}
	close(fd);
}

static void say_int(const char *name, int v)
{
	char b[32], d[12];
	int n = 0, k = 0;

	strcpy(b, name);
	k = (int)strlen(b);
	b[k++] = ' ';
	if (v < 0) {
		b[k++] = '-';
		v = -v;
	}
	do
		d[n++] = (char)('0' + v % 10);
	while ((v /= 10));
	while (n)
		b[k++] = d[--n];
	b[k++] = '\n';
	write(1, b, (size_t)k);
}

/* flashlight-host KEYS OUT.ppm: \p the first START_MS pass, \e Esc, \f
 * Fn down, \g Fn up, \u \d \l \r the arrows, \1..\6 the symbol keys. Prints
 * the mode, whether the keys and the help show and the backlight (-1: the
 * system's). */
int main(int argc, char **argv)
{
	const char *k = argc > 1 ? argv[1] : "";

	wb_init();
	S.start = 1;
	mode_load();
	present();
	for (; *k; k++) {
		int code = -1;

		if (*k != '\\' || !k[1])
			continue;
		k++;
		if (*k == 'p' && S.start) {
			start_over();
			continue;
		}
		if (*k == 'f' || *k == 'g') {
			S.fn = *k == 'f';
			continue;
		}
		code = *k == 'e' ? KEY_ESC : *k == 'u' ? KEY_UP : *k == 'd' ? KEY_DOWN :
		       *k == 'l' ? KEY_LEFT : *k == 'r' ? KEY_RIGHT :
		       *k >= '1' && *k <= '6' ? KEY_SYM_MENU + (*k - '1') : -1;
		if (code >= 0 && key(code)) {
			write(1, "closed\n", 7);
			break;
		}
	}
	{
		/* what was drawn key by key must be the screen drawn whole */
		static uint16_t drawn[W * H];

		memcpy(drawn, screen, sizeof(screen));
		wb_rec_invalidate();
		present();
		if (memcmp(drawn, screen, sizeof(screen)))
			write(1, "stale\n", 6);
	}
	if (argc > 2)
		save_ppm(argv[2]);
	say_int("mode", S.mode);
	say_int("keys", S.start);
	say_int("help", S.help);
	say_int("light", bl_level);
	return 0;
}

#else

/* ---- the badge ---------------------------------------------------------- */

#define FIFO		"/tmp/flashlight.keys"

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t stop_sig, cont_sig;

static void on_signal(int s)
{
	stop_sig = s;
}

/* continued after a stop (the menu's console or screenshot): the panel may
 * show something else meanwhile */
static void on_cont(int s)
{
	(void)s;
	cont_sig = 1;
}

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* decimal key codes, one per word; 1: close */
static int fifo_keys(int fd)
{
	char b[64];
	int n = (int)read(fd, b, sizeof(b)), code = -1;

	for (int i = 0; i < n; i++) {
		if (b[i] >= '0' && b[i] <= '9') {
			code = (code < 0 ? 0 : code) * 10 + b[i] - '0';
		} else if (code >= 0) {
			if (key(code))
				return 1;
			code = -1;
		}
	}
	return 0;
}

int main(void)
{
	struct sigaction sa;
	struct pollfd p[2];
	long t0, touched = -TOUCH_S;
	int ev, fifo_fd = -1, was_standby = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	sa.sa_handler = on_cont;
	sigaction(SIGCONT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev < 0)
		return 1;
	if (!mkfifo(FIFO, 0600))
		fifo_fd = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	wb_init();
	S.start = 1;
	mode_load();
	present();
	t0 = now_ms();
	p[0].fd = ev;
	p[0].events = POLLIN;
	p[1].fd = fifo_fd;
	p[1].events = POLLIN;
	while (!stop_sig) {
		long left = START_MS - (now_ms() - t0), now;
		int k, standby, quit = 0;

		k = poll(p, fifo_fd >= 0 ? 2 : 1, S.start && left > 0 && left < 250 ? (int)left : 250);
		now = power_now();
		standby = power_in_standby();
		/* the panel again after standby or a stop; a light that came
		 * on in standby would shine on a blank screen */
		if (cont_sig || (was_standby && !standby)) {
			cont_sig = 0;
			wb_rec_invalidate();
			present();
		}
		was_standby = standby;
		if (S.start && !standby && now_ms() - t0 >= START_MS)
			start_over();
		if (!standby && now - touched >= TOUCH_S) {
			power_touch(now);
			touched = now;
		}
		if (k <= 0)
			continue;
		if (fifo_fd >= 0 && (p[1].revents & POLLIN))
			quit = fifo_keys(fifo_fd);
		if (p[0].revents & POLLIN) {
			struct ev evs[16];
			int got = (int)read(ev, evs, sizeof(evs)) / (int)sizeof(evs[0]);

			for (int q = 0; q < got && !quit; q++) {
				if (evs[q].type != 1)
					continue;
				if (evs[q].code == KEY_FN)
					S.fn = evs[q].value != 0;
				else if (evs[q].value == 1)
					quit = key(evs[q].code);
			}
		}
		if (quit)
			break;
	}
	if (fifo_fd >= 0)
		unlink(FIFO);
	return 0;
}
#endif
