/* SPDX-License-Identifier: MIT */
/*
 * The header of the small iconv in iconv.c (sdapps/README.md). The badge's
 * C library has no iconv, and the libiconv headers a Buildroot build may
 * leave in its staging tree belong to the GNU library this replaces.
 */
#ifndef BADGE_ICONV_H
#define BADGE_ICONV_H

#include <stddef.h>

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t iconv(iconv_t cd, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft);
int iconv_close(iconv_t cd);

#endif
