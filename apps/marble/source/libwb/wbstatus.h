// SPDX-License-Identifier: MIT
/*
 * wbstatus.h — the right of the status bar, which the system draws (the
 * menu, and sysui over the apps): LoRa while the radio is on (pink), Wi-Fi,
 * AirPlay, mute, the clock. Apps do not include it: the top WB_STATUS_BAR_H
 * rows are the system's, and an app draws only its background there.
 * Include after wbdraw.h.
 *
 * Wi-Fi is asked of wpa_supplicant (netd keeps it on wlan0): STATUS for
 * whether it is connected, SIGNAL_POLL for the RSSI. Not connected: no
 * icon at all. Connected: the arcs lit from the dot outwards
 * by signal strength, as the network list in Settings draws them (the
 * same steps: -55, -67, -78 dBm). wlan0's operstate stays "up" through a lost connection with esp-hosted.
 */
#ifndef WBSTATUS_H
#define WBSTATUS_H

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "lora_state.h"
#include "sound_state.h"
#include "wifi_ask.h"

/*
 * -1: not connected; 0..3: lit arcs. Only wpa_supplicant saying so counts
 * as not connected (wpa_state other than COMPLETED, or no wpa_supplicant:
 * Wi-Fi off). When it does not answer in time, which happens on a weak
 * link, the last answer stands, so the icon does not flicker.
 *
 * Asked at most every WIFI_BARS_S: SIGNAL_POLL goes through the driver to
 * the C6 and takes ~230 ms, which the menu would pay on every key.
 */
#define WIFI_BARS_S	5

static int wifi_bars(void)
{
	static int last = -1;
	static time_t asked;
	struct timespec now;
	char r[512], *q;
	int rssi;

	if (access(WIFI_CTRL, F_OK))
		return last = -1;		/* Wi-Fi off */
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (asked && now.tv_sec - asked < WIFI_BARS_S)
		return last;
	asked = now.tv_sec;
	if (wifi_ask("STATUS", r, sizeof(r)) < 0 || !strstr(r, "wpa_state="))
		return last;		/* no (fitting) answer */
	if (!strstr(r, "wpa_state=COMPLETED"))
		return last = -1;
	if (wifi_ask("SIGNAL_POLL", r, sizeof(r)) < 0 || !(q = strstr(r, "RSSI=")))
		return last >= 0 ? last : 0;	/* connected; how well, not now */
	rssi = atoi(q + 5);
	return last = rssi >= -55 ? 3 : rssi >= -67 ? 2 : rssi >= -78 ? 1 : 0;
}

/*
 * The sound (sound_state.h; the design's WhyOS Audio UI, Spec 4.8): two
 * icons of their own, each only while it is so, right to left before the
 * clock: muted (the crossed speaker, text-mid), then AirPlay: cyan while
 * the badge is connected to an AirPlay speaker, warn while that connection
 * is gone or being made (Wi-Fi lost, the speaker out of reach). Nothing for
 * the speaker or no output.
 */
static int status_sound(int x)
{
	struct sound_state s;

	sound_state_read(&s);
	if (!s.output[0] || !strcmp(s.output, "none"))
		return x;
	if (s.muted) {
		x -= 14 + i_spkmute22.w;
		icon(&i_spkmute22, x, (WB_STATUS_BAR_H - i_spkmute22.h) / 2, WB_COLOR_TEXT_MID, 255);
	}
	if (!strcmp(s.output, "airplay") && strcmp(s.link, "needs_code") && strcmp(s.link, "refused") &&
	    strcmp(s.link, "idle")) {
		/* cyan while connected; warn while the connection is gone or being made again */
		int ok = !strcmp(s.link, "ready") || !strcmp(s.link, "connected");

		x -= 14 + i_airplay22.w;
		icon(&i_airplay22, x, (WB_STATUS_BAR_H - i_airplay22.h) / 2, ok ? WB_COLOR_CYAN : WB_COLOR_WARN, 255);
	}
	return x;
}

/* LoRa, Wi-Fi (bars from wifi_bars()), the sound and the clock, right-aligned. */
static void status_right(const char *hhmm, int clock_ok, int bars, int lora)
{
	static const struct wb_icon *const lit[4] = { &i_sig0_22, &i_sig1_22, &i_sig2_22, &i_wifi22 };
	const struct wb_font *f = &f_mono16b;
	int cy = WB_STATUS_BAR_H / 2, x = W - WB_CHROME_PAD - text_w(f, hhmm, 0);
	int iy = (WB_STATUS_BAR_H - i_wifi22.h) / 2;

	text(f, x, text_top(f, cy), hhmm, clock_ok ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_LOW, 0);
	x = status_sound(x);
	if (bars >= 0) {
		x -= 14 + i_wifi22.w;
		if (bars < 3)
			icon(&i_wifi22, x, iy, WB_COLOR_LINE, 255);
		icon(lit[bars > 3 ? 3 : bars], x, iy, WB_COLOR_CYAN, 255);
	}
	if (lora)
		text(f, x - 14 - text_w(f, "LoRa", 0), text_top(f, cy), "LoRa", WB_COLOR_PINK, 0);
}

#endif
