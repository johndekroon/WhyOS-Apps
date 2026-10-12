// SPDX-License-Identifier: MIT
/*
 * doom — starts fbDOOM as an app (sdapps/README.md), under apprun like
 * any other; it was doomrun in the image before (git history).
 *
 * - Zone size: the largest of 4/3/2 MB that fits next to fbdoom's own
 *   image and screen buffer, from /proc/userpool's largest free block
 *   (kernel patch 0046); fbdoom is started once with it.
 * - The logo key: apprun ends us with SIGTERM. fbdoom must not get that
 *   (killed, it skipped its exit functions, and the menu started right
 *   after it hung the badge), so we type its own quit key into the VT it
 *   reads in K_MEDIUMRAW and wait for it; SIGTERM only if it is still
 *   running 2 s later. The keyboard mode and termios are restored
 *   whatever way it ended.
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
 * screen buffer; without /proc/userpool, probe with mmap. */
static int zone_mb(void)
{
	long largest = pool_largest();

	if (largest >= 0) {
		long mb = (largest - (1800L << 10)) >> 20;

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

/* End DOOM the way its own Backspace does (0x0e down, 0x8e up). */
static void quit_doom(int tty, int pid)
{
	static const char bs[2] = { 0x0e, (char)0x8e };

	for (int i = 0; i < 2; i++)
		ioctl(tty, TIOCSTI, &bs[i]);
	for (int t = 0; t < 20; t++) {
		if (waitpid(pid, 0, WNOHANG) == pid)
			return;
		usleep(100000);
	}
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
		execv(argv[0], argv);
		_exit(127);
	}
	while (pid > 0) {
		int r = waitpid(pid, &status, 0);	/* SIGTERM interrupts it */

		if (r == pid)
			break;
		if (quit) {
			quit_doom(tty, pid);
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
