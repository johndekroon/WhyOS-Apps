/* SPDX-License-Identifier: MIT */
/*
 * libwb: the drawing library of WhyOS's screens, after the design system
 * "Neon Orbit" and the canvas "Menu's met frontpaneel" (Docs/06). The menu
 * (home) and the apps include this one header:
 *
 *	#define W 720	(optional: the panel's size by default)
 *	#include "wb.h"
 *	...
 *	wb_init();
 *
 * with -I pointing at this folder (why2025-linux/libwb). Everything is
 * static and header-only (FLAT programs link statically; --gc-sections
 * drops what a program does not use). The parts:
 *
 *	wb_tokens.h	colours, sizes (design/tokens.json)
 *	wb_assets.h	fonts, icons and the glow fall-off (gen/gen.py)
 *	wbdraw.h	pixels, boxes, text, icons, key caps, the hint bar, help
 *	wbscene.h	the frontpanel's figures behind the screen, coloured
 *	wbglass.h	glass and frosted pop-ups, the focus's glow
 *	wbui.h		switch, slider buttons, pop-up list, dialog, loupe
 *	wbstatus.h	the status bar's clock, Wi-Fi and LoRa (include it too
 *			when the program shows the status bar)
 *
 * A program draws into fb (rows fb_y0..fb_y1: a whole panel, or one band at
 * a time) and writes it to /dev/fb0 itself. README.md says more.
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wb_tokens.h"
#include "wb_assets.h"

#ifndef W
#define W		WB_SCREEN_W
#endif
#ifndef H
#define H		WB_SCREEN_H
#endif

/* The canvas's style: square corners (every radius 4), pop-ups 96 px from
 * the sides over the void at 68 %. */
#define WB_SQUARE	4
#define POP_X		96
#define WB_DIM		WB_COLOR_VOID, 173

/* wbdraw.h's panels, cards and hint bar are glass over the scene
 * (wbglass.h) */
struct glow;
static void hint_glass(int y0);
static int wb_glass_panel(int x, int y, int w, int h, int r);
static int wb_glass_card(int x, int y, int w, int h, int r, int on, const struct glow *g);
#define WB_HINT_GLASS	hint_glass
#define WB_PANEL	wb_glass_panel
#define WB_CARD		wb_glass_card

#include "wbdraw.h"
#include "wbscene.h"
#include "wbglass.h"
#include "wbui.h"

/* The glow tables (also made at their first use). */
static void wb_init(void)
{
	glow_init();
}
