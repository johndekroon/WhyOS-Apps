// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * gnuboy on the WHY2025 badge: the sys/ backend for its panel and keypad
 * (built into sys/badge/ by sdapps/build.sh; see sdapps/README.md).
 *
 * Video: gnuboy draws the 160x144 screen in RGB565 into a frame of its
 * own (delegate_scaling, as its SDL2 backend does); vid_end writes each
 * line that changed since the last frame shown, scaled up (4.5x: the
 * panel's whole width, 720x648) into the middle of the panel. Lines that
 * changed side by side go in one pwrite, from buffers that stay in the cache:
 * a write costs a call and a turn of the kernel's worker, and in the sandbox
 * c1urun copies it while the next ones are made, so two buffers take turns.
 * The framebuffer cannot be mmapped on this NOMMU kernel.
 *
 * Speed: every write costs the kernel a quarter turn of those lines into
 * the panel's scan-out buffer (drm/esp32p4-dsi, ~70 ms for all 720x720),
 * in a worker that shares the one CPU with the game. gnuboy's own timing
 * sleeps for what is left of a frame and lets the game slow down when
 * nothing is left; here vid_end keeps the time instead (gnuboy's
 * framelen is 0): a deadline per Game Boy frame, a frame not drawn when
 * the game is behind it (at most MAX_UNDRAWN in a row), and a sleep up to
 * the deadline, which also makes up for usleep's ticks (HZ=250).
 * panelskip frames are left out after each one drawn in any case.
 *
 * Keys: evdev events from the keypad, as gnuboy key codes; what they do is
 * bound in badge.rc. The keypad is not grabbed: the menu's runner
 * (apprun) reads it too, for the logo key. Esc pauses and the diamond key
 * shows the keys (sdapps/pause, docs/APPS.md): neither reaches
 * gnuboy.
 *
 * Sound: gnuboy mixes its four channels into pcm.buf as it emulates (mono,
 * unsigned 8-bit) and hands that over in pcm_submit, which puts it in a
 * ring; bsound's thread (sdapps/sound) takes a period out at a time and
 * plays it. The game never waits for its sound: its pace is vid_end's.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>

#include "defs.h"
#include "fb.h"
#include "input.h"
#include "pcm.h"
#include "rc.h"
#include "sys.h"
#include "bsound.h"
#include "pause.h"
#include "wbppa.h"

#define FRAME_US	16743		/* a Game Boy frame: 59.7 per second */
#define MAX_UNDRAWN	8		/* frames not drawn in a row, at most */

struct fb fb;

extern char *saveprefix;		/* loader.c's; build.sh opens it up */
static int fbfd = -1, evfd = -1, ppafd = -1;
static int scale = 9, skip = 0, stats;	/* in halves: 9 is 4.5x */
#define RUN		18		/* Game Boy lines in one pwrite */
static unsigned line;			/* bytes per panel line */
static unsigned short frame[160 * 144];
static unsigned short shown[160 * 144];	/* the frame last written */
static int shown_ok;
static unsigned short *rows[2];		/* RUN Game Boy lines of the panel, in turn */
static off_t band_off;			/* where the picture's panel lines start */
static unsigned short *doubled;		/* the frame twice as large, for the PPA */
#define PPA_SCALE	36		/* from there to 4.5x, in sixteenths */
static long long deadline;		/* when this frame should be done, in us */
static int undrawn;			/* frames since the last one drawn */
static void snd_stats(void);

rcvar_t vid_exports[] = {
	RCV_INT("panelscale", &scale, "scale on the panel, in halves: 2 to 9"),
	RCV_INT("panelskip", &skip, "frames not drawn after each one that is"),
	RCV_INT("panelstats", &stats, "every 5 s to stderr: frames, drawn, lines"),
	RCV_END
};

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

void vid_preinit(void)
{
}

void vid_init(void)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	unsigned w, h;

	fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fbfd, FBIOGET_FSCREENINFO, &fi) < 0 || vi.bits_per_pixel != 16)
		die("gnuboy: needs a 16-bit /dev/fb0\n");
	w = vi.xres;
	h = vi.yres;
	line = fi.line_length;
	if (scale < 2)
		scale = 2;
	while (scale > 2 && (80u * scale > w || 72u * scale > h))
		scale--;
	for (int i = 0; i < 2; i++) {
		rows[i] = calloc(1, (size_t)line * RUN * ((scale + 1) / 2));	/* black either side */
		if (!rows[i])
			die("gnuboy: no memory for the lines\n");
	}
	band_off = (off_t)((h - 72 * scale) / 2) * line;
	if (scale == 9 && w == 720) {
		ppafd = open(WB_PPA_DEV, O_RDONLY | O_CLOEXEC);
		doubled = ppafd >= 0 ? malloc(320 * 288 * 2) : 0;
	}
	{
		/* black around it, once */
		char *z = calloc(1, line);

		for (unsigned y = 0; z && y < h; y++)
			pwrite(fbfd, z, line, (off_t)y * line);
		free(z);
	}

	fb.w = 160;
	fb.h = 144;
	fb.pelsize = 2;
	fb.pitch = 160 * 2;
	fb.indexed = 0;
	fb.ptr = (byte *)frame;
	fb.cc[0].r = 3;			/* RGB565: bits lost, shift */
	fb.cc[0].l = 11;
	fb.cc[1].r = 2;
	fb.cc[1].l = 5;
	fb.cc[2].r = 3;
	fb.cc[2].l = 0;
	fb.yuv = 0;
	fb.enabled = 1;
	fb.dirty = 0;
	fb.delegate_scaling = 1;
	{
		char zero[] = "0", *v = zero;

		rc_setvar("framelen", 1, &v);	/* vid_end keeps the time */
	}

	evfd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

