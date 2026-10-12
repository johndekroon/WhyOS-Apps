/* SPDX-License-Identifier: MIT */
/* store: small helpers shared by the client's modules. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "store.h"

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p) {
		warn("out of memory (%zu bytes)", n);
		exit(3);
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p) {
		warn("out of memory (%zu bytes)", n);
		exit(3);
	}
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

char *path_join(char *out, size_t size, const char *a, const char *b)
{
	size_t la = strlen(a);

	snprintf(out, size, "%s%s%s", a, la && a[la - 1] == '/' ? "" : "/", b);
	return out;
}

int mkdir_p(const char *path)
{
	char buf[512];
	size_t n = strlen(path);

	if (n >= sizeof(buf))
		return -1;
	memcpy(buf, path, n + 1);
	for (char *p = buf + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(buf, 0755) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	return mkdir(buf, 0755) && errno != EEXIST ? -1 : 0;
}

int rm_rf(const char *path)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	int r = 0;

	if (lstat(path, &st))
		return errno == ENOENT ? 0 : -1;
	if (!S_ISDIR(st.st_mode))
		return unlink(path);
	d = opendir(path);
	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char sub[512];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (rm_rf(path_join(sub, sizeof(sub), path, e->d_name)))
			r = -1;
	}
	closedir(d);
	return rmdir(path) ? -1 : r;
}

int is_dir(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int is_file(const char *path)
{
	struct stat st;

	return !stat(path, &st) && S_ISREG(st.st_mode);
}

long long file_size(const char *path)
{
	struct stat st;

	return stat(path, &st) ? -1 : (long long)st.st_size;
}

long long dir_size(const char *path, long *files)
{
	struct stat st;
	DIR *d;
	struct dirent *e;
	long long sum = 0;

	if (lstat(path, &st))
		return 0;
	if (!S_ISDIR(st.st_mode)) {
		if (files)
			(*files)++;
		return st.st_size;
	}
	d = opendir(path);
	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char sub[512];

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		sum += dir_size(path_join(sub, sizeof(sub), path, e->d_name), files);
	}
	closedir(d);
	return sum;
}

int read_file(const char *path, char **data, size_t *len, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t cap = 4096, n = 0;
	char *buf;
	ssize_t r;

	if (fd < 0)
		return -1;
	buf = xmalloc(cap + 1);
	while ((r = read(fd, buf + n, cap - n)) > 0) {
		n += (size_t)r;
		if (n > max) {
			close(fd);
			free(buf);
			errno = EFBIG;
			return -1;
		}
		if (n == cap) {
			cap *= 2;
			buf = xrealloc(buf, cap + 1);
		}
	}
	close(fd);
	if (r < 0) {
		free(buf);
		return -1;
	}
	buf[n] = 0;
	*data = buf;
	if (len)
		*len = n;
	return 0;
}

/* write <path>.new, fsync, rename (device.md §2.2): never half a file */
int write_atomic(const char *path, const void *data, size_t len)
{
	char tmp[520];
	int fd, ok;

	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	ok = write(fd, data, len) == (ssize_t)len && fsync(fd) == 0;
	if (close(fd) || !ok) {
		unlink(tmp);
		return -1;
	}
	return rename(tmp, path);
}

void sha256_hex(const unsigned char d[32], char hex[65])
{
	static const char x[] = "0123456789abcdef";

	for (int i = 0; i < 32; i++) {
		hex[2 * i] = x[d[i] >> 4];
		hex[2 * i + 1] = x[d[i] & 15];
	}
	hex[64] = 0;
}

int sha256_file(const char *path, char hex[65])
{
	mbedtls_sha256_context c;
	unsigned char buf[8192], d[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	mbedtls_sha256_init(&c);
	mbedtls_sha256_starts(&c, 0);
	while ((r = read(fd, buf, sizeof(buf))) > 0)
		mbedtls_sha256_update(&c, buf, (size_t)r);
	close(fd);
	mbedtls_sha256_finish(&c, d);
	mbedtls_sha256_free(&c);
	if (r < 0)
		return -1;
	sha256_hex(d, hex);
	return 0;
}

void now_iso(char out[24])
{
	time_t t = time(0);
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* a random (version 4) UUID, from /dev/urandom (device.md §4) */
void uuid4(char out[37])
{
	unsigned char b[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd < 0 || read(fd, b, sizeof(b)) != sizeof(b)) {
		srand((unsigned)time(0) ^ (unsigned)getpid());
		for (int i = 0; i < 16; i++)
			b[i] = (unsigned char)rand();
	}
	if (fd >= 0)
		close(fd);
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
		 b[13], b[14], b[15]);
}

long long fs_free(const char *path)
{
	struct statvfs v;

	return statvfs(path, &v) ? -1 : (long long)v.f_bavail * (long long)v.f_frsize;
}

/*
 * Run a hook (package.md §2.8): prog in dir, with env added to ours, a 30 s
 * timeout. Returns its exit status, or -1. vfork: there is no fork on NOMMU.
 */
int run_hook(const char *dir, const char *prog, char *const env[], char *const extra_args[])
{
	extern char **environ;
	char path[512];
	char *argv[8], **envp;
	int n = 0, status = 0, ne = 0, ours = 0;
	pid_t pid;

	path_join(path, sizeof(path), dir, prog);
	argv[n++] = path;
	for (int i = 0; extra_args && extra_args[i] && n < 7; i++)
		argv[n++] = extra_args[i];
	argv[n] = 0;
	/* the child shares our memory (vfork): build its environment here */
	while (environ[ne])
		ne++;
	while (env && env[ours])
		ours++;
	envp = xmalloc((size_t)(ne + ours + 1) * sizeof(*envp));
	n = 0;
	for (int i = 0; i < ours; i++)
		envp[n++] = env[i];
	for (int i = 0; i < ne; i++) {
		int dup = 0;

		for (int k = 0; k < ours && !dup; k++) {
			size_t l = strcspn(env[k], "=");

			dup = !strncmp(environ[i], env[k], l + 1);
		}
		if (!dup)
			envp[n++] = environ[i];
	}
	envp[n] = 0;
	pid = vfork();
	if (pid == 0) {
		if (chdir(dir) == 0)
			execve(path, argv, envp);
		_exit(127);
	}
	free(envp);
	if (pid < 0)
		return -1;
	for (int t = 0; t < HOOK_TIMEOUT * 10; t++) {
		pid_t r = waitpid(pid, &status, WNOHANG);

		if (r == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	warn("%s: no answer after %d s, stopped", prog, HOOK_TIMEOUT);
	return -1;
}

void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

void warn(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fputs("store: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* split s in place at spaces; returns the number of words */
int split_words(char *s, char **words, int max)
{
	int n = 0;

	while (*s && n < max) {
		while (*s == ' ')
			*s++ = 0;
		if (!*s)
			break;
		words[n++] = s;
		while (*s && *s != ' ')
			s++;
	}
	return n;
}
