/* SPDX-License-Identifier: MIT */
/*
 * Files: drawing the app (ui.c) with libwb (WhyOS libwb: wb.h),
 * in bands of BAND_H rows: no 1 MB frame buffer next to the
 * program, as the store, Write and appterm. ui_present draws every band and
 * hands it to out(); a frame with a pop-up is drawn whole while there is
 * room for it (the pop-up's frost blurs the picture under it).
 */
#ifndef FILES_UI_H
#define FILES_UI_H

#include <stdint.h>

#include "app.h"

#define BAND_H	40

/* has the badge turned since the last frame? (the scene turns with it) */
int ui_turned(void);
/* something else was on the panel (SIGCONT): the next frame is drawn whole */
void ui_again(void);
/* the whole screen, band by band */
void ui_present(const struct files *f, void (*out)(const uint16_t *band, int y0, int y1));
/* a full-screen one-liner while a copy, move or delete runs */
void ui_message(const char *eyebrow, const char *msg, void (*out)(const uint16_t *band, int y0, int y1));

#endif
