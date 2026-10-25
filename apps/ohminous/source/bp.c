// SPDX-License-Identifier: MIT
/*
 * bp.c — a Bus Pirate 6 on the bottom port (bp.h). Taken over from the
 * menu's Bus Pirate screens (overlay-src/home.c, docs/HOME.md "Bus
 * Pirate"): the terminal is typed to as a user would; a reply ends at the
 * ETX (0x03) the firmware sends after every prompt; ANSI sequences are
 * dropped; the prompt gives the mode ("DIO> "). Opening the terminal makes
 * it eject and re-insert its USB drive, so connecting takes 2-5 s. The
 * logic analyzer is SUMP on its second port (its default binmode).
 *
 * Built with -DOHM_HOST there is a simulated Bus Pirate (OHM_BP=1 in the
 * environment), for the host tests.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bp.h"

struct bp B = { .dv = 33, .ma = 300, .led_set = -1, .led_n = 8, .led_pct = 50, .led_rgb = 0x00ff40 };
const char *const bp_led_types[N_LTYPES] = { "WS2812 strip", "APA102 strip", "Onboard LED" };

static int fd = -1;
#ifndef OHM_HOST
static int la_fd = -1;
static char tty[2][16];
#endif

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void note(const char *s, int bad)
{
	snprintf(B.notice, sizeof(B.notice), "%s", s);
	B.notice_bad = bad;
}

int bp_dv_of(const char *v)
{
	int d = 0, frac = 0;

	for (; *v >= '0' && *v <= '9'; v++)
		d = d * 10 + (*v - '0');
	if (*v == '.' && v[1] >= '0' && v[1] <= '9')
		frac = v[1] - '0';
	return d * 10 + frac;
}

int bp_fd(void)
{
	return fd;
}

/* A mode prompt ("DIO> ", "LED-(WS2812)> ") sets B.mode; menus have spaces. */
static int prompt(void)
{
	const char *nl = strrchr(B.rx, '\n'), *l = nl ? nl + 1 : B.rx, *gt = strchr(l, '>');
	int n = 0;

	if (!gt || gt == l || memchr(l, ' ', (size_t)(gt - l)))
		return 0;
	while (l + n < gt && l[n] != '-' && n < (int)sizeof(B.mode) - 1)
		n++;
	memcpy(B.mode, l, (size_t)n);
	B.mode[n] = 0;
	if (strcmp(B.mode, "LED"))
		B.led_set = -1;
	return 1;
}

/* An error line in the last reply, as the notice; 1 if there was one. */
static int error_line(void)
{
	for (char *l = B.rx; l && *l; ) {
		char *e = strchr(l, '\n');
		int n = e ? (int)(e - l) : (int)strlen(l);
		char line[120];

		snprintf(line, sizeof(line), "%.*s", n, l);
		if (strstr(line, "rror") || strstr(line, "nvalid")) {
			note(line, 1);
			return 1;
		}
		l = e ? e + 1 : 0;
	}
	return 0;
}

