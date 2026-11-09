/* SPDX-License-Identifier: MIT */
/*
 * whyos/core.h: what every app needs apart from its screen. The app's
 * places (APP_CONFIG and the rest), small files written whole and safely,
 * its settings, its log, the clock, keeping the screen on, the backlight,
 * and the signals that end it. Part of the WhyOS SDK: include whyos.h.
 *
 * No stdio: programs are FLAT and NOMMU (README.md, "Limits").
 */
#pragma once

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>		/* rename(); no other stdio */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WOS_SDK_VERSION	"0.1.0"

/* ---- text without stdio ------------------------------------------------- */

/* s into d (size bytes with its 0), cut if needed; returns d */
static char *wos_copy(char *d, const char *s, int size)
{
	int n = 0;

	if (size <= 0)
		return d;
	while (s && s[n] && n < size - 1) {
		d[n] = s[n];
		n++;
	}
	d[n] = 0;
	return d;
}

/* s added to the end of d */
static char *wos_add(char *d, const char *s, int size)
{
	int n = (int)strlen(d);

	wos_copy(d + n, s, size - n);
	return d;
}

/* v in decimal added to the end of d */
static char *wos_add_int(char *d, long v, int size)
{
	char b[24];
	int n = 0, k = 0;
	unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;

	do
		b[n++] = (char)('0' + u % 10);
	while ((u /= 10));
	{
		char o[24];

		if (v < 0)
			o[k++] = '-';
		while (n)
			o[k++] = b[--n];
		o[k] = 0;
		return wos_add(d, o, size);
	}
}

/* the byte where the UTF-8 character before i starts, and after i */
static int wos_prev_char(const char *s, int i)
{
	if (i <= 0)
		return 0;
	while (--i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		;
	return i;
}

static int wos_next_char(const char *s, int i)
{
	if (!s[i])
		return i;
	while (((unsigned char)s[++i] & 0xc0) == 0x80)
		;
	return i;
}

/* the number at the start of s, or def when there is none */
static long wos_int(const char *s, long def)
{
	long v = 0;
	int neg = 0, any = 0;

	if (!s)
		return def;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s++ - '0');
		any = 1;
	}
	return any ? (neg ? -v : v) : def;
}

/* ---- the app's places ---------------------------------------------------- */

enum { WOS_CONFIG, WOS_DATA, WOS_CACHE, WOS_DIR, WOS_TMP };

/* the app's id: APP_ID, which apprun sets ("app" without it) */
static const char *wos_id(void)
{
	const char *id = getenv("APP_ID");

	return id && id[0] && !strchr(id, '/') ? id : "app";
}

/* a directory and those above it, when it is missing (apprun makes the
 * places; above them the sandbox refuses writes) */
static void wos_mkdirs(const char *path)
{
	char b[256];
	struct stat st;

	if (!stat(path, &st) || mkdir(path, 0755) == 0 || errno != ENOENT)
		return;
	wos_copy(b, path, sizeof(b));
	for (char *p = b + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(b, 0755);
			*p = '/';
		}
	mkdir(b, 0755);
}

/*
 * A path in one of the app's places: WOS_CONFIG (settings, small state),
 * WOS_DATA (on the card), WOS_CACHE, WOS_DIR (the app's own files, read
 * only) or WOS_TMP (/tmp/app-<id>, gone when the app ends). The place is
 * made when missing. name may be 0 for the place itself. 0 when the place
 * is not there (no card for WOS_DATA) or the path does not fit.
 */
static const char *wos_path(int place, const char *name, char *buf, int size)
{
	static const char *const vars[] = { "APP_CONFIG", "APP_DATA", "APP_CACHE", "APP_DIR" };
	const char *dir = place < WOS_TMP ? getenv(vars[place]) : 0;
	char tmp[96];

	if (place == WOS_TMP) {
		wos_copy(tmp, "/tmp/app-", sizeof(tmp));
		dir = wos_add(tmp, wos_id(), sizeof(tmp));
	}
	if (!dir || !dir[0] || (int)(strlen(dir) + (name ? strlen(name) + 1 : 0)) >= size)
		return 0;
	if (place != WOS_DIR)
		wos_mkdirs(dir);
	wos_copy(buf, dir, size);
	if (name) {
		wos_add(buf, "/", size);
		wos_add(buf, name, size);
	}
	return buf;
}

/* ---- files --------------------------------------------------------------- */

