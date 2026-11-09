// SPDX-License-Identifier: MIT
/*
 * What a sandboxed app may reach: its own places, what every app gets, and what
 * its permissions add (PERMISSIONS in app.conf, which the store writes). Paths
 * come in as the app gives them and leave as absolute paths for Linux to open,
 * so a path checked here is the path used. No stdio: this is part of c1urun.
 */
#pragma once
#include <string.h>

#define P_NETWORK	(1u << 0)	/* the internet and devices on the network */
#define P_BLUETOOTH	(1u << 1)
#define P_NETWORK_ADMIN	(1u << 8)	/* network, whole packets, and wpa_supplicant */
#define P_FILES_ALL	(1u << 2)	/* the card outside badge/, and the screenshots */
#define P_LORA		(1u << 3)	/* meshcored's socket */
#define P_USB		(1u << 4)	/* serial devices on J8 */
#define P_HID		(1u << 5)	/* keyboard and mouse towards a computer */
#define P_GPIO		(1u << 6)	/* the pins of the expansion headers */
#define P_SYSTEM	(1u << 7)	/* everything, as root: the store warns about it */

#define POLICY_WIFI_STATUS	"/run/wifi-status"
#define POLICY_PATH	256
#define POLICY_GRANTS	8

struct policy {
	unsigned perms;
	char dir[POLICY_PATH];		/* read only */
	char lent[POLICY_PATH];		/* read only: another app's program it runs */
	char data[POLICY_PATH], config[POLICY_PATH], cache[POLICY_PATH];
	char settings[POLICY_PATH];	/* its own, in /etc/badge/settings.d */
	char tmp[POLICY_PATH];		/* what the app calls /tmp */
	char grants[POLICY_GRANTS][POLICY_PATH];	/* files for this run */
	int ngrants;
};

static const struct {
	const char *name;
	unsigned bit;
} policy_names[] = {
	{ "network", P_NETWORK }, { "network-admin", P_NETWORK | P_NETWORK_ADMIN },
	{ "bluetooth", P_BLUETOOTH }, { "files-all", P_FILES_ALL },
	{ "lora", P_LORA }, { "usb", P_USB }, { "hid", P_HID },
	{ "gpio", P_GPIO }, { "system", P_SYSTEM },
};

/* Every app: a trailing / is a whole tree, anything else that path alone. */
static const char *const policy_read[] = {
	"/etc/whyos-release", "/etc/badge/settings", "/etc/localtime", "/etc/TZ", "/etc/timezone",
	"/etc/resolv.conf", "/etc/hosts", "/etc/services", "/etc/protocols", "/etc/nsswitch.conf",
	"/etc/passwd", "/etc/group", "/etc/ssl/", "/usr/", "/lib/", "/bin/", "/sbin/",
	"/proc/self/", "/proc/meminfo", "/proc/uptime", "/proc/cpuinfo", "/proc/stat",
	"/proc/loadavg", "/proc/version", "/proc/userpool", "/sys/class/graphics/",
	"/sys/class/backlight/", "/sys/class/leds/", "/sys/class/drm/", "/sys/bus/iio/",
	"/sys/devices/", "/var/run/lora.pid", "/var/run/wpa_supplicant/wlan0", "/run/standby", "/run/sound.state", "/dev/urandom", "/dev/random", "/dev/zero", "/dev/input/event0", "/dev/ppa", "/", "/mnt", "/mnt/sd", 0
};
static const char *const policy_write[] = {
	"/dev/null", "/dev/fb0", "/dev/tty0", "/dev/tty1", "/dev/tty", "/dev/console",
	"/dev/snd/pcmC0D0p", "/dev/snd/controlC0",	/* sound out, not in: c1urun picks the output */
	"/sys/class/backlight/", "/sys/class/leds/", "/run/active", 0
};
/* What only the user's own choice of a file opens: writing here reaches past the sandbox. */
static const char *const policy_closed[] = {
	"/tmp/home.keys", "/tmp/sleepd.keys", "/tmp/terminal.pid", "/tmp/rotation-lock",
	"/mnt/sd/badge/", 0
};

