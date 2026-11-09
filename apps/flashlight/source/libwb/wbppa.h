/*
 * /dev/ppa: the panel's PPA scales a small picture onto the screen, which costs a
 * game far less than drawing and writing it at that size. The engine interpolates,
 * so pixel art wants to be repeated twice first: from there it looks the same as
 * repeated pixels. The ioctl fails with EAGAIN while the screen is off or the PPA
 * is not there; draw through /dev/fb0 then. In the sandbox the call returns before
 * the picture is done: a failure shows at the next one, and that picture is not drawn.
 */
#ifndef WBPPA_H
#define WBPPA_H

#include <stdint.h>

#define WB_PPA_DEV	"/dev/ppa"
#define WB_PPA_SCALE	0x40205001u	/* _IOW('P', 1, struct wb_ppa_scale) */

struct wb_ppa_scale {
	uint32_t src;		/* RGB565, 2 bytes a pixel */
	uint32_t stride;	/* its pixels from one line to the next */
	uint32_t w, h;		/* what to take from it */
	uint32_t x, y;		/* where on the screen */
	uint32_t q, qy;		/* the scale across and down in sixteenths, at least 16 */
};

#endif
