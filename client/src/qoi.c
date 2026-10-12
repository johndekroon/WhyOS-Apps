/* SPDX-License-Identifier: MIT */
/*
 * store: a QOI decoder (https://qoiformat.org/qoi-specification.pdf) for the
 * icons and screenshots of the index (repository.md §5: 96×96 and 360×360),
 * straight to the panel's RGB565, with the alpha kept apart for icons.
 */
#include <stdlib.h>
#include <string.h>

#include "store.h"

#define QOI_MAX_SIDE	720

static unsigned be32(const unsigned char *p)
{
	return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

int qoi_decode(const char *path, struct picture *pic)
{
	unsigned char *d, px[4] = { 0, 0, 0, 255 }, seen[64][4];
	size_t len, at = 14;
	unsigned w, h, n, run = 0;

	memset(pic, 0, sizeof(*pic));
	if (read_file(path, (char **)&d, &len, 2 * 1024 * 1024))
		return -1;
	if (len < 14 + 8 || memcmp(d, "qoif", 4)) {
		free(d);
		return -1;
	}
	w = be32(d + 4);
	h = be32(d + 8);
	if (!w || !h || w > QOI_MAX_SIDE || h > QOI_MAX_SIDE) {
		free(d);
		return -1;
	}
	n = w * h;
	memset(seen, 0, sizeof(seen));
	pic->w = (int)w;
	pic->h = (int)h;
	pic->rgb = xmalloc(n * 2);
	pic->alpha = d[12] == 4 ? xmalloc(n) : 0;
	len -= 8;	/* the end marker */
	for (unsigned i = 0; i < n; i++) {
		if (run) {
			run--;
		} else if (at < len) {
			unsigned b = d[at++];

			if (b == 0xfe && at + 3 <= len) {
				memcpy(px, d + at, 3);
				at += 3;
			} else if (b == 0xff && at + 4 <= len) {
				memcpy(px, d + at, 4);
				at += 4;
			} else if ((b & 0xc0) == 0x00) {
				memcpy(px, seen[b], 4);
			} else if ((b & 0xc0) == 0x40) {
				px[0] += ((b >> 4) & 3) - 2;
				px[1] += ((b >> 2) & 3) - 2;
				px[2] += (b & 3) - 2;
			} else if ((b & 0xc0) == 0x80 && at < len) {
				int dg = (b & 0x3f) - 32, b2 = d[at++];

				px[0] += dg - 8 + ((b2 >> 4) & 15);
				px[1] += dg;
				px[2] += dg - 8 + (b2 & 15);
			} else if ((b & 0xc0) == 0xc0) {
				run = b & 0x3f;
			}
			memcpy(seen[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64], px, 4);
		}
		pic->rgb[i] = (unsigned short)(((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3));
		if (pic->alpha)
			pic->alpha[i] = px[3];
	}
	free(d);
	return 0;
}

void picture_free(struct picture *pic)
{
	free(pic->rgb);
	free(pic->alpha);
	memset(pic, 0, sizeof(*pic));
}
