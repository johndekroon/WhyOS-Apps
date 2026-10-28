// SPDX-License-Identifier: MIT
/*
 * ohminous (OHMinous) — an electronics workbench on the badge, after the
 * design canvas "OHMinous" (2026-10-25), in the style of the canvas "Menu's
 * met frontpaneel" (2026-10-27, libwb): the frontpanel's figures behind
 * every screen, the eyebrow and title of the menu's lists, cards, rows and
 * the hint bar of glass with square corners, the focus cyan, pop-ups
 * frosted with their keys inside. The logic analyzer's traces and the
 * terminals' lines stay on black. It replaces the Bus Pirate app (the menu
 * built with -DBP_APP), whose Bus Pirate code it took over (bp.c), and adds
 * the badge's own header pins (hw.c).
 *
 * Six tools on the start screen: Pins, Logic, Buses, LEDs, Power and
 * Serial. Each has two sources, switched with Tab: the badge's J3 and J7
 * headers, always there, and a Bus Pirate 6 on the bottom port, which adds
 * 8 pins, a programmable supply, faster capture and more buses.
 *
 *   Pins     badge: IO0-IO4 and STROBE as inputs (pull-up, pull-down) or
 *            outputs; Bus Pirate: IO0-IO7 in DIO mode
 *   Logic    badge: IO0-IO4 sampled in software; Bus Pirate: 8 channels
 *            over SUMP, up to 50 MHz
 *   Buses    I2C (badge: J7's SDA and SCL, bit-banged) with a scan and
 *            registers; UART (badge: IO3 and IO4 in software, slow rates);
 *            SPI and 1-Wire on the Bus Pirate, in its terminal
 *   LEDs     badge: the WS2812s on J7 pin 4 (kernel patch 0032); Bus
 *            Pirate: WS2812 or APA102 strips, its onboard LED
 *   Power    badge: what the headers' rails are; Bus Pirate: its supply,
 *            0.8-5.0 V with a current limit, and pull-ups
 *   Serial   badge: a USB serial adapter on the bottom port; Bus Pirate:
 *            its terminal
 *
 * Keys (Docs/06-design-system.md 6.6): Enter opens; Tab switches the
 * source; the square has a tool's options, the triangle its main action
 * (toggle a pin, capture, scan, the supply), the cross the second (save a
 * capture); the circle the log of what the tools sent and read, the cloud
 * the settings, the diamond the help; Esc goes back, and closes from the
 * start screen. The settings and the pins' names are kept in
 * APP_CONFIG/settings. Pins it made outputs go back to inputs when it
 * closes (a setting).
 *
 * It draws in bands of 40 rows, a 1 MB whole buffer only while a pop-up is
 * up; libwb records each frame and draws only the bands that changed
 * (redraw). OHM_UI_TIMING=1 logs a line a frame on stderr.
 *
 * Built with -DOHM_HOST the hardware is simulated (hw.c, bp.c) and keys
 * come from the first argument, the screen goes to the second as PPM (the
 * test, test_ohminous.py). On the badge a test sends key codes to
 * /tmp/ohminous.keys and reads /tmp/ohminous.where.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "wb_tokens.h"

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H
#include "wb.h"
#include "lora_state.h"
#include "wbstatus.h"
#include "keyrep.h"

#include "hw.h"
#include "bp.h"

#ifndef OHM_HOST
#include <poll.h>
#endif

#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_TAB		15
#define KEY_ENTER	28
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_FN		0x1d0

/* ---- state ---------------------------------------------------------------- */

enum { T_PINS, T_LOGIC, T_BUSES, T_LEDS, T_POWER, T_SERIAL, N_TOOLS };
enum { SRC_BADGE, SRC_BP };
enum { SC_START, SC_PINS, SC_LOGIC, SC_BUSES, SC_I2C, SC_REGS, SC_TERM, SC_LEDS, SC_POWER,
       SC_LOG, SC_SETTINGS };
enum { POP_NONE, POP_HELP, POP_PICK, POP_INPUT };
enum { BUS_I2C, BUS_UART, BUS_SPI, BUS_1WIRE, N_BUSES };
enum { TM_UART, TM_USB, TM_BP, TM_SPI, TM_1WIRE };	/* what a terminal talks to */

#define LOG_MAX		60
#define TERM_LINES	80

static struct {
	int sc, back;
	int tile;			/* the start screen's focus */
	int src[N_TOOLS];		/* each tool's source */
	int focus;			/* a screen's row */
	int bus;			/* the Buses list's focus */
	int pop, pf;
	int pick_what;			/* what a pick list chooses */
	char notice[120];
	int notice_bad;
	/* settings */
	int back_view, safe, uart_baud, usb_baud;
	char names[HW_PINS][12];
	/* the input pop-up */
	char in_title[48], in_text[24];
	int in_what;
	/* the log */
	char log[LOG_MAX][96];
	int n_log;
} O;

static const char *const tool_names[N_TOOLS] = { "Pins", "Logic", "Buses", "LEDs", "Power", "Serial" };
static const unsigned uart_bauds[] = { 1200, 2400, 4800, 9600, 19200 };
static const unsigned usb_bauds[] = { 9600, 19200, 38400, 57600, 115200 };
#define N_UART_BAUDS	5
#define N_USB_BAUDS	5

/* the Bus Pirate's pins in DIO mode, as set from here */
static int bp_dir[BP_PINS], bp_lvl[BP_PINS];

/* ---- small helpers -------------------------------------------------------- */

static void notice(const char *s, int bad)
{
	snprintf(O.notice, sizeof(O.notice), "%s", s);
	O.notice_bad = bad;
}

static void log_add(const char *fmt, const char *s)
{
	time_t now = time(0);
	struct tm tm;
	char t[96];

	localtime_r(&now, &tm);
	snprintf(t, sizeof(t), "%02d:%02d  ", tm.tm_hour, tm.tm_min);
	snprintf(t + 7, sizeof(t) - 7, fmt, s);
	if (O.n_log == LOG_MAX) {
		memmove(O.log, O.log + 1, sizeof(O.log[0]) * (LOG_MAX - 1));
		O.n_log--;
	}
	strcpy(O.log[O.n_log++], t);
}

static char key_char(int code, int shift)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == 57)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo))
		return 0;
	return shift ? hi[code] : lo[code];
}

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static const char *pin_label(int i)
{
	return O.names[i][0] ? O.names[i] : hw_pins[i].name;
}

/* ---- settings --------------------------------------------------------------- */

static char set_path[300];

static void settings_load(void)
{
	const char *d = getenv("APP_CONFIG"), *home = getenv("HOME");
	char dir[256], line[64];
	FILE *f;

	if (d && *d)
		snprintf(dir, sizeof(dir), "%s", d);
	else
		snprintf(dir, sizeof(dir), "%s/.whyos/config/ohminous", home && *home ? home : "/root");
	for (char *s = dir + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(dir, 0700);
			*s = '/';
		}
	mkdir(dir, 0700);
	snprintf(set_path, sizeof(set_path), "%s/settings", dir);
	O.safe = 1;
	O.uart_baud = 3;		/* 9600 */
	O.usb_baud = 4;			/* 115200 */
	if (!(f = fopen(set_path, "r")))
		return;
	while (fgets(line, sizeof(line), f)) {
		int v, i;
		char name[16];

		if (sscanf(line, "view=%d", &v) == 1)
			O.back_view = !!v;
		else if (sscanf(line, "safe=%d", &v) == 1)
			O.safe = !!v;
		else if (sscanf(line, "uart=%d", &v) == 1 && v >= 0 && v < N_UART_BAUDS)
			O.uart_baud = v;
		else if (sscanf(line, "usb=%d", &v) == 1 && v >= 0 && v < N_USB_BAUDS)
			O.usb_baud = v;
		else if (sscanf(line, "name%d=%11s", &i, name) == 2 && i >= 0 && i < HW_PINS)
			snprintf(O.names[i], sizeof(O.names[i]), "%s", name);
	}
	fclose(f);
}

static void settings_save(void)
{
	char tmp[310];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", set_path);
	if (!(f = fopen(tmp, "w")))
		return;
	fprintf(f, "view=%d\nsafe=%d\nuart=%d\nusb=%d\n", O.back_view, O.safe, O.uart_baud, O.usb_baud);
	for (int i = 0; i < HW_PINS; i++)
		if (O.names[i][0])
			fprintf(f, "name%d=%s\n", i, O.names[i]);
	if (fclose(f) == 0)
		rename(tmp, set_path);
}

/* ---- the logic analyzer's capture, from either source ---------------------- */

static const unsigned badge_rates[] = { 1000, 10000, 50000 };
static const unsigned bp_rates[] = { 10000, 100000, 1000000, 10000000, 50000000 };
static const int la_sizes[] = { 2048, 8192, 32768 };

static struct {
	uint8_t buf[BP_LA_MAX];
	int n, ch, src;			/* samples, channels, whose */
	unsigned rate;
	int rate_i[2], size_i[2], trig[2], falling;
	int pos, zoom;			/* first sample shown; samples a pixel: 2^zoom (minus: pixels a sample) */
	int gen;			/* counts captures: what buf holds, for the recording */
	char status[96];
	int status_bad;
} LA = { .rate_i = { 1, 2 }, .size_i = { 0, 1 }, .trig = { -1, -1 } };

#define LA_W		532

static int span_of(int z)	/* samples across the lanes at zoom z */
{
	return z >= 0 ? LA_W << z : LA_W >> -z;
}

/* the zoom that shows all n samples */
static int la_fit(int n)
{
	int z = 0;

	while (span_of(z) < n)
		z++;
	while (z > -3 && span_of(z - 1) >= n)
		z--;
	return z;
}

static int la_span(void)
{
	return span_of(LA.zoom);
}

/* "1 MHz", "10 kHz" */
static char *hz(char *o, unsigned v)
{
	if (v >= 1000000)
		sprintf(o, "%u MHz", v / 1000000);
	else if (v >= 1000)
		sprintf(o, "%u kHz", v / 1000);
	else
		sprintf(o, "%u Hz", v);
	return o;
}

/* "0", "8 µs", "1.6 ms" */
static char *dur(char *o, long long ns)
{
	if (!ns)
		strcpy(o, "0");
	else if (ns < 1000)
		sprintf(o, "%lld ns", ns);
	else if (ns < 1000000)
		sprintf(o, ns % 1000 ? "%lld.%lld \xc2\xb5s" : "%lld \xc2\xb5s", ns / 1000, ns % 1000 / 100);
	else if (ns < 1000000000)
		sprintf(o, ns % 1000000 ? "%lld.%lld ms" : "%lld ms", ns / 1000000, ns % 1000000 / 100000);
	else
		sprintf(o, "%lld.%lld s", ns / 1000000000, ns % 1000000000 / 100000000);
	return o;
}

/* ---- the terminal (UART, Serial, SPI, 1-Wire) ------------------------------- */

static struct {
	int kind;			/* TM_... */
	char line[TERM_LINES][100];
	int tx[TERM_LINES], n;
	char in[96];
	int len;
	char part[100];			/* a received line not ended yet */
	int from;			/* the screen it goes back to */
	int usb_fd;
	char usb_dev[24];
} TM = { .usb_fd = -1 };

static void term_add(const char *s, int tx)
{
	if (TM.n == TERM_LINES) {
		memmove(TM.line, TM.line + 1, sizeof(TM.line[0]) * (TERM_LINES - 1));
		memmove(TM.tx, TM.tx + 1, sizeof(TM.tx[0]) * (TERM_LINES - 1));
		TM.n--;
	}
	snprintf(TM.line[TM.n], sizeof(TM.line[0]), "%s", s);
	TM.tx[TM.n++] = tx;
}

/* received bytes: lines at a new line, the rest kept */
static void term_recv(const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		size_t l = strlen(TM.part);

		if (b[i] == '\n' || l >= sizeof(TM.part) - 1) {
			term_add(TM.part, 0);
			TM.part[0] = 0;
		} else if (b[i] == '\r' || b[i] == 3) {
			continue;
		} else if ((unsigned char)b[i] >= 32) {
			TM.part[l] = b[i];
			TM.part[l + 1] = 0;
		}
	}
}

#ifndef OHM_HOST
/* A USB serial adapter on the bottom port: a ttyUSB, or a ttyACM that is
 * not the Bus Pirate's. */
