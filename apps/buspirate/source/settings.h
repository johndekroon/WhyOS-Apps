// SPDX-License-Identifier: MIT
/*
 * settings.h — the badge's general settings (docs/SETTINGS.md), shared by
 * the setting command (setting.c) and the start menu (home.c).
 *
 * Two files, one setting a line, fields separated by a tab:
 *
 *   key <TAB> flags <TAB> label <TAB> value
 *
 * flags is "menu" (shown and editable under Settings > General, as label)
 * or "-". The value is the rest of the line. '#' starts a comment line.
 *
 *   SETTINGS_DEFAULTS  in the rootfs: the settings a badge comes with
 *   SETTINGS_FILE      in /etc (flash, survives reboots and new images):
 *                      what was set; a key here wins over its default
 *
 * Only settings that were set are written, so a newer image's defaults
 * still come through for the others. Saving writes a new file and renames
 * it over the old one: a reset halfway leaves one or the other.
 * FLAT/NOMMU (see Makefile): no stdio.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>	/* rename() only: no stdio streams are used */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SETTINGS_DEFAULTS	/* the host test puts them elsewhere */
#define SETTINGS_DEFAULTS	"/usr/share/badge/settings"
#define SETTINGS_DIR		"/etc/badge"
#endif
#define SETTINGS_FILE		SETTINGS_DIR "/settings"
#define SETTINGS_MAX		32

struct setting {
	char key[32];
	char label[40];
	char value[128];
	int menu;	/* shown under Settings > General */
	int set;	/* from SETTINGS_FILE or changed: saved */
};

struct settings {
	int n;
	struct setting s[SETTINGS_MAX];
};

/* A key: letters, digits, '.', '_' and '-', 1 to 31 of them. */
static inline int settings_key_ok(const char *k)
{
	size_t n = strlen(k);

	if (!n || n >= sizeof(((struct setting *)0)->key))
		return 0;
	for (; *k; k++)
		if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') ||
		      (*k >= '0' && *k <= '9') || *k == '.' || *k == '_' || *k == '-'))
			return 0;
	return 1;
}

static inline struct setting *settings_find(struct settings *ss, const char *key)
{
	for (int i = 0; i < ss->n; i++)
		if (!strcmp(ss->s[i].key, key))
			return &ss->s[i];
	return 0;
}

/* copy a field of at most n bytes up to stop (or the end), NUL-terminated */
static inline const char *settings_field(char *dst, size_t size, const char *p,
					 const char *end, char stop)
{
	size_t i = 0;

	while (p < end && *p != stop) {
		if (i + 1 < size)
			dst[i++] = *p;
		p++;
	}
	dst[i] = 0;
	return p < end ? p + 1 : p;
}

/* Read one file into ss, a line overriding an earlier one with its key. */
static inline void settings_read(struct settings *ss, const char *path, int set)
{
	static char buf[8192];
	int fd = open(path, O_RDONLY | O_CLOEXEC), n = 0, r;

	if (fd < 0)
		return;
	while (n < (int)sizeof(buf) - 1 && (r = (int)read(fd, buf + n, sizeof(buf) - 1 - (size_t)n)) > 0)
		n += r;
	close(fd);
	for (const char *p = buf, *end = buf + n; p < end; ) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		struct setting t, *s;
		char flags[16];

		if (!eol)
			eol = end;
		if (*p != '#' && p < eol) {
			memset(&t, 0, sizeof(t));
			p = settings_field(t.key, sizeof(t.key), p, eol, '\t');
			p = settings_field(flags, sizeof(flags), p, eol, '\t');
			p = settings_field(t.label, sizeof(t.label), p, eol, '\t');
			settings_field(t.value, sizeof(t.value), p, eol, '\n');
			t.menu = !strcmp(flags, "menu");
			t.set = set;
			if (settings_key_ok(t.key)) {
				s = settings_find(ss, t.key);
				if (!s && ss->n < SETTINGS_MAX)
					s = &ss->s[ss->n++];
				if (s)
					*s = t;
			}
		}
		p = eol + 1;
	}
}

/* Defaults, then what was set. */
static inline void settings_load(struct settings *ss)
{
	ss->n = 0;
	settings_read(ss, SETTINGS_DEFAULTS, 0);
	settings_read(ss, SETTINGS_FILE, 1);
}

/*
 * Set key to value (a value cannot hold a newline: it is cut there). A new
 * key gets the label key and stays out of the menu. 0, or -1 when the key
 * is not one or there is no room.
 */
static inline int settings_set(struct settings *ss, const char *key, const char *value)
{
	struct setting *s = settings_find(ss, key);

	if (!settings_key_ok(key))
		return -1;
	if (!s) {
		if (ss->n >= SETTINGS_MAX)
			return -1;
		s = &ss->s[ss->n++];
		memset(s, 0, sizeof(*s));
		strncpy(s->key, key, sizeof(s->key) - 1);
		strncpy(s->label, key, sizeof(s->label) - 1);
	}
	memset(s->value, 0, sizeof(s->value));
	for (size_t i = 0; value[i] && value[i] != '\n' && i < sizeof(s->value) - 1; i++)
		s->value[i] = value[i];
	s->set = 1;
	return 0;
}

static inline char *settings_put(char *o, const char *s)
{
	while (*s)
		*o++ = *s++;
	return o;
}

/* Write the settings that were set to SETTINGS_FILE. 0, or -1 (errno). */
static inline int settings_save(const struct settings *ss)
{
	static char buf[8192];
	char *o = buf;
	int fd, ok;

	o = settings_put(o, "# Badge settings (docs/SETTINGS.md): key, menu or -, label, value;\n"
			    "# tab-separated. Written by the setting command and the menu.\n");
	for (int i = 0; i < ss->n; i++) {
		const struct setting *s = &ss->s[i];

		if (!s->set)
			continue;
		o = settings_put(o, s->key);
		*o++ = '\t';
		o = settings_put(o, s->menu ? "menu" : "-");
		*o++ = '\t';
		o = settings_put(o, s->label);
		*o++ = '\t';
		o = settings_put(o, s->value);
		*o++ = '\n';
	}
	mkdir(SETTINGS_DIR, 0755);
	fd = open(SETTINGS_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	ok = write(fd, buf, (size_t)(o - buf)) == o - buf && fsync(fd) == 0;
	if (close(fd) || !ok) {
		unlink(SETTINGS_FILE ".new");
		if (!errno)
			errno = EIO;
		return -1;
	}
	return rename(SETTINGS_FILE ".new", SETTINGS_FILE);
}

#endif
