// SPDX-License-Identifier: MIT
/*
 * keyrep.h — what a held key does in our own screens.
 *
 * The keypad repeats a held key (keypad,autorepeat in kernel patch 0060;
 * sleepd sets the rate: the first repeat after 400 ms, then every 50 ms),
 * as an evdev event with value 2. USB keyboards repeat the same way. In
 * our screens only the keys that take one small step repeat: the arrows,
 * Page Up/Down, Backspace, Delete, Space and the typing keys. Enter, Esc,
 * Tab, the symbol keys and the modifiers act once, so a long press does
 * not run on into the next screen or close the app. appterm passes every
 * repeat on, as a computer's terminal does.
 *
 * A screen that draws slower than the repeats come drops the repeats that
 * waited too long (key_stale()), so nothing runs on after the release.
 * Not "while more input waits": every key event has a SYN after it, so a
 * program that reads one event at a time always finds more.
 *
 * Header-only and without the badge's headers, so test/test_keyrep.c runs
 * it on the host. Codes are Linux's (<linux/input.h>).
 */
#ifndef KEYREP_H
#define KEYREP_H

#include <time.h>

#define KEY_REP_DELAY	400	/* ms from the press to the first repeat */
#define KEY_REP_PERIOD	50	/* ms between repeats */

/* Does a held key repeat? */
static inline int key_repeats(int code)
{
	return (code >= 2 && code <= 14) ||	/* 1..0 - = Backspace */
	       (code >= 16 && code <= 27) ||	/* q..p [ ] */
	       (code >= 30 && code <= 41) ||	/* a..l ; ' ` */
	       (code >= 43 && code <= 53) ||	/* \ z..m , . / */
	       code == 57 ||			/* Space */
	       code == 103 || code == 104 ||	/* Up, Page Up */
	       code == 105 || code == 106 ||	/* Left, Right */
	       code == 108 || code == 109 ||	/* Down, Page Down */
	       code == 111;			/* Delete */
}

/* Does this key event (value 1 press, 2 repeat, 0 release) act? */
static inline int key_acts(int code, int value)
{
	return value == 1 || (value == 2 && key_repeats(code));
}

/* Did this repeat wait longer than two periods (sec, usec: its evdev
 * time, CLOCK_REALTIME)? Then the screen fell behind: skip it. A clock
 * that was set back meanwhile only lets one through. */
static inline int key_stale(unsigned int sec, unsigned int usec)
{
	struct timespec now;
	long long age;

	clock_gettime(CLOCK_REALTIME, &now);
	age = ((long long)now.tv_sec - (long long)sec) * 1000 + (now.tv_nsec / 1000000 - (long long)usec / 1000);
	return age > 2 * KEY_REP_PERIOD;
}

#endif