void vid_close(void)
{
	if (fbfd >= 0)
		close(fbfd);
	if (evfd >= 0)
		close(evfd);
	fbfd = evfd = -1;
	fb.enabled = 0;
}

void vid_settitle(char *title)
{
	(void)title;
}

void vid_setpal(int i, int r, int g, int b)
{
	(void)i; (void)r; (void)g; (void)b;
}

void vid_begin(void)
{
	fb.ptr = (byte *)frame;
}

/* The panel line a Game Boy line or column starts at: with 4.5x they are 4 and 5 wide. */
static inline int at(int n)
{
	return n * scale / 2;
}

/* One Game Boy line into the buffer, its first panel line at top. */
static void draw_line(unsigned short *top, const unsigned short *src)
{
	unsigned x0 = (line / 2 - 80 * scale) / 2;	/* in pixels */
	unsigned short *d = top + x0;

	for (int x = 0; x < 160; x++)
		for (int k = at(x + 1) - at(x); k > 0; k--)
			*d++ = src[x];
}

/*
 * By the PPA, which interpolates: from pixels repeated twice the picture looks
 * the same as repeated 4.5 times, and the CPU writes a quarter of it. Lines in
 * pairs, for whole panel lines, and a pair more each side: it blends across
 * the edge of what it gets. -1 if the PPA would not.
 */
static int draw_ppa(void)
{
	struct wb_ppa_scale s;
	int first = 0, last = 144;

	while (shown_ok && first < 144 && !memcmp(frame + first * 160, shown + first * 160, 320))
		first++;
	if (first == 144)
		return 0;
	while (shown_ok && !memcmp(frame + (last - 1) * 160, shown + (last - 1) * 160, 320))
		last--;
	first = first < 2 ? 0 : (first - 2) & ~1;
	last = last + 3 > 144 ? 144 : (last + 3) & ~1;
	for (int y = first; y < last; y++) {
		unsigned short *d = doubled + y * 2 * 320;

		for (int x = 0; x < 160; x++)
			d[2 * x] = d[2 * x + 1] = frame[y * 160 + x];
		memcpy(d + 320, d, 640);
	}
	s = (struct wb_ppa_scale){ (uint32_t)(doubled + first * 2 * 320), 320, 320, (last - first) * 2, 0,
				   band_off / line + first * 9 / 2, PPA_SCALE, PPA_SCALE };
	if (ioctl(ppafd, WB_PPA_SCALE, &s) < 0) {
		shown_ok = 0;	/* the one before may be gone as well (wbppa.h) */
		return -1;
	}
	memcpy(shown + first * 160, frame + first * 160, (last - first) * 320);
	shown_ok = 1;
	return last - first;
}

