/* SPDX-License-Identifier: MIT */
/*
 * bp.h — a Bus Pirate 6 on the bottom port, as OHMinous uses it (bp.c):
 * its terminal (USB interface 0), typed to as a user would, and its SUMP
 * port (interface 2) for the logic analyzer. Taken over from the menu's
 * Bus Pirate screens (overlay-src/home.c, BP_APP) on 2026-10-25.
 */
#pragma once

#include <stdint.h>

#define BP_PINS		8
#define BP_LA_MAX	32768

enum { LT_WS2812, LT_APA102, LT_ONBOARD, N_LTYPES };

struct bp {
	int present;			/* on USB (bp_find) */
	int open;			/* its terminal is open */
	char mode[16];			/* "DIO", "LED", "I2C" */
	int psu, pullups;
	int dv, ma;			/* the supply's setting: tenths of a volt, mA (0: no limit) */
	char vout[12], iout[16];	/* from `v` */
	char pin_f[BP_PINS][12], pin_v[BP_PINS][8];
	char board[48], fw[48];
	char info[24][80];
	int n_info;
	int sump;			/* its binmode is SUMP */
	int led_type, led_set, led_n, led_pct;
	uint32_t led_rgb;
	char notice[120];
	int notice_bad;
	char rx[4096];			/* its last reply, without ANSI and CRs */
};

extern struct bp B;
extern const char *const bp_led_types[N_LTYPES];

int bp_find(void);			/* 1 if one is on USB */
int bp_open(void (*connecting)(void));	/* 0, or -1 with B.notice */
void bp_close(void);
int bp_cmd(const char *cmd);		/* typed, its reply in B.rx */
int bp_mode(const char *name);		/* "m dio" and its questions' defaults */
void bp_measure(void);
void bp_psu(int on);
void bp_pullups(int on);
void bp_pin(int io, int how);		/* DIO: 0 low, 1 high, -1 input */
int bp_i2c_scan(uint8_t found[128]);
void bp_leds(int off);
int bp_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int (*cancel)(void));
int bp_fd(void);			/* its terminal, for a console */
int bp_dv_of(const char *v);		/* "4.6V" -> 46 */
