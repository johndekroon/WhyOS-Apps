// SPDX-License-Identifier: MIT
/*
 * fork() for Links on the badge (sdapps/build.sh): there is none on NOMMU,
 * and uClibc leaves it out. Links forks only for what the badge does
 * without: helper processes (external viewers, smb://), Ctrl+Z and
 * detaching; DNS is built in (NO_ASYNC_LOOKUP). Each caller handles a
 * failed fork, so this fails.
 */
#include <errno.h>
#include <sys/types.h>

pid_t fork(void)
{
	errno = ENOSYS;
	return -1;
}
