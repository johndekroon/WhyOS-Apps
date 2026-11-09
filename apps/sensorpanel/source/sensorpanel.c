// SPDX-License-Identifier: GPL-2.0-only
/*
 * sensorpanel — the badge's sensors, live (sdapps/README.md), on the
 * WhyOS SDK (sdk/README.md): a canvas over the frontpanel's figures with
 * the temperature, humidity and pressure (BME680), the acceleration
 * (BMI270) and a graph of the last 20 temperatures, read as often as the
 * sensors answer: up to three times a second, about once on the badge,
 * where the BME680 takes 0.6 s for its three values. A sensor that is
 * missing shows a dash and its name.
 */
#include "whyos.h"

#define SAMPLES		20
#define CARD_X		WB_CONTENT_PAD
#define CARD_W		(W - 2 * WB_CONTENT_PAD)
#define GAP		16
#define ROW1_H		132			/* temperature, humidity, pressure */
#define ROW2_H		112			/* motion */

static struct wos_env env;
static struct wos_accel acc;
static int have_env, have_acc;
static long hist[SAMPLES];		/* centi-°C, the oldest first */
static int n_hist;

static void sample(void)
{
	have_env = !wos_env_read(&env);
	have_acc = !wos_accel_read(&acc);
	if (!have_env)
		return;
	if (n_hist == SAMPLES)
		memmove(hist, hist + 1, sizeof(hist[0]) * (SAMPLES - 1));
	else
		n_hist++;
	hist[n_hist - 1] = env.temp_c100;
}

/* a card's caption: mono, upper case, tracked */
static void caption(int x, int y, const char *s)
{
	text(&f_mono13, x, y, s, WB_COLOR_TEXT_MID, 21);
}

/* a big value with its unit on the same baseline; a dash and the sensor's
 * name when it is not there */
static void reading(int x, int y, const char *v, const char *unit, const char *sensor)
{
	if (!v) {
		int e = text(&f_ui36b, x, y, "\xe2\x80\x93", WB_COLOR_TEXT_LOW, -11);

		text(&f_ui16, e + 12, y + f_ui36b.ascent - f_ui16.ascent, sensor, WB_COLOR_TEXT_LOW, 0);
		return;
	}
	x = text(&f_ui36b, x, y, v, WB_COLOR_TEXT_HI, -11);
	text(&f_ui20, x + 6, y + f_ui36b.ascent - f_ui20.ascent, unit, WB_COLOR_TEXT_MID, 0);
}

static void env_card(int x, int y, int w, const char *label, long v, int digits, const char *unit)
{
	char b[24] = "";

	wb_card(x, y, w, ROW1_H, WB_RADIUS_MD, 0, 0);
	caption(x + 20, y + 22, label);
	reading(x + 20, y + ROW1_H - 24 - f_ui36b.ascent, have_env ? wos_add_fixed(b, v, digits, sizeof(b)) : 0, unit,
		"no BME680");
}

/* the last temperatures: a cyan line over a faint fill, the samples as
 * squares, the newest pink */
static void graph(int x0, int y0, int w, int h)
{
	long lo = hist[0], hi = hist[0];
	int prev16 = 0;

	for (int i = 1; i < 4; i++)		/* a faint grid */
		for (int x = x0; x < x0 + w; x++)
			put(x, y0 + i * h / 4, 0xFFFFFF, 18);
	if (n_hist < 2) {
		const char *m = "Collecting samples\xe2\x80\xa6";

		text(&f_ui18, x0 + (w - text_w(&f_ui18, m, 0)) / 2, text_top(&f_ui18, y0 + h / 2), m,
		     WB_COLOR_TEXT_MID, 0);
		return;
	}
	for (int i = 1; i < n_hist; i++) {
		lo = hist[i] < lo ? hist[i] : lo;
		hi = hist[i] > hi ? hist[i] : hi;
	}
	if (hi - lo < 40) {			/* close together: not a flat line at an edge */
		lo = (lo + hi) / 2 - 20;
		hi = lo + 40;
	}
	for (int x = 0; x < w; x++) {
		int p = x * (SAMPLES - 1) * 256 / (w - 1), i = p >> 8, f = p & 255, y16, top16, bot16;
		long a, b, v;

		if (i >= n_hist - 1 && (i > n_hist - 1 || f))
			break;
		a = hist[i];
		b = i + 1 < n_hist ? hist[i + 1] : a;
		v = a + (b - a) * f / 256;
		y16 = (y0 + h - 6) * 16 - (int)((v - lo) * (h - 12) * 16 / (hi - lo));
		if (!x)
			prev16 = y16;
		top16 = (y16 < prev16 ? y16 : prev16) - 16;
		bot16 = (y16 > prev16 ? y16 : prev16) + 16;
		prev16 = y16;
		for (int py = top16 / 16; py <= bot16 / 16; py++) {
			int l = py * 16 > top16 ? py * 16 : top16, r = py * 16 + 16 < bot16 ? py * 16 + 16 : bot16;

			if (r > l)
				put(x0 + x, py, WB_COLOR_CYAN, (unsigned)(r - l) * 255 / 16);
		}
		for (int py = bot16 / 16 + 1; py < y0 + h; py++)	/* the fill under it */
			put(x0 + x, py, WB_COLOR_CYAN, (unsigned)(40 * (y0 + h - py) / (y0 + h - bot16 / 16)));
	}
	for (int i = 0; i < n_hist; i++) {
		int x = x0 + i * (w - 1) / (SAMPLES - 1), y = y0 + h - 6 - (int)((hist[i] - lo) * (h - 12) / (hi - lo));
		int s = i == n_hist - 1 ? 10 : 6;

		box(x - s / 2, y - s / 2, s, s, 2, i == n_hist - 1 ? WB_COLOR_PINK : WB_COLOR_CYAN, 255, 0, 0, 0);
	}
}

