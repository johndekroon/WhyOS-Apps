/* SPDX-License-Identifier: MIT */
/*
 * Files: drawing the app (ui.c) with the menu's drawing code
 * (why2025-linux wbdraw.h, wbstatus.h, home_assets.h), in bands of BAND_H
 * rows: no 1 MB frame buffer next to the program, as the store, Write and
 * appterm. ui_present draws every band and hands it to out().
 */
#ifndef FILES_UI_H
#define FILES_UI_H

#include <stdint.h>

#include "app.h"

#define BAND_H	40

struct ui_status {
	char hhmm[6];
	int clock_ok, bars, lora;
};

extern struct ui_status ui_st;

int ui_wifi_bars(void);		/* wbstatus.h: -1 not connected, else the lit arcs */
int ui_lora_on(void);
/* the whole screen, band by band */
void ui_present(const struct files *f, void (*out)(const uint16_t *band, int y0, int y1));
/* a full-screen one-liner while a copy, move or delete runs */
void ui_message(const char *eyebrow, const char *msg, void (*out)(const uint16_t *band, int y0, int y1));

#endif
