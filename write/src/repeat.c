/* SPDX-License-Identifier: MIT */
/* Write: a key held down repeats (repeat.h). */
#include "repeat.h"
#include "app.h"

static int types(int code)
{
	return (code >= 2 && code <= 13) || (code >= 16 && code <= 27) || (code >= 30 && code <= 41) ||
	       (code >= 43 && code <= 53) || code == K_SPACE;
}

int rep_wants(int code, int ctrl, int alt)
{
	if (code == K_UP || code == K_DOWN || code == K_LEFT || code == K_RIGHT || code == K_BACKSPACE)
		return 1;
	return types(code) && !ctrl && !alt;
}

void rep_press(struct rep *r, int code, int ctrl, int alt, long now)
{
	r->code = rep_wants(code, ctrl, alt) ? code : 0;
	r->next = now + HOLD_DELAY;
}

void rep_release(struct rep *r, int code)
{
	if (code == r->code)
		r->code = 0;
}

int rep_due(struct rep *r, long now)
{
	if (!r->code || now < r->next)
		return 0;
	r->next = now + HOLD_PERIOD;
	return r->code;
}

long rep_wait(const struct rep *r, long now)
{
	if (!r->code)
		return -1;
	return r->next > now ? r->next - now : 0;
}
