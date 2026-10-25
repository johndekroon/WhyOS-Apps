// SPDX-License-Identifier: MIT
/*
 * symbol.c — BARbarious' barcodes (symbol.h): QR Code through Project
 * Nayuki's qrcodegen (overlay-src/qrcodegen, MIT), the others our own:
 *
 *   Data Matrix  ECC 200, square; ASCII encodation (digit pairs, Upper
 *                Shift for bytes 128-255, FNC1 as 232), ISO/IEC 16022
 *   Aztec        compact and full range; Upper, Lower and Digit mode, the
 *                rest in Binary Shift; 23 % + 3 words of error correction,
 *                ISO/IEC 24778
 *   PDF417       Byte Compaction, the recommended error correction level
 *                for the length (ISO/IEC 15438 Table 6)
 *   Code 128     sets A, B and C: C for runs of digits, a SHIFT for one
 *                character of the other of A and B, CODE A/B for more
 *   Code 39      standard, or Full ASCII when the text needs it
 *   EAN-13, UPC-A, ITF (wide 3:1), Codabar (wide 3:1)
 *
 * The bar widths and PDF417's codeword patterns are zint's tables
 * (symbol_tabs.h, BSD-3-Clause). Every type is checked by decoding what
 * the app draws with zxing-cpp (test_barbarious.py).
 *
 * No stdio and no floating point (the FLAT build, a soft-float toolchain).
 */
#include <stdint.h>
#include <string.h>

#include "symbol.h"
#include "symbol_tabs.h"
#include "qrcodegen/qrcodegen.h"

const char *const sym_names[N_TYPES] = {
	"QR Code", "Data Matrix", "Aztec", "PDF417", "Code 128", "Code 39", "EAN-13", "UPC-A", "ITF", "Codabar",
};

const char *const sym_tags[N_TYPES] = {
	"QR", "DM", "Aztec", "PDF417", "128", "39", "EAN-13", "UPC-A", "ITF", "Codabar",
};

#define MAX_UNITS	2100		/* codewords, values: twice the longest text and some */

/* ---- the grid -------------------------------------------------------------- */

static int grid(struct sym *s, int w, int h)
{
	if (w * h > SYM_BITS)
		return -1;
	s->w = w;
	s->h = h;
	memset(s->bits, 0, (size_t)(w * h + 7) / 8);
	return 0;
}

static void dark(struct sym *s, int x, int y)
{
	int i = y * s->w + x;

	s->bits[i >> 3] |= (unsigned char)(1 << (i & 7));
}

/* 1D: a row of elements, bar first; each digit of widths is a width in
 * modules, or with wide a '2' is that many (Code 39, ITF, Codabar). */
static int bar_x;

static int bars(struct sym *s, const char *widths, int n, int wide)
{
	for (int i = 0; i < n; i++) {
		int m = widths[i] - '0';

		if (wide && m == 2)
			m = wide;
		if (bar_x + m > SYM_BITS)
			return -1;
		if (!(i & 1))
			for (int k = 0; k < m; k++)
				dark(s, bar_x + k, 0);
		bar_x += m;
	}
	return 0;
}

static void bars_start(struct sym *s)
{
	s->w = SYM_BITS;	/* row 0: index x, whatever the width */
	s->h = 1;
	memset(s->bits, 0, sizeof(s->bits));
	bar_x = 0;
}

/* ---- Reed-Solomon ---------------------------------------------------------- */

/* GF(2^m) by the primitive polynomial poly (Data Matrix, Aztec) */
static uint16_t gf_exp[2 * 4096], gf_log[4096];
static int gf_size, gf_poly;

static void gf_init(int size, int poly)
{
	int x = 1;

	if (gf_size == size && gf_poly == poly)
		return;
	gf_size = size;
	gf_poly = poly;
	for (int i = 0; i < size - 1; i++) {
		gf_exp[i] = gf_exp[i + size - 1] = (uint16_t)x;
		gf_log[x] = (uint16_t)i;
		x <<= 1;
		if (x & size)
			x ^= poly;
	}
}

static int gf_mul(int a, int b)
{
	return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}

/*
 * ne check words for the nd data words d (every step-th from d[0]), into e
 * (every step-th): the remainder of d(x) x^ne by (x - a)(x - a^2)...(x - a^ne).
 * mod 0: GF(2^m) as gf_init set it; else the prime field of mod with
 * a = 3 (PDF417), where the words are the remainder's negatives.
 */
static void rs(const int *d, int nd, int step, int *e, int ne, int mod)
{
	static int g[2200], r[2200];

	g[0] = 1;
	for (int i = 1; i <= ne; i++) {
		int a = 1;

		if (mod)
			for (int k = 0; k < i; k++)
				a = a * 3 % mod;
		else
			a = gf_exp[i % (gf_size - 1)];
		g[i] = 0;
		for (int k = i; k > 0; k--)
			g[k] = mod ? (g[k] + mod - a * g[k - 1] % mod) % mod : g[k] ^ gf_mul(a, g[k - 1]);
	}
	for (int k = 0; k < ne; k++)
		r[k] = 0;
	for (int i = 0; i < nd; i++) {
		int fb = mod ? (d[i * step] + r[0]) % mod : d[i * step] ^ r[0];

		for (int k = 0; k < ne; k++) {
			int nx = k + 1 < ne ? r[k + 1] : 0;

			r[k] = mod ? (nx + mod - fb * g[k + 1] % mod) % mod : nx ^ gf_mul(fb, g[k + 1]);
		}
	}
	for (int k = 0; k < ne; k++)
		e[k * step] = mod ? (mod - r[k]) % mod : r[k];
}

