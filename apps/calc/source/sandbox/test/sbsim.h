// SPDX-License-Identifier: MIT
/*
 * An app's sandbox on any computer. Built with -include sbsim.h and with sbsim.c, a
 * program runs its badge code, not a host variant, while its system calls meet what they
 * would meet on the badge: policy.h for every path, /dev/fb0 and /dev/ppa that finish
 * as late as c1urun lets them, the kernel's rules for the PPA, sleepd's standby and a
 * clock that only moves when the program waits. README.md says what it does not cover.
 */
#ifndef SBSIM_H
#define SBSIM_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int sim_open(const char *path, int flags, ...);
int sim_close(int fd);
ssize_t sim_read(int fd, void *buf, size_t n);
ssize_t sim_write(int fd, const void *buf, size_t n);
ssize_t sim_pwrite(int fd, const void *buf, size_t n, off_t off);
off_t sim_lseek(int fd, off_t off, int whence);
pid_t sim_getppid(void);
int sim_ioctl(int fd, unsigned long req, ...);
int sim_poll(struct pollfd *p, nfds_t n, int timeout);
int sim_access(const char *path, int mode);
int sim_mkfifo(const char *path, mode_t mode);
int sim_mkdir(const char *path, mode_t mode);
int sim_unlink(const char *path);
int sim_rename(const char *from, const char *to);
int sim_stat(const char *path, struct stat *st);
int sim_fsync(int fd);
FILE *sim_fopen(const char *path, const char *mode);
DIR *sim_opendir(const char *path);
int sim_openat(int dirfd, const char *path, int flags, ...);
int sim_clock_gettime(clockid_t id, struct timespec *t);
int sim_nanosleep(const struct timespec *t, struct timespec *left);
int sim_usleep(unsigned us);
void *sim_malloc(size_t n);
void *sim_calloc(size_t n, size_t size);
void *sim_realloc(void *p, size_t n);
void sim_free(void *p);

#ifndef SBSIM_IMPL
#define open(...)		sim_open(__VA_ARGS__)
#define close(fd)		sim_close(fd)
#define read(fd, b, n)		sim_read(fd, b, n)
#define write(fd, b, n)		sim_write(fd, b, n)
#define pwrite(fd, b, n, o)	sim_pwrite(fd, b, n, o)
#define lseek(fd, o, w)		sim_lseek(fd, o, w)
#define getppid()		sim_getppid()
#define ioctl(...)		sim_ioctl(__VA_ARGS__)
#define poll(p, n, t)		sim_poll(p, n, t)
#define access(p, m)		sim_access(p, m)
#define mkfifo(p, m)		sim_mkfifo(p, m)
#define mkdir(p, m)		sim_mkdir(p, m)
#define unlink(p)		sim_unlink(p)
#define rename(a, b)		sim_rename(a, b)
#define stat(p, s)		sim_stat(p, s)
#define fsync(fd)		sim_fsync(fd)
#define fopen(p, m)		sim_fopen(p, m)
#define opendir(p)		sim_opendir(p)
#define openat(...)		sim_openat(__VA_ARGS__)
#define clock_gettime(i, t)	sim_clock_gettime(i, t)
#define nanosleep(t, l)		sim_nanosleep(t, l)
#define usleep(us)		sim_usleep(us)
#define malloc(n)		sim_malloc(n)
#define calloc(n, s)		sim_calloc(n, s)
#define realloc(p, n)		sim_realloc(p, n)
#define free(p)			sim_free(p)
#endif

#endif