/* Next tab-separated cell of a `v` row, into out. */
static char *cell(char *p, char *out, int max)
{
	int n = 0;

	while (*p && *p != '\t' && *p != '\n') {
		if (n < max - 1)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	return *p == '\t' ? p + 1 : p;
}

/*
 * `v`: a header row (1.Vout 2.IO0 ...), what each pin does in this mode
 * (OFF or the supply's current, then SDO, - ...) and the voltages.
 */
void bp_measure(void)
{
	char *h, *f, *v;
	int was = B.psu;

	if (bp_cmd("v") < 0 || !(h = strstr(B.rx, "1.Vout")) || !(f = strchr(h, '\n')) || !(v = strchr(++f, '\n')))
		return;
	v++;
	f = cell(f, B.iout, sizeof(B.iout));
	v = cell(v, B.vout, sizeof(B.vout));
	for (int i = 0; i < BP_PINS; i++) {
		f = cell(f, B.pin_f[i], sizeof(B.pin_f[i]));
		v = cell(v, B.pin_v[i], sizeof(B.pin_v[i]));
	}
	B.psu = strcmp(B.iout, "OFF") != 0;
	if (was && !B.psu)
		note("The power supply switched off: over the current limit?", 1);
}

/* W, P and the LA need a mode other than HiZ: DIO touches no pins. */
static void need_mode(void)
{
	if (strcmp(B.mode, "HiZ"))
		return;
	bp_mode("dio");
}

void bp_psu(int on)
{
	char cmd[32];

	B.notice[0] = 0;
	need_mode();
	if (!on) {
		bp_cmd("w");
	} else {
		snprintf(cmd, sizeof(cmd), "W %d.%d %d", B.dv / 10, B.dv % 10, B.ma);
		if (bp_cmd(cmd) == 0 && !strstr(B.rx, "Enabled") && !error_line())
			note("The power supply did not switch on", 1);
	}
	bp_measure();
}

void bp_pullups(int on)
{
	B.notice[0] = 0;
	need_mode();
	if (bp_cmd(on ? "P" : "p") < 0)
		return;
	if (strstr(B.rx, on ? "Enabled" : "Disabled"))
		B.pullups = on;
	else
		error_line();
}

/* DIO: IOn low, high (a, A) or an input (@), then the pins measured */
void bp_pin(int io, int how)
{
	char cmd[16];

	if (strcmp(B.mode, "DIO"))
		bp_mode("dio");
	snprintf(cmd, sizeof(cmd), "%c %d", how < 0 ? '@' : how ? 'A' : 'a', io);
	if (bp_cmd(cmd) == 0)
		error_line();
	bp_measure();
}

/* "scan" in I2C mode: "0x3C (0x78 W) (0x79 R)" for each one found */
int bp_i2c_scan(uint8_t found[128])
{
	int n = 0;

	memset(found, 0, 128);
	if (strcmp(B.mode, "I2C") && bp_mode("i2c") < 0)
		return -1;
	if (bp_cmd("scan") < 0)
		return -1;
	for (char *p = B.rx; (p = strstr(p, "0x")); p += 2) {
		unsigned a;

		if (sscanf(p + 2, "%2x (", &a) == 1 && p[4] == ' ' && p[5] == '(' && a < 128 && !found[a]) {
			found[a] = 1;
			n++;
		}
	}
	return n;
}

/*
 * Light the LEDs: "[0x..:n]". WS2812s take green-red-blue; an APA102 frame
 * is 0xE0 | global brightness, then blue, green, red; the onboard LED is
 * plain RGB. LED mode needs the supply on: the Bus Pirate's IO buffers run
 * on Vout (home.c found that out: the strip stayed dark without).
 */
void bp_leds(int off)
{
	char cmd[64];
	uint32_t c = off ? 0 : B.led_rgb, r = (c >> 16) * (unsigned)B.led_pct / 100,
		 g = (c >> 8 & 255) * (unsigned)B.led_pct / 100, b = (c & 255) * (unsigned)B.led_pct / 100;
	int n = B.led_type == LT_ONBOARD ? 1 : B.led_n, was = B.psu;

	B.notice[0] = 0;
	if (B.led_set != B.led_type) {
		char num[4];

		if (bp_cmd("m led") < 0)
			return;
		if (strstr(B.rx, "y/n") && bp_cmd("n") < 0)
			return;
		if (strstr(B.rx, "Type")) {
			snprintf(num, sizeof(num), "%d", B.led_type + 1);
			if (bp_cmd(num) < 0)
				return;
		}
		B.psu = B.pullups = 0;
		if (strcmp(B.mode, "LED")) {
			if (!error_line())
				note("Could not switch to LED mode", 1);
			return;
		}
		B.led_set = B.led_type;
		bp_measure();
		if (was || (B.led_type != LT_ONBOARD && bp_dv_of(B.vout) < 10)) {
			if (!was)
				B.dv = 50;
			bp_psu(1);
		}
	}
	if (B.led_type == LT_WS2812)
		snprintf(cmd, sizeof(cmd), "[0x%06X:%d]", (unsigned)(g << 16 | r << 8 | b), n);
	else if (B.led_type == LT_APA102)
		snprintf(cmd, sizeof(cmd), "[0x%08X:%d]", (unsigned)(0xffu << 24 | b << 16 | g << 8 | r), n);
	else
		snprintf(cmd, sizeof(cmd), "[0x%06X]", (unsigned)(r << 16 | g << 8 | b));
	if (bp_cmd(cmd) == 0)
		error_line();
}

#ifndef OHM_HOST

static int read_str(const char *path, char *buf, int n)
{
	int f = open(path, O_RDONLY | O_CLOEXEC), k;

	if (f < 0)
		return -1;
	k = (int)read(f, buf, (size_t)n - 1);
	close(f);
	if (k < 0)
		return -1;
	while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == ' '))
		k--;
	buf[k] = 0;
	return k;
}

/* Is it on USB (1209:7331)? Also finds its ttys: the two CDC ports'
 * control interfaces are 0 and 2. */
