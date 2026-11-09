// SPDX-License-Identifier: MIT
/*
 * sound_state.h — where sound plays and how loud, as sysui says it in
 * /run/sound.state (docs/SOUND.md): one "key=value" per line. For the status
 * bar of home and the apps, and for the menu's Settings > Sound.
 *
 *   output=none|speaker|airplay   name=<the output's name>   level=5..100
 *   muted=0|1   fitted=0|1   airplay=0|1
 *   link=idle|connecting|ready|connected|needs_code|unreachable|refused
 *   code=|wait|checking|wrong   refused=|owner|password|other   (AirPlay only)
 *
 * connecting: sysui asks a speaker just chosen whether it takes the badge
 * (up to 10 s); ready: it does, and sound goes there once an app plays
 * (connected). needs_code: it shows a code to type ("code NAME DIGITS" on
 * /run/sound.cmd); code says wait (a code is showing), checking, or wrong
 * (it shows a new one). refused and unreachable give way to the output
 * before at once, so the menu sees them only for a moment; refused= keeps
 * why the last speaker said no: owner (only its owner's Apple devices),
 * password, other.
 */
#ifndef SOUND_STATE_H
#define SOUND_STATE_H

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SOUND_STATE_FILE	"/run/sound.state"

struct sound_state {
	char output[8];		/* "" when sysui does not run: no icon */
	char name[64];
	char link[16];
	char code[12];		/* link needs_code: wait, checking, wrong */
	char refused[12];	/* why the last speaker said no */
	int level, muted, fitted, airplay;
};

/* The value of key in "key=value" lines, copied into v; 0 when missing. */
static int sound_value(const char *b, const char *key, char *v, int n)
{
	int k = strlen(key);

	for (const char *l = b; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
		int m = 0;

		if (strncmp(l, key, k) || l[k] != '=')
			continue;
		l += k + 1;
		while (l[m] && l[m] != '\n' && m < n - 1)
			m++;
		memcpy(v, l, m);
		v[m] = 0;
		return 1;
	}
	return 0;
}

static void sound_state_read(struct sound_state *s)
{
	char b[512], v[16];
	int fd = open(SOUND_STATE_FILE, O_RDONLY | O_CLOEXEC), n = -1;

	memset(s, 0, sizeof(*s));
	if (fd >= 0) {
		n = read(fd, b, sizeof(b) - 1);
		close(fd);
	}
	if (n <= 0)
		return;
	b[n] = 0;
	sound_value(b, "output", s->output, sizeof(s->output));
	sound_value(b, "name", s->name, sizeof(s->name));
	sound_value(b, "link", s->link, sizeof(s->link));
	sound_value(b, "code", s->code, sizeof(s->code));
	sound_value(b, "refused", s->refused, sizeof(s->refused));
	s->level = sound_value(b, "level", v, sizeof(v)) ? atoi(v) : 0;
	s->muted = sound_value(b, "muted", v, sizeof(v)) && v[0] == '1';
	s->fitted = sound_value(b, "fitted", v, sizeof(v)) && v[0] == '1';
	s->airplay = sound_value(b, "airplay", v, sizeof(v)) && v[0] == '1';
}

/* AirPlay speakers are outputs only with the AirPlay app (sdapps/airplay), whose
 * raopsend sends to them: without it Settings and the sound panel leave them out. */
static int sound_airplay_app(void)
{
	return !access("/usr/bin/raopsend", X_OK) || !access("/mnt/sd/badge/apps/airplay/raopsend", X_OK);
}

/* Silent although an output is chosen: muted, or the AirPlay speaker out of reach,
 * refusing, or waiting for its code. */
static int sound_silent(const struct sound_state *s)
{
	return s->muted || (!strcmp(s->output, "airplay") &&
			     (!strcmp(s->link, "unreachable") || !strcmp(s->link, "refused") ||
			      !strcmp(s->link, "needs_code")));
}

#endif