static int usb_serial_open(void)
{
	static const speed_t sp[N_USB_BAUDS] = { B9600, B19200, B38400, B57600, B115200 };
	DIR *d = opendir("/dev");
	struct dirent *e;
	struct termios t;

	TM.usb_dev[0] = 0;
	while (d && (e = readdir(d)))
		if (!strncmp(e->d_name, "ttyUSB", 6) || (!strncmp(e->d_name, "ttyACM", 6) && !B.present)) {
			snprintf(TM.usb_dev, sizeof(TM.usb_dev), "/dev/%s", e->d_name);
			break;
		}
	if (d)
		closedir(d);
	if (!TM.usb_dev[0])
		return -1;
	TM.usb_fd = open(TM.usb_dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (TM.usb_fd < 0 || tcgetattr(TM.usb_fd, &t) < 0)
		return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	cfsetispeed(&t, sp[O.usb_baud]);
	cfsetospeed(&t, sp[O.usb_baud]);
	tcsetattr(TM.usb_fd, TCSANOW, &t);
	return 0;
}
#else
static int usb_serial_open(void)
{
	if (!getenv("OHM_USB"))
		return -1;
	strcpy(TM.usb_dev, "/dev/ttyUSB0");
	return 0;
}
#endif

static void term_close(void)
{
	if (TM.kind == TM_UART)
		hw_uart_close();
	if (TM.usb_fd >= 0)
		close(TM.usb_fd);
	TM.usb_fd = -1;
}

/* send the typed line, with a CR (the Bus Pirate's, a device's) or CR LF */
static void term_send(void)
{
	char line[100];

	snprintf(line, sizeof(line), "%s\r\n", TM.in);
	if (TM.kind == TM_UART) {
		hw_uart_send(line, (int)strlen(line));
	} else if (TM.kind == TM_USB) {
		if (TM.usb_fd >= 0 && write(TM.usb_fd, line, strlen(line)) < 0)
			notice("The adapter stopped taking bytes", 1);
	} else {
		if (bp_cmd(TM.in) == 0)
			term_recv(B.rx, (int)strlen(B.rx));
		TM.part[0] = 0;
	}
	snprintf(line, sizeof(line), "\xe2\x80\xba %s", TM.in);
	term_add(line, 1);
	log_add("%s", line);
	TM.in[0] = 0;
	TM.len = 0;
}

/* whatever has come in since */
static int term_poll(void)
{
	char b[256];
	int n = 0;

	if (O.sc != SC_TERM)
		return 0;
	if (TM.kind == TM_UART)
		n = hw_uart_recv(b, sizeof(b));
#ifndef OHM_HOST
	else if (TM.kind == TM_USB && TM.usb_fd >= 0)
		n = (int)read(TM.usb_fd, b, sizeof(b));
#endif
	if (n > 0)
		term_recv(b, n);
	return n > 0;
}

/* ---- the I2C scan ---------------------------------------------------------- */

static struct {
	uint8_t found[128];
	int n, scanned, focus, src;
	int addr;			/* the registers shown */
	uint8_t regs[256];
	int regs_ok;
} I2C;

static const char *i2c_hint(int a)
{
	if (a == 0x3c || a == 0x3d)
		return "Often an OLED display";
	if (a >= 0x48 && a <= 0x4b)
		return "Often a temperature sensor or ADC";
	if (a >= 0x50 && a <= 0x57)
		return "Often an EEPROM";
	if (a == 0x68 || a == 0x69)
		return "Often a clock (RTC) or motion sensor";
	if (a == 0x76 || a == 0x77)
		return "Often a pressure sensor";
	if (a >= 0x20 && a <= 0x27)
		return "Often a port expander";
	if (a == 0x40 || a == 0x41)
		return "Often a current sensor or PWM driver";
	if (a == 0x29 || a == 0x39)
		return "Often a light or distance sensor";
	return "Unknown device";
}

static void i2c_scan(void)
{
	char t[96] = "";

	I2C.src = O.src[T_BUSES];
	I2C.n = I2C.src == SRC_BP ? bp_i2c_scan(I2C.found) : hw_i2c_scan(I2C.found);
	I2C.scanned = 1;
	I2C.focus = 0;
	if (I2C.n < 0) {
		notice(I2C.src == SRC_BP ? B.notice : hw_err, 1);
		I2C.n = 0;
		return;
	}
	O.notice[0] = 0;
	for (int a = 0; a < 128; a++)
		if (I2C.found[a] && strlen(t) < sizeof(t) - 6)
			sprintf(t + strlen(t), " 0x%02X", a);
	log_add("I\xc2\xb2" "C scan:%s", I2C.n ? t : " nothing");
}

static int i2c_found_nth(int k)
{
	for (int a = 0; a < 128; a++)
		if (I2C.found[a] && !k--)
			return a;
	return -1;
}

static void i2c_regs(void)
{
	char t[48];

	I2C.regs_ok = 0;
	if (I2C.src == SRC_BP) {	/* "[0x78 0x00 [0x79 r:16]" in the Bus Pirate's I2C mode */
		char cmd[48];

		for (int r = 0; r < 256; r += 16) {
			snprintf(cmd, sizeof(cmd), "[0x%02x 0x%02x [0x%02x r:16]", I2C.addr << 1, r, I2C.addr << 1 | 1);
			if (bp_cmd(cmd) < 0)
				return;
			for (int i = 0, k = 0; i < (int)strlen(B.rx) && k < 16; i++)
				if (!strncmp(B.rx + i, "RX: ", 4) || !strncmp(B.rx + i, " 0x", 3)) {
					unsigned v;

					if (sscanf(B.rx + i + (B.rx[i] == 'R' ? 4 : 1), "0x%2x", &v) == 1)
						I2C.regs[r + k++] = (uint8_t)v;
				}
		}
		I2C.regs_ok = 1;
	} else if (hw_i2c_read(I2C.addr, 0, I2C.regs, 256) == 256) {
		I2C.regs_ok = 1;
	} else {
		notice(hw_err, 1);
	}
	snprintf(t, sizeof(t), "0x%02X", I2C.addr);
	log_add(I2C.regs_ok ? "I\xc2\xb2" "C %s: registers 0x00-0xFF read" : "I\xc2\xb2" "C %s: no answer", t);
}

/* ---- the Bus Pirate: connect when a screen needs it -------------------------- */

static void draw_message(const char *eb, const char *line);

static void connecting(void)
{
	draw_message("BUS PIRATE", "Connecting\xe2\x80\xa6");
}

/* 0 when it is there and answers */
static int bp_ready(void)
{
	if (B.open)
		return 0;
	if (bp_open(connecting) < 0)
		return -1;
	log_add("%s connected", B.board[0] ? B.board : "Bus Pirate");
	return 0;
}

/* ---- layout ------------------------------------------------------------------ */

#define X0		WB_CONTENT_PAD
#define XW		(W - 2 * WB_CONTENT_PAD)
#define BODY		148				/* under the header (Files) */
#define BOTTOM		(H - WB_HINT_BAR_H)

#define EB_APP		"OHMINOUS \xe2\x80\xba"		/* the eyebrows: where a screen lies */
#define EB_BUSES	"BUSES \xe2\x80\xba"

#define PINK_EDGE	0x99405F			/* pink at 60 % on surface */
#define EDGE_A		66				/* glass's 1 px edge: white at 26 % */

static struct { char hhmm[6]; int clock_ok, wifi, lora; } U;

static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3), text_top(n, cy), "OS",
	     WB_COLOR_CYAN, -3);
	status_right(U.hhmm, U.clock_ok, U.wifi, U.lora);
}

/* every screen: the frontpanel's figures (wb_scene: libwb reads the
 * rotation once a frame, while recording it, and keeps it for the bands),
 * the status bar, the eyebrow (where the screen lies: "APP" on the start
 * screen, else the parent's name and a chevron) and the title */
static void header(const char *eb, const char *title)
{
	wb_scene();
	status_bar();
	wb_header(eb, title, 0);
}

/* a panel of data (the traces, a terminal's lines): the console's black,
 * opaque so nothing of the scene shows through, with glass's faint edge */
static void console_box(int x, int y, int w, int h)
{
	fill(x, y, w, h, WB_COLOR_CONSOLE);
	edge_rect(x, y, w, h, 0xFFFFFF, EDGE_A);
}

/* a message over the screen while something takes a while ("Connecting...") */
static const char *over_eb, *over_msg;
static void redraw(void);

static void draw_message(const char *eb, const char *line)
{
	over_eb = eb;
	over_msg = line;
	redraw();
	over_msg = 0;
}

static void draw_over(void)
{
	int h = 32 + 18 + 6 + 36 + 32, y = (H - h) / 2;

	pop_frame(y, h);
	eyebrow(POP_X + 32, y + 32, over_eb);
	text(&f_ui28b, POP_X + 32, y + 32 + 18 + 6 + 2, over_msg, WB_COLOR_TEXT_HI, 0);
}

/* the BP tag: pink on a pink tint */
static int bp_tag(int x, int cy)
{
	int w = text_w(&f_mono13, "BP", 0) + 14;

	box(x, cy - 10, w, 20, 6, WB_COLOR_PINK, 31, PINK_EDGE, 1, 0);
	text(&f_mono13, x + 7, text_top(&f_mono13, cy), "BP", WB_COLOR_PINK, 0);
	return x + w;
}

