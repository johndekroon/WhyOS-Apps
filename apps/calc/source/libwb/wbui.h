/* SPDX-License-Identifier: MIT */
/*
 * Components of the canvas "Menu's met frontpaneel": faint edges, the
 * switch, the slider's buttons, icons scaled, the loupe, the pop-up list
 * (menu_popup) and the dialog with its keys at its bottom. Part of libwb:
 * include wb.h.
 */
#pragma once

/* A 1 px edge of colour c at alpha a around x, y, w, h (the canvas's
 * faint borders; box() draws its border opaque). */
static void edge_rect(int x, int y, int w, int h, uint32_t c, unsigned a)
{
	for (int i = 1; i < w - 1; i++) {
		put(x + i, y, c, a);
		put(x + i, y + h - 1, c, a);
	}
	for (int j = 0; j < h; j++) {
		put(x, y + j, c, j && j < h - 1 ? a : a / 2);
		put(x + w - 1, y + j, c, j && j < h - 1 ? a : a / 2);
	}
}


/* The canvas's switch: 52 x 30, cyan with a dark knob on the right when
 * on; a faint track with a grey knob on the left when off. */
static void wb_switch(int x, int cy, int on)
{
	int y = cy - 15;

	if (on) {
		box(x, y, 52, 30, 15, WB_COLOR_CYAN, 255, 0, 0, 0);
	} else {
		box(x, y, 52, 30, 15, 0xFFFFFF, 26, 0, 0, 0);
		edge_rect(x, y, 52, 30, 0xFFFFFF, 71);
	}
	box(on ? x + 26 : x + 4, y + 4, 22, 22, 11, on ? WB_COLOR_VOID : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
}


/* A square button of the slider row: 30 px, a faint fill and edge, the
 * glyph in the middle. */
static void step_button(int x, int cy, const char *g)
{
	box(x, cy - 15, 30, 30, 8, 0xFFFFFF, 15, 0, 0, 0);
	edge_rect(x, cy - 15, 30, 30, 0xFFFFFF, 46);
	text(&f_ui18, x + (30 - text_w(&f_ui18, g, 0)) / 2, text_top(&f_ui18, cy), g, WB_COLOR_TEXT_HI, 0);
}

/* A 22 px colour sample centred on cy: kind 1 colour c, 2 a rainbow. */
static void swatch(int x, int cy, int kind, uint32_t c)
{
	static const uint32_t hues[6] = { 0xff0000, 0xffc800, 0x00ff00,
					  0x00ffff, 0x0000ff, 0xff00c0 };

	if (kind == 1) {
		box(x, cy - 11, 22, 22, 6, c, 255, WB_COLOR_LINE, 1, 0);
		return;
	}
	for (int i = 0; i < 6; i++)
		fill(x + 1 + i * 20 / 6, cy - 10, 20 / 6 + 1, 20, hues[i]);
	box(x, cy - 11, 22, 22, 6, 0, 0, WB_COLOR_LINE, 1, 0);
}


/*
 * An icon scaled to size px (bilinear over its alpha): the app panel's
 * 52 px icons from the rows' 26 px, the home tiles' 60 px from 44 px. No
 * new pictures: the rootfs has little room left.
 */
static void icon_scaled(const struct wb_icon *ic, int x, int y, int size, uint32_t c)
{
	int j0 = fb_y0 - y > 0 ? fb_y0 - y : 0, j1 = fb_y1 - y < size ? fb_y1 - y : size;
	short sxs[64];

	if (size > 64)
		return;
	if (wb_rec) {
		int k[] = { 12, WB_P(ic), x, size, (int)c };

		wb_rec_rows(y, y + size, k, sizeof(k));
		return;
	}
	for (int i = 0; i < size; i++)
		sxs[i] = (short)((2 * i + 1) * ic->w * 128 / size - 128);
	for (int j = j0; j < j1; j++) {
		int sy = (2 * j + 1) * ic->h * 128 / size - 128, y0 = sy >> 8, fy = sy & 255;

		if (sy < 0)
			y0 = fy = 0;
		for (int i = 0; i < size; i++) {
			int sx = sxs[i], x0 = sx >> 8, fx = sx & 255;
			int x1, y1;
			unsigned a;

			if (sx < 0)
				x0 = fx = 0;
			x1 = x0 + 1 < ic->w ? x0 + 1 : x0;
			y1 = y0 + 1 < ic->h ? y0 + 1 : y0;
			a = ((ic->a[y0 * ic->w + x0] * (256 - fx) + ic->a[y0 * ic->w + x1] * fx) * (256 - fy) +
			     (ic->a[y1 * ic->w + x0] * (256 - fx) + ic->a[y1 * ic->w + x1] * fx) * fy) >> 16;
			put(x + i, y + j, c, a);
		}
	}
}


/* A ring of radius r and width wd (in 1/16 px) centred on cx, cy (1/16
 * px), anti-aliased: the search field's loupe, as box() would make it
 * square here. */
static void ring16(int cx, int cy, int r, int wd, uint32_t c)
{
	for (int py = (cy - r - wd) / 16 - 1; py <= (cy + r + wd) / 16 + 1; py++)
		for (int px = (cx - r - wd) / 16 - 1; px <= (cx + r + wd) / 16 + 1; px++) {
			int dx = px * 16 + 8 - cx, dy = py * 16 + 8 - cy;
			int d = isqrt((unsigned)(dx * dx + dy * dy)) - r;

			put(px, py, c, cover((d < 0 ? -d : d) - wd / 2));
		}
}

/* The loupe of a search field, 18 px, at x, y. */
static void loupe(int x, int y, uint32_t c)
{
	ring16(x * 16 + 8 * 16 - 8, y * 16 + 8 * 16 - 8, 5 * 16 + 4, 2 * 16 + 4, c);
	for (int i = 0; i < 5; i++) {	/* the handle */
		put(x + 12 + i, y + 12 + i, c, 255);
		put(x + 13 + i, y + 12 + i, c, 110);
		put(x + 12 + i, y + 13 + i, c, 110);
	}
}


#define DLG_X	POP_X
#define DLG_W	(W - 2 * DLG_X)

/* A pop-up's keys at its bottom, under a line from x0, w wide, at top
 * (the canvas: in the panel, not in the hint bar). 48 px with the line. */
static void panel_hints(int x0, int w, int top, const struct hint *hs, int nh)
{
	int hx = x0, cy = top + 1 + 10 + 11;

	fill(x0, top, w, 1, 0x26262F);
	for (int i = 0; i < nh; i++) {
		int ex = hs[i].right ? x0 + w - hint_w(&hs[i]) : hx;

		ex = keycap(ex, cy, hs[i].keys);
		ex = text(&f_ui16, ex + 8, text_top(&f_ui16, cy), hs[i].label, WB_COLOR_TEXT_MID, 0);
		if (!hs[i].right)
			hx = ex + 22;
	}
}

static int dlg_foot;		/* where the last dialog's keys go */

/* The keys of the dialog drawn last. */
static void dlg_hints(const struct hint *hs, int nh)
{
	panel_hints(DLG_X + 32, DLG_W - 64, dlg_foot, hs, nh);
}

/*
 * The design's dialog (Dialog.dc.html) over whatever was drawn: backdrop,
 * eyebrow, title, room for a body of body_h, then option rows (n > 0).
 * Callers draw their body at the returned y.
 */
static int dialog(const char *eb, const char *title, int body_h,
		  const char *const *labels, const struct wb_icon *const *ics,
		  int n, int focus, int danger)
{
	int gap = n && body_h ? 24 : 0;
	int h = 32 + 18 + 6 + 36 + 24 + body_h + gap + (n ? n * 68 + (n - 1) * 12 : 0) + 24 + 48;
	int y = (H - h) / 2, body = y + 32 + 18 + 6 + 36 + 24, by = body + body_h + gap;
	char tbuf[96];

	dlg_foot = y + h - 24 - 48 + 14;
	dim(WB_DIM);
	wb_panel(DLG_X, y, DLG_W, h, WB_RADIUS_LG);
	eyebrow(DLG_X + 32, y + 32, eb);
	text(&f_ui28b, DLG_X + 32, y + 32 + 18 + 6 + 2,
	     wb_fit(&f_ui28b, title, DLG_W - 64, tbuf, sizeof(tbuf)), WB_COLOR_TEXT_HI, 0);
	for (int i = 0; i < n; i++, by += 68 + 12) {
		int on = i == focus;

		wb_card(DLG_X + 32, by, DLG_W - 64, 68, WB_RADIUS_MD, on, &GLOW_FOCUS);
		/* the destructive choice in red (6.5: Dialog) */
		icon(ics[i], DLG_X + 32 + 20, by + (68 - 26) / 2,
		     i == danger ? WB_COLOR_ERROR : on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		text(&f_ui22, DLG_X + 32 + 20 + 26 + 16, text_top(&f_ui22, by + 34),
		     labels[i], i == danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
	}
	return body;
}

/*
 * A pop-up list (6.6; the canvas's Time zone): the square key's context
 * menu, or a choice from an enumeration, on a frosted panel over the
 * dimmed screen. Its eyebrow is the list it opens from ("GENERAL ›"), the
 * count on the title's line; a long one filters, in a field with a loupe;
 * the options are 48 px, the present one's box ticked, the focus cyan; the
 * keys at the bottom of the panel, not in the hint bar.
 */
#define PK_ROW		48
#define PK_GAP		4
#define PK_VIS		6

static void menu_popup(const char *eb_s, const char *title, const char *count,
		       const struct menu_item *items, const int *map, int n, int focus, int cur,
		       const char *filter, const char *filter_hint, const struct hint *hs, int nh)
{
	int ys[96], rh[96], yy = 0, vis, list_h, h, y, x0, w, top, scroll, cut;
	int ticks = cur >= -1;	/* -2: a menu, no choice */
	char buf[96], eb[48];

	if (n > 96)
		n = 96;
	for (int i = 0; i < n; i++) {
		const struct menu_item *it = &items[map ? map[i] : i];

		if (i && it->sep)
			yy += 9;
		ys[i] = yy;
		rh[i] = PK_ROW;
		yy += PK_ROW + PK_GAP;
	}
	vis = filter || n > PK_VIS ? PK_VIS : n ? n : 1;
	list_h = filter || n > PK_VIS ? vis * (PK_ROW + PK_GAP) - PK_GAP : n ? yy - PK_GAP : PK_ROW;
	h = 22 + 55 + 14 + (filter ? 44 + 14 : 0) + list_h + 14 + 1 + 10 + 23 + 16;
	y = (H - h) / 2;
	x0 = POP_X + 22;
	w = POP_W - 44;
	dim(WB_DIM);
	wb_panel(POP_X, y, POP_W, h, WB_RADIUS_LG);
	strncpy(eb, eb_s, sizeof(eb) - 5);
	eb[sizeof(eb) - 5] = 0;
	strcat(eb, " \xe2\x80\xba");
	text(&f_mono13, x0, y + 22, eb, WB_COLOR_CYAN, 21);
	text(&f_ui28b, x0, y + 22 + 17 + 4, wb_fit(&f_ui28b, title, w - 140, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, -4);
	if (count)
		text(&f_mono14, x0 + w - text_w(&f_mono14, count, 0), y + 22 + 55 - 20, count,
		     WB_COLOR_TEXT_MID, 0);
	top = y + 22 + 55 + 14;
	if (filter) {
		int tx = x0 + 14 + 18 + 10;

		box(x0, top, w, 44, 10, WB_COLOR_VOID, 115, 0, 0, 0);
		edge_rect(x0, top, w, 44, 0xFFFFFF, 41);
		loupe(x0 + 14, top + 13, WB_COLOR_TEXT_MID);
		if (filter[0])
			tx = text(&f_ui18, tx, text_top(&f_ui18, top + 22), filter, WB_COLOR_TEXT_HI, 0) + 2;
		else
			text(&f_ui18, tx + 6, text_top(&f_ui18, top + 22),
			     filter_hint, WB_COLOR_PLACEHOLDER, 0);
		fill(tx, top + 12, 2, 20, WB_COLOR_CYAN);
		top += 44 + 14;
	}
	if (!n)
		text(&f_ui18, x0 + 14, text_top(&f_ui18, top + PK_ROW / 2), "Nothing matches",
		     WB_COLOR_TEXT_LOW, 0);
	scroll = list_scroll(ys, rh, n, focus, focus, list_h, &cut);
	clip_rows(top - 16, top + cut + 16);	/* room for the focus's glow */
	for (int i = 0; i < n; i++) {
		const struct menu_item *it = &items[map ? map[i] : i];
		int ry = top + ys[i] - scroll, cy = ry + PK_ROW / 2, x = x0 + 14, rx = x0 + w - 14;
		int on = i == focus;

		if (ry + PK_ROW <= top || ry >= top + cut)
			continue;
		if (i && it->sep)
			fill(x0 + 8, ry - 7, w - 16, 1, 0x2C2C40);
		if (on)
			box_shaped(x0, ry, w, PK_ROW, 10, WB_COLOR_CYAN, 56, WB_COLOR_CYAN, 2, &GLOW_POP);
		if (ticks) {	/* the choice's box, ticked on the present one */
			if (i == cur) {
				box(x, cy - 12, 24, 24, 12, WB_COLOR_CYAN, 255, 0, 0, 0);
				tick(x + 4, cy, WB_COLOR_VOID);
			} else {
				edge_rect(x, cy - 12, 24, 24, 0xFFFFFF, 56);
				edge_rect(x + 1, cy - 11, 22, 22, 0xFFFFFF, 56);
			}
			x += 24 + 14;
		}
		if (it->value[0]) {
			rx -= text_w(&f_mono14, it->value, 0);
			text(&f_mono14, rx, text_top(&f_mono14, cy), it->value, WB_COLOR_TEXT_MID, 0);
			rx -= 16;
		}
		if (it->swatch) {
			menu_swatch(x, cy, it->swatch, it->sw_c);
			x += 22 + 14;
		}
		x = text(&f_ui20, x, text_top(&f_ui20, cy), wb_fit(&f_ui20, it->label, rx - x, buf, sizeof(buf)),
			 it->danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
		if (it->sub && x + 12 < rx)
			text(&f_ui16, x + 12, text_top(&f_ui16, cy), wb_fit(&f_ui16, it->sub, rx - x - 12, buf, sizeof(buf)),
			     WB_COLOR_TEXT_MID, 0);
	}
	clip_end();
	if (n && yy - PK_GAP > list_h)
		scrollbar(x0 + w + 9, top, list_h, yy - PK_GAP, list_h, scroll);
	/* the keys, under a line, in the panel */
	panel_hints(x0, w, top + list_h + 14, hs, nh);
}

/*
 * A screen on the scene: the rotation now, the figures in fb_y0..fb_y1, and
 * this frame marked as on the scene, so cards, the hint bar and pop-ups are
 * glass (and a pop-up blurs what is under it). Draw the rest after it. 1
 * when the badge turned since the last call: draw the whole screen (the
 * panel has turned the old picture meanwhile).
 */
static int wb_rot_hold;	/* drawing a recorded frame: keep its rotation */

static int wb_scene(void)
{
	static int last = -1;
	int turned;

	if (!wb_rot_hold)
		rot = rot_read();
	turned = rot != last;
	last = rot;
	scene_draw();
	scene_shown = 1;
	gb_ok = 0;
	return turned;
}

/*
 * A frame drawn in bands of band_h rows into buf and written out by out():
 * paint(arg) draws the screen, clipped to fb_y0..fb_y1 as libwb does. The
 * frame is recorded first (wb_rec) and only the bands that changed since
 * the last one are drawn and written; it returns how many. paint() runs
 * once more than the bands drawn, so it must draw and do nothing else.
 */
static int wb_bands(void (*paint)(void *), void *arg, uint16_t *buf, int band_h,
		    void (*out)(const uint16_t *band, int y0, int y1))
{
	int n = 0;

	wb_rec_begin();
	paint(arg);
	wb_rec_end();
	wb_rot_hold = 1;
	for (int y0 = 0; y0 < H; y0 += band_h) {
		int y1 = y0 + band_h < H ? y0 + band_h : H;

		if (!wb_rec_dirty(y0, y1))
			continue;
		/* row 0, outside the band (not as an index: UBSan) */
		fb = (uint16_t *)((uintptr_t)buf - (uintptr_t)y0 * W * sizeof(*buf));
		fb_y0 = y0;
		fb_y1 = y1;
		paint(arg);
		out(buf, y0, y1);
		n++;
	}
	wb_rot_hold = 0;
	wb_rec_done();
	return n;
}

/*
 * The same for a frame drawn in a whole buffer (1 MB; a program takes one
 * while a pop-up is open so that it is frosted): recorded, then only the
 * bands that changed drawn in it and written out, the pop-up's blur kept
 * while what is under it stays the same (wb_glass_panel). When that
 * changed, or with more than one pop-up, the frame is drawn whole.
 */
static int wb_frame(void (*paint)(void *), void *arg, uint16_t *buf, int band_h,
		    void (*out)(const uint16_t *band, int y0, int y1))
{
	uint32_t want_before = wb_gb_have;
	int n = 0, all;

	wb_rec_panels = 0;
	wb_rec_whole = 1;
	wb_rec_begin();
	paint(arg);
	wb_rec_end();
	wb_rec_whole = 0;
	all = wb_rec_all || wb_rec_panels > 1 || (wb_rec_panels && (!gb || wb_gb_want != want_before));
	wb_rot_hold = 1;
	wb_rec_drawing = 1;
	fb = buf;
	if (all) {
		fb_y0 = 0;
		fb_y1 = H;
		paint(arg);
	}
	for (int y0 = 0; y0 < H; y0 += band_h) {
		int y1 = y0 + band_h < H ? y0 + band_h : H;

		if (!all) {
			if (!wb_rec_dirty(y0, y1))
				continue;
			fb_y0 = y0;	/* fb holds the rest of the frame */
			fb_y1 = y1;
			fb_whole = 1;
			paint(arg);
			fb_whole = 0;
		}
		out(buf + y0 * W, y0, y1);
		n++;
	}
	fb_y0 = 0;
	fb_y1 = H;
	wb_rec_drawing = 0;
	wb_rot_hold = 0;
	wb_rec_done();
	return n;
}

/* Has the badge turned since wb_scene() drew? Poll it while waiting for a
 * key (four times a second is plenty) and draw again when it has. */
static int wb_scene_turned(void)
{
	return scene_shown && rot_read() != rot;
}

/*
 * A screen's eyebrow (mono, cyan: where it lies, "SETTINGS ›", or "APP")
 * and title (36 px), 54 px from the left with the eyebrow at 72, as the
 * canvas's lists; title_r (0: none) right of the title in text mid. Rows
 * start at WB_ROWS_TOP.
 */
#define WB_ROWS_TOP	160

static void wb_header(const char *eb, const char *title, const char *title_r)
{
	int rw = title_r && title_r[0] ? text_w(&f_ui20, title_r, 0) : 0;
	char buf[96];

	if (eb && eb[0])
		eyebrow(WB_CONTENT_PAD + 6, 72, eb);
	if (rw)
		text(&f_ui20, W - WB_CONTENT_PAD - rw, 72 + 18 + 6 + 12, title_r, WB_COLOR_TEXT_MID, 0);
	text(&f_ui36b, WB_CONTENT_PAD + 6, 72 + 18 + 6,
	     wb_fit(&f_ui36b, title, W - 2 * WB_CONTENT_PAD - 6 - (rw ? rw + 24 : 0), buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, -11);
}

/*
 * A list row as the canvas's (68 px; Settings, Connection): a card (glass
 * on the scene), the icon (cyan on the focus), the label (22 px) with an
 * optional second line (sub, 16 px), and on the right a value in mono and
 * what Enter does there: WB_ROW_NONE, WB_ROW_OPEN (a chevron) or
 * WB_ROW_SWITCH (the switch, sw on or off). x and w: the card, usually
 * WB_CONTENT_PAD and W - 2 * WB_CONTENT_PAD.
 */
enum { WB_ROW_NONE, WB_ROW_OPEN, WB_ROW_SWITCH };

static void wb_row(int x, int y, int w, int h, const struct wb_icon *ic, const char *label,
		   const char *sub, const char *value, int right, int sw, int on)
{
	int top = y + h / 2, rx = x + w - 18, lx = x + 20 + (ic ? 26 + 16 : 0);
	int ly = sub && sub[0] ? y + h / 2 - 11 : top;
	uint32_t ic_c = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	char buf[96];

	wb_card(x, y, w, h, WB_RADIUS_MD, on, &GLOW_FOCUS);
	if (ic)
		icon(ic, x + 20, top - ic->h / 2, ic_c, 255);
	if (right == WB_ROW_OPEN) {
		icon(&r_chevron, rx - 20, top - 10, ic_c, 255);
		rx -= 20 + 16;
	} else if (right == WB_ROW_SWITCH) {
		wb_switch(rx - 52, top, sw);
		rx -= 52 + 16;
	}
	if (value && value[0]) {
		const char *v = wb_fit(&f_mono14, value, (rx - lx) / 2, buf, sizeof(buf));

		rx -= text_w(&f_mono14, v, 0);
		text(&f_mono14, rx, text_top(&f_mono14, top), v, on ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
		rx -= 16;
	}
	text(&f_ui22, lx, text_top(&f_ui22, ly), wb_fit(&f_ui22, label, rx - lx, buf, sizeof(buf)),
	     WB_COLOR_TEXT_HI, 0);
	if (sub && sub[0])
		text(&f_ui16, lx, text_top(&f_ui16, ly + 14 + 2 + 10), wb_fit(&f_ui16, sub, rx - lx, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
}
