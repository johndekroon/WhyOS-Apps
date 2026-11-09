// SPDX-License-Identifier: MIT
/*
 * wifi_ask.h: one request to wpa_supplicant, for the status bar (wbstatus.h) and
 * for netd, which answers apps that may not ask it themselves.
 */
#ifndef WIFI_ASK_H
#define WIFI_ASK_H

#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
 * connected" (the icon came and went).
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

#endif