/* Starts with this entry of a list; see above for what a trailing / means. */
static int policy_listed(const char *path, const char *const *list)
{
	for (int i = 0; list[i]; i++) {
		size_t n = strlen(list[i]);

		/* "/tmp/" is also /tmp itself */
		if (n > 1 && list[i][n - 1] == '/' && !strncmp(path, list[i], n - 1) && !path[n - 1])
			return 1;
		if (strncmp(path, list[i], n))
			continue;
		if (list[i][n - 1] == '/' ? n > 1 || !path[1] : !path[n])
			return 1;
	}
	return 0;
}

static int policy_under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return n && !strncmp(path, dir, n) && (!path[n] || path[n] == '/');
}

/* On the way to one of its own places: mkdir -p and stat walk down these. */
static int policy_above(const char *path, const char *place)
{
	size_t n = strlen(path);

	return place[0] && n > 1 && !strncmp(place, path, n) && place[n] == '/';
}

/* The names in text, which is PERMISSIONS' value: what is unknown gives nothing. */
static unsigned policy_perms(const char *text)
{
	unsigned perms = 0;

	while (*text) {
		size_t n = strcspn(text, " \t\r\n\"");

		for (unsigned i = 0; i < sizeof(policy_names) / sizeof(policy_names[0]); i++)
			if (strlen(policy_names[i].name) == n && !strncmp(text, policy_names[i].name, n))
				perms |= policy_names[i].bit;
		text += n;
		text += strspn(text, " \t\r\n\"");
	}
	return perms;
}

/* "a/b/../c" as /a/c, into out; 0 if it climbs out of the top or is too long. */
static int policy_normal(const char *path, char *out, size_t size)
{
	size_t n = 0;

	while (*path) {
		const char *e = path;

		while (*e && *e != '/')
			e++;
		if (e - path == 2 && path[0] == '.' && path[1] == '.') {
			if (!n)
				return 0;
			while (n && out[--n] != '/')
				;
		} else if (e > path && !(e - path == 1 && path[0] == '.')) {
			if (n + (size_t)(e - path) + 2 >= size)
				return 0;
			out[n++] = '/';
			memcpy(out + n, path, e - path);
			n += e - path;
		}
		path = *e ? e + 1 : e;
	}
	if (!n)
		out[n++] = '/';
	out[n] = 0;
	return 1;
}

/* A device of a family: /dev/ttyUSB3 for "/dev/ttyUSB", with only a number after it. */
static int policy_numbered(const char *path, const char *base)
{
	size_t n = strlen(base);

	if (strncmp(path, base, n) || !path[n])
		return 0;
	return strspn(path + n, "0123456789") == strlen(path + n);
}

static int policy_by_perm(const struct policy *p, const char *path, int write)
{
	if ((p->perms & P_FILES_ALL) && (policy_under(path, "/mnt/sd") ||
					 policy_under(path, "/root/screenshots")))
		return 1;
	if ((p->perms & P_USB) && (policy_numbered(path, "/dev/ttyUSB") ||
				   policy_numbered(path, "/dev/ttyACM")))
		return 1;
	if ((p->perms & P_HID) && policy_numbered(path, "/dev/hidg"))
		return 1;
	if ((p->perms & P_GPIO) && !strcmp(path, "/dev/gpiochip0"))
		return 1;
	if (write)
		return 0;
	if ((p->perms & (P_USB | P_HID)) && (!strcmp(path, "/dev") || !strcmp(path, "/var/run/usb.state")))
		return 1;
	if ((p->perms & P_USB) && policy_under(path, "/sys/bus/usb"))
		return 1;
	return (p->perms & P_NETWORK) && policy_under(path, "/sys/class/net");
}

/*
 * The path the app means, as out, from its current directory cwd: 1 if it may go
 * there. /tmp is its own: another app's files and the system's are not in it.
 */
