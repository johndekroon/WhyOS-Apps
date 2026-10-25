// SPDX-License-Identifier: MIT
/*
 * imgview — shows photos and pictures on the badge's panel, for Files
 * (docs/HOME.md): JPEG and PNG, fitted to the screen, the others in the
 * same folder with the arrow keys.
 *
 *   imgview FILE
 *
 * A phone photo of 12 megapixels is 48 MB decoded, and all programs share
 * a 13 MB pool. So no picture is ever held at full size: libjpeg decodes
 * at a scale of N/8 just above the screen's, and both formats are then
 * averaged down to it row by row as they are read (an area filter). Only
 * the result, at most the screen (or twice it, zoomed), is kept. JPEGs are
 * turned upright from their EXIF orientation. Interlaced PNGs cannot be
 * read row by row; those are decoded whole, up to IL_MAX.
 *
 * Keys: Left/Right the previous/next picture, Enter zoom 2x (then the
 * arrows move it), Esc closes; the logo key is apprun's. The symbol keys
 * as the design has them (Docs/06-design-system.md 6.6): the diamond the
 * help, the circle and cloud the log and settings, which a viewer does not
 * have (a screen that says so); the left three do nothing.
 * Drawn as the menu (wbdraw.h): status bar with the name, hint bar.
 *
 * Built with -DIMGVIEW_HOST it decodes FILE into OUT.ppm on the host
 * instead (the test in sdapps/imgview/test).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>
#include <png.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wbdraw.h"
#include "keyrep.h"

#ifndef IMGVIEW_HOST
#include <poll.h>
#endif

#define VIEW_Y		WB_STATUS_BAR_H
#define VIEW_H		(H - WB_STATUS_BAR_H - WB_HINT_BAR_H)
#define ZOOM_MAX_PX	(1600 * 1000)	/* zoomed picture: at most 3.2 MB */
#define IL_MAX		(3u << 20)	/* interlaced PNG, decoded whole */
#define MAX_PICS	512

#define KEY_ESC		1
#define KEY_ENTER	28
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108

/* ---- the picture: RGB565, fitted --------------------------------------- */

static struct {
	uint16_t *px;
	int w, h;			/* as shown */
	int src_w, src_h;		/* as in the file, upright */
	int zoom, x, y;			/* zoom 2: the part shown starts at x, y */
	char err[96];
} P;

static inline uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/*
 * Area filter: source rows of sw RGB888 pixels come in one by one and are
 * averaged into a dw x dh RGB565 picture. Only down: fit() never makes a
 * picture larger than it is, so every dest pixel gets one source pixel or
 * more.
 */
static struct {
	int sw, sh, dw, dh, sy, dy;
	uint32_t *acc;			/* dw x 3 sums of the dest row being built */
	uint32_t n;			/* source rows in it */
	uint16_t *out;
} F;

static int filt_start(int sw, int sh, int dw, int dh, uint16_t *out)
{
	F.sw = sw; F.sh = sh; F.dw = dw; F.dh = dh;
	F.sy = F.dy = 0;
	F.n = 0;
	F.out = out;
	F.acc = calloc((size_t)dw * 3, sizeof(uint32_t));
	return F.acc ? 0 : -1;
}

static void filt_flush(void)
{
	uint16_t *o = F.out + (size_t)F.dy * F.dw;

	for (int x = 0; x < F.dw; x++) {
		int x0 = (int)((long long)x * F.sw / F.dw), x1 = (int)((long long)(x + 1) * F.sw / F.dw);
		uint32_t c = (uint32_t)(x1 - x0) * F.n;

		o[x] = rgb565(F.acc[3 * x] / c, F.acc[3 * x + 1] / c, F.acc[3 * x + 2] / c);
	}
	memset(F.acc, 0, (size_t)F.dw * 3 * sizeof(uint32_t));
	F.n = 0;
}

