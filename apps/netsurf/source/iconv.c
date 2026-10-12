// SPDX-License-Identifier: MIT
/*
 * A small iconv for NetSurf on the badge (sdapps/README.md), linked in
 * place of GNU libiconv: its CJK tables alone were over 1 MB of NetSurf's
 * memory, and the badge has 12 MB for all programs. libparserutils
 * decodes HTML in UTF-8, UTF-16 and the ISO-8859 and Windows-125x sets
 * itself; what reaches iconv is NetSurf's own conversions. Supported:
 * UTF-8, UTF-16 and UTF-32 (BE, LE, or with a byte order mark), UCS-2,
 * UCS-4, ISO-8859-1 (Latin-1), Windows-1252 and US-ASCII. Other sets fail
 * in iconv_open with EINVAL, as an unknown one would.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef void *iconv_t;

enum cs { CS_UTF8, CS_UTF16BE, CS_UTF16LE, CS_UTF16, CS_UTF32BE, CS_UTF32LE,
	  CS_UTF32, CS_LATIN1, CS_CP1252, CS_ASCII };

struct cd { enum cs from, to; int in_le, bom_done, out_bom; };

/* "UTF-8//TRANSLIT" -> CS_UTF8; -1 if unknown */
static int cs_of(const char *name)
{
	static const struct { const char *n; enum cs c; } names[] = {
		{ "UTF-8", CS_UTF8 }, { "UTF8", CS_UTF8 },
		{ "UTF-16BE", CS_UTF16BE }, { "UTF-16LE", CS_UTF16LE }, { "UTF-16", CS_UTF16 },
		{ "UCS-2BE", CS_UTF16BE }, { "UCS-2LE", CS_UTF16LE }, { "UCS-2", CS_UTF16BE },
		{ "UTF-32BE", CS_UTF32BE }, { "UTF-32LE", CS_UTF32LE }, { "UTF-32", CS_UTF32 },
		{ "UCS-4BE", CS_UTF32BE }, { "UCS-4LE", CS_UTF32LE }, { "UCS-4", CS_UTF32BE },
		{ "ISO-8859-1", CS_LATIN1 }, { "ISO8859-1", CS_LATIN1 }, { "LATIN1", CS_LATIN1 },
		{ "WINDOWS-1252", CS_CP1252 }, { "CP1252", CS_CP1252 },
		{ "US-ASCII", CS_ASCII }, { "ASCII", CS_ASCII },
	};
	char n[24];
	size_t i = 0;

	while (name[i] && name[i] != '/' && i < sizeof(n) - 1) {
		n[i] = name[i];
		i++;
	}
	n[i] = 0;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcasecmp(n, names[i].n))
			return names[i].c;
	return -1;
}

iconv_t iconv_open(const char *tocode, const char *fromcode)
{
	int to = cs_of(tocode), from = cs_of(fromcode);
	struct cd *cd;

	if (to < 0 || from < 0) {
		errno = EINVAL;
		return (iconv_t)-1;
	}
	cd = calloc(1, sizeof(*cd));
	if (!cd) {
		errno = ENOMEM;
		return (iconv_t)-1;
	}
	cd->from = (enum cs)from;
	cd->to = (enum cs)to;
	cd->out_bom = to == CS_UTF16 || to == CS_UTF32;
	return cd;
}

int iconv_close(iconv_t cd)
{
	free(cd);
	return 0;
}

static const uint16_t cp1252[32] = {
	0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
	0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
	0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
	0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178,
};

/* Next code point of in (at most n bytes): its length, 0 if incomplete,
 * -1 if invalid. */
