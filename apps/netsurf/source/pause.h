/* SPDX-License-Identifier: MIT */
/*
 * The pause screen of a full-screen app (pause.c): gnuboy and NetSurf.
 * Without wbdraw.h, whose names (fb) clash with the apps' own.
 */
#pragma once
#include <sys/types.h>

struct pause_key { const char *keys, *what; };	/* as wbdraw.h's help_key */

struct pause_screen {
	int fbfd, evfd;
	unsigned line;			/* bytes per panel line */
	const unsigned short *game;	/* what the app shows: lines of the panel, or 0 */
	int game_lines;
	off_t band_off;			/* where those lines start on the panel */
	const char *app;		/* "GAME BOY": the pause's eyebrow */
	const char *name, *about;	/* the help's title and sentence */
	const struct pause_key *keys;	/* the key overview */
	int n_keys;
	const char *const *items;	/* items[0] is Resume; the last (Quit) goes under a line */
	int n_items;
	int keys_item;			/* the item that shows the key overview */
};

/*
 * Shows the pause (or, with help, only the key overview) until a choice;
 * draws the app's lines back afterwards. Returns the item chosen: 0
 * (Resume) also for Esc, and when the overview opened with help closes.
 */
int badge_pause(const struct pause_screen *s, int help);