/* ---- what a text needs ----------------------------------------------------- */

static const char c39_set[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%";
static const char coda_set[] = "0123456789-$:/.+ABCD";

static int is_digit(int c)
{
	return c >= '0' && c <= '9';
}

static int c39_std(int c)
{
	return c > 0 && c < 128 && strchr(c39_set, c);
}

/* EAN's check digit for the first n digits */
static int ean_check(const unsigned short *t, int n)
{
	int sum = 0;

	for (int i = 0; i < n; i++)
		sum += (t[n - 1 - i] - '0') * (i % 2 ? 1 : 3);
	return (10 - sum % 10) % 10;
}

int sym_needs(int type, const unsigned short *t, int n, const char **note)
{
	int fnc = 0, high = 0, digits = 1, std39 = 1, coda = 1;
	int ends = n >= 2 && t[0] >= 'A' && t[0] <= 'D' && t[n - 1] >= 'A' && t[n - 1] <= 'D';

	for (int i = 0; i < n; i++) {
		int c = t[i];

		fnc |= c == SYM_FNC1;
		high |= c > 127 && c != SYM_FNC1;
		digits &= is_digit(c);
		std39 &= c39_std(c);
		coda &= c > 0 && c < 128 && strchr(coda_set, c) &&
			(c < 'A' || (ends && (i == 0 || i == n - 1)));
	}
	switch (type) {
	case T_DM:
		*note = "any byte";
		return NEED_OK;
	case T_QR:
	case T_AZTEC:
	case T_PDF417:
		*note = fnc ? "no FNC1" : "any byte";
		return fnc ? NEED_BAD : NEED_OK;
	case T_C128:
		*note = high ? "ASCII 0\xe2\x80\x93" "127 only" : "ASCII 0\xe2\x80\x93" "127";
		return high ? NEED_BAD : NEED_OK;
	case T_C39:
		if (fnc || high) {
			*note = fnc ? "no FNC1" : "ASCII 0\xe2\x80\x93" "127 only";
			return NEED_BAD;
		}
		*note = std39 ? "A\xe2\x80\x93Z 0\xe2\x80\x93" "9 - . $ / + %" : "scanner needs Full ASCII";
		return std39 ? NEED_OK : NEED_WARN;
	case T_EAN13:
	case T_UPCA: {
		int need = type == T_EAN13 ? 12 : 11;

		if (!digits || !n) {
			*note = "digits only";
			return NEED_BAD;
		}
		if (n != need && n != need + 1) {
			*note = type == T_EAN13 ? "12 digits" : "11 digits";
			return NEED_BAD;
		}
		if (n == need + 1 && t[need] - '0' != ean_check(t, need)) {
			*note = "wrong check digit";
			return NEED_BAD;
		}
		*note = type == T_EAN13 ? "12 digits + check" : "11 digits + check";
		return NEED_OK;
	}
	case T_ITF:
		if (!digits || !n) {
			*note = "digits only";
			return NEED_BAD;
		}
		*note = n % 2 ? "a 0 goes in front" : "pairs of digits";
		return n % 2 ? NEED_WARN : NEED_OK;
	case T_CODABAR:
		*note = coda ? "0\xe2\x80\x93" "9 - $ : / . +" : "0\xe2\x80\x93" "9 - $ : / . + only";
		return coda ? NEED_OK : NEED_BAD;
	}
	*note = "";
	return NEED_BAD;
}

/* ---- QR Code --------------------------------------------------------------- */

static int make_qr(struct sym *s, const unsigned short *t, int n, int ecc, const char **err)
{
	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	static uint8_t segbuf[3000];
	static char str[3000];
	struct qrcodegen_Segment seg;
	int num = 1, alnum = 1, size;

	if (n > 2953) {
		*err = "Too long for a QR Code";
		return -1;
	}
	for (int i = 0; i < n; i++) {
		num &= is_digit(t[i]);
		alnum &= t[i] < 128 && t[i] && strchr("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:", t[i]);
		str[i] = (char)t[i];
	}
	str[n] = 0;
	if (num)
		seg = qrcodegen_makeNumeric(str, segbuf);
	else if (alnum)
		seg = qrcodegen_makeAlphanumeric(str, segbuf);
	else
		seg = qrcodegen_makeBytes((const uint8_t *)str, (size_t)n, segbuf);
	if (!qrcodegen_encodeSegmentsAdvanced(&seg, 1, (enum qrcodegen_Ecc)ecc, qrcodegen_VERSION_MIN,
					      qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, false, tmp, qr)) {
		*err = "Too long for a QR Code";
		return -1;
	}
	size = qrcodegen_getSize(qr);
	grid(s, size, size);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			if (qrcodegen_getModule(qr, x, y))
				dark(s, x, y);
	s->ecc = ecc;
	s->size2 = (size - 17) / 4;
	s->quiet = 4;
	return 0;
}

/* ---- Data Matrix ----------------------------------------------------------- */

static const struct { short size, data, ecc, blocks, region; } dm_sizes[] = {
	{ 10, 3, 5, 1, 8 }, { 12, 5, 7, 1, 10 }, { 14, 8, 10, 1, 12 }, { 16, 12, 12, 1, 14 },
	{ 18, 18, 14, 1, 16 }, { 20, 22, 18, 1, 18 }, { 22, 30, 20, 1, 20 }, { 24, 36, 24, 1, 22 },
	{ 26, 44, 28, 1, 24 }, { 32, 62, 36, 1, 14 }, { 36, 86, 42, 1, 16 }, { 40, 114, 48, 1, 18 },
	{ 44, 144, 56, 1, 20 }, { 48, 174, 68, 1, 22 }, { 52, 204, 84, 2, 24 }, { 64, 280, 112, 2, 14 },
	{ 72, 368, 144, 4, 16 }, { 80, 456, 192, 4, 18 }, { 88, 576, 224, 4, 20 }, { 96, 696, 272, 4, 22 },
	{ 104, 816, 336, 6, 24 }, { 120, 1050, 408, 6, 18 }, { 132, 1304, 496, 8, 20 },
	{ 144, 1558, 620, 10, 22 },
};

/* the placement (Annex F): codeword bits into the data regions joined up */
static int dm_cw[2200], dm_rows, dm_cols;
static signed char dm_map[132 * 132];

static void dm_module(int row, int col, int pos, int bit)
{
	if (row < 0) {
		row += dm_rows;
		col += 4 - ((dm_rows + 4) % 8);
	}
	if (col < 0) {
		col += dm_cols;
		row += 4 - ((dm_cols + 4) % 8);
	}
	dm_map[row * dm_cols + col] = (signed char)(dm_cw[pos] >> (8 - bit) & 1);
}

static void dm_utah(int row, int col, int pos)
{
	dm_module(row - 2, col - 2, pos, 1);
	dm_module(row - 2, col - 1, pos, 2);
	dm_module(row - 1, col - 2, pos, 3);
	dm_module(row - 1, col - 1, pos, 4);
	dm_module(row - 1, col, pos, 5);
	dm_module(row, col - 2, pos, 6);
	dm_module(row, col - 1, pos, 7);
	dm_module(row, col, pos, 8);
}

/* the four corner shapes: (row, col) of bits 1..8 */
static void dm_corner(int which, int pos)
{
	static const signed char c[4][8][2] = {
		{ { -1, 0 }, { -1, 1 }, { -1, 2 }, { 0, -2 }, { 0, -1 }, { 1, -1 }, { 2, -1 }, { 3, -1 } },
		{ { -3, 0 }, { -2, 0 }, { -1, 0 }, { 0, -4 }, { 0, -3 }, { 0, -2 }, { 0, -1 }, { 1, -1 } },
		{ { -3, 0 }, { -2, 0 }, { -1, 0 }, { 0, -2 }, { 0, -1 }, { 1, -1 }, { 2, -1 }, { 3, -1 } },
		{ { -1, 0 }, { -1, -1 }, { 0, -3 }, { 0, -2 }, { 0, -1 }, { 1, -3 }, { 1, -2 }, { 1, -1 } },
	};

	for (int b = 0; b < 8; b++) {
		int r = c[which][b][0], k = c[which][b][1];

		dm_module(r < 0 ? dm_rows + r : r, k < 0 ? dm_cols + k : k, pos, b + 1);
	}
}

static void dm_place(void)
{
	int pos = 0, row = 4, col = 0;

	memset(dm_map, -1, sizeof(dm_map));
	do {
		if (row == dm_rows && col == 0)
			dm_corner(0, pos++);
		if (row == dm_rows - 2 && col == 0 && dm_cols % 4)
			dm_corner(1, pos++);
		if (row == dm_rows - 2 && col == 0 && dm_cols % 8 == 4)
			dm_corner(2, pos++);
		if (row == dm_rows + 4 && col == 2 && dm_cols % 8 == 0)
			dm_corner(3, pos++);
		do {	/* up and to the right */
			if (row < dm_rows && col >= 0 && dm_map[row * dm_cols + col] < 0)
				dm_utah(row, col, pos++);
			row -= 2;
			col += 2;
		} while (row >= 0 && col < dm_cols);
		row++;
		col += 3;
		do {	/* down and to the left */
			if (row >= 0 && col < dm_cols && dm_map[row * dm_cols + col] < 0)
				dm_utah(row, col, pos++);
			row += 2;
			col -= 2;
		} while (row < dm_rows && col >= 0);
		row += 3;
		col++;
	} while (row < dm_rows || col < dm_cols);
	if (dm_map[dm_rows * dm_cols - 1] < 0) {	/* the fixed pattern in the corner */
		dm_map[dm_rows * dm_cols - 1] = 1;
		dm_map[(dm_rows - 1) * dm_cols - 2] = 1;
		dm_map[dm_rows * dm_cols - 2] = 0;
		dm_map[(dm_rows - 1) * dm_cols - 1] = 0;
	}
}

static int make_dm(struct sym *s, const unsigned short *t, int n, const char **err)
{
	int nc = 0, z;

	for (int i = 0; i < n && nc < 1560; i++) {
		if (is_digit(t[i]) && i + 1 < n && is_digit(t[i + 1])) {
			dm_cw[nc++] = 130 + (t[i] - '0') * 10 + t[i + 1] - '0';
			i++;
		} else if (t[i] == SYM_FNC1) {
			dm_cw[nc++] = 232;
		} else if (t[i] > 127) {
			dm_cw[nc++] = 235;		/* Upper Shift */
			dm_cw[nc++] = t[i] - 128 + 1;
		} else {
			dm_cw[nc++] = t[i] + 1;
		}
	}
	for (z = 0; z < (int)(sizeof(dm_sizes) / sizeof(dm_sizes[0])) && dm_sizes[z].data < nc; z++)
		;
	if (z == (int)(sizeof(dm_sizes) / sizeof(dm_sizes[0]))) {
		*err = "Too long for a Data Matrix";
		return -1;
	}
	{
		int size = dm_sizes[z].size, data = dm_sizes[z].data, ecc = dm_sizes[z].ecc;
		int blocks = dm_sizes[z].blocks, rs_n = dm_sizes[z].region, regions = size / (rs_n + 2);

		for (int i = nc; i < data; i++) {	/* pads: 129, then the 253-state randomized */
			int r = 129 + (149 * (i + 1)) % 253 + 1;

			dm_cw[i] = i == nc ? 129 : r <= 254 ? r : r - 254;
		}
		gf_init(256, 0x12d);
		for (int b = 0; b < blocks; b++) {
			int nd = (data - b + blocks - 1) / blocks;	/* 144: 156 then 155 */

			rs(dm_cw + b, nd, blocks, dm_cw + data + b, ecc / blocks, 0);
		}
		dm_rows = dm_cols = regions * rs_n;
		dm_place();
		grid(s, size, size);
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size; x++) {
				int ry = y % (rs_n + 2), rx = x % (rs_n + 2);
				int on;

				if (rx == 0 || ry == rs_n + 1)		/* the L: left and bottom */
					on = 1;
				else if (ry == 0)			/* the top: alternating */
					on = !(x & 1);
				else if (rx == rs_n + 1)		/* the right: alternating */
					on = y & 1;
				else
					on = dm_map[((y / (rs_n + 2)) * rs_n + ry - 1) * dm_cols +
						    (x / (rs_n + 2)) * rs_n + rx - 1] == 1;
				if (on)
					dark(s, x, y);
			}
	}
	s->quiet = 1;
	return 0;
}