static void paint(int x, int y, int w, int h)
{
	int w3 = (CARD_W - 2 * GAP) / 3, y2 = y + ROW1_H + GAP, y3 = y2 + ROW2_H + GAP, h3 = y + h - GAP - y3;
	char b[48];

	(void)x;
	(void)w;
	env_card(CARD_X, y, w3, "TEMPERATURE", env.temp_c100, 2, "\xc2\xb0" "C");
	env_card(CARD_X + w3 + GAP, y, w3, "HUMIDITY", env.humidity_100, 2, "%");
	env_card(CARD_X + CARD_W - w3, y, w3, "PRESSURE", env.pressure_kpa10, 1, "kPa");

	wb_card(CARD_X, y2, CARD_W, ROW2_H, WB_RADIUS_MD, 0, 0);
	caption(CARD_X + 20, y2 + 22, "MOTION");
	if (have_acc) {
		static const char *const axis[3] = { "X", "Y", "Z" };
		long v[3] = { acc.x_mg, acc.y_mg, acc.z_mg };
		int cw = (CARD_W - 40) / 3, ty = y2 + ROW2_H - 22 - f_ui28b.ascent;

		for (int i = 0; i < 3; i++) {
			int tx = text(&f_mono16b, CARD_X + 20 + i * cw, ty + f_ui28b.ascent - f_mono16b.ascent, axis[i],
				      WB_COLOR_CYAN, 0);

			wos_copy(b, v[i] < 0 ? "-" : "+", sizeof(b));
			tx = text(&f_ui28b, tx + 12, ty, wos_add_fixed(b, v[i] < 0 ? -v[i] : v[i], 3, sizeof(b)),
				  WB_COLOR_TEXT_HI, -4);
			text(&f_ui18, tx + 6, ty + f_ui28b.ascent - f_ui18.ascent, "g", WB_COLOR_TEXT_MID, 0);
		}
	} else {
		reading(CARD_X + 20, y2 + ROW2_H - 22 - f_ui36b.ascent, 0, 0, "no BMI270");
	}

	wb_card(CARD_X, y3, CARD_W, h3, WB_RADIUS_MD, 0, 0);
	caption(CARD_X + 20, y3 + 22, "TEMPERATURE, LAST 20 SAMPLES");
	if (n_hist) {			/* their range, on the right */
		long lo = hist[0], hi = hist[0];

		for (int i = 1; i < n_hist; i++) {
			lo = hist[i] < lo ? hist[i] : lo;
			hi = hist[i] > hi ? hist[i] : hi;
		}
		b[0] = 0;
		wos_add_fixed(b, lo, 2, sizeof(b));
		wos_add_fixed(wos_add(b, " \xe2\x80\x93 ", sizeof(b)), hi, 2, sizeof(b));
		wos_add(b, " \xc2\xb0" "C", sizeof(b));
		text(&f_mono14, CARD_X + CARD_W - 20 - text_w(&f_mono14, b, 0), y3 + 21, b, WB_COLOR_TEXT_MID, 0);
	}
	graph(CARD_X + 20, y3 + 52, CARD_W - 40, h3 - 52 - 20);
}

int main(void)
{
	static const wos_app app = {
		.name = "Sensor panel", .tick = sample, .tick_ms = 333,
		.about = "The badge's sensors, about once a second: temperature, humidity and pressure "
			 "from the BME680, the acceleration from the BMI270.",
	};
	static wos_screen panel = { .kind = WOS_CANVAS, .paint = paint, .scene = 1 };

	sample();
	return wos_run(&app, &panel);
}
