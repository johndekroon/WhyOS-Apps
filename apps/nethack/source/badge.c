// SPDX-License-Identifier: MIT
/*
 * NetHack on the badge (sdapps/build.sh, 0001-badge.patch).
 *
 * badge_tgetch: tty's tgetch. Reads the keys with read(), not stdio, and
 * turns the arrow keys (ESC [ A..D, as appterm and most terminals send
 * them, in one write) into moves: hjkl, or 8246 with number_pad. A lone
 * ESC stays ESC. NetHack has no arrow keys of its own on Unix.
 *
 * fork: there is none on NOMMU, and uClibc leaves it out. NetHack forks
 * only for what the build leaves out or the badge does without (shell,
 * mail, compress, a debugger on panic); each caller handles the failure.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "hack.h"

int badge_tgetch(void)
{
	static unsigned char q[16];
	static int n, i;
	int k;

	if (i >= n) {
		do
			n = (int) read(0, q, sizeof q);
		while (n < 0 && errno == EINTR && !program_state.done_hup);
		i = 0;
		if (n <= 0) {
			n = 0;
			return EOF;
		}
	}
	if (q[i] == 033 && i + 2 < n && (q[i + 1] == '[' || q[i + 1] == 'O') &&
	    q[i + 2] >= 'A' && q[i + 2] <= 'D') {
		static const char vi[] = "kjlh", np[] = "8264";

		k = q[i + 2] - 'A';
		i += 3;
		return Cmd.num_pad ? np[k] : vi[k];
	}
	return q[i++];
}

pid_t fork(void)
{
	errno = ENOSYS;
	return -1;
}