/* the lines that changed since the last frame shown; 0 if none did */
static int draw(void)
{
	static int turn;
	int n = 0;

	if (doubled && (n = draw_ppa()) >= 0)
		return n;
	n = 0;
	for (int y = 0; y < 144; ) {
		unsigned short *buf = rows[turn];
		int first, lines = 0;

		while (y < 144 && shown_ok && !memcmp(frame + y * 160, shown + y * 160, 320))
			y++;
		first = y;
		while (y < 144 && y - first < RUN &&
		       (!shown_ok || memcmp(frame + y * 160, shown + y * 160, 320))) {
			const unsigned short *src = frame + y * 160;
			unsigned short *top = buf + (size_t)lines * (line / 2);
			int tall = at(y + 1) - at(y);

			draw_line(top, src);
			for (int k = 1; k < tall; k++)
				memcpy(top + k * (line / 2), top, line);
			memcpy(shown + y * 160, src, 320);
			lines += tall;
			y++;
			n++;
		}
		if (lines) {
			pwrite(fbfd, buf, (size_t)line * lines, band_off + (off_t)line * at(first));
			turn ^= 1;
		}
	}
	shown_ok = 1;
	return n;
}

void vid_end(void)
{
	static long long t_stats;
	static int n_frames, n_drawn, n_lines, n_late, n_behind, max_behind;
	static long long t_drawing;
	long long now = now_us();

	if (!fb.enabled || fbfd < 0)
		return;
	if (!deadline || now > deadline + 100000) {
		/* the start, after the pause, or the game itself too slow */
		n_late += deadline != 0;
		deadline = now;
	}
	deadline += FRAME_US;
	n_frames++;
	if (now >= deadline) {
		n_behind++;
		if (now - deadline > max_behind)
			max_behind = now - deadline;
	}
	if (++undrawn > skip && (now < deadline || undrawn > MAX_UNDRAWN)) {
		long long t = now_us();
		int n = draw();

		t_drawing += now_us() - t;
		undrawn = 0;
		n_drawn += n > 0;
		n_lines += n;
	}
	if (stats && now - t_stats >= 5000000) {
		if (t_stats)
			fprintf(stderr, "gnuboy: %.1f frames/s, %.1f drawn/s, %d lines a drawing, %.1f ms a drawing, "
				"%d frames late (at most %d ms), %d times 0.1 s behind\n",
				n_frames * 1e6 / (now - t_stats), n_drawn * 1e6 / (now - t_stats),
				n_drawn ? n_lines / n_drawn : 0, n_drawn ? t_drawing / 1000.0 / n_drawn : 0,
				n_behind, max_behind / 1000, n_late);
		if (t_stats)
			snd_stats();
		t_stats = now;
		n_frames = n_drawn = n_lines = n_late = n_behind = max_behind = 0;
		t_drawing = 0;
	}
	now = now_us();
	if (now < deadline) {
		struct timespec t = { deadline / 1000000, deadline % 1000000 * 1000 };

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0);
	}
}

/* evdev code -> gnuboy key code; 0 for keys it does not use. */
static int keycode(int c)
{
	static const char row1[] = "1234567890", row2[] = "qwertyuiop",
			  row3[] = "asdfghjkl", row4[] = "zxcvbnm";

	if (c >= 2 && c <= 11)
		return row1[c - 2];
	if (c >= 16 && c <= 25)
		return row2[c - 16];
	if (c >= 30 && c <= 38)
		return row3[c - 30];
	if (c >= 44 && c <= 50)
		return row4[c - 44];
	if (c >= 59 && c <= 64)		/* the symbol keys, F1..F6 */
		return K_F1 + (c - 59);
	switch (c) {
	case 1: return K_ESC;
	case 14: return K_BS;
	case 15: return K_TAB;
	case 28: return K_ENTER;
	case 29: case 97: return K_CTRL;
	case 42: case 54: return K_SHIFT;
	case 56: case 100: return K_ALT;
	case 57: return K_SPACE;
	case 103: return K_UP;
	case 108: return K_DOWN;
	case 105: return K_LEFT;
	case 106: return K_RIGHT;
	case 110: return K_INS;
	case 111: return K_DEL;
	}
	return 0;
}

struct kev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

/*
 * Esc: the pause screen; the diamond key: the key overview. The game
 * stands still meanwhile (we do not return to its loop). Keys held when it
 * opened are let go, or the game would see them held on. The pause draws
 * over the picture and puts it back: read from the panel into a buffer of
 * its own, only while it is open (without memory for it, the next frame
 * is drawn whole instead).
 */
static void state_name(char *to, size_t n, const char *end)
{
	snprintf(to, n, "%s.000%s", saveprefix, end);	/* slot 0, the pause's */
}