/* ---- Aztec ----------------------------------------------------------------- */

static unsigned char az_b[20000];	/* bits, one a byte */
static int az_n;

static void az_put(int v, int nbits)
{
	for (int i = nbits - 1; i >= 0; i--)
		if (az_n < (int)sizeof(az_b))
			az_b[az_n++] = (unsigned char)(v >> i & 1);
}

enum { AZ_UPPER, AZ_LOWER, AZ_DIGIT };

static int az_kind(int c)
{
	if (c == ' ')
		return 3;		/* in every mode */
	if (c >= 'A' && c <= 'Z')
		return AZ_UPPER;
	if (c >= 'a' && c <= 'z')
		return AZ_LOWER;
	if (is_digit(c) || c == ',' || c == '.')
		return AZ_DIGIT;
	return -1;			/* a byte, in Binary Shift */
}

static int az_code(int c, int mode)
{
	if (c == ' ')
		return 1;
	if (mode == AZ_DIGIT)
		return c == ',' ? 12 : c == '.' ? 13 : c - '0' + 2;
	return c - (mode == AZ_UPPER ? 'A' : 'a') + 2;
}

/* the text as Aztec's bits (az_b) */
static void az_text(const unsigned short *t, int n)
{
	int mode = AZ_UPPER;

	az_n = 0;
	for (int i = 0; i < n; ) {
		int k = az_kind(t[i]);

		if (k == 3 || k == mode) {
			az_put(az_code(t[i], mode), mode == AZ_DIGIT ? 4 : 5);
			i++;
			continue;
		}
		if (k < 0) {			/* a run of bytes */
			int r = 0;

			while (i + r < n && az_kind(t[i + r]) < 0 && r < 2078)
				r++;
			if (mode == AZ_DIGIT) {
				az_put(14, 4);		/* U/L */
				mode = AZ_UPPER;
			}
			az_put(31, 5);			/* B/S */
			if (r <= 31) {
				az_put(r, 5);
			} else {
				az_put(0, 5);
				az_put(r - 31, 11);
			}
			for (int j = 0; j < r; j++)
				az_put(t[i + j], 8);
			i += r;
			continue;
		}
		/* to another mode: a shift for one upper case letter, else a latch */
		if (k == AZ_UPPER) {
			int one = i + 1 >= n || az_kind(t[i + 1]) != AZ_UPPER;

			if (mode == AZ_LOWER && one) {
				az_put(28, 5);		/* U/S */
				az_put(az_code(t[i], AZ_UPPER), 5);
				i++;
				continue;
			}
			if (mode == AZ_DIGIT && one) {
				az_put(15, 4);		/* U/S */
				az_put(az_code(t[i], AZ_UPPER), 5);
				i++;
				continue;
			}
			if (mode == AZ_LOWER)
				az_put(30, 5);		/* D/L, then U/L */
			az_put(14, 4);
		} else if (k == AZ_LOWER) {
			if (mode == AZ_DIGIT)
				az_put(14, 4);		/* U/L, then L/L */
			az_put(28, 5);
		} else {
			az_put(30, 5);			/* D/L */
		}
		mode = k;
	}
}

