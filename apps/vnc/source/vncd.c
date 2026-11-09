// SPDX-License-Identifier: MIT
/*
 * vncd — the badge's screen and keypad over VNC.
 *
 *   vncd [-p PORT] [-f FB] [-i INPUT] [-d DATADIR]
 *
 * An RFB server for one viewer at a time (RFB 3.8; 3.7 and 3.3 viewers
 * too): it sends the panel's picture, FB (/dev/fb0, 720x720 RGB565, as
 * apps draw it), and turns the viewer's keys into the keypad's. INPUT is
 * the keypad's evdev device, the one named "tca8418" in
 * /proc/bus/input/devices (else /dev/input/event0). PORT is 5900.
 *
 * VNC authentication only: the password is the first line of
 * DATADIR/password ($APP_DATA, else /mnt/sd/badge/appdata/vnc), of which
 * VNC uses 8 characters. Without the file vncd makes one with 8 random
 * characters. A wrong password costs a second; after 5 in a row vncd
 * refuses viewers for 60 s. A second viewer is refused while one is
 * connected.
 *
 * The picture is read with pread (fbdev cannot be mapped on NOMMU) into
 * one frame buffer, at most 10 times a second and only while the viewer
 * waits for an update. A hash per 16x16 tile tells what changed since the
 * viewer got it, so an incremental update sends only those tiles.
 * Encodings: Raw and Hextile; pixel formats: true colour at 8, 16 or 32
 * bits per pixel, either byte order.
 *
 * Keys are written to INPUT as input events, which evdev passes on as if
 * the keypad sent them. A key the keypad lacks goes as the badge's Fn
 * layer has it: Delete as Fn+Backspace, Page Up/Down and Home/End as
 * Fn+arrows, F7-F12 as Fn+F1..F6. Super and Meta are the logo key.
 * The pointer and the clipboard are ignored.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/des.h>

#define W		720
#define H		720
#define BPL		(W * 2)		/* bytes per row of FB */
#define TILE		16
#define TW		(W / TILE)
#define TH		(H / TILE)
#define POLL_MS		100		/* at most 10 frames a second */
#define IO_MS		10000		/* a viewer that reads or answers slower is dropped */
#define LOCK_FAILS	5
#define LOCK_MS		60000
#define NREJECT		4		/* refused viewers being told why */
#define NHELD		16
#define KEY_FN_BADGE	0x1d0		/* KEY_FN, the badge's Fn key */

/* no 0/o, 1/l/i: easy to read off the screen and type */
static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
static const char *fb_path = "/dev/fb0", *data_dir;
static char in_path[64] = "/dev/input/event0";
static volatile sig_atomic_t quit;
static int fails;
static int64_t locked_until;

static struct {
	int fd, ver, dead, authed;
	char addr[INET_ADDRSTRLEN];
	int fb, in;			/* FB and INPUT, -1 if not open */
	unsigned char *frame;		/* the last picture read */
	uint32_t hash[TW * TH];		/* of each tile as the viewer has it */
	unsigned char known[TW * TH];	/* ... once it has it */
	int hextile;
	int want, incr, x0, y0, x1, y1;	/* the update the viewer waits for */
	int64_t last;			/* when FB was read */
	unsigned char ib[4096];		/* what the viewer sent, not yet handled */
	size_t ilen;
	uint32_t skip;			/* clipboard text still to skip */
	unsigned char keys[KEY_MAX / 8 + 1];	/* the keys INPUT has */
	struct { uint32_t sym; uint16_t code, extra; } held[NHELD];
} C = { .fd = -1 };

/* The viewer's pixel format: a table per colour of RGB565 */
static struct {
	int bpp, big, native;
	uint32_t r[32], g[64], b[32];
} pf;

static struct {
	int fd;
	int64_t until;
	const char *why;
	size_t n;
	char v[12];
} R[NREJECT];

static unsigned char ob[16384];
static size_t olen;

static int64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

/* Output to the viewer: buffered, sent with a time limit. */
static void flush(void)
{
	size_t off = 0;
	int64_t until = now_ms() + IO_MS;

	while (off < olen && !C.dead) {
		ssize_t n = send(C.fd, ob + off, olen - off, MSG_NOSIGNAL);
		int left;

		if (n > 0) {
			off += n;
			until = now_ms() + IO_MS;
			continue;
		}
		if (n < 0 && errno == EINTR && !quit)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			left = (int)(until - now_ms());
			if (left > 0) {
				struct pollfd p = { .fd = C.fd, .events = POLLOUT };

				poll(&p, 1, left);
				continue;
			}
			fprintf(stderr, "vncd: %s does not read, dropped\n", C.addr);
		}
		C.dead = 1;
	}
	olen = 0;
}

static void room(size_t n)
{
	if (olen + n > sizeof(ob))
		flush();
}