int bp_find(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	int found = 0;

	tty[0][0] = tty[1][0] = 0;
	while (d && !found && (e = readdir(d))) {
		char path[128], v[8];

		if (!strchr(e->d_name, '-') || strchr(e->d_name, ':') || strlen(e->d_name) > 40)
			continue;
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
		if (read_str(path, v, sizeof(v)) <= 0 || strcmp(v, "1209"))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", e->d_name);
		if (read_str(path, v, sizeof(v)) <= 0 || strcmp(v, "7331"))
			continue;
		found = 1;
		for (int i = 0; i < 2; i++) {
			DIR *t;
			struct dirent *te;

			snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s:1.%d/tty", e->d_name, 2 * i);
			if (!(t = opendir(path)))
				continue;
			while ((te = readdir(t)))
				if (te->d_name[0] != '.' && strlen(te->d_name) < sizeof(tty[i])) {
					strcpy(tty[i], te->d_name);
					break;
				}
			closedir(t);
		}
	}
	if (d)
		closedir(d);
	B.present = found;
	return found;
}

static int tty_open_raw(const char *name)
{
	char path[32];
	struct termios t;
	int f;

	snprintf(path, sizeof(path), "/dev/%s", name);
	f = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (f < 0 || tcgetattr(f, &t) < 0)
		return f;
	t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
	t.c_oflag &= ~OPOST;
	t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(CSIZE | PARENB | CRTSCTS);
	t.c_cflag |= CS8 | CLOCAL | CREAD | HUPCL;	/* close drops DTR: a hang-up */
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	tcsetattr(f, TCSANOW, &t);
	return f;
}

void bp_close(void)
{
	if (la_fd >= 0)
		close(la_fd);
	if (fd >= 0)
		close(fd);
	la_fd = fd = -1;
	B.open = 0;
}

static void lost(void)
{
	bp_close();
	note("The Bus Pirate stopped answering", 1);
}

/* Up to the ETX after its prompt, into B.rx without CRs and ANSI. 0, or
 * -1 on a timeout (B.rx holds what came: its colour question has no ETX). */
