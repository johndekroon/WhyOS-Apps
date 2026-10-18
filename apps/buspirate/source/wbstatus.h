// SPDX-License-Identifier: MIT
/*
 * wbstatus.h — the right of the status bar, as home, appterm, meshchat and
 * calc draw it: LoRa while the radio is on (pink), Wi-Fi, the clock.
 * Include after wbdraw.h.
 *
 * Wi-Fi is asked of wpa_supplicant (netd keeps it on wlan0): STATUS for
 * whether it is connected, SIGNAL_POLL for the RSSI. Not connected: no
 * icon at all. Connected: the arcs lit from the dot outwards
 * by signal strength, as the network list in Settings draws them (the
 * same steps: -55, -67, -78 dBm). wlan0's operstate, used before
 * 2026-10-18, stays "up" through a lost connection with esp-hosted.
 */
#ifndef WBSTATUS_H
#define WBSTATUS_H

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lora_state.h"

#define WIFI_CTRL	"/var/run/wpa_supplicant/wlan0"

static char *wifi_num(char *q, unsigned v)
{
	unsigned k = 1;

	while (v / k >= 10)
		k *= 10;
	for (; k; k /= 10)
		*q++ = (char)('0' + v / k % 10);
	*q = 0;
	return q;
}

/*
 * One request to wpa_supplicant; its reply in r, or -1 (no answer). Each
 * request has its own socket name (pid and a count): with one name per
 * process, a reply that came after the timeout landed on the next
 * request's socket, and SIGNAL_POLL's answer read as STATUS's said "not
 * connected" (the icon came and went, 2026-10-18).
 */
static int wifi_ask(const char *cmd, char *r, int size)
{
	static unsigned seq;
	struct sockaddr_un me = { .sun_family = AF_UNIX }, wpa = { .sun_family = AF_UNIX };
	struct pollfd p;
	char *q;
	int s, n = -1;

	if ((s = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
		return -1;
	strcpy(me.sun_path, "/tmp/wbstatus-");
	q = wifi_num(me.sun_path + strlen(me.sun_path), (unsigned)getpid());
	*q++ = '-';
	wifi_num(q, seq++ % 1000);
	unlink(me.sun_path);
	strcpy(wpa.sun_path, WIFI_CTRL);
	if (bind(s, (struct sockaddr *)&me, sizeof(me)) == 0 &&
	    connect(s, (struct sockaddr *)&wpa, sizeof(wpa)) == 0 &&
	    send(s, cmd, strlen(cmd), 0) == (ssize_t)strlen(cmd)) {
		p.fd = s;
		p.events = POLLIN;
		if (poll(&p, 1, 500) > 0)
			n = (int)recv(s, r, (size_t)size - 1, 0);
	}
	close(s);
	unlink(me.sun_path);
	if (n < 0)
		return -1;
	r[n] = 0;
	return n;
}

/*
 * -1: not connected; 0..3: lit arcs. Only wpa_supplicant saying so counts
 * as not connected (wpa_state other than COMPLETED, or no wpa_supplicant:
 * Wi-Fi off). When it does not answer in time, which happens on a weak
 * link, the last answer stands, so the icon does not flicker.
 */
static int wifi_bars(void)
{
	static int last = -1;
	char r[512], *q;
	int rssi;

	if (access(WIFI_CTRL, F_OK))
		return last = -1;		/* Wi-Fi off */
	if (wifi_ask("STATUS", r, sizeof(r)) < 0 || !strstr(r, "wpa_state="))
		return last;		/* no (fitting) answer */
	if (!strstr(r, "wpa_state=COMPLETED"))
		return last = -1;
	if (wifi_ask("SIGNAL_POLL", r, sizeof(r)) < 0 || !(q = strstr(r, "RSSI=")))
		return last >= 0 ? last : 0;	/* connected; how well, not now */
	rssi = atoi(q + 5);
	return last = rssi >= -55 ? 3 : rssi >= -67 ? 2 : rssi >= -78 ? 1 : 0;
}

/* LoRa, Wi-Fi (bars from wifi_bars()) and the clock, right-aligned. */
static void status_right(const char *hhmm, int clock_ok, int bars, int lora)
{
	static const struct wb_icon *const lit[4] = { &i_sig0_22, &i_sig1_22, &i_sig2_22, &i_wifi22 };
	const struct wb_font *f = &f_mono16b;
	int cy = WB_STATUS_BAR_H / 2, x = W - WB_CHROME_PAD - text_w(f, hhmm, 0);
	int iy = (WB_STATUS_BAR_H - i_wifi22.h) / 2;

	text(f, x, text_top(f, cy), hhmm, clock_ok ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_LOW, 0);
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
