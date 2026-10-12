// SPDX-License-Identifier: MIT
/*
 * fork() for nano on the badge (sdapps/build.sh): there is none on NOMMU,
 * and uClibc leaves it out. nano forks only to run a command (^T, piping
 * text through one); the speller, linter and formatter are left out.
 * Each caller handles a failed fork, so this fails.
 */
#include <errno.h>
#include <sys/types.h>

pid_t fork(void)
{
	errno = ENOSYS;
	return -1;
}
