// SPDX-License-Identifier: MIT
/*
 * lora_state.h — is the LoRa radio on? It is while the mesh daemon runs
 * (it holds /dev/lora0; S60lora, docs/LORA.md), which writes its pid here.
 * For the status bar of home, appterm and the SD apps.
 */
#ifndef LORA_STATE_H
#define LORA_STATE_H

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#define LORA_PID_FILE	"/var/run/lora.pid"

static int lora_on(void)
{
	char b[16];
	int fd = open(LORA_PID_FILE, O_RDONLY), n, pid;

	if (fd < 0)
		return 0;
	n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	b[n] = 0;
	pid = atoi(b);
	return pid > 0 && kill(pid, 0) == 0;
}

#endif
