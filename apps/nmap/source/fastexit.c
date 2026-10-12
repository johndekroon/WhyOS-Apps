// SPDX-License-Identifier: MIT
/*
 * nmap ends with _exit, after flushing its output, instead of exit:
 * linked with -Wl,--wrap=exit (sdapps/build.sh).
 *
 * exit() runs the C++ destructors, which free nmap's tables one block at a
 * time. uClibc's malloc on NOMMU walks its whole free list for each free,
 * so freeing tens of thousands of blocks takes long (1.3 s for 30 000 in
 * QEMU), and on the badge that long pointer-chasing loop in PSRAM hung
 * the whole system until the watchdog reset it (docs: sdapps/README.md).
 * The kernel gives a NOMMU program's memory back at exit anyway.
 */
#include <stdio.h>
#include <unistd.h>

void __wrap_exit(int status)
{
	fflush(NULL);
	_exit(status);
}