static int get(struct cd *cd, const unsigned char *in, size_t n, uint32_t *cp)
{
	switch (cd->from) {
	case CS_UTF8: {
		int len = in[0] < 0x80 ? 1 : in[0] < 0xc2 ? -1 : in[0] < 0xe0 ? 2 :
			  in[0] < 0xf0 ? 3 : in[0] < 0xf5 ? 4 : -1;

		if (len < 0)
			return -1;
		if ((size_t)len > n)
			return 0;
		*cp = len == 1 ? in[0] : in[0] & (0x7f >> len);
		for (int i = 1; i < len; i++) {
			if ((in[i] & 0xc0) != 0x80)
				return -1;
			*cp = *cp << 6 | (in[i] & 0x3f);
		}
		return len;
	}
	case CS_UTF16:
	case CS_UTF16BE:
	case CS_UTF16LE: {
		int le = cd->from == CS_UTF16LE || (cd->from == CS_UTF16 && cd->in_le);
		uint32_t u;

		if (n < 2)
			return 0;
		u = le ? (uint32_t)(in[0] | in[1] << 8) : (uint32_t)(in[0] << 8 | in[1]);
		if (u >= 0xd800 && u < 0xdc00) {
			uint32_t l;

			if (n < 4)
				return 0;
			l = le ? (uint32_t)(in[2] | in[3] << 8) : (uint32_t)(in[2] << 8 | in[3]);
			if (l < 0xdc00 || l > 0xdfff)
				return -1;
			*cp = 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00);
			return 4;
		}
		*cp = u;
		return 2;
	}
	case CS_UTF32:
	case CS_UTF32BE:
	case CS_UTF32LE: {
		int le = cd->from == CS_UTF32LE || (cd->from == CS_UTF32 && cd->in_le);

		if (n < 4)
			return 0;
		*cp = le ? (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
			   (uint32_t)in[3] << 24
			 : (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 |
			   in[3];
		return 4;
	}
	case CS_CP1252:
		*cp = in[0] >= 0x80 && in[0] < 0xa0 ? cp1252[in[0] - 0x80] : in[0];
		return 1;
	case CS_LATIN1:
		*cp = in[0];
		return 1;
	case CS_ASCII:
		if (in[0] >= 0x80)
			return -1;
		*cp = in[0];
		return 1;
	}
	return -1;
}

/* Code point cp to out (room n): bytes written, 0 if no room, -1 if the
 * set cannot hold it. */
static int put(struct cd *cd, uint32_t cp, unsigned char *out, size_t n)
{
	switch (cd->to) {
	case CS_UTF8:
		if (cp < 0x80) {
			if (n < 1) return 0;
			out[0] = (unsigned char)cp;
			return 1;
		}
		if (cp < 0x800) {
			if (n < 2) return 0;
			out[0] = (unsigned char)(0xc0 | cp >> 6);
			out[1] = (unsigned char)(0x80 | (cp & 0x3f));
			return 2;
		}
		if (cp < 0x10000) {
			if (n < 3) return 0;
			out[0] = (unsigned char)(0xe0 | cp >> 12);
			out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
			out[2] = (unsigned char)(0x80 | (cp & 0x3f));
			return 3;
		}
		if (cp > 0x10ffff)
			return -1;
		if (n < 4) return 0;
		out[0] = (unsigned char)(0xf0 | cp >> 18);
		out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
		out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		out[3] = (unsigned char)(0x80 | (cp & 0x3f));
		return 4;
	case CS_UTF16:
	case CS_UTF16BE:
	case CS_UTF16LE: {
		int le = cd->to == CS_UTF16LE, k = cp >= 0x10000 ? 4 : 2;
		uint32_t u[2] = { cp, 0 };

		if (cp > 0x10ffff)
			return -1;
		if (n < (size_t)k)
			return 0;
		if (k == 4) {
			u[0] = 0xd800 + ((cp - 0x10000) >> 10);
			u[1] = 0xdc00 + ((cp - 0x10000) & 0x3ff);
		}
		for (int i = 0; i < k / 2; i++) {
			out[2 * i + !le] = (unsigned char)u[i];
			out[2 * i + le] = (unsigned char)(u[i] >> 8);
		}
		return k;
	}
	case CS_UTF32:
	case CS_UTF32BE:
	case CS_UTF32LE: {
		int le = cd->to == CS_UTF32LE;

		if (n < 4)
			return 0;
		for (int i = 0; i < 4; i++)
			out[le ? i : 3 - i] = (unsigned char)(cp >> (8 * i));
		return 4;
	}
	case CS_CP1252:
		for (int i = 0; i < 32; i++)
			if (cp1252[i] == cp && cp != 0xfffd) {
				if (n < 1) return 0;
				out[0] = (unsigned char)(0x80 + i);
				return 1;
			}
		if (cp >= 0x80 && cp < 0xa0)
			return -1;
		/* fall through */
	case CS_LATIN1:
		if (cp > 0xff)
			return -1;
		if (n < 1) return 0;
		out[0] = (unsigned char)cp;
		return 1;
	case CS_ASCII:
		if (cp > 0x7f)
			return -1;
		if (n < 1) return 0;
		out[0] = (unsigned char)cp;
		return 1;
	}
	return -1;
}

size_t iconv(iconv_t h, char **inbuf, size_t *inleft, char **outbuf, size_t *outleft)
{
	struct cd *cd = h;
	size_t done = 0;

	if (!inbuf || !*inbuf)		/* reset the shift state: none */
		return 0;
	while (*inleft) {
		const unsigned char *in = (const unsigned char *)*inbuf;
		uint32_t cp;
		int r, w;

		if ((cd->from == CS_UTF16 || cd->from == CS_UTF32) && !cd->bom_done) {
			int k = cd->from == CS_UTF16 ? 2 : 4;

			if ((size_t)k > *inleft) {
				errno = EINVAL;
				return (size_t)-1;
			}
			cd->bom_done = 1;	/* a mark only counts at the start */
			if ((in[0] == 0xff && in[1] == 0xfe) || (in[k - 2] == 0xfe && in[k - 1] == 0xff)) {
				cd->in_le = in[0] == 0xff;
				*inbuf += k;
				*inleft -= k;
				continue;
			}
		}
		r = get(cd, in, *inleft, &cp);
		if (r == 0) {
			errno = EINVAL;
			return (size_t)-1;
		}
		if (r < 0) {
			errno = EILSEQ;
			return (size_t)-1;
		}
		if (cd->out_bom) {	/* big endian, with its mark */
			if (*outleft < (cd->to == CS_UTF16 ? 2u : 4u)) {
				errno = E2BIG;
				return (size_t)-1;
			}
			put(cd, 0xfeff, (unsigned char *)*outbuf, *outleft);
			w = cd->to == CS_UTF16 ? 2 : 4;
			*outbuf += w;
			*outleft -= w;
			cd->out_bom = 0;
		}
		w = put(cd, cp, (unsigned char *)*outbuf, *outleft);
		if (w < 0) {			/* not in the target set */
			w = put(cd, '?', (unsigned char *)*outbuf, *outleft);
			done++;
		}
		if (w == 0) {
			errno = E2BIG;
			return (size_t)-1;
		}
		*inbuf += r;
		*inleft -= r;
		*outbuf += w;
		*outleft -= w;
	}
	return done;
}

/* The names GNU libiconv's iconv.h (in the badge's sysroot) maps the
 * calls to. */
iconv_t libiconv_open(const char *, const char *) __attribute__((alias("iconv_open")));
size_t libiconv(iconv_t, char **, size_t *, char **, size_t *) __attribute__((alias("iconv")));
int libiconv_close(iconv_t) __attribute__((alias("iconv_close")));