static int az_word_size(int layers)
{
	return layers <= 2 ? 6 : layers <= 8 ? 8 : layers <= 22 ? 10 : 12;
}

static int az_total_bits(int layers, int compact)
{
	return ((compact ? 88 : 112) + 16 * layers) * layers;
}

static int az_words[2200];

/* az_b as words of ws bits with Aztec's bit stuffing: a word of all 0s or
 * all 1s but its last bit gets the opposite last bit; the end padded with 1s */
static int az_stuff(int ws)
{
	int nw = 0, mask = (1 << ws) - 2;

	for (int i = 0; i < az_n; i += ws) {
		int w = 0;

		for (int j = 0; j < ws; j++)
			if (i + j >= az_n || az_b[i + j])
				w |= 1 << (ws - 1 - j);
		if ((w & mask) == mask) {
			w &= mask;
			i--;
		} else if ((w & mask) == 0) {
			w |= 1;
			i--;
		}
		if (nw < (int)(sizeof(az_words) / sizeof(az_words[0])))
			az_words[nw++] = w;
	}
	return nw;
}

static void az_bullseye(struct sym *s, int c, int size)
{
	for (int i = 0; i < size; i += 2)
		for (int j = c - i; j <= c + i; j++) {
			dark(s, j, c - i);
			dark(s, j, c + i);
			dark(s, c - i, j);
			dark(s, c + i, j);
		}
	/* the orientation marks */
	dark(s, c - size, c - size);
	dark(s, c - size + 1, c - size);
	dark(s, c - size, c - size + 1);
	dark(s, c + size, c - size);
	dark(s, c + size, c - size + 1);
	dark(s, c + size, c + size - 1);
}