static void put(const void *p, size_t n)
{
	room(n);
	memcpy(ob + olen, p, n);
	olen += n;
}

static void put8(unsigned v)
{
	room(1);
	ob[olen++] = v;
}

static void put16(unsigned v)
{
	room(2);
	ob[olen++] = v >> 8;
	ob[olen++] = v;
}

static void put32(uint32_t v)
{
	room(4);
	ob[olen++] = v >> 24;
	ob[olen++] = v >> 16;
	ob[olen++] = v >> 8;
	ob[olen++] = v;
}

static void put_reason(const char *s)
{
	put32(strlen(s));
	put(s, strlen(s));
}

static unsigned be16(const unsigned char *p)
{
	return p[0] << 8 | p[1];
}

static uint32_t be32(const unsigned char *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | p[2] << 8 | p[3];
}

/* Exactly n bytes from the viewer during the handshake, or -1. */
static int rd(void *buf, size_t n)
{
	unsigned char *p = buf;
	int64_t until = now_ms() + IO_MS;

	while (n) {
		ssize_t r = recv(C.fd, p, n, 0);
		struct pollfd q = { .fd = C.fd, .events = POLLIN };
		int left;

		if (r > 0) {
			p += r;
			n -= r;
			continue;
		}
		if (r == 0 || quit || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			return -1;
		left = (int)(until - now_ms());
		if (left <= 0)
			return -1;
		poll(&q, 1, left);
	}
	return 0;
}

static int random_bytes(unsigned char *p, size_t n)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	ssize_t r = -1;

	if (fd >= 0) {
		r = read(fd, p, n);
		close(fd);
	}
	return r == (ssize_t)n ? 0 : -1;
}

/* DATADIR, and its parent (appdata) if need be: not further up, so that
 * without a card nothing lands in the card's mount point. */
static void make_dir(const char *dir)
{
	char p[256], *s;

	if (!mkdir(dir, 0700) || errno != ENOENT)
		return;
	snprintf(p, sizeof(p), "%s", dir);
	s = strrchr(p, '/');
	if (!s || s == p)
		return;
	*s = 0;
	mkdir(p, 0755);
	mkdir(dir, 0700);
}

/* A new password: 8 characters, in DATADIR/password (mode 0600, synced:
 * the card's f2fs loses what is not). */
static void make_password(const char *path, char pw[9])
{
	unsigned char b[64];
	size_t i, n = 0;
	int fd;

	if (random_bytes(b, sizeof(b))) {
		pw[0] = 0;
		return;
	}
	for (i = 0; i < sizeof(b) && n < 8; i++)
		if (b[i] < 248)		/* 8 * 31: as many of each character */
			pw[n++] = alphabet[b[i] % 31];
	pw[n] = 0;
	make_dir(data_dir);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write(fd, pw, n) != (ssize_t)n || write(fd, "\n", 1) != 1 ||
	    fchmod(fd, 0600) || fsync(fd)) {
		fprintf(stderr, "vncd: cannot write %s, password for this run only\n", path);
	} else {
		int dfd = open(data_dir, O_RDONLY | O_CLOEXEC);

		if (dfd >= 0) {
			fsync(dfd);
			close(dfd);
		}
	}
	if (fd >= 0)
		close(fd);
}

/* The password, read again for each viewer: changing the file is enough. */
static void password(char pw[9])
{
	static char mem[9];
	char path[300], buf[64];
	ssize_t n = -1;
	int fd;

	snprintf(path, sizeof(path), "%s/password", data_dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
	}
	if (n > 0) {
		buf[n] = 0;
		buf[strcspn(buf, "\r\n")] = 0;
		if (buf[0]) {
			buf[8] = 0;		/* VNC uses 8 characters */
			memcpy(pw, buf, 9);
			return;
		}
	}
	if (fd < 0 && mem[0] && errno == ENOENT && access(data_dir, W_OK)) {
		memcpy(pw, mem, 9);	/* no card: the one made earlier */
		return;
	}
	make_password(path, pw);
	memcpy(mem, pw, 9);
}

static unsigned char bitrev(unsigned char c)
{
	c = (c & 0xf0) >> 4 | (c & 0x0f) << 4;
	c = (c & 0xcc) >> 2 | (c & 0x33) << 2;
	return (c & 0xaa) >> 1 | (c & 0x55) << 1;
}

/* VNC authentication: the challenge DES-encrypted with the password,
 * each byte's bits reversed, as the key. */
static int auth_ok(const unsigned char chal[16], const unsigned char resp[16], const char *pw)
{
	unsigned char key[8] = { 0 }, want[16];
	mbedtls_des_context d;
	int i, diff = 0;

	for (i = 0; i < 8 && pw[i]; i++)
		key[i] = bitrev(pw[i]);
	mbedtls_des_init(&d);
	mbedtls_des_setkey_enc(&d, key);
	mbedtls_des_crypt_ecb(&d, chal, want);
	mbedtls_des_crypt_ecb(&d, chal + 8, want + 8);
	mbedtls_des_free(&d);
	for (i = 0; i < 16; i++)
		diff |= want[i] ^ resp[i];
	memset(key, 0, sizeof(key));
	return !diff && pw[0];
}