static int wait_reply(int timeout_ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	long end = now_ms() + timeout_ms;
	int n = 0, esc = 0;

	B.rx[0] = 0;
	for (;;) {
		char c[256];
		long left = end - now_ms();
		int k;

		B.rx[n] = 0;
		if (left <= 0)
			return -1;
		k = poll(&p, 1, (int)left);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		k = (int)read(fd, c, sizeof(c));
		if (k < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (k <= 0)
			return -1;
		for (int i = 0; i < k; i++) {
			unsigned char ch = (unsigned char)c[i];

			if (ch == 0x03) {
				B.rx[n] = 0;
				return 0;
			}
			if (esc == 1) {
				esc = ch == '[' ? 2 : 0;
				continue;
			}
			if (esc == 2) {
				if (ch >= 0x40 && ch <= 0x7e)
					esc = 0;
				continue;
			}
			if (ch == 0x1b)
				esc = 1;
			else if (ch != '\r' && n < (int)sizeof(B.rx) - 1)
				B.rx[n++] = (char)ch;
		}
	}
}

int bp_cmd(const char *cmd)
{
	char junk[256];

	if (fd < 0)
		return -1;
	while (read(fd, junk, sizeof(junk)) > 0)
		;		/* stale output */
	if (write(fd, cmd, strlen(cmd)) < 0 || write(fd, "\r", 1) < 0 || wait_reply(5000) < 0) {
		lost();
		return -1;
	}
	prompt();
	return 0;
}

/* "m <name>", and its questions (speed, ...) answered with their defaults */
int bp_mode(const char *name)
{
	char cmd[24];

	snprintf(cmd, sizeof(cmd), "m %s", name);
	if (bp_cmd(cmd) < 0)
		return -1;
	for (int i = 0; i < 8 && !prompt(); i++) {
		if (strstr(B.rx, "y/n") && strstr(B.rx, "previous"))
			bp_cmd("n");		/* not the previous settings: defaults */
		else
			bp_cmd("");
		if (fd < 0)
			return -1;
	}
	B.psu = B.pullups = 0;			/* a mode change switches both off */
	if (!prompt() || strncasecmp(B.mode, name, strlen(B.mode))) {
		if (!error_line())
			note("Could not switch the Bus Pirate's mode", 1);
		return -1;
	}
	return 0;
}

int bp_open(void (*connecting)(void))
{
	long t0 = now_ms();
	int ok = 0;

	if (fd >= 0)
		return 0;
	B.notice[0] = 0;
	if (!bp_find() || !tty[0][0]) {
		note("Connect a Bus Pirate 6 to the bottom port", 1);
		return -1;
	}
	if (connecting)
		connecting();
	fd = tty_open_raw(tty[0]);
	if (fd < 0) {
		note("Cannot open the Bus Pirate's terminal", 1);
		return -1;
	}
	/* it ejects and re-inserts its drive first; a new terminal is asked
	 * about colours first */
	while (!ok && now_ms() - t0 < 20000) {
		int got;

		if (write(fd, "\r", 1) < 0)
			break;
		got = wait_reply(1500);
		if (strstr(B.rx, "(Y/n)"))
			write(fd, "n\r", 2);
		else if (got < 0)
			continue;
		else if (!(ok = prompt()))
			write(fd, "x\r", 2);		/* left in a menu: out of it */
	}
	if (!ok) {
		bp_close();
		note("The Bus Pirate does not answer: unplug it and plug it in again", 1);
		return -1;
	}
	B.open = 1;
	if (bp_cmd("i") == 0) {
		B.n_info = 0;
		B.sump = strstr(B.rx, "binmode: SUMP") != 0;
		for (char *l = strchr(B.rx, '\n'), *e; l && *l; l = e) {
			int n;

			e = strchr(++l, '\n');
			n = e ? (int)(e - l) : (int)strlen(l);
			if (!e || n == 0 || !strncmp(l, "This device", 11) || l[0] == '(')
				continue;
			if (!strncmp(l, "Bus Pirate", 10))
				snprintf(B.board, sizeof(B.board), "%.*s", n, l);
			if (!strncmp(l, "Firmware", 8) && memchr(l, '@', (size_t)n))
				snprintf(B.fw, sizeof(B.fw), "%.*s", (int)(n - ((char *)memchr(l, '@', (size_t)n) + 2 - l)),
					 (char *)memchr(l, '@', (size_t)n) + 2);
			if (B.n_info < (int)(sizeof(B.info) / sizeof(B.info[0])))
				snprintf(B.info[B.n_info++], sizeof(B.info[0]), "%.*s", n, l);
		}
	}
	need_mode();
	bp_measure();
	return fd >= 0 ? 0 : -1;
}

/* ---- the logic analyzer: SUMP on the second port ------------------------- */

static void sump_long(int f, int cmd, uint32_t v)
{
	unsigned char b[5] = { (unsigned char)cmd, (unsigned char)v, (unsigned char)(v >> 8),
			       (unsigned char)(v >> 16), (unsigned char)(v >> 24) };

	write(f, b, 5);
}

static void sump_reset(int f)
{
	static const unsigned char z[5];

	write(f, z, 5);
}

/* The SUMP port; its ID command answers "1ALS". */
static int la_open(void)
{
	unsigned char id[4];
	struct pollfd p;
	int n = 0;

	if (la_fd >= 0)
		return 0;
	if (!tty[1][0] || (la_fd = tty_open_raw(tty[1])) < 0)
		return -1;
	sump_reset(la_fd);
	usleep(20000);
	tcflush(la_fd, TCIFLUSH);
	write(la_fd, "\x02", 1);
	p.fd = la_fd;
	p.events = POLLIN;
	for (long end = now_ms() + 1500; n < 4 && now_ms() < end; ) {
		int k;

		if (poll(&p, 1, 200) <= 0)
			continue;
		k = (int)read(la_fd, id + n, (size_t)(4 - n));
		if (k > 0)
			n += k;
	}
	if (n == 4 && !memcmp(id, "1ALS", 4))
		return 0;
	close(la_fd);
	la_fd = -1;
	return -1;
}

/*
 * 8 channels at rate, after an edge on IOtrig (-1: none): two trigger
 * stages with the same mask and opposite values make the edge. It starts
 * sampling at the trigger; the samples arrive newest first. The number of
 * samples, 0 when cancelled, -1 with B.notice.
 */
int bp_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int (*cancel)(void))
{
	static uint8_t raw[BP_LA_MAX];
	uint32_t m = trig >= 0 ? 1u << trig : 0;
	long end = now_ms() + (long)((unsigned long long)n * 1000 / rate) + 3000;
	struct pollfd p = { 0, POLLIN, 0 };
	int got = 0;

	need_mode();
	if (la_open() < 0) {
		note(B.sump ? "The logic analyzer port does not answer"
			    : "Set the Bus Pirate's binmode to SUMP (in its terminal: binmode)", 1);
		return -1;
	}
	sump_reset(la_fd);
	tcflush(la_fd, TCIFLUSH);
	sump_long(la_fd, 0x80, 100000000u / rate - 1);		/* divider of 100 MHz */
	sump_long(la_fd, 0x81, (uint32_t)(n / 4 - 1) << 16 | (uint32_t)(n / 4 - 1));
	sump_long(la_fd, 0x82, 0x38);				/* 8 channels */
	sump_long(la_fd, 0xc0, m);
	sump_long(la_fd, 0xc1, falling ? m : 0);
	sump_long(la_fd, 0xc2, m ? 0x08000000 : 0);
	sump_long(la_fd, 0xc4, m);
	sump_long(la_fd, 0xc5, falling ? 0 : m);
	write(la_fd, "\x01", 1);				/* arm */
	p.fd = la_fd;
	while (got < n) {
		int k = poll(&p, 1, 200);

		if (cancel && cancel()) {
			sump_reset(la_fd);
			return 0;
		}
		if (k > 0) {
			k = (int)read(la_fd, raw + got, (size_t)(n - got));
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
				lost();
				return -1;
			}
			if (k > 0) {
				got += k;
				end = now_ms() + 3000;
			}
		}
		if (!got && m)
			end = now_ms() + 3000;		/* waiting for the trigger */
		if (now_ms() > end) {
			note("No data from the logic analyzer", 1);
			return -1;
		}
	}
	for (int i = 0; i < n; i++)
		buf[i] = raw[n - 1 - i];
	return n;
}