static void filt_row(const uint8_t *rgb)
{
	int dy = (int)((long long)F.sy * F.dh / F.sh);

	if (dy != F.dy) {
		filt_flush();
		F.dy = dy;
	}
	for (int x = 0, sx = 0; x < F.dw; x++) {
		int x1 = (int)((long long)(x + 1) * F.sw / F.dw);
		uint32_t *a = &F.acc[3 * x];

		for (; sx < x1; sx++) {
			a[0] += rgb[3 * sx];
			a[1] += rgb[3 * sx + 1];
			a[2] += rgb[3 * sx + 2];
		}
	}
	F.n++;
	F.sy++;
}

static void filt_end(void)
{
	if (F.n)
		filt_flush();
	free(F.acc);
	F.acc = 0;
}

/* The size that fits w x h into bw x bh, never larger than the picture. */
static void fit(int w, int h, int bw, int bh, int *ow, int *oh)
{
	if ((long long)w * bh > (long long)h * bw) {
		*ow = w < bw ? w : bw;
		*oh = (int)((long long)h * *ow / w);
	} else {
		*oh = h < bh ? h : bh;
		*ow = (int)((long long)w * *oh / h);
	}
	if (*ow < 1) *ow = 1;
	if (*oh < 1) *oh = 1;
}

static void target(int w, int h, int *dw, int *dh)
{
	fit(w, h, W, VIEW_H, dw, dh);
	if (P.zoom) {
		int zw = *dw * 2, zh = *dh * 2;

		while ((long long)zw * zh > ZOOM_MAX_PX)
			zw = zw * 9 / 10, zh = zh * 9 / 10;
		fit(w, h, zw, zh, dw, dh);
	}
}

/* ---- orientation (EXIF tag 0x0112) ------------------------------------- */

static int exif_orientation(const uint8_t *d, unsigned n)
{
	int le;
	unsigned ifd, cnt;

	if (n < 14 || memcmp(d, "Exif\0\0", 6))
		return 1;
	d += 6;
	n -= 6;
	le = d[0] == 'I';
#define U16(p) (le ? (unsigned)(p)[0] | (unsigned)(p)[1] << 8 : (unsigned)(p)[1] | (unsigned)(p)[0] << 8)
#define U32(p) (le ? (unsigned)(p)[0] | (unsigned)(p)[1] << 8 | (unsigned)(p)[2] << 16 | (unsigned)(p)[3] << 24 \
		   : (unsigned)(p)[3] | (unsigned)(p)[2] << 8 | (unsigned)(p)[1] << 16 | (unsigned)(p)[0] << 24)
	ifd = U32(d + 4);
	if (ifd + 2 > n)
		return 1;
	cnt = U16(d + ifd);
	for (unsigned i = 0; i < cnt && ifd + 2 + 12 * (i + 1) <= n; i++) {
		const uint8_t *e = d + ifd + 2 + 12 * i;

		if (U16(e) == 0x0112) {
			unsigned v = U16(e + 8);

			return v >= 1 && v <= 8 ? (int)v : 1;
		}
	}
#undef U16
#undef U32
	return 1;
}

/* Turn the decoded picture upright: 3 = 180, 6 = 90 clockwise, 8 = 270. */
static int orient(int o)
{
	uint16_t *n;
	int w = P.w, h = P.h;

	if (o == 3) {
		for (long i = 0, j = (long)w * h - 1; i < j; i++, j--) {
			uint16_t t = P.px[i];

			P.px[i] = P.px[j];
			P.px[j] = t;
		}
		return 0;
	}
	if (o != 6 && o != 8)
		return 0;
	n = malloc((size_t)w * h * 2);
	if (!n)
		return -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			n[o == 6 ? (size_t)x * h + (h - 1 - y) : (size_t)(w - 1 - x) * h + y] =
				P.px[(size_t)y * w + x];
	free(P.px);
	P.px = n;
	P.w = h;
	P.h = w;
	return 0;
}

/* ---- JPEG --------------------------------------------------------------- */

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
};

