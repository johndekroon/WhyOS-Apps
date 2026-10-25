/* SPDX-License-Identifier: MIT */
/* A key held down repeats (src/repeat.c): Write's own timer, as the menu's. */
#include "t.h"
#include "app.h"
#include "repeat.h"

static struct rep R;

TEST(a_held_key_repeats_after_a_delay)
{
	rep_press(&R, K_BACKSPACE, 0, 0, 1000);
	EQ(rep_due(&R, 1000 + HOLD_DELAY - 1), 0);
	EQ(rep_wait(&R, 1000), HOLD_DELAY);
	EQ(rep_due(&R, 1000 + HOLD_DELAY), K_BACKSPACE);
	EQ(rep_due(&R, 1000 + HOLD_DELAY + 1), 0);	/* once per period */
	EQ(rep_wait(&R, 1000 + HOLD_DELAY), HOLD_PERIOD);
	EQ(rep_due(&R, 1000 + HOLD_DELAY + HOLD_PERIOD), K_BACKSPACE);
}

TEST(a_late_repeat_does_not_catch_up)
{
	/* a slow frame: one repeat, then a whole period again, so nothing
	 * runs on after the key is let go */
	rep_press(&R, K_LEFT, 0, 0, 0);
	EQ(rep_due(&R, HOLD_DELAY + 5 * HOLD_PERIOD), K_LEFT);
	EQ(rep_due(&R, HOLD_DELAY + 5 * HOLD_PERIOD + 1), 0);
	EQ(rep_wait(&R, HOLD_DELAY + 5 * HOLD_PERIOD), HOLD_PERIOD);
}

TEST(letting_go_stops_it)
{
	rep_press(&R, K_DOWN, 0, 0, 0);
	rep_release(&R, K_UP);				/* another key: still held */
	EQ(rep_due(&R, HOLD_DELAY), K_DOWN);
	rep_release(&R, K_DOWN);
	EQ(rep_due(&R, 10 * HOLD_DELAY), 0);
	EQ(rep_wait(&R, 0), -1);
}

TEST(the_last_key_pressed_repeats)
{
	rep_press(&R, 30, 0, 0, 0);			/* a */
	rep_press(&R, 48, 0, 0, 100);			/* b */
	EQ(rep_due(&R, HOLD_DELAY), 0);
	EQ(rep_due(&R, 100 + HOLD_DELAY), 48);
	rep_press(&R, K_ESC, 0, 0, 200);		/* a key that does not repeat ends it */
	EQ(rep_due(&R, 200 + HOLD_DELAY), 0);
}

TEST(which_keys_repeat)
{
	OK(rep_wants(K_BACKSPACE, 0, 0));
	OK(rep_wants(K_LEFT, 0, 0));
	OK(rep_wants(K_UP, 1, 0));			/* Ctrl+Up too */
	OK(rep_wants(K_BACKSPACE, 1, 0));
	OK(rep_wants(K_SPACE, 0, 0));
	OK(rep_wants(30, 0, 0));			/* letters */
	OK(rep_wants(2, 0, 0));				/* digits */
	OK(rep_wants(53, 0, 0));			/* / */
	OK(!rep_wants(30, 1, 0));			/* not Ctrl+A, Ctrl+S ... */
	OK(!rep_wants(30, 0, 1));
	OK(!rep_wants(K_ESC, 0, 0));
	OK(!rep_wants(K_ENTER, 0, 0));
	OK(!rep_wants(K_TAB, 0, 0));
	OK(!rep_wants(K_F1, 0, 0));			/* the symbol keys open things */
	OK(!rep_wants(K_FN, 0, 0));
	OK(!rep_wants(42, 0, 0));			/* Shift */
	OK(!rep_wants(125, 0, 0));			/* the logo key */
}
