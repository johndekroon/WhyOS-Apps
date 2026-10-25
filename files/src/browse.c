/* SPDX-License-Identifier: MIT */
/* Files: the folder on screen and the file operations (browse.h). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "browse.h"

#ifdef FILES_HOST
#define vfork fork	/* deprecated on macOS; the badge (NOMMU) has only vfork */
#endif

const char *br_ext(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot + 1 : "";
}

int ext_in(const char *list, const char *ext)
{
	size_t n = strlen(ext);

	for (const char *t = list; *t; ) {
		size_t k = 0;

		while (t[k] && t[k] != ' ' && t[k] != ',')
			k++;
		if ((k == 1 && *t == '*') || (n && k == n && !strncasecmp(t, ext, n)))
			return 1;
		t += k;
		while (*t == ' ' || *t == ',')
			t++;
	}
	return 0;
}

enum br_kind br_kind_of(const char *name, int dir)
{
	const char *x = br_ext(name);

	if (dir)
		return BK_FOLDER;
	if (ext_in("jpg jpeg png gif bmp webp", x))
		return BK_IMAGE;
	if (ext_in("html htm svg xhtml", x))
		return BK_WEB;
	if (ext_in("pdf epub", x))
		return BK_DOC;
	if (ext_in("gb gbc", x))
		return BK_GAMEBOY;
	if (ext_in("txt md conf cfg ini log sh py c h json csv xml yml yaml rc toml lua js css", x))
		return BK_TEXT;
	return BK_FILE;
}

int br_join(char *out, size_t out_n, const char *dir, const char *name)
{
	size_t a = strlen(dir), b = strlen(name);

	if (a == 1 && dir[0] == '/')	/* "/" + "bin", not "//bin" */
		a = 0;
	if (a + 1 + b + 1 > out_n)
		return 0;
	memcpy(out, dir, a);
	out[a] = '/';
	memcpy(out + a + 1, name, b + 1);
	return 1;
}

int br_drives(const struct browse *b)
{
	return !b->dir[0];
}

static int br_cmp(const void *a, const void *b)
{
	const struct br_ent *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;		/* folders first */
	return strcasecmp(x->name, y->name);
}

/* dir may be b->dir itself (br_up cuts it short): copied first */
void br_go(struct browse *b, const char *dir, const char *want)
{
	char d[sizeof(b->dir)] = "";

	strncpy(d, dir, sizeof(d) - 1);
	memcpy(b->dir, d, sizeof(b->dir));
	memset(b->want, 0, sizeof(b->want));
	if (want)
		strncpy(b->want, want, sizeof(b->want) - 1);
	b->sel = b->off = 0;
	b->stale = 1;
}

/* also from a mount point; nothing is above "/" (Drives is only the cross) */
int br_up(struct browse *b)
{
	char *slash = strrchr(b->dir, '/');
	char name[64] = "";

	if (br_drives(b) || !b->dir[1] || !slash)
		return 0;
	strncpy(name, slash + 1, sizeof(name) - 1);
	*slash = 0;
	br_go(b, b->dir[0] ? b->dir : "/", name);
	return 1;
}

static int list_dir(struct browse *b)
{
	DIR *d = opendir(b->dir);
	struct dirent *de;

	b->n = b->more = 0;
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		struct br_ent *e = &b->e[b->n];
		char path[320];
		struct stat st;

		if ((de->d_name[0] == '.' && (!b->hidden || !de->d_name[1] ||
					      (de->d_name[1] == '.' && !de->d_name[2]))) ||
		    strlen(de->d_name) >= sizeof(e->name))
			continue;
		if (b->n == BR_MAX) {
			b->more++;
			continue;
		}
		memset(e, 0, sizeof(*e));
		memcpy(e->name, de->d_name, strlen(de->d_name) + 1);
		if (br_join(path, sizeof(path), b->dir, de->d_name) && stat(path, &st) == 0) {
			e->dir = S_ISDIR(st.st_mode);
			e->reg = S_ISREG(st.st_mode);
			e->size = (unsigned long long)st.st_size;
		}
		e->kind = br_kind_of(e->name, e->dir);
		b->n++;
	}
	closedir(d);
	qsort(b->e, (size_t)b->n, sizeof(b->e[0]), br_cmp);
	return 0;
}

void br_say(struct browse *b, const char *a, const char *c, int bad)
{
	snprintf(b->notice, sizeof(b->notice), "%s%s", a, c);
	b->notice_bad = bad;
}

void br_read(struct browse *b)
{
	if (!b->stale)
		return;
	b->stale = 0;
	if (br_drives(b)) {
		b->n = b->more = 0;
		return;
	}
	if (list_dir(b) < 0)
		br_say(b, "Cannot open ", b->dir, 1);
	if (b->want[0]) {
		for (int i = 0; i < b->n; i++)
			if (!strcmp(b->e[i].name, b->want))
				b->sel = i;
		b->want[0] = 0;
	}
	if (b->sel >= b->n)
		b->sel = b->n - 1;
	if (b->sel < 0)
		b->sel = 0;
}

int br_sel_path(const struct browse *b, char *out, size_t n)
{
	out[0] = 0;
	if (br_drives(b) || !b->n)
		return 0;
	return br_join(out, n, b->dir, b->e[b->sel].name);
}