static int make_aztec(struct sym *s, const unsigned short *t, int n, const char **err)
{
	static unsigned char msg[20000], mode_msg[40];
	static int ecw[8];
	int ecc_bits, compact = 0, layers = 0, ws = 0, nw = 0, total = 0, base, size, nmsg = 0;
	int amap[160];

	az_text(t, n);
	ecc_bits = az_n * 23 / 100 + 11;
	for (int i = 0; ; i++) {
		int usable;

		if (i > 32) {
			*err = "Too long for an Aztec code";
			return -1;
		}
		compact = i <= 3;
		layers = compact ? i + 1 : i;
		total = az_total_bits(layers, compact);
		if (az_n + ecc_bits > total)
			continue;
		if (ws != az_word_size(layers)) {
			ws = az_word_size(layers);
			nw = az_stuff(ws);
		}
		usable = total - total % ws;
		if (compact && nw > 64)
			continue;
		if (nw * ws + ecc_bits <= usable)
			break;
	}
	/* the message: the words and their check words, after total % ws zeros */
	{
		static const int polys[13] = { 0, 0, 0, 0, 0, 0, 0x43, 0, 0x12d, 0, 0x409, 0, 0x1069 };
		int tw = total / ws;

		gf_init(1 << ws, polys[ws]);
		rs(az_words, nw, 1, az_words + nw, tw - nw, 0);
		for (int i = 0; i < total % ws; i++)
			msg[nmsg++] = 0;
		for (int w = 0; w < tw; w++)
			for (int b = ws - 1; b >= 0; b--)
				msg[nmsg++] = (unsigned char)(az_words[w] >> b & 1);
	}
	/* the mode message: layers and words, with check words in GF(16) */
	{
		int mw[10], nm = compact ? 2 : 4, ne = compact ? 5 : 6, v, k = 0;

		v = compact ? (layers - 1) << 6 | (nw - 1) : (layers - 1) << 11 | (nw - 1);
		for (int i = nm - 1; i >= 0; i--)
			mw[k++] = v >> (4 * i) & 15;
		gf_init(16, 0x13);
		rs(mw, nm, 1, ecw, ne, 0);
		for (int i = 0; i < ne; i++)
			mw[nm + i] = ecw[i];
		for (int i = 0; i < nm + ne; i++)
			for (int b = 3; b >= 0; b--)
				mode_msg[4 * i + 3 - b] = (unsigned char)(mw[i] >> b & 1);
	}
	base = (compact ? 11 : 14) + layers * 4;
	if (compact) {
		size = base;
		for (int i = 0; i < base; i++)
			amap[i] = i;
	} else {
		int oc, c;

		size = base + 1 + 2 * ((base / 2 - 1) / 15);
		oc = base / 2;
		c = size / 2;
		for (int i = 0; i < oc; i++) {
			int off = i + i / 15;

			amap[oc - i - 1] = c - off - 1;
			amap[oc + i] = c + off + 1;
		}
	}
	if (grid(s, size, size) < 0) {
		*err = "Too long for an Aztec code";
		return -1;
	}
	for (int i = 0, ro = 0; i < layers; i++) {	/* the data, layer by layer, inside out */
		int rsz = (layers - i) * 4 + (compact ? 9 : 12);

		for (int j = 0; j < rsz; j++) {
			int co = j * 2;

			for (int k = 0; k < 2; k++) {
				if (msg[ro + co + k])
					dark(s, amap[i * 2 + k], amap[i * 2 + j]);
				if (msg[ro + rsz * 2 + co + k])
					dark(s, amap[i * 2 + j], amap[base - 1 - i * 2 - k]);
				if (msg[ro + rsz * 4 + co + k])
					dark(s, amap[base - 1 - i * 2 - k], amap[base - 1 - i * 2 - j]);
				if (msg[ro + rsz * 6 + co + k])
					dark(s, amap[base - 1 - i * 2 - j], amap[i * 2 + k]);
			}
		}
		ro += rsz * 8;
	}
	{
		int c = size / 2;

		if (compact) {
			for (int i = 0; i < 7; i++) {
				int o = c - 3 + i;

				if (mode_msg[i])
					dark(s, o, c - 5);
				if (mode_msg[i + 7])
					dark(s, c + 5, o);
				if (mode_msg[20 - i])
					dark(s, o, c + 5);
				if (mode_msg[27 - i])
					dark(s, c - 5, o);
			}
			az_bullseye(s, c, 5);
		} else {
			for (int i = 0; i < 10; i++) {
				int o = c - 5 + i + i / 5;

				if (mode_msg[i])
					dark(s, o, c - 7);
				if (mode_msg[i + 10])
					dark(s, c + 7, o);
				if (mode_msg[29 - i])
					dark(s, o, c + 7);
				if (mode_msg[39 - i])
					dark(s, c - 7, o);
			}
			az_bullseye(s, c, 7);
			/* the reference grid, every 16 modules from the middle */
			for (int i = 0, j = 0; i < base / 2 - 1; i += 15, j += 16)
				for (int k = c & 1; k < size; k += 2) {
					dark(s, c - j, k);
					dark(s, c + j, k);
					dark(s, k, c - j);
					dark(s, k, c + j);
				}
		}
	}
	s->size2 = compact ? -layers : layers;
	s->quiet = 0;
	return 0;
}