/* "RFB 003.00x\n" -> 3, 7 or 8, or -1 */
static int version(const char *v)
{
	int i, major, minor;

	if (memcmp(v, "RFB ", 4) || v[7] != '.' || v[11] != '\n')
		return -1;
	for (i = 4; i < 11; i++)
		if (i != 7 && (v[i] < '0' || v[i] > '9'))
			return -1;
	major = (v[4] - '0') * 100 + (v[5] - '0') * 10 + v[6] - '0';
	minor = (v[8] - '0') * 100 + (v[9] - '0') * 10 + v[10] - '0';
	if (major != 3)
		return major > 3 ? 8 : -1;
	return minor >= 8 ? 8 : minor == 7 ? 7 : 3;
}

static void set_format(int bpp, int big, unsigned rmax, unsigned gmax, unsigned bmax,
		       int rs, int gs, int bs)
{
	unsigned i;

	pf.bpp = bpp;
	pf.big = big;
	for (i = 0; i < 32; i++) {
		pf.r[i] = (uint32_t)((i * rmax + 15) / 31) << rs;
		pf.b[i] = (uint32_t)((i * bmax + 15) / 31) << bs;
	}
	for (i = 0; i < 64; i++)
		pf.g[i] = (uint32_t)((i * gmax + 31) / 63) << gs;
	pf.native = bpp == 16 && !big && rmax == 31 && gmax == 63 && bmax == 31 &&
		    rs == 11 && gs == 5 && bs == 0;
}

static unsigned char *pixel(unsigned char *d, unsigned v)
{
	uint32_t c = pf.r[v >> 11] | pf.g[(v >> 5) & 63] | pf.b[v & 31];

	switch (pf.bpp) {
	case 8:
		*d++ = c;
		break;
	case 16:
		*d++ = pf.big ? c >> 8 : c;
		*d++ = pf.big ? c : c >> 8;
		break;
	default:
		if (pf.big) {
			*d++ = c >> 24;
			*d++ = c >> 16;
			*d++ = c >> 8;
			*d++ = c;
		} else {
			*d++ = c;
			*d++ = c >> 8;
			*d++ = c >> 16;
			*d++ = c >> 24;
		}
	}
	return d;
}

static unsigned fbpix(int x, int y)
{
	const unsigned char *p = C.frame + y * BPL + x * 2;

	return p[0] | p[1] << 8;
}

/* n pixels of row y from x on, in the viewer's format, to the output */
static void row(int x, int y, int n)
{
	const unsigned char *s = C.frame + y * BPL + x * 2;
	unsigned char *d;
	int i;

	room((size_t)n * pf.bpp / 8);
	if (pf.native) {
		memcpy(ob + olen, s, n * 2);
		olen += n * 2;
		return;
	}
	d = ob + olen;
	for (i = 0; i < n; i++, s += 2)
		d = pixel(d, s[0] | s[1] << 8);
	olen = d - ob;
}

static void rect(int x, int y, int w, int h)
{
	int tx, ty, i, j, tw, th, bg = -1;
	unsigned c;

	put16(x);
	put16(y);
	put16(w);
	put16(h);
	put32(C.hextile ? 5 : 0);
	if (!C.hextile) {
		for (j = y; j < y + h && !C.dead; j++)
			row(x, j, w);
		return;
	}
	/* Hextile: a tile of one colour as its background, others raw */
	for (ty = y; ty < y + h && !C.dead; ty += TILE) {
		th = y + h - ty < TILE ? y + h - ty : TILE;
		for (tx = x; tx < x + w; tx += TILE) {
			tw = x + w - tx < TILE ? x + w - tx : TILE;
			c = fbpix(tx, ty);
			for (j = 0; j < th; j++)
				for (i = 0; i < tw; i++)
					if (fbpix(tx + i, ty + j) != c)
						goto raw;
			if ((int)c == bg) {
				put8(0);
			} else {
				room(5);
				ob[olen++] = 2;
				olen = pixel(ob + olen, c) - ob;
				bg = c;
			}
			continue;
raw:
			put8(1);
			for (j = 0; j < th; j++)
				row(tx, ty + j, tw);
			bg = -1;	/* not carried over a raw tile */
		}
	}
}

