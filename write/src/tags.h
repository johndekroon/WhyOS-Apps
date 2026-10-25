/* SPDX-License-Identifier: MIT */
/*
 * Write: which HTML element is still open at a point of the text, so that
 * typing "</" in the code view can finish the closing tag (edit.c).
 *
 * A forgiving scan, not a parser: comments, <!...> and <?...?> are skipped,
 * void elements (<br>, <img>, ...) and <x/> do not open, the text of
 * <script> and <style> is not looked into, and an end tag closes the
 * innermost element of that name with everything opened inside it; an end
 * tag without an open element of that name changes nothing.
 */
#ifndef WRITE_TAGS_H
#define WRITE_TAGS_H

#include <stddef.h>

/* the innermost element open at the end of s[0..n): its name (as written)
 * into name, and the offset of its "<"; -1 when none is open */
int tags_open(const char *s, size_t n, char *name, int cap);

#endif