/* ---- PDF417 ---------------------------------------------------------------- */

static int make_pdf417(struct sym *s, const unsigned short *t, int n, int box_w, int box_h,
		       const char **err)
{
	static int cw[1000];
	int nc = 1, level, k, best_cols = 0, best_rows = 0, best_px = -1, best_pad = 0, rows, cols;

	cw[nc++] = n % 6 ? 901 : 924;		/* Byte Compaction */
	for (int i = 0; i + 6 <= n && nc < 930; i += 6) {	/* 6 bytes in 5 codewords */
		uint64_t v = 0;

		for (int j = 0; j < 6; j++)
			v = v << 8 | t[i + j];
		for (int j = 4; j >= 0; j--) {
			cw[nc + j] = (int)(v % 900);
			v /= 900;
		}
		nc += 5;
	}
	for (int i = n - n % 6; i < n && nc < 930; i++)
		cw[nc++] = t[i];
	level = nc <= 40 ? 2 : nc <= 160 ? 3 : nc <= 320 ? 4 : nc <= 863 ? 5 : -1;
	k = level < 0 ? 0 : 2 << level;
	if (level < 0 || nc + k > 928) {
		*err = "Too long for a PDF417";
		return -1;
	}
	if (box_w <= 0 || box_h <= 0)
		box_w = box_h = 720;
	/* the shape that shows the modules biggest; then the least padding */
	for (cols = 1; cols <= 30; cols++) {
		int px, pw, ph, pad;

		rows = (nc + k + cols - 1) / cols;
		if (rows < 3)
			rows = 3;
		if (rows > 90)
			continue;
		pw = box_w / (17 * cols + 69 + 4);
		ph = box_h / (rows * 3 + 4);
		px = pw < ph ? pw : ph;
		pad = rows * cols - nc - k;
		if (px > best_px || (px == best_px && pad < best_pad)) {
			best_px = px;
			best_cols = cols;
			best_rows = rows;
			best_pad = pad;
		}
	}
	if (!best_cols) {
		*err = "Too long for a PDF417";
		return -1;
	}
	rows = best_rows;
	cols = best_cols;
	while (nc < rows * cols - k)
		cw[nc++] = 900;			/* pads */
	cw[0] = nc;				/* the length descriptor */
	rs(cw, nc, 1, cw + nc, k, 929);
	if (grid(s, 17 * (cols + 4) + 1, rows) < 0) {
		*err = "Too long for a PDF417";
		return -1;
	}
	for (int r = 0; r < rows; r++) {
		int cl = r % 3, base = 30 * (r / 3), x = 0, ind[2];
		static const char start[] = "11111111010101000", stop[] = "111111101000101001";

		ind[0] = base + (cl == 0 ? (rows - 1) / 3 : cl == 1 ? level * 3 + (rows - 1) % 3 : cols - 1);
		ind[1] = base + (cl == 0 ? cols - 1 : cl == 1 ? (rows - 1) / 3 : level * 3 + (rows - 1) % 3);
		for (int i = 0; i < 17; i++, x++)
			if (start[i] == '1')
				dark(s, x, r);
		for (int c = -1; c <= cols; c++) {
			int v = pdf_tab[cl * 929 + (c < 0 ? ind[0] : c == cols ? ind[1] : cw[r * cols + c])];

			for (int i = 15; i >= 0; i--, x++)
				if (v >> i & 1)
					dark(s, x, r);
			x++;			/* the 17th module: a space */
		}
		for (int i = 0; i < 18; i++, x++)
			if (stop[i] == '1')
				dark(s, x, r);
	}
	s->row_h = 3;
	s->quiet = 2;
	s->size2 = cols;
	return 0;
}