/* A state on file goes only when asked, and stays as .bak. */
static int save_allowed(const struct pause_screen *s)
{
	static const char *const items[] = { "Cancel", "Overwrite the saved state" };
	struct pause_screen ask = *s;
	char state[512], old[520];

	state_name(state, sizeof(state), "");
	if (access(state, F_OK))
		return 1;
	ask.items = items;
	ask.n_items = 2;
	ask.keys_item = -1;
	if (badge_pause(&ask, 0) != 1)
		return 0;
	state_name(old, sizeof(old), ".bak");
	rename(state, old);
	return 1;
}

static void pause_game(int help)
{
	static const int held[] = { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_F1, K_F2, K_F3, K_F4,
				    K_ENTER, K_BS, 'x', 'o' };
	static const struct pause_key keys[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "The D-pad" },
		{ "#1", "A (Enter does too)" },
		{ "#2", "B (Backspace does too)" },
		{ "#3", "Start (X does too)" },
		{ "#4", "Select (O does too)" },
		{ "#6", "These keys" },
		{ "Esc", "Pause: resume, save, load, quit" },
		{ "Logo", "Back to the menu" },
	};
	/* Save and Load apart: one line off, a save went over half an hour of play */
	static const char *const items[] = { "Resume", "Load state", "Keys", "Save state", "Quit" };
	size_t size = (size_t)line * 72 * scale;
	unsigned short *game = malloc(size);
	struct pause_screen s = {
		.fbfd = fbfd, .evfd = evfd, .line = line, .game = game, .game_lines = 72 * scale,
		.band_off = band_off, .app = "GAME BOY", .name = "Game Boy",
		.about = "The game is paused. On the badge its buttons are these keys.",
		.keys = keys, .n_keys = sizeof(keys) / sizeof(keys[0]),
		.items = items, .n_items = 5, .keys_item = 2,
	};
	char save[] = "savestate", load[] = "loadstate", quit[] = "quit";	/* rc_command edits */
	int flags = fcntl(evfd, F_GETFL);

	if (game && pread(fbfd, game, size, band_off) != (ssize_t)size) {
		free(game);
		s.game = game = 0;
	}

	for (unsigned i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
		event_t ev = { .type = EV_RELEASE, .code = held[i] };

		ev_postevent(&ev);
	}
	fcntl(evfd, F_SETFL, flags & ~O_NONBLOCK);
	switch (badge_pause(&s, help)) {
	case 1:
		rc_command(load);
		break;
	case 3:
		if (save_allowed(&s))
			rc_command(save);
		break;
	case 4:
		rc_command(quit);
		break;
	}
	fcntl(evfd, F_SETFL, flags);
	if (!game)
		shown_ok = 0;
	free(game);
}

void ev_poll(int wait)
{
	struct pollfd p = { .fd = evfd, .events = POLLIN };
	struct kev e;

	if (evfd < 0)
		return;
	if (wait)
		poll(&p, 1, -1);
	while (read(evfd, &e, sizeof(e)) == (int)sizeof(e)) {
		event_t ev = { 0 };

		if (e.type == 1 && e.value == 1 && (e.code == 1 || e.code == 64)) {
			pause_game(e.code == 64);	/* Esc, diamond */
			continue;
		}
		if (e.type != 1 || e.value == 2 || !(ev.code = keycode(e.code)))
			continue;
		ev.type = e.value ? EV_PRESS : EV_RELEASE;
		ev_postevent(&ev);
	}
}

/*
 * Sound. The ring holds what gnuboy made and bsound has not played yet,
 * between START_MS and MAX_MS of it, plus bsound's own queue (bsound.h).
 * It runs dry when the game stands still or falls behind: silence, until
 * it holds START_MS again. gnuboy makes a little more than the rate (its
 * step is a whole number of clock ticks: 22075 a second for 22050), so
 * above HIGH_MS a period gets one frame fewer, and above MAX_MS (no
 * output, or the game catching up) the oldest go, down to HIGH_MS.
 *
 * The Game Boy's output passes a capacitor, which lets no DC through: a
 * channel's square wave is 0 or its volume, and the speaker should not
 * get that offset. A high pass about as strong (each sample keeps 127/128
 * of the last one's offset: about 27 Hz at 22050 Hz) takes it out.
 */
#define SND_RING	8192		/* frames, a power of two; MAX_MS at 48 kHz fits */
#define SND_LEN		512		/* bytes of pcm.buf: the most one pcm_submit brings */
#define START_MS	60
#define HIGH_MS		100
#define MAX_MS		150

struct pcm pcm;