/* For files without a known extension: /proc/cpuinfo, README, rc scripts. */
int br_textlike(const char *path)
{
	unsigned char c[512];
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), n, odd = 0;

	if (fd < 0)
		return 0;
	n = (int)read(fd, c, sizeof(c));
	close(fd);
	if (n < 0)
		return 0;
	for (int i = 0; i < n; i++) {
		if (!c[i])
			return 0;
		odd += c[i] < 0x20 && c[i] != '\n' && c[i] != '\r' && c[i] != '\t' && c[i] != 0x1b;
	}
	return odd * 20 <= n;
}

static void log_add(const char *log, const char *s, size_t n)
{
	struct stat st;
	int fd;

	if (!log)
		return;
	fd = open(log, O_WRONLY | O_CREAT | O_CLOEXEC |
		  (stat(log, &st) == 0 && st.st_size > 16384 ? O_TRUNC : O_APPEND), 0644);
	if (fd < 0)
		return;
	if (write(fd, s, n) < 0)
		n = 0;
	close(fd);
}

/* the command, then what it said (a success says nothing) */
int br_run(struct browse *b, const char *what, char *const argv[])
{
	char line[1024], *p = line;
	int status = -1, fd;
	pid_t pid;

	(void)what;
	for (int i = 0; argv[i]; i++) {
		size_t n = strlen(argv[i]);

		if (p + n + 2 >= line + sizeof(line))
			break;
		if (i)
			*p++ = ' ';
		memcpy(p, argv[i], n);
		p += n;
	}
	*p++ = '\n';
	log_add(b->log, line, (size_t)(p - line));
	fd = b->log ? open(b->log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644) : -1;
	pid = vfork();
	if (pid == 0) {
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (fd >= 0)
		close(fd);
	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
	if (status) {
		int n = snprintf(line, sizeof(line), "failed, status %d\n", status);

		log_add(b->log, line, (size_t)n);
	}
	return status;
}

void br_copy(struct browse *b, int cut)
{
	char path[sizeof(b->clip)];

	if (!br_sel_path(b, path, sizeof(path)))
		return;
	memcpy(b->clip, path, sizeof(b->clip));
	b->clip_cut = cut;
}

static const char *base_of(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

int br_paste(struct browse *b)
{
	const char *base = base_of(b->clip);
	char dst[320];
	size_t n = strlen(b->clip);
	int rc;

	if (!b->clip[0] || !br_join(dst, sizeof(dst), b->dir, base))
		return -1;
	if (access(dst, F_OK) == 0) {
		br_say(b, "Already here: ", base, 1);
		return -1;
	}
	if (!strncmp(b->dir, b->clip, n) && (b->dir[n] == '/' || !b->dir[n])) {
		br_say(b, "Cannot put a folder inside itself: ", base, 1);
		return -1;
	}
	{
		char *cp[] = { "/bin/cp", "-a", b->clip, dst, 0 };
		char *mv[] = { "/bin/mv", b->clip, dst, 0 };

		rc = b->run(b, b->clip_cut ? "Moving\xe2\x80\xa6" : "Copying\xe2\x80\xa6",
			    b->clip_cut ? mv : cp);
	}
	if (rc) {
		br_say(b, b->clip_cut ? "Could not move " : "Could not copy ", base, 1);
		return -1;
	}
	memset(b->want, 0, sizeof(b->want));
	strncpy(b->want, base, sizeof(b->want) - 1);
	br_say(b, b->clip_cut ? "Moved " : "Copied ", base, 0);
	if (b->clip_cut)
		b->clip[0] = 0;
	b->stale = 1;
	return 0;
}

int br_delete(struct browse *b)
{
	char target[320];
	char *rm[] = { "/bin/rm", "-rf", target, 0 };
	const char *base;
	int rc = 0;

	if (!br_sel_path(b, target, sizeof(target)))
		return -1;
	base = base_of(target);
	if (b->run(b, "Deleting\xe2\x80\xa6", rm) || access(target, F_OK) == 0) {
		br_say(b, "Could not delete ", base, 1);
		rc = -1;
	} else {
		br_say(b, "Deleted ", base, 0);
	}
	if (!strcmp(b->clip, target))
		b->clip[0] = 0;
	b->stale = 1;
	return rc;
}

/* a file or folder name: no slash, not . or .. */
static int name_ok(const char *s)
{
	return s[0] && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, "..");
}

int br_name(struct browse *b, const char *name, int rename_it, const char **err)
{
	char path[320], old[320];

	if (!name_ok(name)) {
		*err = "A name without /";
		return 1;
	}
	if (!br_join(path, sizeof(path), b->dir, name)) {
		*err = "Too long";
		return 1;
	}
	if (access(path, F_OK) == 0) {
		*err = "That name is taken here";
		return 1;
	}
	if (!rename_it) {
		if (mkdir(path, 0755) < 0)
			br_say(b, "Could not make folder ", name, 1);
	} else if (!br_sel_path(b, old, sizeof(old)) || rename(old, path) < 0) {
		br_say(b, "Could not rename to ", name, 1);
	}
	memset(b->want, 0, sizeof(b->want));
	strncpy(b->want, name, sizeof(b->want) - 1);
	b->stale = 1;
	return 0;
}