/* a pin's level as a switch: the canvas's (wb_switch) with HIGH in pink */
static void level_switch(int x, int cy, int on)
{
	int y = cy - 15;

	if (on) {
		box(x, y, 52, 30, 4, WB_COLOR_PINK, 255, 0, 0, 0);
	} else {
		box(x, y, 52, 30, 4, 0xFFFFFF, 26, 0, 0, 0);
		edge_rect(x, y, 52, 30, 0xFFFFFF, 71);
	}
	box(on ? x + 26 : x + 4, y + 4, 22, 22, 4, on ? WB_COLOR_VOID : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
}

/* a setting as a chip of glass: "Rate 1 MHz" */
static int chip(int x, int y, const char *k, const char *v)
{
	int w = text_w(&f_ui16, k, 0) + 6 + text_w(&f_mono16b, v, 0) + 24;

	wb_card(x, y, w, 32, WB_RADIUS_SM, 0, 0);
	x = text(&f_ui16, x + 12, text_top(&f_ui16, y + 17), k, WB_COLOR_TEXT_MID, 0);
	text(&f_mono16b, x + 6, text_top(&f_mono16b, y + 16), v, WB_COLOR_TEXT_HI, 0);
	return x + 6 + text_w(&f_mono16b, v, 0) + 12 + 8;
}

/* a dashed outline: what is not there. Over the scene, the void at half
 * strength inside, so its words read. */
static void dashed(int x, int y, int w, int h)
{
	box(x, y, w, h, 0, WB_COLOR_VOID, 140, 0, 0, 0);
	for (int d = 14; d < w - 14; d += 8)
		for (int i = 0; i < 4; i++) {
			put(x + d + i, y, 0xFFFFFF, EDGE_A);
			put(x + d + i, y + h - 1, 0xFFFFFF, EDGE_A);
		}
	for (int d = 14; d < h - 14; d += 8)
		for (int i = 0; i < 4; i++) {
			put(x, y + d + i, 0xFFFFFF, EDGE_A);
			put(x + w - 1, y + d + i, 0xFFFFFF, EDGE_A);
		}
}

/* a status on the right: a dot and a word */
static void status_dot(int y, const char *s, uint32_t c)
{
	int x = X0 + XW - text_w(&f_ui16, s, 0);

	box(x - 16, y + 12, 8, 8, 2, c, 255, 0, 0, 0);
	text(&f_ui16, x, text_top(&f_ui16, y + 16), s, WB_COLOR_TEXT_MID, 0);
}

/*
 * The hint bar with the source switch: the hints, then Tab and Badge |
 * Bus Pirate, then the right-hand hint. src -1: no switch. Glass over the
 * scene, as hint_bar's.
 */
static void hint_bar_src(const struct hint *hs, int n, int src)
{
	const struct wb_font *f = &f_ui16;
	int y0 = H - WB_HINT_BAR_H, cy = y0 + WB_HINT_BAR_H / 2, x = WB_CHROME_PAD, gap = 20, need = 0;

	/* everything's width: the gaps shrink to fit the switch in */
	for (int i = 0; i < n; i++)
		need += hint_w(&hs[i]) + gap;
	if (src >= 0)
		need += keycap_w("Tab") + 18 + 8 + text_w(f, "Badge", 0) + text_w(f, "Bus Pirate", 0) + 50;
	while (need > W - 2 * WB_CHROME_PAD && gap > 8) {
		need -= n;
		gap--;
	}
	hint_glass(y0);
	for (int i = 0; i < n; i++) {
		int hx = hs[i].right ? W - WB_CHROME_PAD - hint_w(&hs[i]) : x, ex = keycap(hx, cy, hs[i].keys);

		ex = text(f, ex + 8, text_top(f, cy), hs[i].label, WB_COLOR_TEXT_MID, 0);
		if (!hs[i].right)
			x = ex + gap;
	}
	if (src >= 0) {
		static const char *const seg[2] = { "Badge", "Bus Pirate" };
		int sx;

		x = keycap(x, cy, "Tab") + 8;
		sx = x;
		for (int i = 0; i < 2; i++)
			sx += text_w(f, seg[i], 0) + 20 + 2;
		edge_rect(x, cy - 15, sx - x + 4, 30, 0xFFFFFF, EDGE_A);
		x += 3;
		for (int i = 0; i < 2; i++) {
			int w = text_w(f, seg[i], 0) + 20;
			uint32_t c = i == src ? WB_COLOR_CYAN : i == SRC_BP && !B.present ? WB_COLOR_TEXT_LOW :
				     WB_COLOR_TEXT_MID;

			if (i == src)	/* the present source: cyan, as a choice's box */
				box(x, cy - 12, w, 24, 4, WB_COLOR_CYAN, 46, 0, 0, 0);
			text(f, x + 10, text_top(f, cy), seg[i], c, 0);
			x += w + 2;
		}
	}
}

/* text reading upwards from (x, y): the header map's pin names */
static void text_up(const struct wb_font *f, int x, int y, const char *s, uint32_t c)
{
	int pen16 = 0;

	while (*s) {
		unsigned cp = next_cp(&s);
		const struct wb_glyph *g = glyph(f, cp);
		const uint8_t *a = f->bits + g->off;
		int gx = (pen16 + 8) / 16 + g->x;

		for (int j = 0; j < g->h; j++)
			for (int i = 0; i < g->w; i++)
				put(x + g->y + j, y - (gx + i), c, a[j * g->w + i]);
		pen16 += g->adv16;
	}
}

/* ---- the header map: J3 and J7 as they sit on the badge --------------------- */

enum { PK_GPIO, PK_USED, PK_POWER, PK_GND, PK_SYS };

struct hpin { const char *label; int kind; int pin; /* hw_pins index, -1 */ };

static const struct hpin j3[8] = {
	{ "+5V", PK_POWER, -1 }, { "RTC", PK_SYS, -1 }, { "IO0", PK_GPIO, 0 }, { "IO1", PK_GPIO, 1 },
	{ "IO2", PK_GPIO, 2 }, { "IO3", PK_GPIO, 3 }, { "IO4", PK_GPIO, 4 }, { "GND", PK_GND, -1 },
};

static const struct hpin j7[10] = {
	{ "+3V3", PK_POWER, -1 }, { "STROBE", PK_GPIO, 5 }, { "GND", PK_GND, -1 }, { "LED", PK_SYS, -1 },
	{ "INT1", PK_SYS, -1 }, { "INT2", PK_SYS, -1 }, { "SDA", PK_GPIO, -1 }, { "SCL", PK_GPIO, -1 },
	{ "KEY", PK_SYS, -1 }, { "GND", PK_GND, -1 },
};

enum { HM_PINS, HM_UART, HM_I2C };

/* a pin's role in the mode: "TX" on J3 pin 6 for the UART */
static const char *hm_role(int mode, int j, int n)
{
	if (mode == HM_UART)
		return j == 3 && n == 6 ? "TX" : j == 3 && n == 7 ? "RX" : j == 3 && n == 8 ? "GND" : "";
	if (mode == HM_I2C)
		return j == 7 && n == 7 ? "SDA" : j == 7 && n == 8 ? "SCL" : j == 7 && n == 10 ? "GND" : "";
	return "";
}

/*
 * J3 and J7 as they sit on the badge, on a card of glass MAP_H high from
 * y0 (the whole width): a caption, the pads with their names reading
 * upwards and their numbers, which header each is, and the legend.
 */
#define MAP_H		192
#define MAP_PITCH	30			/* pad to pad */
#define MAP_NAME	50			/* room for a name, upwards: STROBE */

static void draw_header_map(int y0, int mode, int focus_pin)
{
	static const struct { const char *name; int kind; } legend[5] = {
		{ "Free", PK_GPIO }, { "In use here", PK_USED }, { "Power", PK_POWER }, { "GND", PK_GND },
		{ "System", PK_SYS },
	};
	int front = !O.back_view, x0 = X0 + 18, x = x0, cy;
	const char *cap = front ? "SEEN FROM THE SCREEN SIDE" : "SEEN FROM THE BACK (SILK SIDE)";

	wb_card(X0, y0, XW, MAP_H, WB_RADIUS_MD, 0, 0);
	y0 += 8;
	text(&f_mono13, x0, y0, cap, WB_COLOR_PLACEHOLDER, 16);
	{
		int rx = X0 + XW - 18 - text_w(&f_mono13, "TOP EDGE", 16);

		text(&f_mono13, rx, y0, "TOP EDGE", WB_COLOR_PLACEHOLDER, 16);
		icon(&s_up, rx - 20, y0 + 1, WB_COLOR_PLACEHOLDER, 255);
	}
	cy = y0 + 22;
	for (int side = 0; side < 2; side++) {
		int jn = (side == 0) == front ? 3 : 7, cnt = jn == 3 ? 8 : 10;
		const struct hpin *P = jn == 3 ? j3 : j7;
		int gx = x;

		fill(gx, cy, cnt * MAP_PITCH - 6, 2, WB_COLOR_LINE);
		for (int k = 0; k < cnt; k++) {
			int idx = front ? cnt - 1 - k : k, n = idx + 1;
			const struct hpin *p = &P[idx];
			const char *role = hm_role(mode, jn, n);
			int kind = role[0] ? PK_USED : p->kind, foc = p->pin >= 0 && p->pin == focus_pin;
			int px = gx + k * MAP_PITCH, py = cy + 6 + 18;
			uint32_t ink = foc ? WB_COLOR_CYAN : kind == PK_USED ? WB_COLOR_PINK : kind == PK_GND ?
				       WB_COLOR_TEXT_MID : kind == PK_SYS ? WB_COLOR_TEXT_LOW : WB_COLOR_TEXT_HI;
			char num[4];

			if (role[0])
				text(&f_mono13, px + 12 - text_w(&f_mono13, role, 0) / 2, cy + 6, role, WB_COLOR_PINK, 0);
			if (foc)
				box(px - 4, py - 4, 32, 32, 4, 0, 0, WB_COLOR_CYAN, 2, &GLOW_FOCUS);
			switch (kind) {
			case PK_GPIO:
				box(px, py, 24, 24, 4, WB_COLOR_OK, 36, WB_COLOR_OK, 2, 0);
				break;
			case PK_USED:
				box(px, py, 24, 24, 4, WB_COLOR_PINK, 255, WB_COLOR_PINK, 2, 0);
				break;
			case PK_POWER:
				box(px, py, 24, 24, 4, WB_COLOR_TEXT_HI, 255, WB_COLOR_TEXT_HI, 2, 0);
				break;
			case PK_GND:
				box(px, py, 24, 24, 4, WB_COLOR_LINE, 255, WB_COLOR_TEXT_LOW, 2, 0);
				break;
			default:	/* dashed: a line of dots round the pad */
				for (int d = 0; d < 24; d += 4) {
					fill(px + d, py, 2, 2, WB_COLOR_TEXT_LOW);
					fill(px + d, py + 22, 2, 2, WB_COLOR_TEXT_LOW);
					fill(px, py + d, 2, 2, WB_COLOR_TEXT_LOW);
					fill(px + 22, py + d, 2, 2, WB_COLOR_TEXT_LOW);
				}
			}
			text_up(&f_mono13, px + 4, py + 24 + 6 + MAP_NAME, p->pin >= 0 && O.names[p->pin][0] ?
				O.names[p->pin] : p->label, ink);
			snprintf(num, sizeof(num), "%d", n);
			text(&f_mono13, px + 12 - text_w(&f_mono13, num, 0) / 2, py + 24 + 6 + MAP_NAME + 1, num,
			     WB_COLOR_PLACEHOLDER, 0);
		}
		{
			char g[24];

			snprintf(g, sizeof(g), "J%d \xc2\xb7 pin 1 at %s", jn, front ? "right" : "left");
			text(&f_mono13, gx, cy + 6 + 18 + 24 + 6 + MAP_NAME + 18, g, WB_COLOR_TEXT_MID, 0);
		}
		x = gx + cnt * MAP_PITCH - 6 + 48;
	}
	x = x0;
	for (int i = 0; i < 5; i++) {
		int ly = y0 + 164;

		switch (legend[i].kind) {
		case PK_GPIO:
			box(x, ly, 12, 12, 3, WB_COLOR_OK, 36, WB_COLOR_OK, 2, 0);
			break;
		case PK_USED:
			box(x, ly, 12, 12, 3, WB_COLOR_PINK, 255, 0, 0, 0);
			break;
		case PK_POWER:
			box(x, ly, 12, 12, 3, WB_COLOR_TEXT_HI, 255, 0, 0, 0);
			break;
		case PK_GND:
			box(x, ly, 12, 12, 3, WB_COLOR_LINE, 255, WB_COLOR_TEXT_LOW, 2, 0);
			break;
		default:
			for (int d = 0; d < 12; d += 3) {
				fill(x + d, ly, 2, 2, WB_COLOR_TEXT_LOW);
				fill(x + d, ly + 10, 2, 2, WB_COLOR_TEXT_LOW);
				fill(x, ly + d, 2, 2, WB_COLOR_TEXT_LOW);
				fill(x + 10, ly + d, 2, 2, WB_COLOR_TEXT_LOW);
			}
		}
		x = text(&f_ui16, x + 18, text_top(&f_ui16, ly + 6), legend[i].name, WB_COLOR_TEXT_MID, 0) + 18;
	}
}

/* ---- the start screen ---------------------------------------------------------- */

static const struct wb_icon *const tile_icons[N_TOOLS] = { &t_pins, &t_logic, &t_buses, &t_leds, &t_power,
							     &t_serial };

static void tile_status(int i, char *o, int n)
{
	int outs = 0;

	for (int k = 0; k < HW_PINS; k++)
		outs += hw_pins[k].mode == PM_OUT;
	if (!B.present) {
		static const char *const s[N_TOOLS] = { "", "5 channels", "I\xc2\xb2" "C \xc2\xb7 UART", "WS2812 \xc2\xb7 4",
							"+5V +3V3 fixed", "USB, bottom port" };

		if (i == T_PINS)
			snprintf(o, (size_t)n, "IO0\xe2\x80\x93IO4 \xc2\xb7 %d out", outs);
		else
			snprintf(o, (size_t)n, "%s", s[i]);
		return;
	}
	switch (i) {
	case T_PINS:
		snprintf(o, (size_t)n, "Badge 5 \xc2\xb7 BP 8");
		break;
	case T_LOGIC:
		snprintf(o, (size_t)n, "8 ch \xc2\xb7 50 MHz");
		break;
	case T_BUSES:
		snprintf(o, (size_t)n, "I\xc2\xb2" "C SPI UART");
		break;
	case T_LEDS:
		snprintf(o, (size_t)n, "+ APA102");
		break;
	case T_POWER:
		snprintf(o, (size_t)n, "%s \xc2\xb7 %d.%02d V", B.psu ? "On" : "Off", B.dv / 10, B.dv % 10 * 10);
		break;
	default:
		snprintf(o, (size_t)n, "BP terminal");
	}
}

static void draw_start(void)
{
	static const struct hint hints[] = { { "Enter", "open", 0 }, { "Esc", "close", 1 } };
	int tw = (XW - 40) / 3, th = 152, y = WB_ROWS_TOP;
	int pw = (XW - 16) / 2;
	char st[40];

	wb_scene();
	status_bar();
	wb_header("APP", "OHMinous", "Electronics workbench");
	/* the two sources */
	for (int i = 0; i < 2; i++) {
		int x = X0 + i * (pw + 16), cy = y + 30, bp = i == 1, on = !bp || B.present;
		const char *sub = !bp ? "J3 + J7 header" : B.present ? "Connected" : "Plug into bottom port";
		char s2[40];

		if (bp && !B.present)	/* dashed: nothing there */
			dashed(x, y, pw, 60);
		else
			wb_card(x, y, pw, 60, WB_RADIUS_MD, 0, 0);
		icon(bp ? &o_plug : &o_badge, x + 16, cy - 13, bp ? (on ? WB_COLOR_PINK : WB_COLOR_TEXT_LOW) :
		     WB_COLOR_TEXT_MID, 255);
		text(&f_ui18m, x + 16 + 26 + 12, cy - 21, bp ? "Bus Pirate 6" : "Badge", on ? WB_COLOR_TEXT_HI :
		     WB_COLOR_TEXT_MID, 0);
		if (bp && B.present && B.open)
			snprintf(s2, sizeof(s2), "Connected \xc2\xb7 %s", B.mode);
		else
			snprintf(s2, sizeof(s2), "%s", sub);
		text(&f_mono14, x + 16 + 26 + 12, cy + 2, s2, on ? WB_COLOR_TEXT_MID : WB_COLOR_PLACEHOLDER, 0);
		if (on)
			box(x + pw - 16 - 10, cy - 5, 10, 10, 2, bp ? WB_COLOR_PINK : WB_COLOR_OK, 255, 0, 0, 0);
	}
	y += 60 + 24;
	for (int i = 0; i < N_TOOLS; i++) {
		int x = X0 + (i % 3) * (tw + 20), ty = y + (i / 3) * (th + 20), on = i == O.tile;

		wb_card(x, ty, tw, th, WB_RADIUS_LG, on, &GLOW_FOCUS);
		icon(tile_icons[i], x + 18, ty + 18, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		if (B.present)
			bp_tag(x + tw - 18 - text_w(&f_mono13, "BP", 0) - 14, ty + 18 + 10);
		text(&f_ui22, x + 18, ty + th - 16 - 20 - 30, tool_names[i], on ? WB_COLOR_CYAN : WB_COLOR_TEXT_HI, 0);
		tile_status(i, st, sizeof(st));
		text(&f_mono14, x + 18, ty + th - 16 - 19, st, WB_COLOR_TEXT_MID, 0);
	}
	hint_bar(hints, 2);
}

/* ---- Pins ---------------------------------------------------------------------- */

#define PIN_H		46			/* a pin's row */
#define PIN_STEP	50

static void pin_row(int y, int on, const char *name, const char *where, const char *mode, int out, int level,
		    const char *val)
{
	int x = X0, cy = y + PIN_H / 2;

	wb_card(x, y, XW, PIN_H, WB_RADIUS_MD, on, &GLOW_FOCUS);
	text(&f_ui20, x + 18, y + 2, name, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_HI, 0);
	text(&f_mono13, x + 18, y + 26, where, WB_COLOR_TEXT_MID, 0);
	text(&f_ui18, x + 18 + 150 + 16, text_top(&f_ui18, cy), mode, on ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	if (out) {
		level_switch(x + XW - 18 - 52, cy, level);
	} else {
		const char *s = val ? val : level ? "HIGH" : "LOW";
		int tx = x + XW - 18 - text_w(&f_mono16b, s, 0);

		text(&f_mono16b, tx, text_top(&f_mono16b, cy), s, level ? WB_COLOR_PINK : WB_COLOR_TEXT_MID, 0);
		if (level)
			box(tx - 18, cy - 5, 10, 10, 2, WB_COLOR_PINK, 255, 0, 0, 0);
		else
			box(tx - 18, cy - 5, 10, 10, 2, 0, 0, WB_COLOR_TEXT_MID, 2, 0);
	}
}

static void draw_missing(void);

static void draw_pins(void)
{
	static const struct hint hints[] = { { "#2", "toggle", 0 }, { "Enter", "mode", 0 }, { "Esc", "back", 1 } };
	int y;

	header(EB_APP, "Pins");
	if (O.src[T_PINS] == SRC_BP) {
		if (!B.open) {
			draw_missing();
			hint_bar_src(hints + 2, 1, SRC_BP);
			return;
		}
		y = BODY;
		for (int i = 0; i < BP_PINS; i++, y += PIN_STEP + 6) {
			char name[8], mode[40], where[24];

			snprintf(name, sizeof(name), "IO%d", i);
			snprintf(where, sizeof(where), "Bus Pirate \xc2\xb7 %s", B.pin_f[i][0] ? B.pin_f[i] : "-");
			snprintf(mode, sizeof(mode), "%s", bp_dir[i] ? "Output" : "Input");
			pin_row(y, O.focus == i, name, where, mode, bp_dir[i], bp_dir[i] ? bp_lvl[i] :
				bp_dv_of(B.pin_v[i]) >= 15, bp_dir[i] ? 0 : B.pin_v[i][0] ? B.pin_v[i] : "\xe2\x80\xa6");
		}
	} else {
		draw_header_map(BODY, HM_PINS, O.focus);
		y = BODY + MAP_H + 8;
		for (int i = 0; i < HW_PINS; i++, y += PIN_STEP) {
			const char *mode = hw_pins[i].busy ? "In use by the UART" : hw_mode_names[hw_pins[i].mode];

			pin_row(y, O.focus == i, pin_label(i), hw_pins[i].where, mode, hw_pins[i].mode == PM_OUT,
				hw_pins[i].level, 0);
		}
	}
	hint_bar_src(hints, 3, O.src[T_PINS]);
}

/* ---- Logic ----------------------------------------------------------------------- */

static void draw_logic(void)
{
	static const struct hint hints[] = { { "#1", "options", 0 }, { "#2", "capture", 0 }, { "#3", "save", 0 },
					     { "Esc", "back", 1 } };
	int src = O.src[T_LOGIC], ch = src == SRC_BP ? BP_PINS : HW_LA_CH, lh = src == SRC_BP ? 48 : 76;
	int px = X0, pw = XW, y = BODY, x, py, ph, wx, lanes_y;
	char v[24];

	header(EB_APP, "Logic analyzer");
	if (src == SRC_BP && !B.open) {
		draw_missing();
		hint_bar_src(hints + 3, 1, SRC_BP);
		return;
	}
	x = chip(X0, y, "Rate", hz(v, src == SRC_BP ? bp_rates[LA.rate_i[1]] : badge_rates[LA.rate_i[0]]));
	if (LA.trig[src] < 0)
		snprintf(v, sizeof(v), "none");
	else
		snprintf(v, sizeof(v), "IO%d %s", LA.trig[src], LA.falling ? "\xe2\x86\x93" : "\xe2\x86\x91");
	x = chip(x, y, "Trigger", v);
	snprintf(v, sizeof(v), "%dk", la_sizes[LA.size_i[src]] / 1024);
	chip(x, y, "Samples", v);
	if (LA.status[0] && LA.src == src)
		status_dot(y, LA.status, LA.status_bad ? WB_COLOR_ERROR : WB_COLOR_OK);
	py = y + 32 + 12;
	ph = BOTTOM - 12 - py;
	console_box(px, py, pw, ph);	/* the traces on black: no scene behind them */
	wx = px + 80;
	lanes_y = py + 8;
	for (int c = 0; c < ch; c++) {
		int ly = lanes_y + c * lh, top = ly + 10, bot = ly + lh - 10, prev = -1;
		char name[8];
		const char *sub = src == SRC_BP ? B.pin_f[c] : hw_pins[c].where + 8;

		if (c)
			fill(px + 1, ly, pw - 2, 1, WB_COLOR_SURFACE1);
		snprintf(name, sizeof(name), "IO%d", c);
		text(&f_mono16b, px + 14, ly + lh / 2 - 17, src == SRC_BADGE ? pin_label(c) : name, WB_COLOR_TEXT_HI, 0);
		text(&f_mono13, px + 14, ly + lh / 2 + 2, src == SRC_BP ? (sub[0] && strcmp(sub, "-") ? sub : "") :
		     sub, WB_COLOR_PLACEHOLDER, 0);
		if (!LA.n || LA.src != src)
			continue;
		/* the trace: recorded as what decides it (the capture, where it
		 * is scrolled to, the zoom), not as its hundreds of fills; drawn
		 * only in the bands it lies in */
		if (wb_rec) {
			int k[] = { 0x4c41, LA.gen, LA.n, LA.pos, LA.zoom, c, wx, top, bot };

			wb_rec_rows(top, bot + 2, k, sizeof(k));
			continue;
		}
		if (bot + 2 <= fb_y0 || top >= fb_y1)
			continue;
		for (int q = 0; q < LA_W; q++) {
			int s0 = LA.pos + (LA.zoom >= 0 ? q << LA.zoom : q >> -LA.zoom);
			int s1 = LA.zoom >= 0 ? s0 + (1 << LA.zoom) : s0 + 1, hi = 0, lo = 0, first;

			if (s0 >= LA.n)
				break;
			if (s1 > LA.n)
				s1 = LA.n;
			first = LA.buf[s0] >> c & 1;
			for (int i = s0; i < s1; i++) {
				if (LA.buf[i] >> c & 1)
					hi = 1;
				else
					lo = 1;
			}
			if ((hi && lo) || (prev >= 0 && prev != first))
				fill(wx + q, top, 2, bot - top + 2, WB_COLOR_CYAN);
			if (hi)
				fill(wx + q, top, 1, 2, WB_COLOR_CYAN);
			if (lo)
				fill(wx + q, bot, 1, 2, WB_COLOR_CYAN);
			prev = LA.buf[s1 - 1] >> c & 1;
		}
	}
	/* the time axis, and the trigger at the start */
	fill(wx, py + ph - 26, LA_W, 1, WB_COLOR_LINE);
	if (LA.n && LA.src == src) {
		int span = la_span();

		for (int k = 0; k < 5; k++) {
			long long s = LA.pos + (long long)span * k / 4;
			int tx = wx + LA_W * k / 4;

			dur(v, LA.rate ? s * 1000000000LL / LA.rate : 0);
			tx -= k == 0 ? 0 : k == 4 ? text_w(&f_mono13, v, 0) : text_w(&f_mono13, v, 0) / 2;
			text(&f_mono13, tx, py + ph - 20, v, WB_COLOR_PLACEHOLDER, 0);
		}
		if (LA.trig[src] >= 0 && LA.pos == 0) {
			for (int d = py + 1; d < py + ph - 26; d += 8)
				fill(wx, d, 2, 5, WB_COLOR_PINK);
			box(wx - 8, py + 4, 18, 18, 2, WB_COLOR_PINK, 255, 0, 0, 0);
			text(&f_mono13, wx - 3, py + 6, "T", WB_COLOR_VOID, 0);
		}
	} else {
		text(&f_ui18, wx + 16, text_top(&f_ui18, lanes_y + ch * lh / 2), src == SRC_BP && !B.sump ?
		     "Set the Bus Pirate's binmode to SUMP" : "No capture yet: the triangle captures", WB_COLOR_TEXT_MID, 0);
	}
	hint_bar_src(hints, 4, src);
}

/* ---- Buses ---------------------------------------------------------------------- */

static const struct wb_icon *const bus_icons[N_BUSES] = { &b_i2c, &b_uart, &b_spi, &b_1wire };
static const char *const bus_names[N_BUSES] = { "I\xc2\xb2" "C", "UART", "SPI", "1-Wire" };
static const char *const bus_what[N_BUSES] = { "Scan the bus, read and write registers", "Terminal to a serial device",
					       "Flash chips, displays, sensors", "Temperature sensors, iButtons" };
static const char *const bus_meta[N_BUSES] = { "J7 \xc2\xb7 SDA, SCL", "J3 \xc2\xb7 TX IO3, RX IO4", "Bus Pirate",
					       "Bus Pirate" };

static int bus_on_badge(int b)
{
	return b == BUS_I2C || b == BUS_UART;
}

static void draw_buses(void)
{
	static const struct hint hints[] = { { "Enter", "open", 0 }, { "Esc", "back", 1 } };
	int y = WB_ROWS_TOP;

	header(EB_APP, "Buses");
	for (int i = 0; i < N_BUSES; i++, y += 76 + 10) {
		int on = i == O.bus, missing = !bus_on_badge(i) && !B.present, x = X0, cy = y + 38, rx;
		char meta[48];

		if (missing && !on)
			dashed(x, y, XW, 76);
		else
			wb_card(x, y, XW, 76, WB_RADIUS_MD, on, &GLOW_FOCUS);
		icon(bus_icons[i], x + 20, cy - 15, on ? WB_COLOR_CYAN : missing ? WB_COLOR_TEXT_LOW : WB_COLOR_TEXT_MID, 255);
		text(&f_ui22, x + 20 + 30 + 16, y + 10, bus_names[i], on ? WB_COLOR_CYAN : missing ? WB_COLOR_TEXT_MID :
		     WB_COLOR_TEXT_HI, 0);
		text(&f_ui16, x + 20 + 30 + 16, y + 42, bus_what[i], WB_COLOR_TEXT_MID, 0);
		icon(&r_chevron, x + XW - 20 - 20, cy - 10, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		rx = x + XW - 20 - 20 - 16;
		if (missing)
			snprintf(meta, sizeof(meta), "Needs Bus Pirate");
		else if (B.present && bus_on_badge(i))
			snprintf(meta, sizeof(meta), "%s \xc2\xb7 BP", bus_meta[i]);
		else
			snprintf(meta, sizeof(meta), "%s", bus_meta[i]);
		text(&f_mono13, rx - text_w(&f_mono13, meta, 0), y + 44, meta, missing ? WB_COLOR_PLACEHOLDER :
		     WB_COLOR_TEXT_MID, 0);
		{
			int ix = rx;

			if (B.present)
				ix = bp_tag(ix - text_w(&f_mono13, "BP", 0) - 14, y + 22) - text_w(&f_mono13, "BP", 0) - 14 - 6;
			if (bus_on_badge(i))
				icon(&o_badge20, ix - 20, y + 12, WB_COLOR_TEXT_MID, 255);
		}
	}
	text_wrap(&f_ui16, X0, y + 10, XW, 22, "Other Bus Pirate modes (2-wire, 3-wire, infrared) are in its terminal, "
		  "under Serial.", WB_COLOR_TEXT_MID, 2, 1);
	hint_bar(hints, 2);
}

static void draw_i2c(void)
{
	static const struct hint hints[] = { { "#1", "options", 0 }, { "#2", "scan", 0 }, { "Enter", "registers", 0 },
					     { "Esc", "back", 1 } };
	int src = O.src[T_BUSES], y = BODY, gy, cw = (XW - 28 - 36 - 15 * 3) / 16;
	char v[48];

	header(EB_BUSES, "I\xc2\xb2" "C scan");
	if (src == SRC_BP && !B.open) {
		draw_missing();
		hint_bar_src(hints + 3, 1, SRC_BP);
		return;
	}
	if (src == SRC_BP) {
		chip(X0, y, "Bus", "Bus Pirate \xc2\xb7 IO0 SDA, IO1 SCL");
	} else {
		int x = chip(X0, y, "Bus", "J7 \xc2\xb7 SDA 7, SCL 8");

		snprintf(v, sizeof(v), "%u kHz", hw_i2c_khz());
		chip(x, y, "Speed", v);
	}
	if (I2C.scanned && I2C.src == src) {
		snprintf(v, sizeof(v), "%d found", I2C.n);
		status_dot(y, v, I2C.n ? WB_COLOR_OK : WB_COLOR_TEXT_MID);
	}
	gy = y + 32 + 12;
	wb_card(X0, gy, XW, 12 + 16 + 4 + 8 * 25 + 12, WB_RADIUS_MD, 0, 0);
	for (int c = 0; c < 16; c++) {
		char h[2] = { "0123456789ABCDEF"[c], 0 };

		text(&f_mono13, X0 + 14 + 36 + c * (cw + 3) + cw / 2 - 4, gy + 12, h, WB_COLOR_PLACEHOLDER, 0);
	}
	for (int r = 0; r < 8; r++) {
		int ry = gy + 12 + 16 + 4 + r * 25;

		snprintf(v, sizeof(v), "%02X", r * 16);
		text(&f_mono13, X0 + 14, ry + 3, v, WB_COLOR_PLACEHOLDER, 0);
		for (int c = 0; c < 16; c++) {
			int a = r * 16 + c, cx = X0 + 14 + 36 + c * (cw + 3);

			if (I2C.scanned && I2C.src == src && I2C.found[a]) {
				box(cx, ry, cw, 22, 4, WB_COLOR_PINK, 41, WB_COLOR_PINK, 1, 0);
				snprintf(v, sizeof(v), "%02X", a);
				text(&f_mono13, cx + cw / 2 - text_w(&f_mono13, v, 0) / 2, ry + 3, v, WB_COLOR_PINK, 0);
			} else if (a >= 8 && a < 0x78) {
				fill(cx + cw / 2 - 1, ry + 10, 2, 2, WB_COLOR_LINE);
			}
		}
	}
	y = gy + 12 + 16 + 4 + 8 * 25 + 12 + 12;
	if (!I2C.scanned || I2C.src != src) {
		text(&f_ui18, X0 + 4, y + 8, "The triangle scans the bus.", WB_COLOR_TEXT_MID, 0);
	} else if (!I2C.n) {
		text(&f_ui18, X0 + 4, y + 8, "Nothing answered. Check the wiring and the pull-ups.", WB_COLOR_TEXT_MID, 0);
	} else {
		int first = I2C.focus > 2 ? I2C.focus - 2 : 0;

		for (int k = first; k < I2C.n && y + 52 <= BOTTOM - 4; k++, y += 60) {
			int a = i2c_found_nth(k), on = k == I2C.focus;

			wb_card(X0, y, XW, 52, WB_RADIUS_MD, on, &GLOW_FOCUS);
			snprintf(v, sizeof(v), "0x%02X", a);
			text(&f_mono18, X0 + 18, text_top(&f_mono18, y + 26), v, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
			text(&f_ui18, X0 + 18 + 70 + 16, text_top(&f_ui18, y + 26), i2c_hint(a), on ? WB_COLOR_TEXT_HI :
			     WB_COLOR_TEXT_MID, 0);
			icon(&r_chevron, X0 + XW - 18 - 20, y + 16, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		}
	}
	hint_bar_src(hints, 4, src);
}

static void draw_regs(void)
{
	static const struct hint hints[] = { { "#2", "read again", 0 }, { "#1", "write", 0 }, { "Esc", "back", 1 } };
	char t[64];
	int y = BODY;

	snprintf(t, sizeof(t), "0x%02X", I2C.addr);
	header(EB_BUSES " I\xc2\xb2" "C", t);
	text(&f_ui18, X0 + 6, y, i2c_hint(I2C.addr), WB_COLOR_TEXT_MID, 0);
	y += 34;
	wb_card(X0, y, XW, 16 * 24 + 40, WB_RADIUS_MD, 0, 0);
	for (int c = 0; c < 16; c++) {
		snprintf(t, sizeof(t), "%X", c);
		text(&f_mono13, X0 + 14 + 40 + c * 34 + 8, y + 10, t, WB_COLOR_PLACEHOLDER, 0);
	}
	for (int r = 0; r < 16; r++) {
		int ry = y + 30 + r * 24;

		snprintf(t, sizeof(t), "%02X", r * 16);
		text(&f_mono13, X0 + 14, ry, t, WB_COLOR_PLACEHOLDER, 0);
		for (int c = 0; c < 16 && I2C.regs_ok; c++) {
			snprintf(t, sizeof(t), "%02X", I2C.regs[r * 16 + c]);
			text(&f_mono14, X0 + 14 + 40 + c * 34, ry, t, I2C.regs[r * 16 + c] ? WB_COLOR_TEXT_HI :
			     WB_COLOR_TEXT_LOW, 0);
		}
	}
	if (!I2C.regs_ok)
		text(&f_ui18, X0 + 60, y + 120, "Could not read the registers", WB_COLOR_ERROR, 0);
	hint_bar(hints, 3);
}

/* ---- the terminal ---------------------------------------------------------------- */

static void draw_term(void)
{
	static const struct hint hints[] = { { "#1", "options", 0 }, { "Enter", "send", 0 }, { "Esc", "clear", 1 } };
	static const struct hint back[] = { { "#1", "options", 0 }, { "Enter", "send", 0 }, { "Esc", "back", 1 } };
	int y = BODY, py, ph, ly, tool = TM.kind == TM_USB || TM.kind == TM_BP ? T_SERIAL : T_BUSES;
	char v[48];
	const char *title = TM.kind == TM_UART ? "UART" : TM.kind == TM_USB ? "Serial" : TM.kind == TM_SPI ? "SPI" :
			    TM.kind == TM_1WIRE ? "1-Wire" : "Bus Pirate";

	header(TM.kind == TM_USB || TM.kind == TM_BP ? EB_APP : EB_BUSES, title);
	if (O.src[tool] == SRC_BP && !B.open) {
		draw_missing();
		hint_bar_src(back + 2, 1, SRC_BP);
		return;
	}
	if (TM.kind == TM_UART) {
		int x;

		draw_header_map(y, HM_UART, -1);
		y += MAP_H + 12;
		x = chip(X0, y, "TX", "IO3");
		x = chip(x, y, "RX", "IO4");
		snprintf(v, sizeof(v), "%u 8N1", uart_bauds[O.uart_baud]);
		chip(x, y, "Baud", v);
		{
			const char *w = "3.3 V only";
			int wx = X0 + XW - text_w(&f_ui16, w, 0);

			icon(&s_warn, wx - 22, y + 8, WB_COLOR_YELLOW, 255);
			text(&f_ui16, wx, text_top(&f_ui16, y + 16), w, WB_COLOR_TEXT_MID, 0);
		}
		y += 32 + 12;
	} else if (TM.kind == TM_USB) {
		int x = chip(X0, y, "Port", TM.usb_dev[0] ? TM.usb_dev : "none");

		snprintf(v, sizeof(v), "%u 8N1", usb_bauds[O.usb_baud]);
		chip(x, y, "Baud", v);
		y += 32 + 12;
	} else {
		chip(X0, y, "Mode", B.mode[0] ? B.mode : "\xe2\x80\xa6");
		y += 32 + 12;
	}
	py = y;
	ph = BOTTOM - 12 - py;
	console_box(X0, py, XW, ph);	/* the lines on black: no scene behind them */
	fill(X0 + 1, py + ph - 44, XW - 2, 1, WB_COLOR_LINE);
	/* the lines, newest at the bottom */
	ly = py + ph - 44 - 10 - 21;
	if (TM.part[0]) {
		text(&f_mono14, X0 + 16, ly, wb_fit(&f_mono14, TM.part, XW - 32, v, sizeof(v)), WB_COLOR_TEXT_MID, 0);
		ly -= 21;
	}
	for (int i = TM.n - 1; i >= 0 && ly > py + 6; i--, ly -= 21) {
		char b[110];

		text(&f_mono14, X0 + 16, ly, wb_fit(&f_mono14, TM.line[i], XW - 32, b, sizeof(b)),
		     TM.tx[i] ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
	}
	if (!TM.n && !TM.part[0])
		text(&f_ui16, X0 + 16, py + 12, TM.kind == TM_USB && TM.usb_fd < 0 && !getenv("OHM_USB") ?
		     "No USB serial adapter on the bottom port" : "Type below; Enter sends the line.", WB_COLOR_PLACEHOLDER, 0);
	{
		int iy = py + ph - 22, x = text(&f_mono14, X0 + 16, text_top(&f_mono14, iy), "\xe2\x80\xba",
						WB_COLOR_PINK, 0) + 10;

		x = text(&f_mono14, x, text_top(&f_mono14, iy), wb_fit(&f_mono14, TM.in, XW - 60, v, sizeof(v)),
			 WB_COLOR_TEXT_HI, 0);
		fill(x + 1, iy - 9, 9, 18, WB_COLOR_CYAN);
	}
	hint_bar_src(TM.len ? hints : back, 3, TM.kind == TM_SPI || TM.kind == TM_1WIRE ? -1 : O.src[tool]);
}

/* ---- LEDs --------------------------------------------------------------------------- */

static const char *const colour_names[] = { "Red", "Green", "Blue", "White", "Yellow", "Cyan", "Magenta", "Orange" };
static const uint32_t colours[] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff, 0xffc000, 0x00ffff, 0xff00ff, 0xff6000 };
#define N_COLOURS	8

static struct { int colour, pct, on; } BL = { 1, 30, 0 };	/* the badge's LEDs */

static int bp_colour(void)
{
	for (int i = 0; i < N_COLOURS; i++)
		if (colours[i] == B.led_rgb)
			return i;
	return 1;
}

/* a row as the menu's (wb_row): right is WB_ROW_OPEN where Enter opens a
 * choice, WB_ROW_SWITCH for on and off */
#define LED_STEP	78

static void led_row(int y, int on, const char *label, const char *value, int right, int swon)
{
	wb_row(X0, y, XW, WB_LIST_ROW_H, 0, label, 0, value, right, swon, on);
}

static int led_rows(void)
{
	return O.src[T_LEDS] == SRC_BP ? 5 : 3;
}

static void draw_leds(void)
{
	static const struct hint hints[] = { { "#2", "on / off", 0 }, { "\xe2\x86\x90 \xe2\x86\x92", "adjust", 0 },
					     { "Esc", "back", 1 } };
	int y = WB_ROWS_TOP;
	char v[32];

	header(EB_APP, "LEDs");
	if (O.src[T_LEDS] == SRC_BP) {
		if (!B.open) {
			draw_missing();
			hint_bar_src(hints + 2, 1, SRC_BP);
			return;
		}
		led_row(y, O.focus == 0, "Type", bp_led_types[B.led_type], WB_ROW_OPEN, 0);
		led_row(y += LED_STEP, O.focus == 1, "Colour", colour_names[bp_colour()], WB_ROW_OPEN, 0);
		snprintf(v, sizeof(v), "%d %%", B.led_pct);
		led_row(y += LED_STEP, O.focus == 2, "Brightness", v, WB_ROW_NONE, 0);
		snprintf(v, sizeof(v), "%d", B.led_n);
		led_row(y += LED_STEP, O.focus == 3, "LEDs on the strip", B.led_type == LT_ONBOARD ? "1" : v,
			WB_ROW_OPEN, 0);
		led_row(y += LED_STEP, O.focus == 4, "Lit", 0, WB_ROW_SWITCH, B.led_set >= 0);
		y += LED_STEP + 4;
		text_wrap(&f_ui16, X0, y, XW, 22, B.led_type == LT_ONBOARD ? "The Bus Pirate's own LED." :
			  "Data on IO0 (APA102: clock on IO1). The strip runs on Vout: the supply goes on at 5 V if "
			  "nothing is on it.", WB_COLOR_TEXT_MID, 3, 1);
	} else {
		led_row(y, O.focus == 0, "Colour", colour_names[BL.colour], WB_ROW_OPEN, 0);
		snprintf(v, sizeof(v), "%d %%", BL.pct);
		led_row(y += LED_STEP, O.focus == 1, "Brightness", v, WB_ROW_NONE, 0);
		led_row(y += LED_STEP, O.focus == 2, "Lit", 0, WB_ROW_SWITCH, BL.on);
		y += LED_STEP + 4;
		text_wrap(&f_ui16, X0, y, XW, 22, "A WS2812 chain on J7 pin 4 (LED), from +3V3 or +5V and GND: the four "
			  "LEDs of WhyOS's LED driver (kernel patch 0032).", WB_COLOR_TEXT_MID, 3, 1);
	}
	hint_bar_src(hints, 3, O.src[T_LEDS]);
}

static void leds_apply(void)
{
	if (O.src[T_LEDS] == SRC_BP) {
		bp_leds(B.led_set < 0);
		return;
	}
	for (int i = 0; i < hw_leds(); i++)
		hw_led_set(i, BL.on ? colours[BL.colour] : 0, BL.pct);
}

/* ---- Power ---------------------------------------------------------------------------- */

static void draw_missing(void)
{
	int y = BODY + 70;
	const char *t = "No Bus Pirate connected";

	if (B.present && B.notice[0]) {	/* there, but it did not answer */
		icon(&o_plug72, W / 2 - 36, y, WB_COLOR_TEXT_LOW, 255);
		text(&f_ui28b, W / 2 - text_w(&f_ui28b, "The Bus Pirate does not answer", 0) / 2, y + 72 + 16,
		     "The Bus Pirate does not answer", WB_COLOR_TEXT_HI, 0);
		text_wrap(&f_ui20, W / 2 - 230, y + 72 + 16 + 50, 460, 28, B.notice, WB_COLOR_TEXT_MID, 4, 1);
		return;
	}
	icon(&o_plug72, W / 2 - 36, y, WB_COLOR_TEXT_LOW, 255);
	text(&f_ui28b, W / 2 - text_w(&f_ui28b, t, 0) / 2, y + 72 + 16, t, WB_COLOR_TEXT_HI, 0);
	{	/* centred lines */
		static const char *const l[3] = { "Plug a Bus Pirate 6 into the badge's bottom", "USB port. It takes a few seconds to connect;",
						  "this screen fills in by itself." };

		for (int i = 0; i < 3; i++)
			text(&f_ui20, W / 2 - text_w(&f_ui20, l[i], 0) / 2, y + 72 + 16 + 50 + i * 28, l[i], WB_COLOR_TEXT_MID, 0);
	}
}

static void slider(int x, int y, int w, int pct, int on)
{
	int kx = x + w * pct / 100;

	box(x, y, w, 8, 2, 0xFFFFFF, 36, 0, 0, 0);
	box(x, y, kx - x + 4, 8, 2, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
	box(kx - 12, y - 8, 24, 24, 4, WB_COLOR_TEXT_HI, 255, on ? WB_COLOR_CYAN : WB_COLOR_LINE, 3, on ? &GLOW_FOCUS : 0);
}

static void draw_power(void)
{
	static const struct hint hints[] = { { "#2", "supply", 0 }, { "\xe2\x86\x90 \xe2\x86\x92", "adjust", 0 },
					     { "Esc", "back", 1 } };
	int y = BODY;
	char v[24];

	header(EB_APP, "Power");
	if (O.src[T_POWER] == SRC_BADGE) {
		static const struct { const char *name, *where, *note; } rails[3] = {
			{ "+5V", "J3 pin 1", "Fixed, not switchable, not measured" },
			{ "+3V3", "J7 pin 1", "Fixed, not switchable, not measured" },
			{ "GND", "J3 pin 8 \xc2\xb7 J7 pins 3, 10", "Common ground" },
		};

		y += 6;
		text(&f_mono13, X0 + 6, y, "HEADER RAILS", WB_COLOR_PLACEHOLDER, 21);
		y += 17 + 10;
		for (int i = 0; i < 3; i++, y += 78) {
			int on = O.focus == i;

			wb_card(X0, y, XW, 68, WB_RADIUS_MD, on, &GLOW_FOCUS);
			text(&f_mono18, X0 + 20, text_top(&f_mono18, y + 34), rails[i].name, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_HI, 0);
			text(&f_ui20, X0 + 20 + 92 + 16, y + 8, rails[i].where, WB_COLOR_TEXT_HI, 0);
			text(&f_ui16, X0 + 20 + 92 + 16, y + 38, rails[i].note, WB_COLOR_TEXT_MID, 0);
		}
		y += 10;
		dashed(X0, y, XW, 100);
		icon(&t_plug, X0 + 20, y + 32, WB_COLOR_PINK, 255);
		text(&f_ui20, X0 + 20 + 34 + 18, y + 16, "Need a set voltage or a current limit?", WB_COLOR_TEXT_HI, 0);
		text_wrap(&f_ui16, X0 + 20 + 34 + 18, y + 46, XW - 20 - 34 - 18 - 20, 22,
			  "A Bus Pirate 6 on the bottom port adds a supply of 0.8\xe2\x80\x93" "5.0 V with a current limit. "
			  "Tab shows it.", WB_COLOR_TEXT_MID, 2, 1);
		hint_bar_src(hints + 2, 1, SRC_BADGE);
		return;
	}
	if (!B.open) {
		draw_missing();
		hint_bar_src(hints + 2, 1, SRC_BP);
		return;
	}
	/* the readout */
	wb_card(X0, y, XW, 92, WB_RADIUS_MD, 0, 0);
	text(&f_mono13, X0 + 22, y + 16, "VOUT", WB_COLOR_PLACEHOLDER, 21);
	snprintf(v, sizeof(v), "%.*s", (int)strcspn(B.vout, "V"), B.vout[0] ? B.vout : "0.0");
	{
		int x = text(&f_ui36b, X0 + 22, y + 36, v, WB_COLOR_TEXT_HI, -11);

		text(&f_ui22, x + 6, y + 46, "V", WB_COLOR_TEXT_MID, 0);
	}
	fill(X0 + 22 + 170, y + 16, 1, 60, WB_COLOR_LINE);
	text(&f_mono13, X0 + 22 + 194, y + 16, "CURRENT", WB_COLOR_PLACEHOLDER, 21);
	snprintf(v, sizeof(v), "%.*s", (int)strcspn(B.iout, "m"), B.psu ? B.iout : "0");
	{
		int x = text(&f_ui36b, X0 + 22 + 194, y + 36, v, WB_COLOR_TEXT_HI, -11);

		text(&f_ui22, x + 6, y + 46, "mA", WB_COLOR_TEXT_MID, 0);
	}
	box(X0 + XW - 22 - text_w(&f_ui16, "every 1 s", 0) - 14, y + 22, 8, 8, 2, WB_COLOR_OK, 255, 0, 0, 0);
	text(&f_ui16, X0 + XW - 22 - text_w(&f_ui16, "every 1 s", 0), y + 16, "every 1 s", WB_COLOR_TEXT_MID, 0);
	y += 92 + 10;
	/* supply, voltage, current limit, pull-ups */
	for (int i = 0; i < 4; i++) {
		int on = O.focus == i, h = i == 1 || i == 2 ? 84 : 60;

		wb_card(X0, y, XW, h, WB_RADIUS_MD, on, &GLOW_FOCUS);
		if (i == 0 || i == 3) {
			icon(i ? &p_pullup : &p_power, X0 + 20, y + 18, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
			text(&f_ui22, X0 + 20 + 24 + 16, text_top(&f_ui22, y + 30), i ? "Pull-ups" : "Supply", WB_COLOR_TEXT_HI, 0);
			wb_switch(X0 + XW - 18 - 52, y + 30, i ? B.pullups : B.psu);
		} else {
			if (i == 1)
				snprintf(v, sizeof(v), "%d.%02d V", B.dv / 10, B.dv % 10 * 10);
			else if (B.ma)
				snprintf(v, sizeof(v), "%d mA", B.ma);
			else
				snprintf(v, sizeof(v), "no limit");
			text(&f_ui22, X0 + 20, y + 12, i == 1 ? "Voltage" : "Current limit", WB_COLOR_TEXT_HI, 0);
			text(&f_mono18, X0 + XW - 20 - text_w(&f_mono18, v, 0), y + 14, v, on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 0);
			slider(X0 + 24, y + 56, XW - 48, i == 1 ? (B.dv - 8) * 100 / 42 : B.ma * 100 / 500, on);
		}
		y += h + 10;
	}
	if (B.notice[0])
		text(&f_ui16, X0, y, B.notice, B.notice_bad ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);
	hint_bar_src(hints, 3, SRC_BP);
}

/* ---- log, settings, help, pop-ups -------------------------------------------------- */

static void draw_log(void)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int y = BODY + 16;
	char b[110];

	header(EB_APP, "Log");
	if (!O.n_log) {
		text(&f_ui20, X0 + 6, WB_ROWS_TOP, "Nothing sent or read yet. What the tools do shows here.",
		     WB_COLOR_TEXT_MID, 0);
		hint_bar(hints, 1);
		return;
	}
	wb_card(X0, BODY, XW, BOTTOM - 12 - BODY, WB_RADIUS_MD, 0, 0);
	for (int i = O.n_log - 1; i >= 0 && y + 28 < BOTTOM - 12; i--, y += 28)
		text(&f_mono14, X0 + 18, y, wb_fit(&f_mono14, O.log[i], XW - 36, b, sizeof(b)),
		     i == O.n_log - 1 ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	hint_bar(hints, 1);
}

#define N_SET		5
#define SET_STEP	78

static void draw_settings(void)
{
	static const struct hint hints[] = { { "\xe2\x86\x90 \xe2\x86\x92", "change", 0 }, { "Enter", "open", 0 },
					     { "Esc", "back", 1 } };
	static const char *const labels[N_SET] = { "Header map seen from", "Outputs back to inputs on close",
						   "Badge UART speed", "USB serial speed", "Pin names" };
	static const char *const notes[N_SET] = {
		"How the pin map draws J3 and J7: from the screen side, or turned over.",
		"So nothing stays driven when OHMinous closes.",
		"The badge's UART is software: the faster rates may lose bytes.",
		"For a USB serial adapter on the bottom port (Serial).",
		"Your names for IO0 to IO4 and STROBE, shown on every tool.",
	};
	int y = WB_ROWS_TOP;
	char v[32];

	header(EB_APP, "Settings");
	for (int i = 0; i < N_SET; i++, y += SET_STEP) {
		if (i == 0)
			snprintf(v, sizeof(v), "%s", O.back_view ? "The back" : "The screen side");
		else if (i == 2)
			snprintf(v, sizeof(v), "%u baud", uart_bauds[O.uart_baud]);
		else if (i == 3)
			snprintf(v, sizeof(v), "%u baud", usb_bauds[O.usb_baud]);
		else
			v[0] = 0;
		wb_row(X0, y, XW, WB_LIST_ROW_H, 0, labels[i], 0, v, i == 1 ? WB_ROW_SWITCH : WB_ROW_OPEN, O.safe,
		       O.focus == i);
	}
	text_wrap(&f_ui16, X0 + 6, y + 4, XW - 6, 22, notes[O.focus], WB_COLOR_TEXT_MID, 2, 1);
	hint_bar(hints, 3);
}

/* a key cap as the design's help has it */
static int help_cap(int x, int cy, const char *keys)
{
	int sym = one_sym(keys), w = (sym ? 12 : text_w(&f_mono14, keys, 0)) + 18;

	box(x, cy - 12, w, 25, 4, WB_COLOR_SURFACE2, 255, WB_COLOR_LINE, 1, 0);
	if (sym)
		icon(keycap_syms[sym - 1], x + 9, cy - 6, key_colors[sym - 1], 255);
	else
		text(&f_mono14, x + 9, text_top(&f_mono14, cy), keys, WB_COLOR_TEXT_HI, 0);
	return x + w;
}

static void draw_help(void)
{
	static const struct hint hs[] = { { "Esc", "close", 1 } };
	static const struct help_key start_keys[] = {
		{ "Enter", "Open the tool" }, { "Tab", "In a tool: switch Badge / Bus Pirate" },
		{ "#4", "Log: what the tools sent and read" }, { "#5", "Settings: pin names, safe defaults" },
		{ "Esc", "Close OHMinous" }, { "Logo", "Back to the menu" },
	};
	static const struct help_key tool_keys[] = {
		{ "Tab", "Badge or Bus Pirate" }, { "#1", "Options" }, { "#2", "The main action: toggle, capture, scan" },
		{ "#3", "Logic: save the capture" }, { "\xe2\x86\x90\xe2\x86\x92", "Adjust; Logic: scroll" },
		{ "\xe2\x86\x91\xe2\x86\x93", "Choose a row; Logic: zoom" }, { "Esc", "Back to the start" },
	};
	const struct help_key *k = O.sc == SC_START ? start_keys : tool_keys;
	int n = O.sc == SC_START ? 6 : 7, x = POP_X + 32, tw = POP_W - 64;
	const char *about = O.sc == SC_START ?
		"Drive the pins on the badge's J3 and J7 headers. Plug a Bus Pirate 6 into the bottom port and each "
		"tool gets a Bus Pirate tab: 8 pins, a programmable supply, faster capture and more buses." :
		"Every tool works on the badge's own header pins, and with a Bus Pirate 6 on the bottom port.";
	int lines = text_wrap(&f_ui18, 0, 0, tw, 25, about, 0, 4, 0);
	int h = 26 + 18 + 6 + 36 + 6 + lines * 25 + 16 + (O.sc == SC_START ? 46 + 16 : 0) + 17 + 8 + n * HELP_ROW +
		24 + 48;
	int y = (H - h) / 2;

	/* frosted, as the menu's pop-ups, with its key at the bottom */
	pop_frame(y, h);
	panel_hints(x, tw, y + h - 48 - 10, hs, 1);
	y += 26;
	icon(&kc_diamond, x, y + 1, WB_COLOR_KEY_DIAMOND, 255);
	eyebrow(x + 20, y, "HELP");
	text(&f_ui28b, x, y + 26, O.sc == SC_START ? "OHMinous" : "Tools", WB_COLOR_TEXT_HI, 0);
	y += 18 + 6 + 36 + 6;
	text_wrap(&f_ui18, x, y, tw, 25, about, WB_COLOR_TEXT_MID, 4, 1);
	y += lines * 25 + 16;
	if (O.sc == SC_START) {
		int bw = (tw - 12) / 2;

		box(x, y, bw, 46, 4, 0xFFFFFF, 15, 0, 0, 0);
		edge_rect(x, y, bw, 46, 0xFFFFFF, 46);
		icon(&o_badge20, x + 12, y + 13, WB_COLOR_TEXT_MID, 255);
		text(&f_ui16, x + 12 + 22 + 10, text_top(&f_ui16, y + 23), "Badge tab: always there", WB_COLOR_TEXT_HI, 0);
		box(x + bw + 12, y, bw, 46, 4, 0xFFFFFF, 15, 0, 0, 0);
		edge_rect(x + bw + 12, y, bw, 46, 0xFFFFFF, 46);
		text(&f_ui16, bp_tag(x + bw + 12 + 12, y + 23) + 10, text_top(&f_ui16, y + 23), "Bus Pirate adds options",
		     WB_COLOR_TEXT_HI, 0);
		y += 46 + 16;
	}
	text(&f_mono13, x, y, "KEYS ON THIS SCREEN", WB_COLOR_PLACEHOLDER, 21);
	y += 17 + 8;
	for (int i = 0; i < n; i++, y += HELP_ROW) {
		char b[80];

		if (i)
			fill(x, y, tw, 1, 0x26262F);
		help_cap(x, y + HELP_ROW / 2, k[i].keys);
		text(&f_ui18, x + 88, text_top(&f_ui18, y + HELP_ROW / 2), wb_fit(&f_ui18, k[i].what, tw - 88, b, sizeof(b)),
		     WB_COLOR_TEXT_HI, 0);
	}
}

/* the pick list (options, choices): items and the current one */
static struct menu_item pick_it[12];
static int pick_n, pick_cur;
static char pick_title[48], pick_eb[24];

enum { PK_LA_OPTS, PK_LA_RATE, PK_LA_TRIG, PK_LA_SIZE, PK_LA_EDGE, PK_UART_BAUD, PK_USB_BAUD, PK_LED_TYPE,
       PK_LED_COLOUR, PK_PIN_OPTS, PK_I2C_OPTS, PK_VIEW };

static void pick_open(int what, const char *eb, const char *title, int cur)
{
	O.pop = POP_PICK;
	O.pick_what = what;
	O.pf = cur > 0 ? cur : 0;
	pick_cur = cur;
	snprintf(pick_eb, sizeof(pick_eb), "%s", eb);
	snprintf(pick_title, sizeof(pick_title), "%s", title);
}

static struct menu_item *pick_add(int reset, const char *label)
{
	if (reset)
		pick_n = 0;
	memset(&pick_it[pick_n], 0, sizeof(pick_it[0]));
	pick_it[pick_n].label = label;
	return &pick_it[pick_n++];
}

static void draw_input(void)
{
	static const struct hint hs[] = { { "Enter", "done", 0 }, { "Esc", "cancel", 1 } };
	int x = DLG_X + 32, w = DLG_W - 64, y, tx;

	/* the menu's text dialog: the field in the dialog, the keys under it */
	y = dialog("OHMINOUS", O.in_title, 56, 0, 0, 0, 0, -1);
	box_focus(x, y, w, 56, WB_RADIUS_MD, &GLOW_FOCUS);
	tx = text(&f_mono18, x + 20, text_top(&f_mono18, y + 28), O.in_text, WB_COLOR_TEXT_HI, 0);
	fill(tx + 1, y + 16, 2, 24, WB_COLOR_CYAN);
	dlg_hints(hs, 2);
}

static void draw_pop(void)
{
	static const struct hint hs[] = { { "Enter", "choose", 0 }, { "Esc", "cancel", 1 } };

	if (O.pop == POP_HELP)
		draw_help();
	else if (O.pop == POP_PICK)
		menu_popup(pick_eb, pick_title, 0, pick_it, 0, pick_n, O.pf, pick_cur, 0, 0, hs, 2);
	else if (O.pop == POP_INPUT)
		draw_input();
}

/*
 * The screen, whole: libwb clips it to the band in fb_y0..fb_y1. It runs
 * once more a frame than the bands drawn (the recording, redraw), so it
 * only draws: no state changes, no reading the hardware.
 */
static void paint(void *unused)
{
	(void)unused;
	switch (O.sc) {
	case SC_PINS:
		draw_pins();
		break;
	case SC_LOGIC:
		draw_logic();
		break;
	case SC_BUSES:
		draw_buses();
		break;
	case SC_I2C:
		draw_i2c();
		break;
	case SC_REGS:
		draw_regs();
		break;
	case SC_TERM:
		draw_term();
		break;
	case SC_LEDS:
		draw_leds();
		break;
	case SC_POWER:
		draw_power();
		break;
	case SC_LOG:
		draw_log();
		break;
	case SC_SETTINGS:
		draw_settings();
		break;
	default:
		draw_start();
	}
	if (O.notice[0] && O.sc != SC_START && O.sc != SC_POWER && !O.pop) {
		int w = text_w(&f_ui16, O.notice, 0) + 32;

		w = w > XW ? XW : w;
		/* glass, edged in red when something went wrong */
		wb_card(W / 2 - w / 2, BOTTOM - 46, w, 36, WB_RADIUS_MD, 0, 0);
		if (O.notice_bad)
			edge_rect(W / 2 - w / 2, BOTTOM - 46, w, 36, WB_COLOR_ERROR, 255);
		{
			char b[128];

			text(&f_ui16, W / 2 - w / 2 + 16, text_top(&f_ui16, BOTTOM - 28),
			     wb_fit(&f_ui16, O.notice, w - 32, b, sizeof(b)), O.notice_bad ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
		}
	}
	/* A frosted pop-up blurs what is under it, which needs the whole
	 * picture (redraw); in a band, libwb draws it as darker unblurred
	 * glass. */
	draw_pop();
	if (over_msg)
		draw_over();
}

/* ---- output: the panel, or a PPM on the host --------------------------------------- */

#define BAND_H	40
static uint16_t band[W * BAND_H];
#ifdef OHM_HOST
static uint16_t screen[W * H];
#else
static int fb_fd = -1;
#endif
static long tm_out;		/* us writing out this frame (OHM_UI_TIMING) */

/* rows y0..y1 of the frame, in b, to the panel */
static void panel_out(const uint16_t *b, int y0, int y1)
{
	long t = wb_now_us();

#ifdef OHM_HOST
	memcpy(screen + y0 * W, b, (size_t)(y1 - y0) * W * 2);
#else
	if (pwrite(fb_fd, b, (size_t)(y1 - y0) * W * 2, (off_t)y0 * W * 2) < 0)
		return;
#endif
	tm_out += wb_now_us() - t;
}

/* OHM_UI_TIMING=1: a line a frame on stderr (the app's log): the time, the
 * bands drawn, libwb's time in the scene and in glass, and writing out */
static void timing(long t0, int whole_frame, int bands)
{
	static int on = -1;
	char line[112];

	if (on < 0)
		on = getenv("OHM_UI_TIMING") != 0;
	if (on) {
		int n = snprintf(line, sizeof(line), "ohminous: frame %ld us (%s, %d bands), scene %ld, glass %ld, out %ld\n",
				 wb_now_us() - t0, whole_frame ? "whole" : "bands", bands, tm_scene, tm_glass, tm_out);

		if (write(2, line, (size_t)n) < 0)
			on = 0;
	}
	tm_scene = tm_glass = tm_out = 0;
}

/*
 * The screen in bands of 40 rows (57 KB); with a pop-up or a message up,
 * in a whole buffer (1 MB, taken while it is up), as a frosted pop-up blurs
 * the picture under it, rows above and below the band too. Either way the
 * frame is recorded first and only the bands that changed since the last
 * one are drawn and written out (libwb wb_bands, wb_frame): a focus that
 * moves draws its two rows, a pin whose level changed its row, a pop-up's
 * focus the pop-up's rows (its blur kept).
 */
static uint16_t *whole;

static void redraw(void)
{
	long t0 = wb_now_us();
	int n;

	if (O.pop || over_msg) {
		if (!whole && (whole = malloc((size_t)W * H * 2)))
			wb_rec_invalidate();	/* nothing in it yet */
	} else if (whole) {
		free(whole);
		whole = 0;
	}
	if (whole) {
		n = wb_frame(paint, 0, whole, BAND_H, panel_out);
		timing(t0, 1, n);
		return;
	}
	n = wb_bands(paint, 0, band, BAND_H, panel_out);
	timing(t0, 0, n);
}

/* ---- keys ------------------------------------------------------------------------------- */

static int la_cancel(void);
static struct { int shift, fn; } K;

static void la_capture(void)
{
	int src = O.src[T_LOGIC], n = la_sizes[LA.size_i[src]], got = 0;
	unsigned rate = src == SRC_BP ? bp_rates[LA.rate_i[1]] : badge_rates[LA.rate_i[0]], rgot = rate;
	char r[24], d[24];

	snprintf(LA.status, sizeof(LA.status), "%s", LA.trig[src] >= 0 ? "Waiting for the trigger\xe2\x80\xa6" :
		 "Capturing\xe2\x80\xa6");
	LA.status_bad = 0;
	LA.src = src;
	LA.n = 0;
	LA.gen++;
	redraw();
	if (src == SRC_BP) {
		got = bp_capture(LA.buf, n, rate, LA.trig[1], LA.falling, la_cancel);
		if (got < 0) {
			snprintf(LA.status, sizeof(LA.status), "%s", B.notice);
			LA.status_bad = 1;
			return;
		}
		LA.ch = BP_PINS;
	} else {
		int rc;

		if (rate > hw_rate_max())
			rate = hw_rate_max();
		rc = hw_capture(LA.buf, n, rate, LA.trig[0], LA.falling, &got, &rgot, la_cancel);
		if (rc < 0) {
			snprintf(LA.status, sizeof(LA.status), "%s", hw_err);
			LA.status_bad = 1;
			return;
		}
		LA.ch = HW_LA_CH;
		rate = rgot;
	}
	if (!got) {
		snprintf(LA.status, sizeof(LA.status), "Cancelled");
		return;
	}
	LA.n = got;
	LA.rate = rate;
	LA.zoom = la_fit(got);
	LA.pos = 0;
	snprintf(LA.status, sizeof(LA.status), src == SRC_BP ? "Captured" : "Captured \xc2\xb7 software");
	snprintf(d, sizeof(d), "%d samples", got);
	log_add("Logic: %s", d);
	hz(r, rate);
	(void)r;
}

static void la_save(void)
{
	char path[64] = "/root/capture.bin", m[96];
	FILE *f;

#ifdef OHM_HOST
	if (getenv("APP_CONFIG"))
		snprintf(path, sizeof(path), "%.50s/capture.bin", getenv("APP_CONFIG"));
#endif
	if (!LA.n) {
		notice("Nothing captured yet", 1);
		return;
	}
	if (!(f = fopen(path, "wb")) || fwrite(LA.buf, 1, (size_t)LA.n, f) != (size_t)LA.n) {
		if (f)
			fclose(f);
		notice("Could not write /root/capture.bin", 1);
		return;
	}
	fclose(f);
	snprintf(m, sizeof(m), "Saved to /root/capture.bin: %d channels, a byte per sample", LA.ch);
	notice(m, 0);
	log_add("%s", m);
}

/* the source switched, or a tool opened: connect what it needs */
static void source_changed(int tool)
{
	O.notice[0] = 0;
	if (O.src[tool] == SRC_BP) {
		if (bp_ready() < 0)
			return;
		if (tool == T_PINS || tool == T_POWER)
			bp_measure();
	}
}

static void open_term(int kind)
{
	term_close();
	memset(TM.line, 0, sizeof(TM.line));
	TM.n = TM.len = 0;
	TM.in[0] = TM.part[0] = 0;
	TM.kind = kind;
	if (O.sc != SC_TERM)
		TM.from = O.sc;
	O.sc = SC_TERM;
	if (kind == TM_UART) {
		if (hw_uart_open(uart_bauds[O.uart_baud]) < 0)
			notice(hw_err, 1);
	} else if (kind == TM_USB) {
		if (usb_serial_open() < 0)
			notice("No USB serial adapter on the bottom port", 1);
	} else {
		if (bp_ready() < 0)
			return;
		if (kind == TM_SPI)
			bp_mode("spi");
		else if (kind == TM_1WIRE)
			bp_mode("1wire");
		term_recv(B.rx, (int)strlen(B.rx));
	}
}

static void open_tool(int t)
{
	O.focus = 0;
	O.notice[0] = 0;
	switch (t) {
	case T_PINS:
		O.sc = SC_PINS;
		break;
	case T_LOGIC:
		O.sc = SC_LOGIC;
		break;
	case T_BUSES:
		O.sc = SC_BUSES;
		return;
	case T_LEDS:
		O.sc = SC_LEDS;
		break;
	case T_POWER:
		O.sc = SC_POWER;
		break;
	default:
		open_term(O.src[T_SERIAL] == SRC_BP ? TM_BP : TM_USB);
		return;
	}
	source_changed(t);
}

static int tool_of(int sc)
{
	switch (sc) {
	case SC_PINS:
		return T_PINS;
	case SC_LOGIC:
		return T_LOGIC;
	case SC_I2C:
	case SC_BUSES:
		return T_BUSES;
	case SC_LEDS:
		return T_LEDS;
	case SC_POWER:
		return T_POWER;
	case SC_TERM:
		return TM.kind == TM_USB || TM.kind == TM_BP ? T_SERIAL : T_BUSES;
	}
	return -1;
}

static void open_input(int what, const char *title, const char *value)
{
	O.pop = POP_INPUT;
	O.in_what = what;
	snprintf(O.in_title, sizeof(O.in_title), "%s", title);
	snprintf(O.in_text, sizeof(O.in_text), "%s", value);
}

enum { IN_NAME, IN_REG, IN_LED_N };

static void input_done(void)
{
	switch (O.in_what) {
	case IN_NAME:
		snprintf(O.names[O.focus < HW_PINS ? O.focus : 0], sizeof(O.names[0]), "%s", O.in_text);
		settings_save();
		break;
	case IN_REG: {
		unsigned reg, val;
		char m[48];

		if (sscanf(O.in_text, "%x %x", &reg, &val) != 2 || reg > 255 || val > 255) {
			notice("Type the register and the value in hex: 10 FF", 1);
			break;
		}
		{
			uint8_t b = (uint8_t)val;

			if (I2C.src == SRC_BP) {
				char cmd[48];

				snprintf(cmd, sizeof(cmd), "[0x%02x 0x%02x 0x%02x]", I2C.addr << 1, reg, val);
				bp_cmd(cmd);
			} else if (hw_i2c_write(I2C.addr, (int)reg, &b, 1) < 0) {
				notice(hw_err, 1);
				break;
			}
		}
		snprintf(m, sizeof(m), "0x%02X: register 0x%02X = 0x%02X", I2C.addr, reg, val);
		log_add("I\xc2\xb2" "C %s", m);
		i2c_regs();
		break;
	}
	case IN_LED_N: {
		int n = atoi(O.in_text);

		if (n >= 1 && n <= 300) {
			B.led_n = n;
			if (B.led_set >= 0)
				bp_leds(0);
		}
		break;
	}
	}
}

static void pick_done(int i)
{
	int src;

	switch (O.pick_what) {
	case PK_LA_OPTS:
		src = O.src[T_LOGIC];
		if (i == 0) {
			pick_add(1, "");
			pick_n = 0;
			for (int k = 0; k < (src == SRC_BP ? 5 : 3); k++) {
				static char names[5][16];

				hz(names[k], src == SRC_BP ? bp_rates[k] : badge_rates[k]);
				pick_add(0, names[k]);
			}
			pick_open(PK_LA_RATE, "LOGIC ANALYZER", "Sample rate", LA.rate_i[src]);
		} else if (i == 1) {
			static char names[9][8];

			pick_add(1, "None");
			for (int k = 0; k < (src == SRC_BP ? BP_PINS : HW_LA_CH); k++) {
				snprintf(names[k], sizeof(names[k]), "IO%d", k);
				pick_add(0, names[k]);
			}
			pick_open(PK_LA_TRIG, "LOGIC ANALYZER", "Trigger", LA.trig[src] + 1);
		} else if (i == 2) {
			pick_add(1, "Rising");
			pick_add(0, "Falling");
			pick_open(PK_LA_EDGE, "LOGIC ANALYZER", "Edge", LA.falling);
		} else {
			pick_add(1, "2k");
			pick_add(0, "8k");
			if (src == SRC_BP)
				pick_add(0, "32k");
			pick_open(PK_LA_SIZE, "LOGIC ANALYZER", "Samples", LA.size_i[src]);
		}
		return;
	case PK_LA_RATE:
		LA.rate_i[O.src[T_LOGIC]] = i;
		break;
	case PK_LA_TRIG:
		LA.trig[O.src[T_LOGIC]] = i - 1;
		break;
	case PK_LA_EDGE:
		LA.falling = i;
		break;
	case PK_LA_SIZE:
		LA.size_i[O.src[T_LOGIC]] = i;
		break;
	case PK_UART_BAUD:
		O.uart_baud = i;
		settings_save();
		if (O.sc == SC_TERM && TM.kind == TM_UART)
			open_term(TM_UART);
		break;
	case PK_USB_BAUD:
		O.usb_baud = i;
		settings_save();
		if (O.sc == SC_TERM && TM.kind == TM_USB)
			open_term(TM_USB);
		break;
	case PK_LED_TYPE:
		B.led_type = i;
		if (B.led_set >= 0)
			bp_leds(0);
		break;
	case PK_LED_COLOUR:
		if (O.src[T_LEDS] == SRC_BP) {
			B.led_rgb = colours[i];
			if (B.led_set >= 0)
				bp_leds(0);
		} else {
			BL.colour = i;
			leds_apply();
		}
		break;
	case PK_PIN_OPTS:
		if (i == 0)
			open_input(IN_NAME, "Name for the pin", O.names[O.focus]);
		else if (i == 1) {
			O.names[O.focus][0] = 0;
			settings_save();
		} else {
			for (int k = 0; k < HW_PINS; k++)
				if (!hw_pins[k].busy)
					hw_set_mode(k, PM_IN);
			notice("Every pin is an input again", 0);
		}
		return;
	case PK_I2C_OPTS:
		if (i == 0)
			open_input(IN_REG, "Register and value, hex", "");
		return;
	case PK_VIEW:
		O.back_view = i;
		settings_save();
		break;
	}
}

static int pop_key(int code)
{
	if (O.pop == POP_HELP) {
		if (code == KEY_ESC || code == KEY_SYM_HELP)
			O.pop = POP_NONE;
		return 1;
	}
	if (O.pop == POP_INPUT) {
		int len = (int)strlen(O.in_text);
		char ch = key_char(code, K.shift);

		if (code == KEY_ESC) {
			O.pop = POP_NONE;
		} else if (code == KEY_ENTER) {
			O.pop = POP_NONE;
			input_done();
		} else if (code == KEY_BACKSPACE) {
			if (len)
				O.in_text[len - 1] = 0;
		} else if (ch && len < (int)sizeof(O.in_text) - 1 && (O.in_what != IN_NAME || ch != ' ')) {
			O.in_text[len] = ch;
			O.in_text[len + 1] = 0;
		}
		return 1;
	}
	if (code == KEY_UP && O.pf > 0)
		O.pf--;
	else if (code == KEY_DOWN && O.pf < pick_n - 1)
		O.pf++;
	else if (code == KEY_ESC)
		O.pop = POP_NONE;
	else if (code == KEY_ENTER) {
		O.pop = POP_NONE;
		pick_done(O.pf);
	}
	return 1;
}

static int la_cancel(void)
{
#ifdef OHM_HOST
	return 0;
#else
	extern int ev_cancel(void);

	return ev_cancel();
#endif
}

/* the badge's or the Bus Pirate's pin: Enter its mode, the triangle its level */
static void pins_key(int code)
{
	int src = O.src[T_PINS], n = src == SRC_BP ? BP_PINS : HW_PINS, i = O.focus;
	char t[64];

	if (code == KEY_UP && O.focus > 0)
		O.focus--;
	else if (code == KEY_DOWN && O.focus < n - 1)
		O.focus++;
	if (src == SRC_BP) {
		if (!B.open)
			return;
		if (code == KEY_ENTER) {
			bp_dir[i] = !bp_dir[i];
			bp_pin(i, bp_dir[i] ? bp_lvl[i] : -1);
		} else if (code == KEY_SYM_MAIN && bp_dir[i]) {
			bp_lvl[i] = !bp_lvl[i];
			bp_pin(i, bp_lvl[i]);
			snprintf(t, sizeof(t), "BP IO%d %s", i, bp_lvl[i] ? "high" : "low");
			log_add("Pins: %s", t);
		}
		return;
	}
	if (hw_pins[i].busy) {
		if (code == KEY_ENTER || code == KEY_SYM_MAIN)
			notice("The UART uses this pin while its screen is open", 1);
		return;
	}
	if (code == KEY_ENTER) {
		if (hw_set_mode(i, (hw_pins[i].mode + 1) % N_PMODES) < 0)
			notice(hw_err, 1);
	} else if (code == KEY_SYM_MAIN) {
		if (hw_pins[i].mode != PM_OUT) {
			notice("An input: Enter makes it an output", 1);
			return;
		}
		hw_set_level(i, !hw_pins[i].level);
		snprintf(t, sizeof(t), "%s %s", pin_label(i), hw_pins[i].level ? "high" : "low");
		log_add("Pins: %s", t);
	} else if (code == KEY_SYM_MENU) {
		pick_add(1, "Name");
		pick_add(0, "Name back to the default");
		pick_add(0, "Every pin an input")->sep = 1;
		pick_open(PK_PIN_OPTS, "PINS", pin_label(i), -2);
	}
}

static void logic_key(int code)
{
	int span = la_span(), src = O.src[T_LOGIC];

	if (src == SRC_BP && !B.open)
		return;
	switch (code) {
	case KEY_SYM_MAIN:
		la_capture();
		break;
	case KEY_SYM_SECOND:
		la_save();
		break;
	case KEY_SYM_MENU: {
		static char v[4][24];

		pick_add(1, "Sample rate");
		hz(v[0], src == SRC_BP ? bp_rates[LA.rate_i[1]] : badge_rates[LA.rate_i[0]]);
		strcpy(pick_it[0].value, v[0]);
		pick_add(0, "Trigger");
		if (LA.trig[src] < 0)
			strcpy(pick_it[1].value, "none");
		else
			snprintf(pick_it[1].value, sizeof(pick_it[1].value), "IO%d", LA.trig[src]);
		pick_add(0, "Edge");
		strcpy(pick_it[2].value, LA.falling ? "falling" : "rising");
		pick_add(0, "Samples");
		snprintf(pick_it[3].value, sizeof(pick_it[3].value), "%dk", la_sizes[LA.size_i[src]] / 1024);
		pick_open(PK_LA_OPTS, "LOGIC ANALYZER", "Capture", -2);
		break;
	}
	case KEY_LEFT:
	case KEY_RIGHT:
		if (!LA.n)
			break;
		LA.pos += (code == KEY_LEFT ? -1 : 1) * span / 4;
		if (LA.pos > LA.n - span)
			LA.pos = LA.n - span;
		if (LA.pos < 0)
			LA.pos = 0;
		break;
	case KEY_UP:
		if (LA.n && LA.zoom > -3)
			LA.zoom--;
		break;
	case KEY_DOWN:
		if (LA.n && LA.zoom < la_fit(LA.n))
			LA.zoom++;
		if (LA.pos > LA.n - la_span())
			LA.pos = LA.n - la_span() > 0 ? LA.n - la_span() : 0;
		break;
	}
}

static void power_key(int code)
{
	int d = code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : 0;

	if (O.src[T_POWER] == SRC_BADGE) {
		if (code == KEY_UP && O.focus > 0)
			O.focus--;
		else if (code == KEY_DOWN && O.focus < 2)
			O.focus++;
		return;
	}
	if (!B.open)
		return;
	if (code == KEY_UP && O.focus > 0)
		O.focus--;
	else if (code == KEY_DOWN && O.focus < 3)
		O.focus++;
	else if (code == KEY_SYM_MAIN || (code == KEY_ENTER && O.focus == 0)) {
		bp_psu(!B.psu);
		log_add(B.psu ? "Power: the supply on at %s" : "Power: the supply off%s", B.psu ? B.vout : "");
	} else if (code == KEY_ENTER && O.focus == 3) {
		bp_pullups(!B.pullups);
	} else if (d && O.focus == 1) {
		B.dv += d;
		B.dv = B.dv < 8 ? 8 : B.dv > 50 ? 50 : B.dv;
		if (B.psu)
			bp_psu(1);
	} else if (d && O.focus == 2) {
		B.ma += d * 10;
		B.ma = B.ma < 0 ? 0 : B.ma > 500 ? 500 : B.ma;
		if (B.psu)
			bp_psu(1);
	} else if (d && (O.focus == 0 || O.focus == 3)) {
		if (O.focus == 0 && (d > 0) != B.psu)
			bp_psu(d > 0);
		else if (O.focus == 3 && (d > 0) != B.pullups)
			bp_pullups(d > 0);
	}
}

static void leds_key(int code)
{
	int d = code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : 0, bp = O.src[T_LEDS] == SRC_BP;
	int lit = bp ? 4 : 2;

	if (bp && !B.open)
		return;
	if (code == KEY_UP && O.focus > 0)
		O.focus--;
	else if (code == KEY_DOWN && O.focus < led_rows() - 1)
		O.focus++;
	else if (code == KEY_SYM_MAIN || (code == KEY_ENTER && O.focus == lit)) {
		if (bp)
			bp_leds(B.led_set >= 0);
		else {
			BL.on = !BL.on;
			leds_apply();
		}
		log_add("LEDs: %s", (bp ? B.led_set >= 0 : BL.on) ? "on" : "off");
	} else if (code == KEY_ENTER && bp && O.focus == 0) {
		for (int i = 0; i < N_LTYPES; i++)
			pick_add(!i, bp_led_types[i]);
		pick_open(PK_LED_TYPE, "LEDS", "Type", B.led_type);
	} else if (code == KEY_ENTER && O.focus == (bp ? 1 : 0)) {
		for (int i = 0; i < N_COLOURS; i++)
			pick_add(!i, colour_names[i]);
		pick_open(PK_LED_COLOUR, "LEDS", "Colour", bp ? bp_colour() : BL.colour);
	} else if (code == KEY_ENTER && bp && O.focus == 3) {
		char v[8];

		snprintf(v, sizeof(v), "%d", B.led_n);
		open_input(IN_LED_N, "LEDs on the strip, 1 to 300", v);
	} else if (d && O.focus == (bp ? 2 : 1)) {
		int *p = bp ? &B.led_pct : &BL.pct;

		*p += d * 10;
		*p = *p < 10 ? 10 : *p > 100 ? 100 : *p;
		if (bp ? B.led_set >= 0 : BL.on)
			leds_apply();
	} else if (d && O.focus == (bp ? 1 : 0)) {
		int c = ((bp ? bp_colour() : BL.colour) + d + N_COLOURS) % N_COLOURS;

		if (bp) {
			B.led_rgb = colours[c];
			if (B.led_set >= 0)
				bp_leds(0);
		} else {
			BL.colour = c;
			if (BL.on)
				leds_apply();
		}
	}
}

static void term_key(int code)
{
	char ch = key_char(code, K.shift);

	if (code == KEY_ENTER) {
		if (TM.kind == TM_USB && TM.usb_fd < 0 && !getenv("OHM_USB"))
			open_term(TM_USB);
		else if (TM.kind >= TM_BP && !B.open)
			return;
		else
			term_send();
	} else if (code == KEY_BACKSPACE) {
		if (TM.len)
			TM.in[--TM.len] = 0;
	} else if (code == KEY_SYM_MENU) {
		if (TM.kind == TM_UART) {
			static char v[N_UART_BAUDS][16];

			for (int i = 0; i < N_UART_BAUDS; i++) {
				snprintf(v[i], sizeof(v[i]), "%u", uart_bauds[i]);
				pick_add(!i, v[i]);
			}
			pick_open(PK_UART_BAUD, "UART", "Speed", O.uart_baud);
		} else if (TM.kind == TM_USB) {
			static char v[N_USB_BAUDS][16];

			for (int i = 0; i < N_USB_BAUDS; i++) {
				snprintf(v[i], sizeof(v[i]), "%u", usb_bauds[i]);
				pick_add(!i, v[i]);
			}
			pick_open(PK_USB_BAUD, "SERIAL", "Speed", O.usb_baud);
		}
	} else if (ch && TM.len < (int)sizeof(TM.in) - 1) {
		TM.in[TM.len++] = ch;
		TM.in[TM.len] = 0;
	}
}

static void settings_key(int code)
{
	int d = code == KEY_LEFT ? -1 : code == KEY_RIGHT ? 1 : code == KEY_ENTER ? 1 : 0;

	if (code == KEY_UP && O.focus > 0)
		O.focus--;
	else if (code == KEY_DOWN && O.focus < N_SET - 1)
		O.focus++;
	else if (d) {
		switch (O.focus) {
		case 0:
			O.back_view = !O.back_view;
			break;
		case 1:
			O.safe = code == KEY_ENTER ? !O.safe : d > 0;
			break;
		case 2:
			O.uart_baud = (O.uart_baud + d + N_UART_BAUDS) % N_UART_BAUDS;
			break;
		case 3:
			O.usb_baud = (O.usb_baud + d + N_USB_BAUDS) % N_USB_BAUDS;
			break;
		case 4:
			if (code == KEY_ENTER) {	/* the pins, to name: from Pins' options */
				O.back = SC_START;
				O.sc = SC_PINS;
				O.src[T_PINS] = SRC_BADGE;
				O.focus = 0;
				notice("The square key on a pin: its name", 0);
				return;
			}
			break;
		}
		settings_save();
	}
}

/* One key; 1 to close. */
static int key(int code)
{
	int tool = tool_of(O.sc);

	if (O.pop) {
		pop_key(code);
		return 0;
	}
	if (code == KEY_SYM_HELP) {
		O.pop = POP_HELP;
		return 0;
	}
	if (code == KEY_SYM_LOG || code == KEY_SYM_SETTINGS) {
		if (O.sc != SC_LOG && O.sc != SC_SETTINGS)
			O.back = O.sc;
		O.sc = code == KEY_SYM_LOG ? SC_LOG : SC_SETTINGS;
		O.focus = 0;
		return 0;
	}
	if (code == KEY_TAB && tool >= 0 && !(O.sc == SC_TERM && TM.kind >= TM_SPI)) {
		O.src[tool] = !O.src[tool];
		O.focus = 0;
		if (O.sc == SC_TERM && TM.kind == TM_UART && O.src[tool] == SRC_BP)
			open_term(TM_BP), TM.kind = TM_BP;	/* the Bus Pirate's UART: its terminal */
		else if (O.sc == SC_TERM && tool == T_BUSES && O.src[tool] == SRC_BADGE)
			open_term(TM_UART);
		else if (O.sc == SC_TERM)
			open_term(O.src[tool] == SRC_BP ? TM_BP : TM_USB);
		else
			source_changed(tool);
		return 0;
	}
	if (code == KEY_ESC) {
		if (O.notice[0] && O.sc != SC_START) {
			O.notice[0] = 0;
			return 0;
		}
		switch (O.sc) {
		case SC_START:
			return 1;
		case SC_LOG:
		case SC_SETTINGS:
			O.sc = O.back;
			O.focus = 0;
			return 0;
		case SC_REGS:
			O.sc = SC_I2C;
			return 0;
		case SC_I2C:
			O.sc = SC_BUSES;
			return 0;
		case SC_TERM:
			if (TM.len) {
				TM.in[0] = 0;
				TM.len = 0;
				return 0;
			}
			term_close();
			O.sc = TM.from;
			return 0;
		default:
			O.sc = SC_START;
			return 0;
		}
	}
	switch (O.sc) {
	case SC_START:
		if (code == KEY_LEFT && O.tile % 3)
			O.tile--;
		else if (code == KEY_RIGHT && O.tile % 3 < 2)
			O.tile++;
		else if (code == KEY_UP && O.tile >= 3)
			O.tile -= 3;
		else if (code == KEY_DOWN && O.tile < 3)
			O.tile += 3;
		else if (code == KEY_ENTER)
			open_tool(O.tile);
		break;
	case SC_PINS:
		pins_key(code);
		break;
	case SC_LOGIC:
		logic_key(code);
		break;
	case SC_BUSES:
		if (code == KEY_UP && O.bus > 0)
			O.bus--;
		else if (code == KEY_DOWN && O.bus < N_BUSES - 1)
			O.bus++;
		else if (code == KEY_ENTER) {
			O.back = SC_BUSES;
			if (O.bus == BUS_I2C) {
				O.sc = SC_I2C;
				source_changed(T_BUSES);
			} else if (O.bus == BUS_UART) {
				open_term(O.src[T_BUSES] == SRC_BP ? TM_BP : TM_UART);
				if (O.src[T_BUSES] == SRC_BP && B.open)
					bp_mode("uart");
			} else if (!B.present) {
				notice("This bus needs a Bus Pirate on the bottom port", 1);
			} else {
				open_term(O.bus == BUS_SPI ? TM_SPI : TM_1WIRE);
			}
		}
		break;
	case SC_I2C:
		if (O.src[T_BUSES] == SRC_BP && !B.open)
			break;
		if (code == KEY_SYM_MAIN)
			i2c_scan();
		else if (code == KEY_UP && I2C.focus > 0)
			I2C.focus--;
		else if (code == KEY_DOWN && I2C.focus < I2C.n - 1)
			I2C.focus++;
		else if (code == KEY_ENTER && I2C.n && I2C.src == O.src[T_BUSES]) {
			I2C.addr = i2c_found_nth(I2C.focus);
			i2c_regs();
			O.sc = SC_REGS;
		} else if (code == KEY_SYM_MENU) {
			pick_add(1, "Write a register");
			pick_open(PK_I2C_OPTS, "I\xc2\xb2" "C", "Options", -2);
		}
		break;
	case SC_REGS:
		if (code == KEY_SYM_MAIN)
			i2c_regs();
		else if (code == KEY_SYM_MENU)
			open_input(IN_REG, "Register and value, hex", "");
		break;
	case SC_TERM:
		term_key(code);
		break;
	case SC_LEDS:
		leds_key(code);
		break;
	case SC_POWER:
		power_key(code);
		break;
	case SC_SETTINGS:
		settings_key(code);
		break;
	}
	return 0;
}

/* what is on screen, for a test */
static int where_text(char *o, int size)
{
	static const char *const scn[] = { "start", "pins", "logic", "buses", "i2c", "regs", "term", "leds", "power",
					   "log", "settings" };
	int n = 0, tool = tool_of(O.sc);

	n += snprintf(o + n, (size_t)(size - n), "%s %s\nsrc %s\nfocus %d\ntile %d\nbp %d %d %s\nnotice %s\n", scn[O.sc],
		      O.pop == POP_HELP ? "help" : O.pop == POP_PICK ? "pick" : O.pop == POP_INPUT ? "input" : "",
		      tool < 0 ? "-" : O.src[tool] == SRC_BP ? "bp" : "badge", O.focus, O.tile, B.present, B.open, B.mode,
		      O.notice);
	n += snprintf(o + n, (size_t)(size - n), "pins");
	for (int i = 0; i < HW_PINS; i++)
		n += snprintf(o + n, (size_t)(size - n), " %d:%d", hw_pins[i].mode, hw_pins[i].level);
	n += snprintf(o + n, (size_t)(size - n), "\ni2c %d", I2C.scanned ? I2C.n : -1);
	for (int a = 0; a < 128; a++)
		if (I2C.found[a])
			n += snprintf(o + n, (size_t)(size - n), " %02x", a);
	n += snprintf(o + n, (size_t)(size - n), "\nla %d %u %s\nterm %d", LA.n, LA.rate, LA.status, TM.n);
	for (int i = 0; i < TM.n; i++)
		n += snprintf(o + n, (size_t)(size - n), "|%s", TM.line[i]);
	n += snprintf(o + n, (size_t)(size - n), "\npsu %d %d %d %s %s\nlog %d\n", B.psu, B.dv, B.ma, B.vout, B.iout,
		      O.n_log);
	return n;
}

/* pins made outputs go back to inputs, and the badge's LEDs off */
static void finish(void)
{
	if (O.safe)
		for (int i = 0; i < HW_PINS; i++)
			if (hw_pins[i].mode == PM_OUT && !hw_pins[i].busy)
				hw_set_mode(i, PM_IN);
	if (BL.on) {
		BL.on = 0;
		leds_apply();
	}
	term_close();
	hw_close();
	bp_close();
}

#ifdef OHM_HOST
/* ---- the host test ------------------------------------------------------------------- */

static void save_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");

	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p = screen[i];
		unsigned char c[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2),
				       (unsigned char)((p & 31) << 3) };

		fwrite(c, 3, 1, f);
	}
	fclose(f);
}

/* the screen drawn as the badge draws it after a key, to DIR/NNN.ppm */
static void shot(const char *dir, int n)
{
	char path[300];

	redraw();
	snprintf(path, sizeof(path), "%s/%03d.ppm", dir, n);
	save_ppm(path);
}

/*
 * ohminous-host KEYS [OUT.ppm]: \n Enter, \e Esc, \t Tab, \b Backspace,
 * \u \d \l \r the arrows, \1..\6 the symbol keys, \s Shift for the next
 * key, \iN the input IOn flips (read as the badge's poll reads it); other
 * characters typed. OHM_BP=1 a Bus Pirate, OHM_I2C="3c 48" the devices on
 * J7, OHM_UART_RX what the UART receives, OHM_LEVELS="1 0 1" what the
 * inputs read. OHM_FRAMES=DIR: the screen drawn after every key, as on the
 * badge, each to DIR/NNN.ppm (the test compares them with WB_FULL=1's).
 * Prints where_text.
 */
int main(int argc, char **argv)
{
	const char *k = argc > 1 ? argv[1] : "", *e, *frames = getenv("OHM_FRAMES");
	static char out[16384];
	int closed = 0, lv[HW_PINS] = { 0 }, nf = 0;

	settings_load();
	if ((e = getenv("OHM_I2C")))
		for (const char *p = e; *p; ) {
			char *end;
			long a = strtol(p, &end, 16);

			if (end == p)
				break;
			hw_sim_i2c((int)a, 1);
			p = end;
		}
	if ((e = getenv("OHM_LEVELS")))
		for (int i = 0; i < HW_PINS && *e; i++, e += e[1] ? 2 : 1)
			hw_sim_level(i, lv[i] = *e == '1');
	if ((e = getenv("OHM_UART_RX")))
		hw_sim_uart(e);
	memcpy(U.hhmm, "14:32", 6);
	U.clock_ok = 1;
	U.wifi = 3;
	U.lora = 1;
	if (hw_open() < 0)
		notice(hw_err, 1);
	bp_find();
	if (frames)		/* the first frame, as the badge draws it */
		shot(frames, nf++);
	for (; *k && !closed; k++) {
		int code = -1;

		if (*k == '\\' && k[1] == 's') {
			K.shift = 1;
			k += 2;
		}
		if (*k == '\\' && k[1] == 'i' && k[2] >= '0' && k[2] < '0' + HW_PINS) {
			int i = k[2] - '0';

			hw_sim_level(i, lv[i] ^= 1);
			hw_read_all();
			k += 2;
		} else if (*k == '\\' && k[1] && k[1] != '\\') {
			k++;
			code = *k == 'n' ? KEY_ENTER : *k == 'e' ? KEY_ESC : *k == 't' ? KEY_TAB : *k == 'b' ? KEY_BACKSPACE :
			       *k == 'u' ? KEY_UP : *k == 'd' ? KEY_DOWN : *k == 'l' ? KEY_LEFT : *k == 'r' ? KEY_RIGHT :
			       *k >= '1' && *k <= '6' ? KEY_SYM_MENU + (*k - '1') : -1;
		} else {
			k += *k == '\\';
			for (int c = 0; c < 64 && code < 0; c++) {
				if (key_char(c, 0) == *k)
					code = c;
				else if (key_char(c, 1) == *k)
					code = c, K.shift = 1;
			}
			if (*k == ' ')
				code = 57;
		}
		if (code >= 0)
			closed = key(code);
		term_poll();
		K.shift = 0;
		if (frames && !closed)
			shot(frames, nf++);
	}
	redraw();
	if (argc > 2 && argv[2][0])
		save_ppm(argv[2]);
	where_text(out, sizeof(out));
	fputs(out, stdout);
	if (closed)
		puts("closed");
	printf("uart_tx %s\nleds %06x %06x\n", hw_sim_uart_tx, (unsigned)hw_sim_led[0], (unsigned)hw_sim_led[3]);
	finish();
	return 0;
}

#else

/* ---- the badge --------------------------------------------------------------------------- */

#define FIFO	"/tmp/ohminous.keys"
#define WHERE	"/tmp/ohminous.where"

struct ev {
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static volatile sig_atomic_t stop;
static int ev_fd = -1, fifo_fd = -1, fifo_used;

static void on_signal(int s)
{
	stop = s;
}

/* Esc pressed while a capture waits for its trigger */
int ev_cancel(void)
{
	struct pollfd p = { ev_fd, POLLIN, 0 };
	struct ev e;

	while (poll(&p, 1, 0) > 0 && read(ev_fd, &e, sizeof(e)) == sizeof(e))
		if (e.type == 1 && e.value == 1 && e.code == KEY_ESC)
			return 1;
	return stop != 0;
}

static int status_read(void)
{
	time_t t = time(0);
	struct tm tm;
	char old[6];
	int wifi = U.wifi, ok = U.clock_ok, lora = U.lora;

	memcpy(old, U.hhmm, sizeof(old));
	localtime_r(&t, &tm);
	U.clock_ok = tm.tm_year >= 124;
	snprintf(U.hhmm, sizeof(U.hhmm), "%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100);
	U.wifi = wifi_bars();
	U.lora = lora_on();
	return wifi != U.wifi || ok != U.clock_ok || lora != U.lora || memcmp(old, U.hhmm, sizeof(old));
}

static void where_write(void)
{
	static char b[16384];
	int n = where_text(b, sizeof(b));
	FILE *f = fopen(WHERE ".new", "w");

	if (!f)
		return;
	fwrite(b, 1, (size_t)n, f);
	if (fclose(f) == 0)
		rename(WHERE ".new", WHERE);
}

/* keys from a test: decimal codes, s30 Shift+A */
static int fifo_keys(void)
{
	char buf[1024], *p = buf;
	int n = (int)read(fifo_fd, buf, sizeof(buf) - 1);

	if (n <= 0)
		return 0;
	buf[n] = 0;
	fifo_used = 1;
	while (*p) {
		int code = 0;

		while (*p == ' ' || *p == '\n')
			p++;
		if (!*p)
			break;
		K.shift = *p == 's';
		p += K.shift;
		while (*p >= '0' && *p <= '9')
			code = code * 10 + *p++ - '0';
		while (*p && *p != ' ' && *p != '\n')
			p++;
		if (key(code))
			return 1;
		K.shift = 0;
		redraw();
	}
	return 0;
}

int main(void)
{
	struct sigaction sa;
	long last_bp = 0, last_read = 0;
	int ticks = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, 0);
	sigaction(SIGHUP, &sa, 0);
	sigaction(SIGINT, &sa, 0);
	fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	ev_fd = open("/dev/input/event0", O_RDONLY | O_CLOEXEC);
	if (fb_fd < 0 || ev_fd < 0)
		return 1;
	unlink(FIFO);
	if (!mkfifo(FIFO, 0600))
		fifo_fd = open(FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	settings_load();
	status_read();
	if (hw_open() < 0)
		notice(hw_err, 1);
	bp_find();
	redraw();
	while (!stop) {
		struct pollfd p[4];
		int np = 0, k, draw = 0, quit = 0;
		long now;

		p[np].fd = ev_fd;
		p[np++].events = POLLIN;
		if (fifo_fd >= 0) {
			p[np].fd = fifo_fd;
			p[np++].events = POLLIN;
		}
		if (O.sc == SC_TERM && TM.kind == TM_USB && TM.usb_fd >= 0) {
			p[np].fd = TM.usb_fd;
			p[np++].events = POLLIN;
		}
		if (O.sc == SC_TERM && TM.kind == TM_UART && hw_uart_fd() >= 0) {
			p[np].fd = hw_uart_fd();
			p[np++].events = POLLIN;
		}
		k = poll(p, (nfds_t)np, 200);
		now = now_ms();
		if (term_poll())
			draw = 1;
		/* the scene turns with the badge */
		if (wb_scene_turned())
			draw = 1;
		/* the Bus Pirate comes and goes; its readings every second */
		if (now - last_bp >= 2000 && !B.open) {
			int was = B.present;

			last_bp = now;
			if (bp_find() != was)
				draw = 1;
		}
		if (now - last_read >= 1000) {
			last_read = now;
			if (O.sc == SC_PINS && O.src[T_PINS] == SRC_BADGE && !O.pop) {
				int was[HW_PINS];

				for (int i = 0; i < HW_PINS; i++)
					was[i] = hw_pins[i].level;
				hw_read_all();
				for (int i = 0; i < HW_PINS; i++)
					draw |= was[i] != hw_pins[i].level;
			}
			if (B.open && (O.sc == SC_POWER || O.sc == SC_PINS) && O.src[tool_of(O.sc)] == SRC_BP && !O.pop) {
				bp_measure();
				draw = 1;
			}
			if (++ticks % 5 == 0 && status_read())
				draw = 1;
		}
		if (O.sc == SC_PINS && O.src[T_PINS] == SRC_BADGE && !O.pop && now - last_read >= 200) {
			int was[HW_PINS], ch = 0;

			for (int i = 0; i < HW_PINS; i++)
				was[i] = hw_pins[i].level;
			hw_read_all();
			for (int i = 0; i < HW_PINS; i++)
				ch |= was[i] != hw_pins[i].level;
			draw |= ch;
		}
		if (k > 0 && fifo_fd >= 0 && np > 1 && (p[1].revents & POLLIN)) {
			quit = fifo_keys();
			draw = 1;
		}
		if (k > 0 && (p[0].revents & POLLIN)) {
			struct ev evs[16];
			int got = (int)read(ev_fd, evs, sizeof(evs)) / (int)sizeof(evs[0]);

			for (int q = 0; q < got && !quit; q++) {
				const struct ev e = evs[q];

				if (e.type != 1)
					continue;
				if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT)
					K.shift = e.value != 0;
				else if (e.code == KEY_FN)
					K.fn = e.value != 0;
				else if (key_acts(e.code, e.value) && !(e.value == 2 && key_stale(e.sec, e.usec))) {
					quit = key(e.code);
					draw = 1;
				}
			}
		}
		if (quit)
			break;
		if (draw) {
			redraw();
			if (fifo_used)
				where_write();
		}
	}
	finish();
	if (fifo_fd >= 0)
		unlink(FIFO);
	return 0;
}
#endif