/* A small file whole into buf, ended with a 0; its length, or -1. */
static int wos_read_file(const char *path, char *buf, int size)
{
	int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1, n = 0, k;

	if (fd < 0)
		return -1;
	while (n < size - 1 && (k = (int)read(fd, buf + n, (size_t)(size - 1 - n))) > 0)
		n += k;
	close(fd);
	buf[n] = 0;
	return n;
}

/*
 * A file written whole: to path.new, synced, then renamed over path, so a
 * reset or a full card leaves the old file or the new one, never half of
 * one. 0 when it is written.
 */
static int wos_write_file(const char *path, const void *data, int n)
{
	char tmp[264];
	int fd, ok;

	if (!path || (int)strlen(path) + 5 > (int)sizeof(tmp))
		return -1;
	wos_copy(tmp, path, sizeof(tmp));
	wos_add(tmp, ".new", sizeof(tmp));
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	ok = write(fd, data, (size_t)n) == n && !fsync(fd);
	close(fd);
	if (ok && !rename(tmp, path))
		return 0;
	unlink(tmp);
	return -1;
}

/* ---- the clock ----------------------------------------------------------- */

/* Milliseconds since boot (CLOCK_MONOTONIC: NTP does not move it). */
static long wos_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Has the wall clock been set? The badge has no clock that keeps time
 * when it is off; NTP sets it once Wi-Fi is up. */
static int wos_clock_set(void)
{
	return time(0) > 1767225600;	/* 2026-01-01 */
}

/* ---- settings ------------------------------------------------------------ */

/*
 * The app's settings and small state, as key=value lines in
 * APP_CONFIG/settings. Read at the first use; written whole at every
 * change. A setting in the app's schema (wos_setting, app.h) gets its
 * default from there.
 */
#define WOS_KV_MAX	32
#define WOS_KEY_LEN	24
#define WOS_VAL_LEN	72

static struct {
	char key[WOS_KEY_LEN], val[WOS_VAL_LEN];
} wos_kv[WOS_KV_MAX];
static int wos_n_kv, wos_kv_read;
static const char *(*wos_kv_default)(const char *key);	/* the schema's (app.h) */

static void wos_kv_load(void)
{
	char path[256], buf[WOS_KV_MAX * (WOS_KEY_LEN + WOS_VAL_LEN)];
	char *p = buf;

	wos_kv_read = 1;
	if (!wos_path(WOS_CONFIG, "settings", path, sizeof(path)) || wos_read_file(path, buf, sizeof(buf)) < 0)
		return;
	while (*p && wos_n_kv < WOS_KV_MAX) {
		char *nl = strchr(p, '\n'), *eq;

		if (nl)
			*nl = 0;
		eq = strchr(p, '=');
		if (eq && eq > p && eq - p < WOS_KEY_LEN) {
			*eq = 0;
			wos_copy(wos_kv[wos_n_kv].key, p, WOS_KEY_LEN);
			wos_copy(wos_kv[wos_n_kv].val, eq + 1, WOS_VAL_LEN);
			wos_n_kv++;
		}
		if (!nl)
			break;
		p = nl + 1;
	}
}

static void wos_kv_save(void)
{
	char path[256], buf[WOS_KV_MAX * (WOS_KEY_LEN + WOS_VAL_LEN + 2)] = "";

	for (int i = 0; i < wos_n_kv; i++) {
		wos_add(buf, wos_kv[i].key, sizeof(buf));
		wos_add(buf, "=", sizeof(buf));
		wos_add(buf, wos_kv[i].val, sizeof(buf));
		wos_add(buf, "\n", sizeof(buf));
	}
	if (wos_path(WOS_CONFIG, "settings", path, sizeof(path)))
		wos_write_file(path, buf, (int)strlen(buf));
}

/* A setting, or def; with def 0 the schema's default ("" without one). */
static const char *wos_get(const char *key, const char *def)
{
	if (!wos_kv_read)
		wos_kv_load();
	for (int i = 0; i < wos_n_kv; i++)
		if (!strcmp(wos_kv[i].key, key))
			return wos_kv[i].val;
	if (!def && wos_kv_default)
		def = wos_kv_default(key);
	return def ? def : "";
}

/* A setting as a number: the schema's default when none was set, def
 * when there is neither or it is no number. */
static long wos_get_int(const char *key, long def)
{
	return wos_int(wos_get(key, 0), def);
}

/* A setting changed, and written at once (only when it changed). Values
 * hold no newline; longer than WOS_VAL_LEN - 1 they are cut. */