static void jpeg_fail(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static int load_jpeg(FILE *f)
{
	struct jpeg_decompress_struct c;
	struct jerr e;
	uint8_t *row = 0;
	volatile int o = 1;	/* set before setjmp can return twice */
	int dw, dh, uw, uh, sideways;

	c.err = jpeg_std_error(&e.pub);
	e.pub.error_exit = jpeg_fail;
	if (setjmp(e.jb)) {
		jpeg_destroy_decompress(&c);
		free(row);
		if (!P.err[0])
			snprintf(P.err, sizeof(P.err), "Not a JPEG this viewer can read");
		return -1;
	}
	jpeg_create_decompress(&c);
	jpeg_stdio_src(&c, f);
	jpeg_save_markers(&c, JPEG_APP0 + 1, 0xffff);
	jpeg_read_header(&c, TRUE);
	for (jpeg_saved_marker_ptr m = c.marker_list; m; m = m->next)
		if (m->marker == JPEG_APP0 + 1)
			o = exif_orientation(m->data, m->data_length);
	sideways = o >= 5;
	uw = (int)(sideways ? c.image_height : c.image_width);
	uh = (int)(sideways ? c.image_width : c.image_height);
	P.src_w = uw;
	P.src_h = uh;
	target(uw, uh, &dw, &dh);
	if (sideways) {		/* decode as stored, turn afterwards */
		int t = dw;

		dw = dh;
		dh = t;
	}
	/* the smallest N/8 that is still at least the target */
	c.scale_denom = 8;
	c.scale_num = 8;
	for (int num = 1; num <= 8; num++)
		if ((long long)c.image_width * num / 8 >= dw && (long long)c.image_height * num / 8 >= dh) {
			c.scale_num = (unsigned)num;
			break;
		}
	c.out_color_space = JCS_RGB;
	c.dct_method = JDCT_IFAST;
	jpeg_start_decompress(&c);
	P.w = dw;
	P.h = dh;
	P.px = malloc((size_t)dw * dh * 2);
	row = malloc((size_t)c.output_width * 3);
	if (!P.px || !row || filt_start((int)c.output_width, (int)c.output_height, dw, dh, P.px) < 0) {
		snprintf(P.err, sizeof(P.err), "Not enough memory for this picture");
		longjmp(e.jb, 1);
	}
	while (c.output_scanline < c.output_height) {
		JSAMPROW r = row;

		jpeg_read_scanlines(&c, &r, 1);
		filt_row(row);
	}
	filt_end();
	jpeg_finish_decompress(&c);
	jpeg_destroy_decompress(&c);
	free(row);
	if (orient(o) < 0) {
		snprintf(P.err, sizeof(P.err), "Not enough memory to turn this picture");
		return -1;
	}
	return 0;
}

/* ---- PNG ---------------------------------------------------------------- */

static int load_png(FILE *f)
{
	png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
	png_infop info = p ? png_create_info_struct(p) : 0;
	uint8_t *row = 0, *whole = 0;
	png_uint_32 w, h;
	int depth, type, il, dw, dh;
	png_color_16 bg = { 0, 0x0a, 0x0a, 0x14, 0 };	/* transparency over WB_COLOR_VOID */

	if (!info) {
		snprintf(P.err, sizeof(P.err), "Not enough memory");
		return -1;
	}
	if (setjmp(png_jmpbuf(p))) {
		png_destroy_read_struct(&p, &info, 0);
		free(row);
		free(whole);
		if (!P.err[0])
			snprintf(P.err, sizeof(P.err), "Not a PNG this viewer can read");
		return -1;
	}
	png_init_io(p, f);
	png_read_info(p, info);
	png_get_IHDR(p, info, &w, &h, &depth, &type, &il, 0, 0);
	png_set_expand(p);
	png_set_strip_16(p);
	png_set_gray_to_rgb(p);
	png_set_background(p, &bg, PNG_BACKGROUND_GAMMA_SCREEN, 0, 1.0);
	png_read_update_info(p, info);
	P.src_w = (int)w;
	P.src_h = (int)h;
	target((int)w, (int)h, &dw, &dh);
	P.w = dw;
	P.h = dh;
	P.px = malloc((size_t)dw * dh * 2);
	if (!P.px || filt_start((int)w, (int)h, dw, dh, P.px) < 0) {
		snprintf(P.err, sizeof(P.err), "Not enough memory for this picture");
		png_error(p, "oom");
	}
	if (il != PNG_INTERLACE_NONE) {
		png_bytep *rows;

		if ((unsigned long long)w * h * 3 > IL_MAX) {
			snprintf(P.err, sizeof(P.err), "Interlaced PNG too large (%ux%u)", (unsigned)w, (unsigned)h);
			png_error(p, "too large");
		}
		whole = malloc((size_t)w * h * 3);
		rows = malloc(h * sizeof(*rows));
		if (!whole || !rows) {
			free(rows);
			snprintf(P.err, sizeof(P.err), "Not enough memory for this picture");
			png_error(p, "oom");
		}
		for (png_uint_32 y = 0; y < h; y++)
			rows[y] = whole + (size_t)y * w * 3;
		png_read_image(p, rows);
		free(rows);
		for (png_uint_32 y = 0; y < h; y++)
			filt_row(whole + (size_t)y * w * 3);
	} else {
		row = malloc((size_t)w * 3);
		if (!row) {
			snprintf(P.err, sizeof(P.err), "Not enough memory for this picture");
			png_error(p, "oom");
		}
		for (png_uint_32 y = 0; y < h; y++) {
			png_read_row(p, row, 0);
			filt_row(row);
		}
	}
	filt_end();
	png_read_end(p, 0);
	png_destroy_read_struct(&p, &info, 0);
	free(row);
	free(whole);
	return 0;
}

static int is_image(const char *name)
{
	const char *x = strrchr(name, '.');

	return x && (!strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg") || !strcasecmp(x, ".png"));
}

static int load(const char *path)
{
	FILE *f;
	uint8_t sig[8];
	int rc;

	free(P.px);
	free(F.acc);
	F.acc = 0;
	P.px = 0;
	P.w = P.h = P.src_w = P.src_h = 0;
	P.x = P.y = 0;
	P.err[0] = 0;
	f = fopen(path, "rb");
	if (!f) {
		snprintf(P.err, sizeof(P.err), "Cannot open it: %s", strerror(errno));
		return -1;
	}
	rc = (int)fread(sig, 1, 8, f) == 8 ? 0 : -1;
	rewind(f);
	if (rc == 0 && !png_sig_cmp(sig, 0, 8))
		rc = load_png(f);
	else if (rc == 0 && sig[0] == 0xff && sig[1] == 0xd8)
		rc = load_jpeg(f);
	else {
		snprintf(P.err, sizeof(P.err), "Not a JPEG or PNG picture");
		rc = -1;
	}
	fclose(f);
	if (rc < 0) {
		free(P.px);
		P.px = 0;
	}
	return rc;
}

/* ---- the folder's pictures ---------------------------------------------- */

static char dir[256], *pics[MAX_PICS];
static int n_pics, cur;

static int cmp(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void scan(const char *file)
{
	const char *slash = strrchr(file, '/'), *base = slash ? slash + 1 : file;
	DIR *d;
	struct dirent *e;

	snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - file) : 1, slash ? file : ".");
	d = opendir(dir);
	while (d && (e = readdir(d)) && n_pics < MAX_PICS)
		if (e->d_name[0] != '.' && is_image(e->d_name))
			pics[n_pics++] = strdup(e->d_name);
	if (d)
		closedir(d);
	qsort(pics, (size_t)n_pics, sizeof(pics[0]), cmp);
	cur = -1;
	for (int i = 0; i < n_pics; i++)
		if (!strcmp(pics[i], base))
			cur = i;
	if (cur < 0 && n_pics < MAX_PICS) {	/* not a JPEG/PNG name: show it anyway */
		memmove(pics + 1, pics, sizeof(pics[0]) * (size_t)n_pics);
		pics[0] = strdup(base);
		n_pics++;
		cur = 0;
	}
}

static void path_of(int i, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", dir, pics[i]);
}

/* ---- drawing ------------------------------------------------------------ */

static uint16_t band[W * 40];

enum { SC_VIEW, SC_LOG, SC_SETTINGS };
static int sc, help;

static void draw_help(void)
{
	static const struct help_key keys[] = {
		{ "\xe2\x86\x90\xe2\x86\x92", "Previous or next picture in the folder" },
		{ "Enter", "Zoom in 2x, or fit the screen again" },
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "Zoomed in: move the picture" },
		{ "Esc", "Close" },
		{ "#6", "This help" },
		{ "Logo", "Back to the menu" },
	};

	help_popup("Picture viewer", "The pictures in this folder, JPEG and PNG, fitted to the screen.",
		   keys, sizeof(keys) / sizeof(keys[0]));
}

