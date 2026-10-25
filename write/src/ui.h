/* SPDX-License-Identifier: MIT */
/*
 * Write: drawing the app (ui.c). The screen is drawn in bands of BAND_H rows
 * (no 1 MB frame buffer next to the program, as the store and appterm);
 * ui_present hands each band to out() for the panel or a host screenshot.
 * A band whose contents did not change since the last frame is neither
 * drawn nor sent (ui.c, "Bands").
 */
#ifndef WRITE_UI_H
#define WRITE_UI_H

#include <stdint.h>

#include "app.h"

#define BAND_H	40

struct ui_status {
	char hhmm[6];
	int clock_ok, bars, lora;
	int blink;		/* the caret shows */
};

extern struct ui_status ui_st;

void ui_init(void);
int ui_wifi_bars(void);	/* wbstatus.h, asked at most every 5 s */
int ui_lora_on(void);
/* draw rows y0..y1 (all when force), calling out for every band that changed */
int ui_present(struct app *a, int force, void (*out)(const uint16_t *band, int y0, int y1));

#endif
