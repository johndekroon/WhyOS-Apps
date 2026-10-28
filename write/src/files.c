/* SPDX-License-Identifier: MIT */
/* Write: files (files.h). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"
#include "syntax.h"

static void why(char *err, int n, int e)
{
	const char *s = e == ENOENT ? "No such file" : e == ENOSPC ? "The disk is full" :
			e == EROFS ? "The disk is read-only" : e == EACCES || e == EPERM ? "No permission" :
			e == ENOMEM ? "Not enough memory" : e == EISDIR ? "That is a folder" : strerror(e);

	snprintf(err, (size_t)n, "%s", s);
}

int file_exists(const char *path)
{
	return access(path, F_OK) == 0;
}

int file_load(const char *path, char **data, size_t *len, char *err, int errn)
{
	struct stat st;
	char *buf;
	size_t got = 0;
	int fd;

	*data = 0;
	*len = 0;
	if (stat(path, &st)) {
		why(err, errn, errno);
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		snprintf(err, (size_t)errn, S_ISDIR(st.st_mode) ? "That is a folder" : "Not a regular file");
		return -1;
	}
	if (st.st_size > FILE_MAX) {
		snprintf(err, (size_t)errn, "Too large for Write (over %d KB)", FILE_MAX / 1024);
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		why(err, errn, errno);
		return -1;
	}
	/* to the end, not to st_size: /proc says 0 bytes and has more */
	{
		size_t cap = (size_t)st.st_size + 4096;

		buf = malloc(cap);
		if (!buf) {
			close(fd);
			why(err, errn, ENOMEM);
			return -1;
		}
		for (;;) {
			ssize_t r;

			if (got + 1 >= cap) {
				char *nb;

				if (cap > FILE_MAX + 1)
					break;
				nb = realloc(buf, cap * 2);
				if (!nb)
					break;
				buf = nb;
				cap *= 2;
			}
			r = read(fd, buf + got, cap - 1 - got);
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				break;
			got += (size_t)r;
		}
	}
	close(fd);
	if (got > FILE_MAX) {
		free(buf);
		snprintf(err, (size_t)errn, "Too large for Write (over %d KB)", FILE_MAX / 1024);
		return -1;
	}
	if (memchr(buf, 0, got)) {
		free(buf);
		snprintf(err, (size_t)errn, "Not a text file");
		return -1;
	}
	buf[got] = 0;
	*data = buf;
	*len = got;
	return 0;
}

int file_save(const char *path, const char *data, size_t len, char *err, int errn)
{
	char dir[512], base[256], tmp[800];
	struct stat st;
	mode_t mode = 0644;
	size_t done = 0;
	int fd, e;

	path_split(path, dir, sizeof(dir), base, sizeof(base));
	snprintf(tmp, sizeof(tmp), "%s/.%s.write", strcmp(dir, "/") ? dir : "", base);
	if (!stat(path, &st))
		mode = st.st_mode & 07777;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0) {
		why(err, errn, errno == ENOENT ? ENOTDIR : errno);
		if (errno == ENOENT)
			snprintf(err, (size_t)errn, "No such folder");
		return -1;
	}
	while (done < len) {
		ssize_t w = write(fd, data + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			goto fail;
		done += (size_t)w;
	}
	if (fsync(fd))
		goto fail;
	if (close(fd)) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	chmod(tmp, mode);	/* the umask may have narrowed it: what the file had */
	if (rename(tmp, path))
		goto fail;
	return 0;
fail:
	e = errno ? errno : EIO;
	if (fd >= 0)
		close(fd);
	unlink(tmp);
	why(err, errn, e);
	return -1;
}

int name_ok(const char *name, char *whyn, int n)
{
	if (!*name) {
		snprintf(whyn, (size_t)n, "Type a name");
		return 0;
	}
	if (strchr(name, '/')) {
		snprintf(whyn, (size_t)n, "A name cannot have a /");
		return 0;
	}
	if (!strcmp(name, ".") || !strcmp(name, "..")) {
		snprintf(whyn, (size_t)n, "Not a name");
		return 0;
	}
	for (const unsigned char *p = (const unsigned char *)name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      *p == ' ' || *p == '-' || *p == '_' || *p == '.' || *p >= 0x80)) {
			snprintf(whyn, (size_t)n, "Use letters, digits, spaces, - _ and .");
			return 0;
		}
	if (strlen(name) > NAME_MAX_W) {
		snprintf(whyn, (size_t)n, "The name is too long");
		return 0;
	}
	return 1;
}