static void draw_band(int y0, int y1)
{
	static const struct hint hints[] = {
		{ "\xe2\x86\x90 \xe2\x86\x92", "previous / next", 0 }, { "Enter", "zoom", 0 },
		{ "Esc", "close", 1 },
	};
	static const struct hint zoomed[] = {
		{ "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "move", 0 }, { "Enter", "fit", 0 },
		{ "Esc", "close", 1 },
	};
	char right[64], name[96];
	int ox, oy;

	if (sc != SC_VIEW) {
		empty_screen("PICTURE VIEWER", sc == SC_LOG ? "Log" : "Settings",
			     sc == SC_LOG ? "No log yet." : "This app has no settings.");
		if (help)
			draw_help();
		return;
	}
	fill(0, y0, W, y1 - y0, WB_COLOR_VOID);
	/* the picture */
	if (P.px) {
		int vw = P.w < W ? P.w : W, vh = P.h < VIEW_H ? P.h : VIEW_H;

		ox = (W - vw) / 2;
		oy = VIEW_Y + (VIEW_H - vh) / 2;
		for (int y = y0 > oy ? y0 : oy; y < y1 && y < oy + vh; y++)
			memcpy(&fb[y * W + ox], &P.px[(size_t)(y - oy + P.y) * P.w + P.x], (size_t)vw * 2);
	} else {
		const char *m = P.err[0] ? P.err : "Loading\xe2\x80\xa6";

		text(&f_ui22, (W - text_w(&f_ui22, m, 0)) / 2, VIEW_Y + VIEW_H / 2 - 12, m,
		     P.err[0] ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	}
	/* status bar: name; position and size */
	snprintf(right, sizeof(right), "%d / %d", cur + 1, n_pics);
	if (P.src_w)
		snprintf(right + strlen(right), sizeof(right) - strlen(right), "   %d x %d",
			 P.src_w, P.src_h);
	snprintf(name, sizeof(name), "%s", n_pics ? pics[cur] : "");
	while (strlen(name) > 4 && text_w(&f_ui18, name, 0) > W - 2 * WB_CHROME_PAD - 24 -
	       text_w(&f_mono14, right, 0))
		strcpy(name + strlen(name) - 4, "\xe2\x80\xa6");
	text(&f_ui18, WB_CHROME_PAD, text_top(&f_ui18, WB_STATUS_BAR_H / 2), name, WB_COLOR_TEXT_HI, 0);
	text(&f_mono14, W - WB_CHROME_PAD - text_w(&f_mono14, right, 0),
	     text_top(&f_mono14, WB_STATUS_BAR_H / 2), right, WB_COLOR_TEXT_MID, 0);
	hint_bar(P.zoom ? zoomed : hints, 3);
	if (help)
		draw_help();
}

#ifdef IMGVIEW_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif

static void redraw(void)
{
	for (int y = 0; y < H; y += 40) {
		int y1 = y + 40 > H ? H : y + 40;

		fb = band - (long)y * W;
		fb_y0 = y;
		fb_y1 = y1;
		draw_band(y, y1);
#ifdef IMGVIEW_HOST
		memcpy(screen + y * W, band, (size_t)(y1 - y) * W * 2);
#else
		pwrite(fb_fd, band, (size_t)(y1 - y) * W * 2, (off_t)y * W * 2);
#endif
	}
}

static void show(int i)
{
	char path[600];

	cur = i;
	free(P.px);
	P.px = 0;
	P.err[0] = 0;
	P.src_w = 0;
	redraw();		/* "Loading..." while it decodes */
	path_of(i, path, sizeof(path));
	if (load(path) < 0)	/* also in apprun's log */
		fprintf(stderr, "imgview: %s: %s\n", path, P.err);
	if (P.zoom) {		/* start in the middle */
		P.x = P.w > W ? (P.w - W) / 2 : 0;
		P.y = P.h > VIEW_H ? (P.h - VIEW_H) / 2 : 0;
	}
	redraw();
}

static void pan(int dx, int dy)
{
	int mx = P.w > W ? P.w - W : 0, my = P.h > VIEW_H ? P.h - VIEW_H : 0;

	P.x = P.x + dx < 0 ? 0 : P.x + dx > mx ? mx : P.x + dx;
	P.y = P.y + dy < 0 ? 0 : P.y + dy > my ? my : P.y + dy;
}

#ifdef IMGVIEW_HOST
/* imgview-host FILE OUT.ppm [zoom]: the screen after showing FILE; prints
 * the picture's size as shown and in the file. */
int main(int argc, char **argv)
{
	FILE *o;

	if (argc < 3)
		return 2;
	P.zoom = argc > 3;
	scan(argv[1]);
	show(cur);
	o = fopen(argv[2], "wb");
	fprintf(o, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];
		unsigned char c[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2),
				       (unsigned char)((p & 31) << 3) };

		fwrite(c, 3, 1, o);
	}
	fclose(o);
	printf("shown %d %d\nsource %d %d\nindex %d of %d\nerror %s\n", P.w, P.h, P.src_w, P.src_h,
	       cur + 1, n_pics, P.err);
	return 0;
}
#else

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