/* ---- the 1D codes ---------------------------------------------------------- */

static int c128_in(int c, int set)	/* set 0 A, 1 B */
{
	return set ? c >= 32 && c < 128 : c < 96;
}

static int c128_val(int c, int set)
{
	return set ? c - 32 : c >= 32 ? c - 32 : c + 64;
}

static int digits_at(const unsigned short *t, int n, int i)
{
	int d = 0;

	while (i + d < n && is_digit(t[i + d]))
		d++;
	return d;
}

/* A or B for t[i..]: A when a control character comes before a lower case one */
static int c128_ab(const unsigned short *t, int n, int i)
{
	for (; i < n; i++) {
		if (t[i] == SYM_FNC1)
			continue;
		if (t[i] < 32)
			return 0;
		if (t[i] >= 96)
			return 1;
	}
	return 1;
}

static int make_c128(struct sym *s, const unsigned short *t, int n, const char **err)
{
	static int v[MAX_UNITS];
	int nv = 0, set, i = 0, lead = digits_at(t, n, t[0] == SYM_FNC1), sum;

	if (lead >= 4 || (lead == 2 && lead + (t[0] == SYM_FNC1) == n))
		set = 2;
	else
		set = c128_ab(t, n, 0);
	v[nv++] = 103 + set;
	while (i < n && nv < MAX_UNITS - 4) {
		int c = t[i], d, other;

		if (c == SYM_FNC1) {
			v[nv++] = 102;
			i++;
			continue;
		}
		if (c > 127) {
			*err = "Code 128 takes ASCII 0\xe2\x80\x93" "127 only";
			return -1;
		}
		if (set == 2) {
			if (i + 1 < n && is_digit(c) && is_digit(t[i + 1])) {
				v[nv++] = (c - '0') * 10 + t[i + 1] - '0';
				i += 2;
				continue;
			}
			set = c128_ab(t, n, i);
			v[nv++] = set ? 100 : 101;	/* CODE B, CODE A */
			continue;
		}
		d = digits_at(t, n, i);
		if (d % 2 == 0 && (d >= 6 || (d >= 4 && i + d == n))) {
			v[nv++] = 99;			/* CODE C */
			set = 2;
			continue;
		}
		if (c128_in(c, set)) {
			v[nv++] = c128_val(c, set);
			i++;
			continue;
		}
		other = !set;
		if (i + 1 < n && t[i + 1] < 128 && !c128_in(t[i + 1], set) && c128_in(t[i + 1], other)) {
			v[nv++] = other ? 100 : 101;
			set = other;
			s->latched = 1;
			continue;
		}
		v[nv++] = 98;				/* SHIFT */
		v[nv++] = c128_val(c, other);
		s->shifted = 1;
		i++;
	}
	sum = v[0];
	for (int k = 1; k < nv; k++)
		sum += k * v[k];
	v[nv++] = sum % 103;
	bars_start(s);
	for (int k = 0; k < nv; k++)
		bars(s, c128_tab[v[k]], 6, 0);
	bars(s, "2331112", 7, 0);
	s->w = bar_x;
	return 0;
}

static int make_c39(struct sym *s, const unsigned short *t, int n, const char **err)
{
	int std = 1;

	for (int i = 0; i < n; i++)
		std &= c39_std(t[i]);
	bars_start(s);
	bars(s, c39_tab[43], 10, 3);
	for (int i = 0; i < n; i++) {
		const char *p = std ? (const char[2]){ (char)t[i], 0 } : t[i] < 128 ? c39_full[t[i]] : "";

		if (!*p) {
			*err = "Code 39 takes ASCII 0\xe2\x80\x93" "127 only";
			return -1;
		}
		for (; *p; p++)
			if (bars(s, c39_tab[strchr(c39_set, *p) - c39_set], 10, 3) < 0) {
				*err = "Too long for Code 39";
				return -1;
			}
	}
	bars(s, c39_tab[43], 9, 3);
	s->w = bar_x;
	return 0;
}

