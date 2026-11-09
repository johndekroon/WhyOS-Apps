/* SPDX-License-Identifier: MIT */
/*
 * whyos/keys.h: the badge's keys by what they do, and the characters they
 * type. Part of the WhyOS SDK: include whyos.h.
 *
 * Codes are Linux's (linux/input-event-codes.h): the keypad reports them
 * through evdev, as a USB keyboard does. The six symbol keys arrive as F1
 * to F6. The SDK reads the keys itself (app.h); an app gets them as
 * actions, or in a canvas's key function.
 */
#pragma once

#include "keyrep.h"	/* which held keys repeat (libwb) */

/* the symbol keys, by their role in every app (the design system, 6.6) */
#define WOS_KEY_OPTIONS		59	/* square: the context menu */
#define WOS_KEY_MAIN		60	/* triangle: the screen's main action */
#define WOS_KEY_SECOND		61	/* cross: its second action */
#define WOS_KEY_LOG		62	/* circle: the log (the SDK's) */
#define WOS_KEY_SETTINGS	63	/* clover: the settings (the SDK's) */
#define WOS_KEY_HELP		64	/* diamond: the help (the SDK's) */

#define WOS_KEY_ESC		1
#define WOS_KEY_BACKSPACE	14
#define WOS_KEY_TAB		15
#define WOS_KEY_ENTER		28
#define WOS_KEY_SPACE		57
#define WOS_KEY_HOME		102
#define WOS_KEY_UP		103
#define WOS_KEY_PAGEUP		104
#define WOS_KEY_LEFT		105
#define WOS_KEY_RIGHT		106
#define WOS_KEY_END		107
#define WOS_KEY_DOWN		108
#define WOS_KEY_PAGEDOWN	109
#define WOS_KEY_DELETE		111

/* modifiers: held, not keys of their own */
#define WOS_KEY_LEFTCTRL	29
#define WOS_KEY_LEFTSHIFT	42
#define WOS_KEY_RIGHTSHIFT	54
#define WOS_KEY_LEFTALT		56
#define WOS_KEY_FN		0x1d0

/*
 * Not keys but groups, for an action (app.h) that steps: its function
 * gets -1 for left or up and +1 for right or down.
 */
#define WOS_KEY_ARROWS		-1	/* all four arrows */
#define WOS_KEY_LEFTRIGHT	-2
#define WOS_KEY_UPDOWN		-3

/* A character as an action's key: WOS_CHAR('r') acts when r is typed
 * (with Shift, 'R' is another character). */
#define WOS_CHAR(c)		(0x10000 | (c))

/* modifiers held, as bits */
enum { WOS_SHIFT = 1, WOS_CTRL = 2, WOS_ALT = 4, WOS_FN = 8 };

/* The character a key types in the US layout, as the keypad is labelled;
 * 0 for a key that types none. */
static int wos_key_char(int code, int mods)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (mods & (WOS_CTRL | WOS_ALT | WOS_FN))
		return 0;
	if (code == WOS_KEY_SPACE)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo) - 1)
		return 0;
	return (mods & WOS_SHIFT) ? hi[code] : lo[code];
}

/* -1, 0 or +1: how far an arrow steps for an action of the group g */
static int wos_key_step(int g, int code)
{
	int lr = code == WOS_KEY_LEFT ? -1 : code == WOS_KEY_RIGHT ? 1 : 0;
	int ud = code == WOS_KEY_UP ? -1 : code == WOS_KEY_DOWN ? 1 : 0;

	return g == WOS_KEY_ARROWS ? lr + ud : g == WOS_KEY_LEFTRIGHT ? lr : g == WOS_KEY_UPDOWN ? ud : 0;
}

/* A key's name on its keycap in the key bar and the help: "#1".."#6" are
 * the symbol keys, drawn in their colours (libwb's keycap()). */
static const char *wos_key_cap(int code)
{
	static const char *const syms[] = { "#1", "#2", "#3", "#4", "#5", "#6" };
	static char chars[128][2];
	int ch = code > 0 && (code & 0x10000) ? code & 0x7f : code > 0 ? wos_key_char(code, WOS_SHIFT) : 0;

	if (code > 0 && (code & 0x10000)) {
		chars[ch][0] = (char)ch;
		return chars[ch];
	}
	if (code >= WOS_KEY_OPTIONS && code <= WOS_KEY_HELP)
		return syms[code - WOS_KEY_OPTIONS];
	switch (code) {
	case WOS_KEY_ESC: return "Esc";
	case WOS_KEY_ENTER: return "Enter";
	case WOS_KEY_TAB: return "Tab";
	case WOS_KEY_BACKSPACE: return "Backspace";
	case WOS_KEY_DELETE: return "Delete";
	case WOS_KEY_SPACE: return "Space";
	case WOS_KEY_HOME: return "Home";
	case WOS_KEY_END: return "End";
	case WOS_KEY_PAGEUP: return "PgUp";
	case WOS_KEY_PAGEDOWN: return "PgDn";
	case WOS_KEY_UP: return "\xe2\x86\x91";
	case WOS_KEY_DOWN: return "\xe2\x86\x93";
	case WOS_KEY_LEFT: return "\xe2\x86\x90";
	case WOS_KEY_RIGHT: return "\xe2\x86\x92";
	case WOS_KEY_ARROWS: return "\xe2\x86\x91\xe2\x86\x93\xe2\x86\x90\xe2\x86\x92";
	case WOS_KEY_LEFTRIGHT: return "\xe2\x86\x90\xe2\x86\x92";
	case WOS_KEY_UPDOWN: return "\xe2\x86\x91\xe2\x86\x93";
	}
	chars[ch][0] = (char)ch;
	return ch ? chars[ch] : "?";
}
