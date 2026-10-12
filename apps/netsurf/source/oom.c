// SPDX-License-Identifier: MIT
/*
 * Out of memory ends NetSurf on the badge (sdapps/README.md), linked with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc.
 *
 * Programs run without memory protection here: a code path that goes on
 * after a failed allocation and writes through a stale or half-built
 * structure can damage other programs or the kernel. NetSurf ran out of
 * memory on large pages, and one such run hung the badge right after an
 * allocation was refused. So a failed allocation ends the program at once
 * with a message; apprun then reports it in the menu.
 */
#include <stddef.h>
#include <unistd.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);

static void out_of_memory(void)
{
	static const char msg[] = "netsurf: out of memory, stopping\n";

	write(2, msg, sizeof(msg) - 1);
	_exit(3);
}

void *__wrap_malloc(size_t size)
{
	void *p = __real_malloc(size);

	if (!p && size)
		out_of_memory();
	return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
	void *p = __real_calloc(n, size);

	if (!p && n && size)
		out_of_memory();
	return p;
}

void *__wrap_realloc(void *old, size_t size)
{
	void *p = __real_realloc(old, size);

	if (!p && size)
		out_of_memory();
	return p;
}
