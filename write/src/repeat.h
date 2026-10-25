/* SPDX-License-Identifier: MIT */
/*
 * Write: a key held down repeats, as on a computer. The keypad sends no
 * repeats (the badge's tca8418 node has no keypad,autorepeat), so Write
 * keeps its own timer, as the menu does (why2025-linux docs/HOME.md): a
 * repeat comes after the frame of the last one, and a late one does not
 * catch up, so nothing runs on after the key is let go.
 *
 * What repeats: the arrows and Backspace (with any modifier: Fn+Backspace
 * deletes forward, Fn+arrows select), Space and the keys that type a
 * character, without Ctrl or Alt. Not Enter, Tab, Esc or the symbol keys.
 */
#ifndef WRITE_REPEAT_H
#define WRITE_REPEAT_H

#define HOLD_DELAY	400	/* ms from the press to the first repeat */
#define HOLD_PERIOD	50	/* ms between repeats */

struct rep {
	int code;		/* the key held down, 0: none */
	long next;		/* when it repeats next */
};

int rep_wants(int code, int ctrl, int alt);
void rep_press(struct rep *r, int code, int ctrl, int alt, long now);
void rep_release(struct rep *r, int code);
int rep_due(struct rep *r, long now);		/* the key to repeat now, or 0 */
long rep_wait(const struct rep *r, long now);	/* ms to the next repeat, -1: none */

#endif