static const char ean_l[10][5] = { "3211", "2221", "2122", "1411", "1132", "1231", "1114", "1312", "1213", "3112" };
static const char ean_g[10][5] = { "1123", "1222", "2212", "1141", "2311", "1321", "4111", "2131", "3121", "2113" };
static const char ean_par[10][7] = { "LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG", "LGGLLG", "LGGGLL",
				     "LGLGLG", "LGLGGL", "LGGLGL" };

/* EAN-13 and UPC-A (an EAN-13 starting with 0) */
static int make_ean(struct sym *s, const unsigned short *t, int n, int upc)
{
	char d[14];
	int k = 0;

	if (upc)
		d[k++] = '0';
	for (int i = 0; i < n && k < 12; i++)
		d[k++] = (char)t[i];
	d[12] = (char)('0' + ean_check((const unsigned short[12]){ d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
								   d[8], d[9], d[10], d[11] }, 12));
	d[13] = 0;
	bars_start(s);
	bars(s, "111", 3, 0);
	for (int i = 1; i <= 6; i++) {
		const char *w = ean_par[d[0] - '0'][i - 1] == 'L' ? ean_l[d[i] - '0'] : ean_g[d[i] - '0'];
		char sp[6] = { '0', w[0], w[1], w[2], w[3], 0 };	/* a bar of 0: space first */

		bars(s, sp, 5, 0);
	}
	bars(s, "011111", 6, 0);	/* the centre: space bar space bar space */
	for (int i = 7; i <= 12; i++)
		bars(s, ean_l[d[i] - '0'], 4, 0);
	bars(s, "111", 3, 0);
	s->w = bar_x;
	s->quiet = 11;
	strcpy(s->text, upc ? d + 1 : d);
	return 0;
}

static int make_itf(struct sym *s, const unsigned short *t, int n)
{
	static const char p[10][6] = { "11221", "21112", "12112", "22111", "11212", "21211", "12211", "11122",
				       "21121", "12121" };
	static char d[MAX_UNITS];
	int k = 0;

	if (n % 2)
		d[k++] = '0';
	for (int i = 0; i < n; i++)
		d[k++] = (char)t[i];
	d[k] = 0;
	bars_start(s);
	bars(s, "1111", 4, 0);
	for (int i = 0; i + 1 < k; i += 2)
		for (int j = 0; j < 5; j++) {
			char e[3] = { p[d[i] - '0'][j], p[d[i + 1] - '0'][j], 0 };

			bars(s, e, 2, 3);
		}
	bars(s, "211", 3, 3);
	s->w = bar_x;
	memcpy(s->text, d, SYM_TEXT);
	s->text[SYM_TEXT] = 0;
	return 0;
}

static int make_codabar(struct sym *s, const unsigned short *t, int n)
{
	int ends = n >= 2 && t[0] >= 'A' && t[0] <= 'D' && t[n - 1] >= 'A' && t[n - 1] <= 'D';

	bars_start(s);
	if (!ends)
		bars(s, coda_tab[16], 8, 3);	/* A */
	for (int i = 0; i < n; i++)
		bars(s, coda_tab[strchr(coda_set, t[i]) - coda_set], i == n - 1 && ends ? 7 : 8, 3);
	if (!ends)
		bars(s, coda_tab[16], 7, 3);
	s->w = bar_x;
	return 0;
}

/* ---- all ------------------------------------------------------------------- */

int sym_make(struct sym *s, int type, const unsigned short *t, int n, int ecc,
	     int box_w, int box_h, const char **err)
{
	const char *note;
	int r;

	s->type = type;
	s->row_h = 1;
	s->quiet = 10;
	s->ecc = s->size2 = s->shifted = s->latched = 0;
	s->bytes = n;
	s->text[0] = 0;
	s->w = s->h = 0;
	if (!n) {
		*err = "Type a text first";
		return -1;
	}
	if (sym_needs(type, t, n, &note) == NEED_BAD) {
		static char why[64];

		strcpy(why, sym_names[type]);
		strcat(why, ": ");
		strcat(why, note);
		*err = why;
		return -1;
	}
	switch (type) {
	case T_QR:
		r = make_qr(s, t, n, ecc, err);
		break;
	case T_DM:
		r = make_dm(s, t, n, err);
		break;
	case T_AZTEC:
		r = make_aztec(s, t, n, err);
		break;
	case T_PDF417:
		r = make_pdf417(s, t, n, box_w, box_h, err);
		break;
	case T_C128:
		r = make_c128(s, t, n, err);
		break;
	case T_C39:
		r = make_c39(s, t, n, err);
		break;
	case T_EAN13:
	case T_UPCA:
		r = make_ean(s, t, n, type == T_UPCA);
		break;
	case T_ITF:
		r = make_itf(s, t, n);
		break;
	default:
		r = make_codabar(s, t, n);
		break;
	}
	if (!r && !sym_is_2d(type) && !s->text[0]) {	/* what goes under it: the text */
		int k = 0;

		for (int i = 0; i < n && k < SYM_TEXT; i++)
			if (t[i] >= 32 && t[i] < 127)
				s->text[k++] = (char)t[i];
		s->text[k] = 0;
	}
	if (!r && s->w > SYM_BITS) {
		*err = "Too long";
		return -1;
	}
	return r;
}
