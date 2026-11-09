// SPDX-License-Identifier: MIT
/*
 * bsound: a game's sound on the badge (bsound.h). One thread, tinyalsa,
 * the device an app plays on (/dev/snd/pcmC0D0p): the sandbox sends it to
 * the speaker or to the loopback, whichever /run/sound.output names when it
 * is opened. So the thread watches /run/sound.state, which apps may read,
 * and opens the device again when the output changes.
 *
 * BSOUND_FILE=path in the environment writes the samples there instead (raw,
 * 16-bit little-endian, two channels), in real time: for tests without a
 * sound card, as in the simulation.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <tinyalsa/pcm.h>

#include "bsound.h"

#define PERIODS		4
#define PERIOD_MAX	1008		/* frames: the speaker's driver takes 4032 bytes a period */
#define LEAD_MS		50		/* silence first: AirPlay's start may cut the first ms */
#define STATE		"/run/sound.state"
#define STACK		(32 * 1024)

static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int running, paused;
static unsigned int rate, channels, period;
static bsound_fill fill;
static void *user;

/* "output=... name=...": where sound goes now; "" when sysui says nothing (the speaker) */
static void output_now(char *out, size_t n)
{
	char line[128];
	FILE *f = fopen(STATE, "r");
	size_t o = 0;

	out[0] = 0;
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "output=", 7) || !strncmp(line, "name=", 5)) {
			size_t k = strcspn(line, "\n");

			if (o + k + 2 < n) {
				memcpy(out + o, line, k);
				o += k;
				out[o++] = ' ';
				out[o] = 0;
			}
		}
	fclose(f);
}

static struct pcm *open_device(void)
{
	struct pcm_config c = {
		.channels = 2, .rate = rate, .format = PCM_FORMAT_S16_LE,
		.period_size = period, .period_count = PERIODS,
		.start_threshold = period, .stop_threshold = period * PERIODS,
	};
	struct pcm *p = pcm_open(0, 0, PCM_OUT, &c);
	static int16_t silence[PERIOD_MAX * 2];

	if (!pcm_is_ready(p)) {
		pcm_close(p);
		return NULL;
	}
	for (unsigned int n = 0; n < rate * LEAD_MS / 1000; n += period)
		if (pcm_writei(p, silence, period) < 0)
			break;
	return p;
}

static void sleep_period(void)
{
	struct timespec t = { 0, (long)period * 1000000000L / rate };

	nanosleep(&t, NULL);
}

static void next(int16_t *buf)
{
	pthread_mutex_lock(&lock);
	if (paused) {
		memset(buf, 0, period * 4);
	} else {
		fill(user, buf, period);
		if (channels == 1)	/* the speaker takes two channels */
			for (int i = period - 1; i >= 0; i--)
				buf[2 * i] = buf[2 * i + 1] = buf[i];
	}
	pthread_mutex_unlock(&lock);
}

/* BSOUND_FILE: the samples into a file, a period every period of the clock */
static void to_file(FILE *f)
{
	static int16_t buf[PERIOD_MAX * 2];
	struct timespec due;

	clock_gettime(CLOCK_MONOTONIC, &due);
	while (running) {
		next(buf);
		fwrite(buf, 4, period, f);
		fflush(f);
		due.tv_nsec += (long)period * 1000000000L / rate;
		if (due.tv_nsec >= 1000000000L) {
			due.tv_sec++;
			due.tv_nsec -= 1000000000L;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
	}
	fclose(f);
}

static void *player(void *arg)
{
	static int16_t buf[PERIOD_MAX * 2];
	struct pcm *p = NULL;
	char was[160] = "?", now[160];
	time_t seen = 0;
	unsigned int n = 0, every = rate / period / 2 + 1;	/* the output is looked at twice a second */

	(void)arg;
	if (getenv("BSOUND_FILE")) {
		FILE *f = fopen(getenv("BSOUND_FILE"), "wb");

		if (f)
			to_file(f);
		return NULL;
	}
	while (running) {
		if (n++ % every == 0) {
			struct stat st;

			if (stat(STATE, &st))
				st.st_mtime = 0;
			if (st.st_mtime != seen || !strcmp(was, "?")) {
				seen = st.st_mtime;
				output_now(now, sizeof(now));
				if (strcmp(now, was)) {
					if (p)
						pcm_close(p);
					p = NULL;
					snprintf(was, sizeof(was), "%s", now);
					/* no output: nothing to open, and nothing played */
					if (strncmp(now, "output=none", 11))
						p = open_device();
				}
			}
		}
		if (!p) {
			sleep_period();
			continue;
		}
		next(buf);
		if (pcm_writei(p, buf, period) < 0) {
			/* the device went away: try again at the next look at the output */
			pcm_close(p);
			p = NULL;
			strcpy(was, "?");
		}
	}
	if (p)
		pcm_close(p);
	return NULL;
}

int bsound_start(unsigned int r, unsigned int ch, bsound_fill f, void *u)
{
	pthread_attr_t a;
	int e;

	if (running || r < 8000 || r > 48000 || (ch != 1 && ch != 2) || !f)
		return -1;
	rate = r;
	channels = ch;
	fill = f;
	user = u;
	/* about 10 ms, in whole 64-byte lines of the driver's buffer: the
	 * PERIODS of them, 40 ms, are what an app's sound waits before it goes
	 * out (to AirPlay every ms of delay counts) */
	period = (rate / 100 + 8) & ~15u;
	if (period > PERIOD_MAX)
		period = PERIOD_MAX & ~15u;
	running = 1;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, STACK);
	e = pthread_create(&thread, &a, player, NULL);
	pthread_attr_destroy(&a);
	if (e) {
		running = 0;
		return -1;
	}
	return 0;
}

unsigned int bsound_period(void)
{
	return period;
}

void bsound_lock(void)
{
	pthread_mutex_lock(&lock);
}

void bsound_unlock(void)
{
	pthread_mutex_unlock(&lock);
}

void bsound_pause(int on)
{
	paused = on;
}

void bsound_stop(void)
{
	if (!running)
		return;
	running = 0;
	pthread_join(thread, NULL);
}
