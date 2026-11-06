// SPDX-License-Identifier: MIT
/*
 * doom — starts fbDOOM as an app (sdapps/README.md), under apprun like
 * any other; it was doomrun in the image before (git history).
 *
 * - Zone size: the largest of 4/3/2 MB that fits next to fbdoom's own
 *   image and screen buffer, from /proc/userpool's largest free block
 *   (kernel patch 0046); fbdoom is started once with it.
 * - The logo key: apprun ends us with SIGTERM, and we end fbdoom. Keyboard
 *   mode and termios are restored here, whatever way it ended.
 * - Both this and fbdoom run in the sandbox on core 1, as every app does.
 *   SANDBOX in app.conf limits the heap, above the zone and the screen buffer.
 * - Saved games go to APP_DATA (on the card), the directory fbdoom runs in.
 * - Exit status 3 when there is no room for its zone: the menu says the
 *   app did not start, and /tmp/app-doom.log says why.
 *
 * No stdio (see overlay-src/Makefile for why), vfork + execv.
 */
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define CONSOLE_TTY	"/dev/tty1"

static volatile sig_atomic_t quit;

static void on_term(int s)
{
	(void)s;
	quit = 1;
}

static void say(const char *s)
{
	if (write(2, s, strlen(s)) < 0)
		return;
}

/* Largest free block of the user pool, -1 without /proc/userpool. */
static long pool_largest(void)
{
	char buf[128], *p;
	int fd = open("/proc/userpool", O_RDONLY), n;
	long v = 0;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	p = strstr(buf, "largest ");
	if (!p)
		return -1;
	for (p += 8; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	return v;
}

/* Zone size in MB: what fits next to fbdoom's ~0.8 MB image and ~1 MB
 * screen buffer, its c1urun and what apprun's memory guard wants left over;
 * without /proc/userpool, probe with mmap. */
static int zone_mb(void)
{
	long largest = pool_largest();

	if (largest >= 0) {
		long mb = (largest - (3000L << 10)) >> 20;

		return mb > 4 ? 4 : mb < 2 ? 0 : (int)mb;
	}
	for (int mb = 4; mb >= 2; mb--) {
		size_t len = (size_t)(mb + 1) << 20;
		void *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (p != MAP_FAILED) {
			munmap(p, len);
			return mb;
		}
	}
	return 0;
}

static void quit_doom(int pid)
{
	kill(pid, SIGTERM);
	waitpid(pid, 0, 0);
}

int main(void)
{
	const char *dir = getenv("APP_DIR"), *data = getenv("APP_DATA");
	static char prog[300], wad[300], mb[2] = "4";
	char *argv[] = { prog, "-iwad", wad, "-mb", mb, 0 };
	int tty = open(CONSOLE_TTY, O_RDWR | O_NOCTTY | O_CLOEXEC);
	int kbmode = K_UNICODE, have_tio, status = 0, pid;
	struct sigaction sa;
	struct termios tio;

	if (!dir)
		dir = ".";
	strncpy(prog, dir, sizeof(prog) - 20);
	strcat(prog, "/fbdoom");
	strncpy(wad, dir, sizeof(wad) - 20);
	strcat(wad, "/doom1.wad");
	mb[0] = (char)('0' + zone_mb());
	if (mb[0] == '0') {
		say("doom: not enough free memory for its zone (2 MB in one piece)\n");
		return 3;
	}
	if (data && chdir(data) != 0)
		say("doom: no APP_DATA: saved games go to the current directory\n");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	ioctl(tty, KDGKBMODE, &kbmode);
	have_tio = tcgetattr(tty, &tio) == 0;

	pid = vfork();
	if (pid == 0) {		/* exec resets our handlers to the default */
		execv(prog, argv);
		_exit(127);
	}
	while (pid > 0) {
		int r = waitpid(pid, &status, 0);	/* SIGTERM interrupts it */

		if (r == pid)
			break;
		if (quit) {
			quit_doom(pid);
			status = 0;
			break;
		}
	}
	ioctl(tty, KDSKBMODE, kbmode);
	if (have_tio)
		tcsetattr(tty, TCSAFLUSH, &tio);
	if (pid < 0)
		return 127;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