static int sound = 1, samplerate = 22050;
static byte snd_buf[SND_LEN];
static int16_t ring[SND_RING];
static unsigned ring_r, ring_w;		/* frames taken and put, ever: what is in it is ring_w - ring_r */
static unsigned snd_start, snd_high, snd_max;	/* START_MS, HIGH_MS, MAX_MS in frames */
static int snd_on, snd_flowing;		/* bsound plays; the ring has held snd_start since it ran dry */
static volatile int snd_pushing;	/* in pcm_submit, perhaps with bsound's lock */
static int hp_x, hp_y;			/* the high pass: last sample in, last out in 1/256 */
static unsigned n_dry, n_skipped, n_dropped;	/* for panelstats */

rcvar_t pcm_exports[] = {
	RCV_BOOL("sound", &sound, "sound on the badge's output"),
	RCV_INT("samplerate", &samplerate, "8000, 11025, 16000, 22050, 32000, 44100 or 48000"),
	RCV_END
};

/* bsound's thread, with its lock held */
static void snd_fill(void *user, int16_t *out, unsigned int n)
{
	unsigned level = ring_w - ring_r, k;

	(void)user;
	if (!snd_flowing && level >= snd_start)
		snd_flowing = 1;
	if (!snd_flowing) {
		memset(out, 0, n * sizeof(*out));
		return;
	}
	if (level > snd_high) {
		ring_r++;		/* one frame fewer: the latency does not creep up */
		level--;
		n_skipped++;
	}
	k = level < n ? level : n;
	for (unsigned i = 0; i < k; i++)
		out[i] = ring[(ring_r + i) & (SND_RING - 1)];
	ring_r += k;
	if (k < n) {
		memset(out + k, 0, (n - k) * sizeof(*out));
		snd_flowing = 0;
		n_dry++;
	}
}

void pcm_init(void)
{
	memset(&pcm, 0, sizeof(pcm));	/* pcm.hz 0: gnuboy mixes nothing */
	if (!sound)
		return;
	if (samplerate != 8000 && samplerate != 11025 && samplerate != 16000 && samplerate != 22050 &&
	    samplerate != 32000 && samplerate != 44100 && samplerate != 48000)
		samplerate = 22050;		/* the rates the speaker takes */
	snd_start = samplerate * START_MS / 1000;
	snd_high = samplerate * HIGH_MS / 1000;
	snd_max = samplerate * MAX_MS / 1000;
	ring_r = ring_w = 0;
	snd_flowing = 0;
	if (bsound_start(samplerate, 1, snd_fill, 0)) {
		fprintf(stderr, "gnuboy: no sound\n");
		return;
	}
	snd_on = 1;
	pcm.hz = samplerate;
	pcm.stereo = 0;			/* the speaker is mono: gnuboy mixes left and right */
	pcm.buf = snd_buf;
	pcm.len = SND_LEN;
}

void pcm_close(void)
{
	/* a signal in pcm_submit (the logo key's SIGTERM): the thread may wait for the lock; exit ends it */
	if (snd_on && !snd_pushing)
		bsound_stop();
	snd_on = 0;
	memset(&pcm, 0, sizeof(pcm));
}

int pcm_submit(void)
{
	int16_t v[SND_LEN];
	unsigned n = pcm.pos;

	pcm.pos = 0;
	if (!snd_on || !sound)
		return 0;
	for (unsigned i = 0; i < n; i++) {
		int x = (pcm.buf[i] - 128) * 256, y;

		hp_y += (x - hp_x) * 256 - (hp_y >> 7);
		hp_x = x;
		y = hp_y >> 8;
		v[i] = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
	}
	snd_pushing = 1;
	bsound_lock();
	if (ring_w - ring_r + n > snd_max) {
		unsigned r = ring_w + n - snd_high;

		n_dropped += r - ring_r;
		ring_r = r;
	}
	for (unsigned i = 0; i < n; i++)
		ring[ring_w++ & (SND_RING - 1)] = v[i];
	bsound_unlock();
	snd_pushing = 0;
	return 1;			/* the time is vid_end's in any case (framelen 0) */
}

void pcm_pause(int on)
{
	bsound_pause(on);
}

static void snd_stats(void)
{
	if (!snd_on)
		return;
	fprintf(stderr, "gnuboy: sound %u ms queued, ran dry %u times, %u frames skipped, %u ms dropped\n",
		(ring_w - ring_r) * 1000 / samplerate, n_dry, n_skipped, n_dropped * 1000 / samplerate);
	n_dry = n_skipped = n_dropped = 0;
}
