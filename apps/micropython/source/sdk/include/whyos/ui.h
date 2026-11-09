/* SPDX-License-Identifier: MIT */
/*
 * whyos/ui.h: the design system's parts that libwb does not draw by
 * itself: the key bar of an app (the circle, clover and diamond small on
 * the right, then Esc), a text line with its cursor, and a card of log
 * lines. Part of the WhyOS SDK: include whyos.h. The app layer (app.h)
 * puts these and libwb's rows and pop-ups together into whole screens.
 */
#pragma once

/* the words under the circle, clover and diamond in an app's key bar */
static const char *const wos_sym_words[3] = { "log", "settings", "help" };

static int wos_syms_w(void)
{
	return 12 + 6 + text_w(&f_mono13, "settings", 0);
}

/*
 * An app's key bar (the design system, 6.8): hs[0..n) on the left in the
 * order given (Enter first, then the symbol keys, then others), the
 * circle, clover and diamond stacked small with their words (variant G),
 * a thin line, and Esc with its word on the right. What does not fit
 * drops out from the end, but for Enter's "open", which goes first: the
 * help lists every key. syms 0 leaves the three out (a pop-up's own keys).
 */
static void wos_keybar(const struct hint *hs, int n, const char *esc, int syms)
{
	struct hint all[12];
	int k = 0, cy = H - WB_HINT_BAR_H / 2, ex, sx, room;
	struct hint e = { "Esc", esc, 1 };

	ex = W - WB_CHROME_PAD - hint_w(&e) - 20;
	sx = ex - 20 - wos_syms_w();
	room = (syms ? sx : ex) - 20 - WB_CHROME_PAD;
	for (int i = 0; i < n && k < 11; i++)
		all[k++] = hs[i];
	for (;;) {
		int w = 0;

		for (int i = 0; i < k; i++)
			w += hint_w(&all[i]) + (i ? WB_SPACE_4 : 0);
		if (w <= room || !k)
			break;
		if (!strcmp(all[0].keys, "Enter") && !strcmp(all[0].label, "open")) {
			memmove(all, all + 1, sizeof(all[0]) * (size_t)--k);
			continue;
		}
		k--;
	}
	all[k++] = e;
	hint_bar(all, k);
	if (!syms)
		return;
	fill(ex, cy - 16, 1, 32, WB_COLOR_LINE);
	for (int i = 0; i < 3; i++) {
		int yc = cy - 16 + i * 16;

		icon(keycap_syms[3 + i], sx, yc - 6, key_colors[3 + i], 255);
		text(&f_mono13, sx + 18, text_top(&f_mono13, yc), wos_sym_words[i], WB_COLOR_TEXT_MID, 0);
	}
}

/* the width of s[a..b) in f */
static int wos_span_w(const struct wb_font *f, const char *s, int a, int b)
{
	char buf[256];
	int n = b - a < (int)sizeof(buf) - 1 ? b - a : (int)sizeof(buf) - 1;

	memcpy(buf, s + a, (size_t)n);
	buf[n] = 0;
	return text_w(f, buf, 0);
}

/*
 * A line being typed, with its cursor (cur bytes in), in f, from x to
 * x + w, its middle at cy; right-aligned when right is set. What does not
 * fit is cut at the far end from the cursor, with an ellipsis. hint shows
 * in the placeholder colour while the line is empty.
 */
static void wos_line(const struct wb_font *f, int x, int w, int cy, const char *s, int cur,
		     int right, const char *hint, uint32_t c)
{
	int len = (int)strlen(s), a = 0, b = len, tx, cx, top = text_top(f, cy);
	int dots = text_w(f, "\xe2\x80\xa6", 0);
	char vis[256];

	while (a < cur && wos_span_w(f, s, a, b) > w - dots)
		a++;
	while (b > cur && wos_span_w(f, s, a, b) > w - dots)
		b--;
	memcpy(vis, s + a, (size_t)(b - a < 255 ? b - a : 255));
	vis[b - a < 255 ? b - a : 255] = 0;
	tx = right ? x + w - text_w(f, vis, 0) : x;
	if (!len && hint) {
		text(f, right ? x + w - 10 - text_w(f, hint, 0) : x + 10, top, hint, WB_COLOR_PLACEHOLDER, 0);
	}
	if (a > 0)
		text(f, tx - dots, top, "\xe2\x80\xa6", WB_COLOR_PLACEHOLDER, 0);
	text(f, tx, top, vis, c, 0);
	if (b < len && !right)
		text(f, tx + text_w(f, vis, 0), top, "\xe2\x80\xa6", WB_COLOR_PLACEHOLDER, 0);
	cx = tx + wos_span_w(f, s, a, cur);
	fill(cx, top + 2, 2, f->ascent + f->descent - 2, WB_COLOR_CYAN);
}

/*
 * Lines in a card from top to bottom (h px), the newest at the bottom, as
 * many as fit; back lines up from the newest. line(i) gives the i-th from
 * the oldest, n of them.
 */
#define WOS_LOG_ROW	44

static void wos_lines_card(int top, int h, int n, int back, const char *(*line)(int i))
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, fit = (h - 16) / WOS_LOG_ROW;
	int last = n - 1 - back, first = last - fit + 1;
	char buf[WOS_LOG_LEN + 4];

	if (first < 0)
		first = 0;
	if (last < first)
		return;
	wb_card(x, top, w, (last - first + 1) * WOS_LOG_ROW + 16, WB_SQUARE, 0, 0);
	for (int i = first; i <= last; i++) {
		int cy = top + 8 + (i - first) * WOS_LOG_ROW + WOS_LOG_ROW / 2;

		if (i > first)
			fill(x + 20, cy - WOS_LOG_ROW / 2, w - 40, 1, 0x26262F);
		text(&f_mono16b, x + 20, text_top(&f_mono16b, cy), wb_fit(&f_mono16b, line(i), w - 40, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
	}
	if (n > fit)
		scrollbar(x + w + 9, top, h, n * WOS_LOG_ROW, fit * WOS_LOG_ROW, (first) * WOS_LOG_ROW);
}