#else

/* ---- the host: a simulated Bus Pirate (OHM_BP=1) -------------------------- */

int bp_find(void)
{
	return B.present = getenv("OHM_BP") != 0;
}

void bp_close(void)
{
	B.open = 0;
}

static int sim_psu;			/* the simulated supply */

int bp_cmd(const char *cmd)
{
	if (!B.open)
		return -1;
	B.rx[0] = 0;
	if (cmd[0] == 'W' || !strcmp(cmd, "w"))
		sim_psu = cmd[0] == 'W';
	if (!strcmp(cmd, "v"))
		snprintf(B.rx, sizeof(B.rx), "Pin voltage\n1.Vout\t2.IO0\t3.IO1\t4.IO2\t5.IO3\t6.IO4\t7.IO5\t8.IO6\t"
			 "9.IO7\t10.GND\n%s\tSCK\tMOSI\tCS\tMISO\t-\t-\t-\t-\tGND\n%s\t3.3V\t0.0V\t3.3V\t0.0V\t"
			 "0.0V\t0.0V\t0.0V\t0.0V\tGND\n%s> ", sim_psu ? "42mA" : "OFF", sim_psu ? "3.29V" : "0.0V",
			 B.mode);
	else if (cmd[0] == 'W')
		strcpy(B.rx, "Power supply\nEnabled\n");
	else if (!strcmp(cmd, "P") || !strcmp(cmd, "p"))
		strcpy(B.rx, cmd[0] == 'P' ? "Pull-up resistors: Enabled\n" : "Pull-up resistors: Disabled\n");
	else if (!strcmp(cmd, "scan"))
		strcpy(B.rx, "I2C address search:\n0x50 (0xA0 W) (0xA1 R)\n0x68 (0xD0 W) (0xD1 R)\n\nFound 4 addresses, 2 W/R pairs.\n");
	return 0;
}

int bp_mode(const char *name)
{
	int i;

	for (i = 0; name[i] && i < (int)sizeof(B.mode) - 1; i++)
		B.mode[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	B.mode[i] = 0;
	B.psu = B.pullups = 0;
	return 0;
}

int bp_open(void (*connecting)(void))
{
	if (!bp_find()) {
		note("Connect a Bus Pirate 6 to the bottom port", 1);
		return -1;
	}
	if (connecting)
		connecting();
	B.open = 1;
	B.sump = 1;
	strcpy(B.mode, "DIO");
	strcpy(B.board, "Bus Pirate 6 REV2");
	strcpy(B.fw, "main branch (2026-10-01)");
	strcpy(B.info[0], "Bus Pirate 6 REV2");
	strcpy(B.info[1], "Firmware main branch (2026-10-01)");
	B.n_info = 2;
	bp_measure();
	return 0;
}

int bp_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int (*cancel)(void))
{
	(void)rate;
	(void)trig;
	(void)falling;
	(void)cancel;
	for (int i = 0; i < n; i++)
		buf[i] = (uint8_t)((i / 64 % 2) | ((i / 128 * 3 / 5) % 3 == 0) << 1 | (i % 4096 < 3600) << 2 |
				   (i / 200 % 2) << 3 | (i % 2048 > 1500) << 4 | (i / 500 % 2) << 5 |
				   (i < 300 || i > n - 300) << 6);
	return n;
}
#endif