static int read_frame(void)
{
	size_t off = 0, size = (size_t)BPL * H;

	while (off < size) {
		ssize_t n = pread(C.fb, C.frame + off, size - off, off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		off += n;
	}
	return 0;
}

static uint32_t tile_hash(int tx, int ty)
{
	const unsigned char *p;
	uint32_t h = 2166136261u, w;	/* FNV-1a, a word at a time */
	int y, i;

	for (y = 0; y < TILE; y++) {
		p = C.frame + (ty * TILE + y) * BPL + tx * TILE * 2;
		for (i = 0; i < TILE * 2; i += 4) {
			memcpy(&w, p + i, 4);
			h = (h ^ w) * 16777619u;
		}
	}
	return h;
}

/* Runs of changed tiles in the requested area, one rectangle each; sent
 * when send is set, else counted. */
static int runs(const unsigned char *dirty, int send)
{
	int tx, ty, n = 0, a, x, y, x1, y1;

	for (ty = C.y0 / TILE; ty * TILE < C.y1; ty++) {
		for (tx = C.x0 / TILE; tx * TILE < C.x1; tx++) {
			if (!dirty[ty * TW + tx])
				continue;
			for (a = tx; tx * TILE < C.x1 && dirty[ty * TW + tx]; tx++)
				;
			n++;
			if (!send)
				continue;
			x = a * TILE > C.x0 ? a * TILE : C.x0;
			y = ty * TILE > C.y0 ? ty * TILE : C.y0;
			x1 = tx * TILE < C.x1 ? tx * TILE : C.x1;
			y1 = (ty + 1) * TILE < C.y1 ? (ty + 1) * TILE : C.y1;
			rect(x, y, x1 - x, y1 - y);
		}
	}
	return n;
}

static void update(void)
{
	static unsigned char dirty[TW * TH];
	int tx, ty, t, n, any = 0;
	uint32_t h;

	if (read_frame()) {
		fprintf(stderr, "vncd: cannot read %s\n", fb_path);
		C.dead = 1;
		return;
	}
	C.last = now_ms();
	memset(dirty, 0, sizeof(dirty));
	for (ty = C.y0 / TILE; ty * TILE < C.y1; ty++) {
		for (tx = C.x0 / TILE; tx * TILE < C.x1; tx++) {
			t = ty * TW + tx;
			h = tile_hash(tx, ty);
			dirty[t] = !C.known[t] || C.hash[t] != h;
			any |= dirty[t];
			/* a tile only partly asked for stays as it was */
			if (tx * TILE >= C.x0 && (tx + 1) * TILE <= C.x1 &&
			    ty * TILE >= C.y0 && (ty + 1) * TILE <= C.y1) {
				C.hash[t] = h;
				C.known[t] = 1;
			}
		}
	}
	if (C.incr && C.x1 <= C.x0) {
		C.want = 0;		/* asked for nothing */
		return;
	}
	if (C.incr && !any)
		return;			/* nothing new: read FB again later */
	put8(0);
	put8(0);
	if (C.incr) {
		n = runs(dirty, 0);
		put16(n);
		runs(dirty, 1);
	} else if (C.x1 > C.x0 && C.y1 > C.y0) {
		put16(1);
		rect(C.x0, C.y0, C.x1 - C.x0, C.y1 - C.y0);
	} else {
		put16(0);
	}
	flush();
	C.want = 0;
}

/* Keys */

static int has_key(int code)
{
	return code > 0 && code <= KEY_MAX && C.keys[code / 8] & 1 << (code % 8);
}

static void emit(int type, int code, int value)
{
	struct input_event e;

	memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.value = value;
	if (C.in >= 0 && write(C.in, &e, sizeof(e)) != sizeof(e))
		return;
}

static void key(int code, int down)
{
	emit(EV_KEY, code, down);
	emit(EV_SYN, SYN_REPORT, 0);
}

/* X11 keysym -> key code, with *shift set for a shifted character (US) */
static int keysym_code(uint32_t ks, int *shift)
{
	static const unsigned char letter[26] = {
		KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
		KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
		KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	};
	static const struct { char c; unsigned char code, shift; } punct[] = {
		{ ' ', KEY_SPACE, 0 }, { '!', KEY_1, 1 }, { '"', KEY_APOSTROPHE, 1 },
		{ '#', KEY_3, 1 }, { '$', KEY_4, 1 }, { '%', KEY_5, 1 }, { '&', KEY_7, 1 },
		{ '\'', KEY_APOSTROPHE, 0 }, { '(', KEY_9, 1 }, { ')', KEY_0, 1 },
		{ '*', KEY_8, 1 }, { '+', KEY_EQUAL, 1 }, { ',', KEY_COMMA, 0 },
		{ '-', KEY_MINUS, 0 }, { '.', KEY_DOT, 0 }, { '/', KEY_SLASH, 0 },
		{ ':', KEY_SEMICOLON, 1 }, { ';', KEY_SEMICOLON, 0 }, { '<', KEY_COMMA, 1 },
		{ '=', KEY_EQUAL, 0 }, { '>', KEY_DOT, 1 }, { '?', KEY_SLASH, 1 },
		{ '@', KEY_2, 1 }, { '[', KEY_LEFTBRACE, 0 }, { '\\', KEY_BACKSLASH, 0 },
		{ ']', KEY_RIGHTBRACE, 0 }, { '^', KEY_6, 1 }, { '_', KEY_MINUS, 1 },
		{ '`', KEY_GRAVE, 0 }, { '{', KEY_LEFTBRACE, 1 }, { '|', KEY_BACKSLASH, 1 },
		{ '}', KEY_RIGHTBRACE, 1 }, { '~', KEY_GRAVE, 1 },
	};
	unsigned i;

	*shift = 0;
	if (ks >= 'a' && ks <= 'z')
		return letter[ks - 'a'];
	if (ks >= 'A' && ks <= 'Z') {
		*shift = 1;
		return letter[ks - 'A'];
	}
	if (ks >= '1' && ks <= '9')
		return KEY_1 + ks - '1';
	if (ks == '0')
		return KEY_0;
	for (i = 0; i < sizeof(punct) / sizeof(punct[0]); i++)
		if ((uint32_t)punct[i].c == ks) {
			*shift = punct[i].shift;
			return punct[i].code;
		}
	if (ks >= 0xffbe && ks <= 0xffc7)	/* F1..F10 */
		return KEY_F1 + ks - 0xffbe;
	if (ks >= 0xffb0 && ks <= 0xffb9)	/* the keypad's digits */
		return ks == 0xffb0 ? KEY_0 : KEY_1 + ks - 0xffb1;
	switch (ks) {
	case 0xff08: return KEY_BACKSPACE;
	case 0xff09: return KEY_TAB;
	case 0xff0d: case 0xff8d: return KEY_ENTER;
	case 0xff1b: return KEY_ESC;
	case 0xffff: return KEY_DELETE;
	case 0xff50: return KEY_HOME;
	case 0xff51: return KEY_LEFT;
	case 0xff52: return KEY_UP;
	case 0xff53: return KEY_RIGHT;
	case 0xff54: return KEY_DOWN;
	case 0xff55: return KEY_PAGEUP;
	case 0xff56: return KEY_PAGEDOWN;
	case 0xff57: return KEY_END;
	case 0xffc8: return KEY_F11;
	case 0xffc9: return KEY_F12;
	case 0xffe1: return KEY_LEFTSHIFT;
	case 0xffe2: return KEY_RIGHTSHIFT;
	case 0xffe3: return KEY_LEFTCTRL;
	case 0xffe4: return KEY_RIGHTCTRL;
	case 0xffe7: case 0xffe8: case 0xffeb: case 0xffec:	/* Meta, Super */
		return KEY_LEFTMETA;
	case 0xffe9: return KEY_LEFTALT;
	case 0xffea: case 0xfe03: return KEY_RIGHTALT;	/* Alt_R, AltGr */
	}
	return 0;
}

/* What the keypad has instead of code: a key, and Fn with it or not */
static int instead(int code, int *fn)
{
	*fn = 1;
	switch (code) {
	case KEY_DELETE: return KEY_BACKSPACE;
	case KEY_PAGEUP: return KEY_UP;
	case KEY_PAGEDOWN: return KEY_DOWN;
	case KEY_HOME: return KEY_LEFT;
	case KEY_END: return KEY_RIGHT;
	case KEY_F7: case KEY_F8: case KEY_F9: case KEY_F10:
		return KEY_F1 + code - KEY_F7;
	case KEY_F11: return KEY_F5;
	case KEY_F12: return KEY_F6;
	}
	*fn = 0;
	switch (code) {
	case KEY_RIGHTCTRL: return KEY_LEFTCTRL;
	case KEY_RIGHTSHIFT: return KEY_LEFTSHIFT;
	case KEY_RIGHTALT: return KEY_LEFTALT;
	}
	return 0;
}

static int shift_held(void)
{
	int i;

	for (i = 0; i < NHELD; i++)
		if (C.held[i].sym && !C.held[i].extra &&
		    (C.held[i].code == KEY_LEFTSHIFT || C.held[i].code == KEY_RIGHTSHIFT))
			return 1;
	return 0;
}

static void key_event(int down, uint32_t ks)
{
	int i, code, shift, fn, extra = 0, slot = -1;

	if (!ks)
		return;
	for (i = 0; i < NHELD; i++) {
		if (C.held[i].sym == ks)
			break;
		if (!C.held[i].sym && slot < 0)
			slot = i;
	}
	if (i < NHELD) {
		/* the viewer's own repeat is ignored: the kernel repeats a held key */
		if (!down) {
			key(C.held[i].code, 0);
			if (C.held[i].extra)
				key(C.held[i].extra, 0);
			C.held[i].sym = 0;
		}
		return;
	}
	if (!down || slot < 0 || !(code = keysym_code(ks, &shift)))
		return;
	if (shift && !shift_held())
		extra = KEY_LEFTSHIFT;
	if (!has_key(code)) {
		code = instead(code, &fn);
		if (!has_key(code) || (fn && !has_key(KEY_FN_BADGE)))
			return;
		if (fn)
			extra = KEY_FN_BADGE;
	}
	if (extra)
		key(extra, 1);
	key(code, 1);
	C.held[slot].sym = ks;
	C.held[slot].code = code;
	C.held[slot].extra = extra;
}

static void release_keys(void)
{
	int i;

	for (i = 0; i < NHELD; i++)
		if (C.held[i].sym)
			key_event(0, C.held[i].sym);
}

/* The keypad's event device: the handler of the device named tca8418 */
static void find_keypad(void)
{
	char line[256], *e;
	int match = 0;
	FILE *f = fopen("/proc/bus/input/devices", "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "N: Name=", 8))
			match = strstr(line, "tca8418") != NULL;
		else if (match && !strncmp(line, "H: Handlers=", 12) && (e = strstr(line, "event"))) {
			e[strspn(e, "event0123456789")] = 0;
			snprintf(in_path, sizeof(in_path), "/dev/input/%s", e);
			break;
		}
	}
	fclose(f);
}

/* Messages from the viewer */

static void set_pixel_format(const unsigned char *m)
{
	int bpp = m[4];

	if ((bpp != 8 && bpp != 16 && bpp != 32) || !m[7] ||
	    m[14] >= bpp || m[15] >= bpp || m[16] >= bpp) {
		fprintf(stderr, "vncd: %s: pixel format not supported (%d bpp, true colour %d)\n",
			C.addr, bpp, m[7]);
		C.dead = 1;
		return;
	}
	set_format(bpp, m[6] != 0, be16(m + 8), be16(m + 10), be16(m + 12), m[14], m[15], m[16]);
}

static void set_encodings(const unsigned char *m, unsigned n)
{
	unsigned i;
	uint32_t e;

	C.hextile = 0;
	for (i = 0; i < n; i++) {
		e = be32(m + 4 + i * 4);
		if (e == 0 || e == 5) {		/* the first of the two the viewer lists */
			C.hextile = e == 5;
			return;
		}
	}
}

static void request(const unsigned char *m)
{
	int x = be16(m + 2), y = be16(m + 4), x1 = x + be16(m + 6), y1 = y + be16(m + 8);

	x1 = x1 < W ? x1 : W;
	y1 = y1 < H ? y1 : H;
	if (x >= x1 || y >= y1)
		x = y = x1 = y1 = 0;
	if (!C.want || C.x1 <= C.x0) {
		C.x0 = x;
		C.y0 = y;
		C.x1 = x1;
		C.y1 = y1;
		C.incr = m[1] != 0;
	} else {
		if (x1 > x) {	/* both: what covers them */
			C.x0 = x < C.x0 ? x : C.x0;
			C.y0 = y < C.y0 ? y : C.y0;
			C.x1 = x1 > C.x1 ? x1 : C.x1;
			C.y1 = y1 > C.y1 ? y1 : C.y1;
		}
		C.incr &= m[1] != 0;
	}
	C.want = 1;
}

static void client_input(void)
{
	ssize_t r = recv(C.fd, C.ib + C.ilen, sizeof(C.ib) - C.ilen, 0);
	size_t off = 0, left, need, k;
	unsigned char *m;

	if (r <= 0) {
		if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			C.dead = 1;
		return;
	}
	C.ilen += r;
	while (!C.dead) {
		m = C.ib + off;
		left = C.ilen - off;
		if (C.skip) {
			k = left < C.skip ? left : C.skip;
			off += k;
			C.skip -= k;
			if (C.skip)
				break;
			continue;
		}
		if (!left)
			break;
		switch (m[0]) {
		case 0: need = 20; break;
		case 2:
			need = 4;
			/* at most what fits in ib, far more than a viewer has */
			if (left >= 4 && 4 + be16(m + 2) * 4 > sizeof(C.ib)) {
				fprintf(stderr, "vncd: %s: too many encodings\n", C.addr);
				C.dead = 1;
				continue;
			}
			if (left >= 4)
				need += be16(m + 2) * 4;
			break;
		case 3: need = 10; break;
		case 4: need = 8; break;
		case 5: need = 6; break;
		case 6: need = 8; break;
		default:
			fprintf(stderr, "vncd: %s: unknown message %d\n", C.addr, m[0]);
			C.dead = 1;
			continue;
		}
		if (left < need)
			break;
		switch (m[0]) {
		case 0: set_pixel_format(m); break;
		case 2: set_encodings(m, be16(m + 2)); break;
		case 3: request(m); break;
		case 4: key_event(m[1], be32(m + 4)); break;
		case 6: C.skip = be32(m + 4); break;	/* the clipboard: not used */
		}
		off += need;
	}
	memmove(C.ib, C.ib + off, C.ilen - off);
	C.ilen -= off;
}

static void end_session(void)
{
	release_keys();
	if (C.authed)
		fprintf(stderr, "vncd: %s disconnected\n", C.addr);
	if (C.in >= 0)
		close(C.in);
	if (C.fb >= 0)
		close(C.fb);
	free(C.frame);
	close(C.fd);
	C.frame = NULL;
	C.fd = C.in = C.fb = -1;
	olen = 0;
}

/* The handshake, as one call: one viewer at a time anyway. */
static void greet(int fd, const char *addr)
{
	static const char name[] = "WhyBadge";
	char v[12], pw[9];
	unsigned char chal[16], resp[16], t;
	int one = 1, idle = 30, intvl = 10, cnt = 3;

	memset(&C.held, 0, sizeof(C.held));
	C.fd = fd;
	C.in = C.fb = -1;
	C.dead = C.authed = C.want = C.hextile = 0;
	C.ilen = C.skip = 0;
	C.last = 0;
	snprintf(C.addr, sizeof(C.addr), "%s", addr);
	memset(C.known, 0, sizeof(C.known));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

	put("RFB 003.008\n", 12);
	flush();
	if (C.dead || rd(v, 12) || (C.ver = version(v)) < 0)
		goto drop;
	if (C.ver >= 7) {
		put8(1);
		put8(2);		/* VNC authentication, nothing else */
		flush();
		if (rd(&t, 1))
			goto drop;
		if (t != 2) {
			if (C.ver == 8) {
				put32(1);
				put_reason("Only VNC authentication is supported");
				flush();
			}
			goto drop;
		}
	} else {
		put32(2);
		flush();
	}
	if (random_bytes(chal, sizeof(chal)))
		goto drop;
	password(pw);
	put(chal, sizeof(chal));
	flush();
	if (C.dead || rd(resp, sizeof(resp)))
		goto drop;
	if (!auth_ok(chal, resp, pw)) {
		fprintf(stderr, "vncd: %s: wrong password\n", C.addr);
		sleep(1);
		if (++fails >= LOCK_FAILS) {
			fails = 0;
			locked_until = now_ms() + LOCK_MS;
			fprintf(stderr, "vncd: %d wrong passwords, no viewers for %d s\n",
				LOCK_FAILS, LOCK_MS / 1000);
		}
		put32(1);
		if (C.ver == 8)
			put_reason("Wrong password");
		flush();
		goto drop;
	}
	memset(pw, 0, sizeof(pw));
	fails = 0;
	put32(0);
	flush();
	if (C.dead || rd(&t, 1))	/* ClientInit: shared or not, there is one */
		goto drop;

	C.frame = malloc((size_t)BPL * H);
	C.fb = open(fb_path, O_RDONLY | O_CLOEXEC);
	if (!C.frame || C.fb < 0) {
		fprintf(stderr, "vncd: %s: %s\n", C.addr, C.frame ? "cannot open the frame buffer" :
			"out of memory");
		goto drop;
	}
	C.in = open(in_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (C.in < 0)
		fprintf(stderr, "vncd: cannot open %s, no keys\n", in_path);
	/* a device that does not say which keys it has (a file in a test) takes all */
	memset(C.keys, 0, sizeof(C.keys));
	if (C.in < 0 || ioctl(C.in, EVIOCGBIT(EV_KEY, sizeof(C.keys)), C.keys) < 0)
		memset(C.keys, 0xff, sizeof(C.keys));
	set_format(16, 0, 31, 63, 31, 11, 5, 0);
	put16(W);
	put16(H);
	put8(16);		/* RGB565, little-endian, as FB */
	put8(16);
	put8(0);
	put8(1);
	put16(31);
	put16(63);
	put16(31);
	put8(11);
	put8(5);
	put8(0);
	put(name, 3);		/* padding */
	put_reason(name);
	flush();
	if (C.dead)
		goto drop;
	C.authed = 1;
	fprintf(stderr, "vncd: %s connected\n", C.addr);
	return;
drop:
	end_session();
}

/* A viewer that cannot come in is told why, after the version exchange. */
static void refuse(int fd, const char *addr, const char *why)
{
	int i;

	for (i = 0; i < NREJECT && R[i].fd >= 0; i++)
		;
	fprintf(stderr, "vncd: %s refused: %s\n", addr, why);
	if (i == NREJECT || send(fd, "RFB 003.008\n", 12, MSG_NOSIGNAL) != 12) {
		close(fd);
		return;
	}
	R[i].fd = fd;
	R[i].why = why;
	R[i].n = 0;
	R[i].until = now_ms() + 5000;
}

static void refuse_input(int i)
{
	unsigned char m[128];
	size_t len = strlen(R[i].why);
	ssize_t r = recv(R[i].fd, R[i].v + R[i].n, sizeof(R[i].v) - R[i].n, 0);
	int v;

	if (r < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (r > 0)
		R[i].n += r;
	if (r > 0 && R[i].n == sizeof(R[i].v) && (v = version(R[i].v)) >= 0) {
		/* 3.3: security type 0; 3.7, 3.8: no security types; then why */
		size_t h = v == 3 ? 4 : 1;

		memset(m, 0, h);
		m[h] = len >> 24;
		m[h + 1] = len >> 16;
		m[h + 2] = len >> 8;
		m[h + 3] = len;
		memcpy(m + h + 4, R[i].why, len);
		send(R[i].fd, m, h + 4 + len, MSG_NOSIGNAL);
	} else if (r > 0 && R[i].n < sizeof(R[i].v)) {
		return;
	}
	close(R[i].fd);
	R[i].fd = -1;
}

static void accept_one(int lfd)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);
	char addr[INET_ADDRSTRLEN];
	int fd = accept(lfd, (struct sockaddr *)&sa, &len);

	if (fd < 0)
		return;
	fcntl(fd, F_SETFL, O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (!inet_ntop(AF_INET, &sa.sin_addr, addr, sizeof(addr)))
		strcpy(addr, "?");
	if (now_ms() < locked_until)
		refuse(fd, addr, "Too many wrong passwords, try again in a minute");
	else if (C.fd >= 0)
		refuse(fd, addr, "Another viewer is connected");
	else
		greet(fd, addr);
}

static void usage(void)
{
	fprintf(stderr, "usage: vncd [-p PORT] [-f FB] [-i INPUT] [-d DATADIR]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	static char dd[256];
	struct sigaction sa;
	struct sockaddr_in a;
	struct pollfd p[2 + NREJECT];
	char pw[9];
	int c, port = 5900, lfd, one = 1, i, np, ci, ri[NREJECT], timeout, in_set = 0;
	int64_t now;

	while ((c = getopt(argc, argv, "p:f:i:d:")) != -1) {
		switch (c) {
		case 'p':
			port = atoi(optarg);
			if (port <= 0 || port > 65535)
				usage();
			break;
		case 'f': fb_path = optarg; break;
		case 'i':
			snprintf(in_path, sizeof(in_path), "%s", optarg);
			in_set = 1;
			break;
		case 'd': data_dir = optarg; break;
		default: usage();
		}
	}
	if (optind != argc)
		usage();
	if (!data_dir) {
		const char *e = getenv("APP_DATA");

		snprintf(dd, sizeof(dd), "%s", e && *e ? e : "/mnt/sd/badge/appdata/vnc");
		data_dir = dd;
	}
	if (!in_set)
		find_keypad();
	for (i = 0; i < NREJECT; i++)
		R[i].fd = -1;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	password(pw);		/* made now, so the app can show it */
	memset(pw, 0, sizeof(pw));

	lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 4)) {
		perror("vncd: port");
		return 1;
	}
	fprintf(stderr, "vncd: port %d, keys to %s\n", port, in_path);

	while (!quit) {
		now = now_ms();
		np = 0;
		ci = -1;
		timeout = -1;
		p[np].fd = lfd;
		p[np++].events = POLLIN;
		if (C.fd >= 0) {
			ci = np;
			p[np].fd = C.fd;
			p[np++].events = POLLIN;
			if (C.want)
				timeout = C.last + POLL_MS > now ? (int)(C.last + POLL_MS - now) : 0;
		}
		for (i = 0; i < NREJECT; i++) {
			ri[i] = -1;
			if (R[i].fd < 0)
				continue;
			ri[i] = np;
			p[np].fd = R[i].fd;
			p[np++].events = POLLIN;
			if (timeout < 0 || R[i].until - now < timeout)
				timeout = R[i].until > now ? (int)(R[i].until - now) : 0;
		}
		for (i = 0; i < np; i++)
			p[i].revents = 0;
		if (poll(p, np, timeout) < 0 && errno != EINTR) {
			perror("vncd: poll");
			break;
		}
		now = now_ms();
		for (i = 0; i < NREJECT; i++) {
			if (ri[i] >= 0 && p[ri[i]].revents)
				refuse_input(i);
			else if (ri[i] >= 0 && now >= R[i].until) {
				close(R[i].fd);
				R[i].fd = -1;
			}
		}
		if (ci >= 0 && p[ci].revents)
			client_input();
		if (C.fd >= 0 && !C.dead && C.want && now >= C.last + POLL_MS)
			update();
		if (C.fd >= 0 && C.dead)
			end_session();
		if (p[0].revents & POLLIN)
			accept_one(lfd);
	}
	if (C.fd >= 0)
		end_session();
	return 0;
}