int main(int argc, char **argv)
{
	int ev;

	if (argc < 2)
		return 2;
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev < 0)
		return 1;
	scan(argv[1]);
	if (!n_pics)
		return 1;
	show(cur);
	for (;;) {
		struct ev e;

		if (read(ev, &e, sizeof(e)) != sizeof(e))
			break;
		if (e.type != 1 || !key_acts(e.code, e.value))
			continue;
		if (e.value == 2 && key_stale(e.sec, e.usec))
			continue;	/* fell behind a held arrow: skip what waited */
		if (help) {		/* only an explanation: Esc or the diamond closes it */
			if (e.code == KEY_ESC || e.code == KEY_SYM_HELP) {
				help = 0;
				redraw();
			}
			continue;
		}
		if (e.code == KEY_SYM_HELP || e.code == KEY_SYM_LOG || e.code == KEY_SYM_SETTINGS) {
			if (e.code == KEY_SYM_HELP)
				help = 1;
			else
				sc = e.code == KEY_SYM_LOG ? SC_LOG : SC_SETTINGS;
			redraw();
			continue;
		}
		if (sc != SC_VIEW) {	/* the log or settings screen: Esc back */
			if (e.code == KEY_ESC) {
				sc = SC_VIEW;
				redraw();
			}
			continue;
		}
		if (e.code == KEY_ESC)
			break;
		if (e.code == KEY_ENTER) {
			P.zoom = !P.zoom;
			show(cur);
		} else if (P.zoom && P.px) {
			int step = 120;

			pan(e.code == KEY_LEFT ? -step : e.code == KEY_RIGHT ? step : 0,
			    e.code == KEY_UP ? -step : e.code == KEY_DOWN ? step : 0);
			redraw();
		} else if ((e.code == KEY_LEFT || e.code == KEY_RIGHT) && n_pics > 1) {
			show((cur + (e.code == KEY_LEFT ? n_pics - 1 : 1)) % n_pics);
		}
	}
	return 0;
}
#endif
