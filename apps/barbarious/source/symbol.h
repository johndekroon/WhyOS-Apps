/* SPDX-License-Identifier: MIT */
/*
 * symbol.h — BARbarious' barcodes: a text in, the modules out (symbol.c).
 *
 * A text is units: the bytes 0-255, and SYM_FNC1 for GS1's Function 1.
 * A symbol is a grid of w x h modules, dark or light, read with
 * sym_dark(); a 1D barcode is one row (h 1), drawn as tall as the screen
 * wants. PDF417 has rows of row_h modules (3) each.
 */
#pragma once

#define SYM_FNC1	256		/* a unit that is no byte: GS1's FNC1 */

enum {
	T_QR, T_DM, T_AZTEC, T_PDF417,		/* 2D */
	T_C128, T_C39, T_EAN13, T_UPCA, T_ITF, T_CODABAR,	/* 1D */
	N_TYPES
};

enum { NEED_OK, NEED_WARN, NEED_BAD };

/* QR's error correction: L 7 %, M 15 %, Q 25 %, H 30 % */
enum { ECC_L, ECC_M, ECC_Q, ECC_H };

#define SYM_BITS	(64 * 1024)	/* the most modules: PDF417 30 x 90 rows is 52110 */
#define SYM_TEXT	40		/* what goes under a 1D code */

struct sym {
	int type, w, h;			/* modules */
	int row_h;			/* modules a row is high: PDF417 3, else 1 */
	int quiet;			/* the least quiet zone, in modules */
	int ecc;			/* QR: the level used */
	int size2;			/* QR: version; Aztec: layers (minus: compact); PDF417: columns */
	int bytes;			/* data units encoded */
	int shifted, latched;		/* Code 128: SHIFT or CODE A/B used for a character */
	char text[SYM_TEXT + 1];	/* 1D: the human readable line (EAN's check digit too) */
	unsigned char bits[SYM_BITS / 8];
};

/* What a type is called, and its short name for the list ("QR", "128"). */
extern const char *const sym_names[N_TYPES];
extern const char *const sym_tags[N_TYPES];

static inline int sym_is_2d(int type)
{
	return type <= T_PDF417;
}

static inline int sym_dark(const struct sym *s, int x, int y)
{
	int i = y * s->w + x;

	return s->bits[i >> 3] >> (i & 7) & 1;
}

/*
 * What text t (n units) needs from type: NEED_OK, NEED_WARN (it works,
 * with a caveat) or NEED_BAD (it cannot be made); note says why, or what
 * the type takes ("any byte", "digits only").
 */
int sym_needs(int type, const unsigned short *t, int n, const char **note);

/*
 * Make the symbol: 0, or -1 with *err set ("Too long for a QR Code").
 * ecc is QR's level. PDF417 is shaped to fit box_w x box_h pixels best
 * (its rows are row_h modules high).
 */
int sym_make(struct sym *s, int type, const unsigned short *t, int n, int ecc,
	     int box_w, int box_h, const char **err);