static void wos_set(const char *key, const char *val)
{
	int i;

	if (!wos_kv_read)
		wos_kv_load();
	if (strlen(key) >= WOS_KEY_LEN || strchr(key, '=') || strchr(key, '\n') || strchr(val, '\n'))
		return;
	for (i = 0; i < wos_n_kv && strcmp(wos_kv[i].key, key); i++)
		;
	if (i == wos_n_kv) {
		if (i == WOS_KV_MAX)
			return;
		wos_copy(wos_kv[i].key, key, WOS_KEY_LEN);
		wos_kv[i].val[0] = 0;
		wos_n_kv++;
	} else if (!strncmp(wos_kv[i].val, val, WOS_VAL_LEN - 1) && strlen(val) < WOS_VAL_LEN) {
		return;
	}
	wos_copy(wos_kv[i].val, val, WOS_VAL_LEN);
	wos_kv_save();
}

static void wos_set_int(const char *key, long v)
{
	char b[24] = "";

	wos_set(key, wos_add_int(b, v, sizeof(b)));
}

/* ---- the log ------------------------------------------------------------- */

/*
 * A line in the app's log: the last WOS_LOG_N lines are what the circle
 * key's screen shows, and each also goes to stderr, which apprun keeps in
 * /tmp/app-<id>.log.
 */
#define WOS_LOG_N	40
#define WOS_LOG_LEN	96

static char wos_log_ring[WOS_LOG_N][WOS_LOG_LEN];
static int wos_log_count;		/* lines so far */
static int wos_log_seen;		/* the log changed: draw again (app.h) */

static void wos_log(const char *line)
{
	char *d = wos_log_ring[wos_log_count % WOS_LOG_N];

	wos_copy(d, line, WOS_LOG_LEN);
	for (char *p = d; *p; p++)
		if (*p == '\n')
			*p = ' ';
	wos_log_count++;
	wos_log_seen = 0;
	write(2, d, strlen(d));
	write(2, "\n", 1);
}

/* the i-th line of those kept, the oldest first; 0 past the last */
static const char *wos_log_line(int i)
{
	int kept = wos_log_count < WOS_LOG_N ? wos_log_count : WOS_LOG_N;

	if (i < 0 || i >= kept)
		return 0;
	return wos_log_ring[(wos_log_count - kept + i) % WOS_LOG_N];
}

/* ---- power ---------------------------------------------------------------- */

#define WOS_ACTIVE	"/run/active"	/* the monotonic second of the last key */
#define WOS_STANDBY	"/run/standby"	/* there while the badge is in standby */
#define WOS_BACKLIGHT	"/sys/class/backlight/backlight/brightness"

/* Is the badge in standby (screen off; Fn+Esc or no key for a while)? */
static int wos_standby(void)
{
	return access(WOS_STANDBY, F_OK) == 0;
}

/* As a key would: the screen stays on for the system's time from now.
 * Never in standby: the user turned the screen off. */
static void wos_keep_on(void)
{
	char b[24] = "";
	int fd;

	if (wos_standby())
		return;
	wos_add_int(b, wos_ms() / 1000, sizeof(b));
	wos_add(b, "\n", sizeof(b));
	fd = open(WOS_ACTIVE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		write(fd, b, strlen(b));
		close(fd);
	}
}

/*
 * The backlight while the app runs: the panel's duty, 10 to 80 (about 80
 * is the brightest; above that it gets less bright again). apprun puts
 * the system's back when the app ends. -1 when nothing was set.
 */
static int wos_duty = -1;

static void wos_backlight(int duty)
{
	char b[16] = "";
	int fd;

	duty = duty < 10 ? 10 : duty > 80 ? 80 : duty;
	wos_add_int(b, duty, sizeof(b));
	wos_add(b, "\n", sizeof(b));
	fd = open(WOS_BACKLIGHT, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, b, strlen(b)) > 0)
		wos_duty = duty;
	close(fd);
}

/* ---- the end ------------------------------------------------------------- */

/*
 * SIGTERM (the logo key: apprun), SIGHUP and SIGINT end the app after the
 * key or frame at hand; SIGCONT (after the screenshot's question or the
 * console) has the screen drawn again, as something else was on it.
 */
static volatile sig_atomic_t wos_stop_sig, wos_cont_sig;

static void wos_on_stop(int s)
{
	wos_stop_sig = s;
}

static void wos_on_cont(int s)
{
	(void)s;
	wos_cont_sig = 1;
}

static void wos_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = wos_on_stop;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	sa.sa_handler = wos_on_cont;
	sigaction(SIGCONT, &sa, 0);
}
