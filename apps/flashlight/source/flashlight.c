// SPDX-License-Identifier: MIT
/*
 * flashlight — the whole screen as a light (sdapps/README.md), on the
 * WhyOS SDK (sdk/README.md). For two seconds the keys show at the
 * system's brightness, so a dark room is not lit at once; then the screen
 * is one colour at the mode's brightness. The mode comes back next time.
 */
#include "whyos.h"

/* the backlight's duty: about 80 is the brightest; 30 looks like less
 * than half, which suits a light that should not dazzle */
static const struct { uint32_t colour; int duty; } modes[] = {
	{ 0xFFFFFF, 80 }, { 0xFFFFFF, 30 }, { 0xFF0000, 80 },
};

static wos_screen light = { .kind = WOS_COLOUR, .keys_ms = 2000 };

static void show(int mode)
{
	mode = (mode + 3) % 3;
	wos_set_int("mode", mode);
	light.colour = modes[mode].colour;
	light.brightness = modes[mode].duty;
}

static void step(int by)
{
	show((int)wos_get_int("mode", 0) + by);
}

static const wos_action keys[] = {
	{ WOS_KEY_OPTIONS, "White 100%", "White at full brightness", show, 0 },
	{ WOS_KEY_MAIN, "White 50%", "White at half brightness", show, 1 },
	{ WOS_KEY_SECOND, "Red 100%", "Red at full brightness", show, 2 },
	{ WOS_KEY_ARROWS, 0, "Step through the modes", step },
	{ 0 }
};

int main(void)
{
	static const wos_app app = {
		.name = "Flashlight", .keep_on = 1,
		.about = "The whole screen as a light. It stays on until you close it, "
			 "and the mode you chose comes back next time.",
	};

	light.actions = keys;
	show((int)wos_get_int("mode", 0));
	return wos_run(&app, &light);
}