void name_suggest(const char *dir, const char *cur, const char *heading, const char *ext,
		  char *out, int n)
{
	char base[NAME_MAX_W + 1];
	int v = 1;

	if (cur && *cur) {
		const char *e = path_ext(cur);
		int bl = (int)(strlen(cur) - strlen(e)), k;

		snprintf(base, sizeof(base), "%.*s", bl, cur);
		/* "-vN" at the end: count on from N */
		k = (int)strlen(base);
		while (k > 0 && base[k - 1] >= '0' && base[k - 1] <= '9')
			k--;
		if (k >= 2 && k < (int)strlen(base) && base[k - 1] == 'v' && base[k - 2] == '-') {
			v = atoi(base + k);
			base[k - 2] = 0;
		}
		for (v = v + 1; v < 1000; v++) {
			char p[1024];

			snprintf(p, sizeof(p), "%s/%s-v%d%s", dir, base, v, ext);
			if (!file_exists(p))
				break;
		}
		snprintf(out, (size_t)n, "%s-v%d", base, v);
		return;
	}
	{
		int o = 0, dash = 0;

		for (const unsigned char *p = (const unsigned char *)(heading ? heading : ""); *p && o < 40; p++) {
			unsigned char c = *p;

			if (c == '\'')
				continue;
			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
				if (dash && o)
					base[o++] = '-';
				dash = 0;
				base[o++] = (char)c;
			} else if (c >= 'A' && c <= 'Z') {
				if (dash && o)
					base[o++] = '-';
				dash = 0;
				base[o++] = (char)(c + 32);
			} else {
				dash = 1;
			}
		}
		base[o] = 0;
		snprintf(out, (size_t)n, "%s", o ? base : "untitled");
	}
}

void rescue_path(const char *dir, char *out, int n)
{
	snprintf(out, (size_t)n, "%s/untitled.md", dir);
	for (int i = 2; file_exists(out) && i < 1000; i++)
		snprintf(out, (size_t)n, "%s/untitled-%d.md", dir, i);
}

static int by_entry(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;
	if (x->dir)
		return strcasecmp(x->name, y->name);
	return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : strcasecmp(x->name, y->name);
}

static int count_items(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)))
		n += de->d_name[0] != '.';
	closedir(d);
	return n;
}

int list_dir(const char *dir, int dirs_only, struct entry **out, char *err, int errn)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	struct entry *v = 0;
	int n = 0, cap = 0;

	*out = 0;
	if (!d) {
		why(err, errn, errno);
		return -1;
	}
	while ((de = readdir(d))) {
		char p[1024];
		struct stat st;
		int isdir;

		if (de->d_name[0] == '.' || strlen(de->d_name) > NAME_MAX_W)
			continue;
		snprintf(p, sizeof(p), "%s/%s", strcmp(dir, "/") ? dir : "", de->d_name);
		if (stat(p, &st))
			continue;
		isdir = S_ISDIR(st.st_mode);
		if (!isdir && (dirs_only || !S_ISREG(st.st_mode) || lang_of_path(de->d_name) == L_NONE))
			continue;
		if (n == cap) {
			struct entry *nv;

			cap = cap ? cap * 2 : 32;
			nv = realloc(v, (size_t)cap * sizeof(*v));
			if (!nv)
				break;
			v = nv;
		}
		snprintf(v[n].name, sizeof(v[n].name), "%s", de->d_name);
		v[n].dir = isdir;
		v[n].mtime = st.st_mtime;
		v[n].items = isdir ? count_items(p) : 0;
		n++;
	}
	closedir(d);
	if (n)
		qsort(v, (size_t)n, sizeof(*v), by_entry);
	*out = v;
	return n;
}

void when_text(time_t t, time_t now, char *out, int n)
{
	static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
					   "Sep", "Oct", "Nov", "Dec" };
	struct tm a, b, y;
	time_t yest = now - 86400;

	/* the badge has no clock that runs while it is off: a time before
	 * 2020 was taken before the clock was set over Wi-Fi, and "1 Jan
	 * 1970" says nothing; no date then */
	out[0] = 0;
	if (t < 1577836800 || now < 1577836800)
		return;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	localtime_r(&yest, &y);
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
		snprintf(out, (size_t)n, "Today %02d:%02d", a.tm_hour, a.tm_min);
	else if (a.tm_year == y.tm_year && a.tm_yday == y.tm_yday)
		snprintf(out, (size_t)n, "Yesterday");
	else if (a.tm_year == b.tm_year)
		snprintf(out, (size_t)n, "%d %s", a.tm_mday, mon[a.tm_mon]);
	else
		snprintf(out, (size_t)n, "%d %s %d", a.tm_mday, mon[a.tm_mon], a.tm_year + 1900);
}

void path_split(const char *path, char *dir, int dn, char *base, int bn)
{
	const char *s = strrchr(path, '/');

	if (!s) {
		snprintf(dir, (size_t)dn, ".");
		snprintf(base, (size_t)bn, "%s", path);
	} else if (s == path) {
		snprintf(dir, (size_t)dn, "/");
		snprintf(base, (size_t)bn, "%s", s + 1);
	} else {
		snprintf(dir, (size_t)dn, "%.*s", (int)(s - path), path);
		snprintf(base, (size_t)bn, "%s", s + 1);
	}
}

const char *path_ext(const char *path)
{
	const char *base = strrchr(path, '/'), *dot;

	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	return dot && dot != base ? dot : base + strlen(base);
}
