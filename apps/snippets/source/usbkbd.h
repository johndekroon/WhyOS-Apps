// SPDX-License-Identifier: MIT
/*
 * usbkbd.h — the badge's keys as a USB boot keyboard (docs/USB.md, "Device
 * mode"): Linux key codes from the keypad in, 8-byte HID reports out
 * (modifiers, reserved, six keys), for /dev/hidg0.
 *
 * The badge's own layer: Fn+Backspace Delete, Fn+arrows Page Up/Down,
 * Home and End, Fn+symbol keys F7-F12 (the symbol keys alone are F1-F6).
 * The logo key is the computer's Cmd/Windows key only together with
 * another key; pressed and released on its own it ends keyboard mode
 * (kb_event() returns KB_STOP). Auto-repeat is the computer's job.
 *
 * Header-only and without the badge's headers, so test/test_usbkbd.c
 * runs it on the host.
 */
#ifndef USBKBD_H
#define USBKBD_H

#include <string.h>

#define KB_FN		0x1d0	/* the Fn key (the badge's keymap) */
#define KB_LOGO		125	/* KEY_LEFTMETA */
#define KB_STOP		(-1)

struct kb {
	unsigned char mods;
	unsigned char keys[6];	/* usages held, in press order */
	unsigned char held[0x200];	/* usage per Linux code still down, so a key
					 * released after Fn lets go of the right one */
	int fn, logo, logo_used;
};

/* Linux key code to HID usage (keyboard page), 0 if none; mods via kb_mod(). */
static inline int kb_usage(int code, int fn)
{
	static const unsigned char u[] = {
		[1] = 0x29, [2] = 0x1e, [3] = 0x1f, [4] = 0x20, [5] = 0x21, [6] = 0x22,
		[7] = 0x23, [8] = 0x24, [9] = 0x25, [10] = 0x26, [11] = 0x27,
		[12] = 0x2d, [13] = 0x2e, [14] = 0x2a, [15] = 0x2b,
		[16] = 0x14, [17] = 0x1a, [18] = 0x08, [19] = 0x15, [20] = 0x17,
		[21] = 0x1c, [22] = 0x18, [23] = 0x0c, [24] = 0x12, [25] = 0x13,
		[26] = 0x2f, [27] = 0x30, [28] = 0x28,
		[30] = 0x04, [31] = 0x16, [32] = 0x07, [33] = 0x09, [34] = 0x0a,
		[35] = 0x0b, [36] = 0x0d, [37] = 0x0e, [38] = 0x0f, [39] = 0x33,
		[40] = 0x34, [41] = 0x35, [43] = 0x31,
		[44] = 0x1d, [45] = 0x1b, [46] = 0x06, [47] = 0x19, [48] = 0x05,
		[49] = 0x11, [50] = 0x10, [51] = 0x36, [52] = 0x37, [53] = 0x38,
		[57] = 0x2c,
		[59] = 0x3a, [60] = 0x3b, [61] = 0x3c, [62] = 0x3d, [63] = 0x3e,
		[64] = 0x3f, [65] = 0x40, [66] = 0x41, [67] = 0x42, [68] = 0x43,
		[87] = 0x44, [88] = 0x45,
		[102] = 0x4a, [103] = 0x52, [104] = 0x4b, [105] = 0x50, [106] = 0x4f,
		[107] = 0x4d, [108] = 0x51, [109] = 0x4e, [111] = 0x4c,
	};

	if (fn) {
		switch (code) {
		case 14: return 0x4c;			/* Backspace: Delete */
		case 103: return 0x4b;			/* Up: Page Up */
		case 108: return 0x4e;			/* Down: Page Down */
		case 105: return 0x4a;			/* Left: Home */
		case 106: return 0x4d;			/* Right: End */
		}
		if (code >= 59 && code <= 64)		/* symbol keys: F7-F12 */
			return 0x40 + code - 59;
	}
	return code > 0 && code < (int)sizeof(u) ? u[code] : 0;
}

static inline int kb_mod(int code)
{
	switch (code) {
	case 29: return 0x01;		/* left Ctrl */
	case 42: return 0x02;		/* left Shift */
	case 56: return 0x04;		/* left Alt */
	case 54: return 0x20;		/* right Shift */
	case 100: return 0x40;		/* right Alt */
	}
	return 0;
}

static inline void kb_report(const struct kb *k, unsigned char rep[8])
{
	memset(rep, 0, 8);
	rep[0] = k->mods | (k->logo && k->logo_used ? 0x08 : 0);
	memcpy(rep + 2, k->keys, 6);
}

/*
 * One key event (value 1 down, 0 up, 2 repeat). 1: rep holds a new
 * report to send; 0: nothing to send; KB_STOP: the logo key alone.
 */
static inline int kb_event(struct kb *k, int code, int value, unsigned char rep[8])
{
	int m, u, i;

	if (value == 2 || code < 0 || code >= (int)sizeof(k->held))
		return 0;
	if (code == KB_FN) {
		k->fn = value;
		return 0;
	}
	if (code == KB_LOGO) {
		if (value) {
			k->logo = 1;
			k->logo_used = 0;
			return 0;
		}
		if (!k->logo_used) {
			k->logo = 0;
			return KB_STOP;
		}
		k->logo = 0;
		kb_report(k, rep);
		return 1;
	}
	if ((m = kb_mod(code))) {
		if (value)
			k->mods |= (unsigned char)m;
		else
			k->mods &= (unsigned char)~m;
		kb_report(k, rep);
		return 1;
	}
	if (value) {
		if (!(u = kb_usage(code, k->fn)) || k->held[code])
			return 0;
		k->logo_used |= k->logo;
		k->held[code] = (unsigned char)u;
		for (i = 0; i < 6 && k->keys[i]; i++)
			;
		if (i == 6) {		/* a seventh key: drop the oldest */
			memmove(k->keys, k->keys + 1, 5);
			i = 5;
		}
		k->keys[i] = (unsigned char)u;
	} else {
		if (!(u = k->held[code]))
			return 0;
		k->held[code] = 0;
		for (i = 0; i < 6 && k->keys[i] != u; i++)
			;
		if (i == 6)
			return 0;
		memmove(k->keys + i, k->keys + i + 1, (size_t)(5 - i));
		k->keys[5] = 0;
	}
	kb_report(k, rep);
	return 1;
}

/*
 * The mouse next to it (/dev/hidg1): 4-byte reports of buttons (bit 0
 * left, 1 right, 2 middle), X, Y and the wheel, each -127..127. The boot
 * mouse's report (HID 1.11, appendix B.2) with a wheel after it: a
 * computer in boot protocol reads the first three bytes.
 */
#define MS_LEFT		0x01
#define MS_RIGHT	0x02
#define MS_MIDDLE	0x04

static const unsigned char ms_report_desc[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
	0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
	0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
	0x81, 0x06, 0xc0, 0xc0,
};

/* The boot keyboard's report descriptor (HID 1.11, appendix B.1). */
static const unsigned char kb_report_desc[] = {
	0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01,
	0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01,
	0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x06,
	0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
	0x81, 0x00, 0xc0,
};

#endif