static int policy_path(const struct policy *p, const char *cwd, const char *path, int write,
		       char *out, size_t size)
{
	char joined[2 * POLICY_PATH], full[POLICY_PATH];
	size_t n = strlen(path);

	if (n >= POLICY_PATH)
		return 0;
	if (path[0] == '/') {
		memcpy(joined, path, n + 1);
	} else {
		size_t c = strlen(cwd);

		memcpy(joined, cwd, c);
		joined[c] = '/';
		memcpy(joined + c + 1, path, n + 1);
	}
	if (!policy_normal(joined, full, sizeof(full)))
		return 0;
	if (p->perms & P_SYSTEM) {
		if (strlen(full) + 1 > size)
			return 0;
		memcpy(out, full, strlen(full) + 1);
		return 1;
	}
	for (int i = 0; i < p->ngrants; i++)
		if (!strcmp(full, p->grants[i])) {
			if (strlen(full) + 1 > size)
				return 0;
			memcpy(out, full, strlen(full) + 1);
			return 1;
		}
	/* the wrappers the menu writes for a shell app, read only */
	if (!write && policy_under(full, "/tmp/app-bin")) {
		if (strlen(full) + 1 > size)
			return 0;
		memcpy(out, full, strlen(full) + 1);
		return 1;
	}
	if (policy_under(full, "/tmp") && p->tmp[0]) {
		size_t t = strlen(p->tmp), rest = strlen(full + 4);

		if (t + rest + 1 > size)
			return 0;
		if (!policy_under(full, p->tmp)) {
			memcpy(out, p->tmp, t);
			memcpy(out + t, full + 4, rest + 1);
			return 1;
		}
	}
	if (strlen(full) + 1 > size)
		return 0;
	memcpy(out, full, strlen(full) + 1);
	if ((p->tmp[0] && policy_under(full, p->tmp)) || policy_under(full, p->data) ||
	    policy_under(full, p->config) || policy_under(full, p->cache) ||
	    (p->settings[0] && !strcmp(full, p->settings)))
		return 1;
	if (!write && (policy_under(full, p->dir) || policy_under(full, p->lent) ||
		       policy_above(full, p->dir) || policy_above(full, p->data) ||
		       policy_above(full, p->config) || policy_above(full, p->cache)))
		return 1;
	if (policy_listed(full, policy_closed))
		return 0;
	if (policy_by_perm(p, full, write) || policy_listed(full, policy_write))
		return 1;
	return !write && policy_listed(full, policy_read);
}

/* socket(): families and types. AF_UNIX stays, for the app's own sockets. */
static int policy_socket(const struct policy *p, int family, int type, int protocol)
{
	int kind = type & 0xf;		/* without SOCK_CLOEXEC and SOCK_NONBLOCK */

	if (p->perms & P_SYSTEM)
		return 1;
	switch (family) {
	case 1:				/* AF_UNIX */
		return 1;
	case 2:				/* AF_INET */
	case 10:			/* AF_INET6 */
		if (kind == 3)		/* SOCK_RAW */
			return !!(p->perms & P_NETWORK_ADMIN);
		return !!(p->perms & P_NETWORK);
	case 16:			/* AF_NETLINK: routes, for getaddrinfo */
		return (p->perms & P_NETWORK) && protocol == 0;
	case 17:			/* AF_PACKET */
		return !!(p->perms & P_NETWORK_ADMIN);
	case 31:			/* AF_BLUETOOTH */
		return !!(p->perms & P_BLUETOOTH);
	default:
		return 0;
	}
}

/* A Unix socket by name: its own files, and what a permission opens. No abstract ones. */
static int policy_unix(const struct policy *p, const char *cwd, const char *path, char *out, size_t size)
{
	if (!path[0])
		return 0;
	if (p->perms & P_SYSTEM)
		return policy_path(p, cwd, path, 1, out, size);
	if (((p->perms & P_LORA) && !strcmp(path, "/var/run/meshcored.sock")) ||
	    ((p->perms & P_NETWORK_ADMIN) && !strcmp(path, "/var/run/wpa_supplicant/wlan0"))) {
		if (strlen(path) + 1 > size)
			return 0;
		memcpy(out, path, strlen(path) + 1);
		return 1;
	}
	/* the status bar of any app: netd says connected and how well, and nothing else */
	if (!strcmp(path, "/var/run/wpa_supplicant/wlan0")) {
		if (sizeof(POLICY_WIFI_STATUS) > size)
			return 0;
		memcpy(out, POLICY_WIFI_STATUS, sizeof(POLICY_WIFI_STATUS));
		return 1;
	}
	return policy_path(p, cwd, path, 1, out, size) && !policy_listed(out, policy_write);
}

/*
 * A file for this run, that file only. The user's choice is the consent, also
 * outside the user's files; what a parent hands over it must reach itself,
 * which the caller checks with policy_path().
 */
static void policy_grant(struct policy *p, const char *path)
{
	char full[POLICY_PATH];

	if (p->ngrants < POLICY_GRANTS && path[0] == '/' && strlen(path) < POLICY_PATH &&
	    policy_normal(path, full, sizeof(full)))
		strcpy(p->grants[p->ngrants++], full);
}
