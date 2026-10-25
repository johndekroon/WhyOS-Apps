// SPDX-License-Identifier: MIT
/*
 * power.h — when the badge saves power, after how long without a key
 * (docs/POWER.md "Power management"). home applies it while it is in
 * front, sleepd otherwise (apps, the shell); both note every key in
 * POWER_ACTIVE, so the time runs on when one hands over to the other.
 *
 *   keys off   the keyboard light goes out; the next key lights it again
 *   standby    screen, both lights and the scan-out off, POWER_STANDBY
 *              exists (sleepd stops tilting, usbd stops probing J8, netd
 *              lets Wi-Fi save power); the next key wakes, and is eaten
 *   shut down  poweroff
 *
 * Times are CLOCK_MONOTONIC seconds: the wall clock jumps when NTP sets
 * it, decades at a time after a boot.
 *
 * No stdio (sleepd is FLAT and uses raw write(2)).
 */
#ifndef POWER_H
#define POWER_H

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#define POWER_ACTIVE	"/run/active"	/* monotonic second of the last key */
#define POWER_STANDBY	"/run/standby"	/* there while in standby */

/* Seconds after the last key; 0 = never. */
struct power_cfg {
	int keys, screen, shutdown;
	int wifi;		/* power.wifi: Wi-Fi saves power in standby */
};

enum power_state { PW_AWAKE, PW_KEYS_OFF, PW_STANDBY, PW_SHUTDOWN };

/* The choices Settings > Power offers, in seconds (0 = Never). */
static const int power_keys_choices[] = { 10, 30, 60, 120, 0 };
static const int power_screen_choices[] = { 30, 60, 120, 300, 600, 0 };
static const int power_shutdown_choices[] = { 0, 1800, 3600, 7200, 14400 };

#define POWER_N(a)	((int)(sizeof(a) / sizeof((a)[0])))

/* What idle seconds without a key mean. A later stage includes the earlier
 * ones: standby has the keyboard light off too. */
static inline enum power_state power_state(const struct power_cfg *c, long idle)
{
	if (c->shutdown > 0 && idle >= c->shutdown)
		return PW_SHUTDOWN;
	if (c->screen > 0 && idle >= c->screen)
		return PW_STANDBY;
	if (c->keys > 0 && idle >= c->keys)
		return PW_KEYS_OFF;
	return PW_AWAKE;
}

/* Seconds until power_state() changes with no key, or -1 if it never
 * does. */
static inline long power_next(const struct power_cfg *c, long idle)
{
	const int at[3] = { c->keys, c->screen, c->shutdown };
	long next = -1;

	for (int i = 0; i < 3; i++)
		if (at[i] > 0 && at[i] > idle && (next < 0 || at[i] - idle < next))
			next = at[i] - idle;
	return next;
}

/* A setting's value in seconds; dflt when empty or not a number. */
static inline int power_secs(const char *v, int dflt)
{
	int n = 0;

	if (!v || !*v)
		return dflt;
	for (; *v; v++) {
		if (*v < '0' || *v > '9' || n > 1000000)
			return dflt;
		n = n * 10 + (*v - '0');
	}
	return n;
}

/* "30 s", "2 min", "1 hour", "Never" into buf (16 bytes is enough). */
static inline const char *power_label(char *buf, int secs)
{
	int n, k = 0;
	char d[8];
	const char *unit;

	if (secs <= 0)
		return "Never";
	if (secs % 3600 == 0) {
		n = secs / 3600;
		unit = n == 1 ? " hour" : " hours";
	} else if (secs % 60 == 0) {
		n = secs / 60;
		unit = " min";
	} else {
		n = secs;
		unit = " s";
	}
	do
		d[k++] = (char)('0' + n % 10);
	while ((n /= 10) && k < (int)sizeof(d));
	for (int i = 0; i < k; i++)
		buf[i] = d[k - 1 - i];
	for (int i = 0; ; i++)
		if (!(buf[k + i] = unit[i]))
			break;
	return buf;
}

#ifdef SETTINGS_H
/* The brightness a light goes back to after idling: what the menu stored
 * (display.brightness, display.keys; 0 when it was switched off), else
 * dflt. Not what sysfs says, which may be the idle 0. */
static inline int power_lit(struct settings *ss, const char *key, int dflt)
{
	struct setting *v = settings_find(ss, key);

	return power_secs(v ? v->value : 0, dflt);
}

static inline void power_cfg_load(struct power_cfg *c, struct settings *ss)
{
	struct setting *v;

	v = settings_find(ss, "power.keys");
	c->keys = power_secs(v ? v->value : 0, 30);
	v = settings_find(ss, "power.screen");
	c->screen = power_secs(v ? v->value : 0, 120);
	v = settings_find(ss, "power.shutdown");
	c->shutdown = power_secs(v ? v->value : 0, 0);
	v = settings_find(ss, "power.wifi");
	c->wifi = !v || v->value[0] != 'o' || v->value[1] != 'f';
}
#endif

#ifndef POWER_NO_RUNTIME
static inline long power_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec;
}

/* A key now: POWER_ACTIVE gets the monotonic second. */
static inline void power_touch(long now)
{
	char buf[16];
	int n = 0, fd;

	if (now < 0)
		now = 0;
	do
		buf[n++] = (char)('0' + now % 10);
	while ((now /= 10) && n < (int)sizeof(buf) - 1);
	for (int i = 0; i < n / 2; i++) {
		char t = buf[i];

		buf[i] = buf[n - 1 - i];
		buf[n - 1 - i] = t;
	}
	buf[n++] = '\n';
	fd = open(POWER_ACTIVE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0) {
		write(fd, buf, (size_t)n);
		close(fd);
	}
}

/* The last key's monotonic second; -1 when none was noted yet. */
static inline long power_last(void)
{
	char buf[16];
	int fd = open(POWER_ACTIVE, O_RDONLY | O_CLOEXEC), n;
	long v = 0;

	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0 || buf[0] < '0' || buf[0] > '9')
		return -1;
	for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
		v = v * 10 + (buf[i] - '0');
	return v;
}

/* Seconds without a key, now; a badge nobody touched yet counts from
 * boot (the monotonic clock starts there). */
static inline long power_idle(long now)
{
	long last = power_last();

	return last < 0 || last > now ? now : now - last;
}

static inline void power_standby_flag(int on)
{
	if (on) {
		int fd = open(POWER_STANDBY, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

		if (fd >= 0)
			close(fd);
	} else {
		unlink(POWER_STANDBY);
	}
}

static inline int power_in_standby(void)
{
	return access(POWER_STANDBY, F_OK) == 0;
}
#endif

#endif
