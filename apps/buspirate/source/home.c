// SPDX-License-Identifier: MIT
/*
 * home — the badge's start menu, drawn after the WhyBadge design system
 * ("Neon Orbit": the Home, Dialog and Settings screens in design/canvas).
 *
 *   Home      3 x 2 tiles. A symbol key opens its tile directly (the keys
 *             reach Linux as F1..F6: ■ Apps, ▲ Games, ✕ Tools,
 *             ● Terminal, ♣ Settings, ◆ Power); arrows + Enter also work.
 *   Lists     Apps, Games, Tools, Settings, and the network and USB detail
 *             pages: ↑↓ select, ←→ adjust, Enter open, Esc back.
 *   Power     dialog: Restart (focused, the safe choice) or Shut down.
 *   Terminal  leaves the menu for a shell on the panel.
 *   Logo key  back to the home screen; closes the menu when it popped up.
 *   Fn+Esc    screen off, any key on (what sleepd does, which gets no
 *             keys while this program holds the keypad).
 *
 * Sensor panel, DOOM and the Bus Pirate are apps from the store
 * (sdapps/). The Bus Pirate is this same program built with -DBP_APP:
 * it starts on its screens and ends where the menu would go home; the
 * menu itself is built with -DHAS_BP=0, and its Bus Pirate code drops
 * out at link time (--gc-sections).
 *
 * Started by init on the panel console with --session (inittab): then
 * Terminal execs the console getty, logged in as root (/usr/bin/autologin),
 * and init starts the menu again when that shell exits. Without
 * --session it is a pop-up over whatever the panel shows: sleepd starts
 * it on the logo key, and Terminal, Esc or the logo key on the home
 * screen close it again.
 *
 * Drawing: a 720x720 RGB565 shadow buffer, written to /dev/fb0 with one
 * pwrite per frame (no fbdev mmap on NOMMU), like
 * sensorpanel. The console is switched to KD_GRAPHICS so fbcon does not
 * draw over the menu. Fonts, icons and the glow fall-off come
 * pre-rendered from home_assets.h (home-assets/gen.py); the rest is
 * integer-only rounded rectangles, since the toolchain is soft-float.
 *
 * Build with -DHOME_PREVIEW for the host to render the screens to PPM
 * files instead (home-assets/preview.sh).
 *
 * FLAT/NOMMU constraints (see Makefile): -fPIC, no stdio in the badge
 * build, vfork+execv for children.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "home_assets.h"
#include "home-assets/wb_tokens.h"

#ifndef HOME_PREVIEW
#include <dirent.h>
#include <poll.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/un.h>
#include <linux/fs.h>
#include <sys/wait.h>
#include <strings.h>
#include <termios.h>
#include <pwd.h>
#include <stdio.h>		/* rename() only: no stdio streams in the FLAT build */
#endif

#define W		WB_SCREEN_W
#define H		WB_SCREEN_H

/* evdev codes (linux/input-event-codes.h); the symbol keys are F1..F6 in
 * the badge DT keymap, in the same order as BadgeVMS's tca8418 keymap. */
#define KEY_ESC		1
#define KEY_BACKSPACE	14
#define KEY_ENTER	28
#define KEY_F1		59
#define KEY_F6		64
#define KEY_UP		103
#define KEY_LEFT	105
#define KEY_RIGHT	106
#define KEY_DOWN	108
#define KEY_LEFTMETA	125	/* the logo key (BadgeVMS: LGUI) */
#define KEY_FN		0x1d0

#include "wbdraw.h"

/* the Bus Pirate's screens: in the menu (1) or not; BP_APP: only them */
#ifdef BP_APP
#undef HAS_BP
#define HAS_BP		1
#define APP_MODE	1
#else
#define APP_MODE	0
#endif
#ifndef HAS_BP
#define HAS_BP		1
#endif
#include "settings.h"
#include "lora_presets.h"
#include "lora_state.h"
#include "wbstatus.h"

/* ------------------------------------------------------------------ */
/* Number formatting (no stdio in the FLAT build)                      */

static char *utoa(char *p, unsigned v, int width)
{
	char tmp[12];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n < width)
		tmp[n++] = '0';
	while (n)
		*p++ = tmp[--n];
	*p = 0;
	return p;
}

static char *cat(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	*p = 0;
	return p;
}

/* ------------------------------------------------------------------ */
/* System state                                                        */

struct sys {
	int clock_ok;			/* year looks set (no RTC; NTP or by hand) */
	char hhmm[8];
	char date[40];			/* "Friday 9 October" */
	char user[40];			/* the name setting, first letter capital */
	char temp[16];			/* "21.4°C" (BME680), or empty */
	int wifi_up;
	int wifi_bars;			/* the status bar's: -1 not connected (wbstatus.h) */
	char wifi_state[24];
	char wifi_ip[20];
	char wifi_mac[24];
	int bl, bl_max;			/* panel backlight */
	int kbd;			/* keyboard backlight */
	int ble_pid;			/* ble_adv, 0 if not running */
	int lora;			/* the mesh daemon runs: radio on (lora_state.h) */
	int n_usb;
	char usb[6][2][40];		/* product, "vvvv:pppp" */
};

static struct sys S;

/*
 * Installed apps (docs/APPS.md): a directory with an app.conf, on the SD
 * card (badge/apps/<app>/) or in flash (/data/apps/<app>/). The menu
 * lists them under the tile their CATEGORY names and starts them through
 * apprun; nothing about an app is built into the menu.
 */
#define MAX_APPS	64		/* the app store makes 24 too few */
#define MAX_FILES	20

enum { CAT_APPS, CAT_GAMES, CAT_TOOLS, CAT_NONE };	/* CAT_NONE: only opens files */

struct app {
	char dir[64];			/* /mnt/sd/badge/apps/gnuboy */
	char name[32];			/* NAME */
	char info[32];			/* INFO, the row's value */
	char exec[48];			/* EXEC, in dir unless absolute */
	char args[160];			/* ARGS: %d dir, %f the chosen file */
	char files[96];			/* FILES: choose a file here first (%D, %d, %T expanded) */
	char types[32];			/* TYPES: its extensions, "gb gbc" */
	char icon[16];			/* ICON: one of the menu's (app_icon) */
	char ask[48];			/* ASK: ask for %a's text first, with this title */
	char ask_default[96];		/* ASK_DEFAULT: the text offered the first time */
	char opens[96];			/* OPENS: extensions Files opens with it ("*": any) */
	char opens_also[96];		/* OPENS_ALSO: offered under Open with only */
	int cat;			/* CATEGORY */
	int terminal;			/* TERMINAL=1: a command-line program, in appterm;
					 * 2 (TERMINAL=close): the window closes when it ends well;
					 * 3 (TERMINAL=shell): a shell follows, to run it again */
	int esc;			/* ESC=quit: Esc ends it, as its own quit key would */
	/* written by the app store (WhyOS-Apps docs/package.md §2.3) */
	char id[33];			/* ID; else the directory's name */
	char type[8];			/* TYPE: app, game, system */
	int rotation_lock;		/* ROTATION=locked */
};

static struct app A[MAX_APPS];
static int n_apps;

/* The file chooser of an app with FILES. */
static struct {
	int app;			/* index into A, -1 none */
	int n, more;			/* listed; not listed for want of room */
	char file[MAX_FILES][48];
	char name[MAX_FILES][40];	/* without the extension */
} F = { .app = -1 };

static void fill_clock(time_t t)
{
	static const char *const days[] = { "Sunday", "Monday", "Tuesday",
		"Wednesday", "Thursday", "Friday", "Saturday" };
	static const char *const months[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September",
		"October", "November", "December" };
	struct tm tm;
	char *p;

	localtime_r(&t, &tm);
	S.clock_ok = tm.tm_year + 1900 >= 2025;
	if (!S.clock_ok) {
		cat(S.hhmm, "--:--");
		cat(S.date, "Clock not set");
		return;
	}
	p = utoa(S.hhmm, tm.tm_hour, 2);
	p = cat(p, ":");
	utoa(p, tm.tm_min, 2);
	p = cat(S.date, days[tm.tm_wday]);
	p = cat(p, " ");
	p = utoa(p, tm.tm_mday, 1);
	p = cat(p, " ");
	cat(p, months[tm.tm_mon]);
}

/* ------------------------------------------------------------------ */
/* Network state (netd + wpa_supplicant), shared with the preview      */

#define MAX_NETS	12

struct wnet {
	char ssid[64];		/* as wpa_supplicant prints it (\xNN escapes) */
	int signal;		/* dBm; -200 when saved but not seen */
	int secured;		/* WPA/RSN/WEP */
	int supported;		/* open or WPA-PSK: what this manager can join */
	int saved_id;		/* wpa_supplicant network id, -1 if not saved */
	int current;
};

/* /etc/network/{wifi,usb}.conf, as netd reads them */
struct ipconf {
	int enabled;		/* wifi only */
	int is_static;
	char address[24];	/* a.b.c.d/len */
	char gateway[16];
	char dns[64];
};

enum { NK_WIFI, NK_USB };

struct net {
	struct ipconf conf[2];
	int wpa;		/* wpa_supplicant answers on its socket */
	char state[24];		/* wpa_state */
	char ssid[64];		/* the network we are on */
	int n;
	struct wnet nets[MAX_NETS];
	char usb_if[16];	/* first USB Ethernet adapter, "" if none */
	int usb_link;
	char usb_ip[20];
	char usb_mac[24];
	char notice[80];	/* result of the last connect, shown on Wi-Fi */
	int notice_bad;
};

static struct net N;

static void conf_value(char *dst, int max, const char *v)
{
	int n = 0;

	if (*v == '"' || *v == '\'')
		v++;
	while (*v && *v != '"' && *v != '\'' && *v != '\n' && n < max - 1)
		dst[n++] = *v++;
	dst[n] = 0;
}

static void conf_parse(const char *buf, struct ipconf *c)
{
	char mode[16] = "dhcp", en[4] = "1";

	memset(c, 0, sizeof(*c));
	for (const char *l = buf; l && *l; ) {
		const char *nl = strchr(l, '\n');

		if (!strncmp(l, "MODE=", 5))
			conf_value(mode, sizeof(mode), l + 5);
		else if (!strncmp(l, "ENABLED=", 8))
			conf_value(en, sizeof(en), l + 8);
		else if (!strncmp(l, "ADDRESS=", 8))
			conf_value(c->address, sizeof(c->address), l + 8);
		else if (!strncmp(l, "GATEWAY=", 8))
			conf_value(c->gateway, sizeof(c->gateway), l + 8);
		else if (!strncmp(l, "DNS=", 4))
			conf_value(c->dns, sizeof(c->dns), l + 4);
		l = nl ? nl + 1 : 0;
	}
	c->enabled = en[0] != '0';
	c->is_static = !strcmp(mode, "static");
}

static int conf_text(char *buf, const struct ipconf *c, int kind)
{
	char *p = buf;

	p = cat(p, "# Written by the start menu (Settings); read by netd.\n");
	if (kind == NK_WIFI)
		p = cat(p, c->enabled ? "ENABLED=1\n" : "ENABLED=0\n");
	p = cat(p, c->is_static ? "MODE=static\n" : "MODE=dhcp\n");
	p = cat(cat(cat(p, "ADDRESS="), c->address), "\n");
	p = cat(cat(cat(p, "GATEWAY="), c->gateway), "\n");
	p = cat(cat(cat(p, "DNS=\""), c->dns), "\"\n");
	return (int)(p - buf);
}

/* "a.b.c.d" -> 1; with want_len also "/n" (0..32), added as /24 if missing. */
static int ip_ok(char *s, int want_len, int max)
{
	int parts = 0, v = -1, len;
	char *p = s;

	for (;; p++) {
		if (*p >= '0' && *p <= '9') {
			v = (v < 0 ? 0 : v * 10) + (*p - '0');
			if (v > 255)
				return 0;
		} else if (*p == '.' && v >= 0 && parts < 3) {
			parts++;
			v = -1;
		} else {
			break;
		}
	}
	if (parts != 3 || v < 0)
		return 0;
	if (!want_len)
		return *p == 0;
	if (*p == 0) {
		if ((int)strlen(s) + 3 >= max)
			return 0;
		cat(p, "/24");
		return 1;
	}
	if (*p++ != '/' || !*p)
		return 0;
	for (len = 0; *p >= '0' && *p <= '9'; p++)
		len = len * 10 + (*p - '0');
	return *p == 0 && len <= 32;
}

/* Space-separated addresses; commas count as spaces. */
static int dns_ok(char *s)
{
	char one[16];
	int n = 0;

	for (char *p = s; *p; p++)
		if (*p == ',')
			*p = ' ';
	for (char *p = s; *p; ) {
		int k = 0;

		while (*p == ' ')
			p++;
		if (!*p)
			break;
		while (*p && *p != ' ' && k < 15)
			one[k++] = *p++;
		one[k] = 0;
		if (!ip_ok(one, 0, 0))
			return 0;
		n++;
	}
	return 1;
}

/* US layout on the badge's keys; 0 for keys that type nothing. */
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
	if (code < 0 || code >= (int)sizeof(lo) - 1)
		return 0;
	return shift ? hi[code] : lo[code];
}

/* ------------------------------------------------------------------ */
/* Disks (Tools > Disks), shared with the preview                      */

#define MAX_DISKS	6
#define MAX_FS		15

/* A removable disk: the SD card or a USB drive (one per LUN of a card
 * reader, so a hub with several drives gives several). */
struct diskinfo {
	char name[12];		/* mmcblk0, sda */
	char label[40];		/* "SD card", or the drive's vendor and model */
	int usb;
	unsigned long long size;	/* 0: no medium (empty card reader) */
};

struct fsinfo {
	char label[32];		/* "SD card", "USB drive 2", "System", "Data" */
	char dev[24];		/* /dev/mmcblk0p1, /dev/sda1; "" for the badge's own */
	char mnt[32];		/* mount point, "" if not mounted */
	char type[12];		/* FAT32, exFAT, JFFS2, ... */
	unsigned long long size, used;	/* bytes; used only when mounted */
	int disk;		/* index into disks.disk; -1 for the badge's own */
	int part;		/* partition number, 0 = the whole disk */
};

struct disks {
	int n_disk;
	struct diskinfo disk[MAX_DISKS];	/* the SD card first */
	int n;
	struct fsinfo fs[MAX_FS];	/* by disk, then partition; the badge's last */
	int have_fsck;			/* fsck.fat / fsck.exfat installed */
	char notice[96];
	int notice_bad;
};

static struct disks D;

/* "15.7 GB", "512 MB", "980 KB" */
static char *human(char *p, unsigned long long b)
{
	static const char *const unit[] = { "B", "KB", "MB", "GB", "TB" };
	unsigned long long whole = b, frac = 0;
	int u = 0;

	while (whole >= 1024 && u < 4) {
		frac = (whole % 1024) * 10 / 1024;
		whole /= 1024;
		u++;
	}
	p = utoa(p, (unsigned)whole, 1);
	if (u && whole < 100 && frac) {
		p = cat(p, ".");
		p = utoa(p, (unsigned)frac, 1);
	}
	return cat(cat(p, " "), unit[u]);
}

static const struct diskinfo *fs_disk(const struct fsinfo *f)
{
	return f->disk >= 0 && f->disk < D.n_disk ? &D.disk[f->disk] : 0;
}

#ifndef HOME_PREVIEW
/* "SD card" / "USB drive", for messages. */
static const char *disk_noun(const struct fsinfo *f)
{
	const struct diskinfo *k = fs_disk(f);

	return k && k->usb ? "USB drive" : "SD card";
}
#endif

/* The index in D.fs of the partition on device dev, or -1. */
static int fs_find(const char *dev)
{
	for (int i = 0; i < D.n; i++)
		if (D.fs[i].dev[0] && !strcmp(D.fs[i].dev, dev))
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Bus Pirate (Apps > Bus Pirate), shared with the preview             */

/*
 * A Bus Pirate 6 on the bottom port. Its first CDC port is its terminal:
 * the menu types the commands a user would (W, P, m led, v, ...) and
 * reads the replies up to the ETX it sends after every prompt. Its second
 * CDC port speaks SUMP, the logic analyzer protocol, as long as its
 * binmode is the default "SUMP logic analyzer". docs/HOME.md.
 */
#define BP_PINS		8
#define LA_MAX		32768		/* samples; the Bus Pirate keeps 128 K */

enum { LT_WS2812, LT_APA102, LT_ONBOARD };

struct bp {
	int present;			/* 1209:7331 on USB */
	char board[24];			/* "Bus Pirate 6" */
	char mode[16];			/* from its prompt: "DIO", "LED" */
	char fw[48];			/* "de50049 (Oct  9 2025)" */
	int sump;			/* its binmode is the SUMP logic analyzer */
	int psu, dv, ma;		/* power supply: on; volts x 10; mA limit, 0 none */
	int pullups;
	char vout[12], iout[12];	/* measured: "3.3V", "1.4mA" or "OFF" */
	char pin_v[BP_PINS][8];		/* "3.3V" */
	char pin_f[BP_PINS][8];		/* what the mode uses the pin for: "SDO", "-" */
	int led_type, led_set;		/* LT_*; the type its LED mode has, -1 none */
	int led_color, led_pct, led_n;	/* index into led_colors or CUSTOM; brightness; strip length */
	uint32_t led_custom;		/* 0xRRGGBB typed in */
	char info[14][64];		/* its `i`, without the FCC paragraph */
	int n_info;
	char notice[96];
	int notice_bad;
};

static struct bp B = { .dv = 33, .ma = 100, .led_set = -1, .led_pct = 25, .led_n = 8,
		      .led_custom = 0xff8000 };

static const char *const led_types[3] = { "WS2812 strip", "APA102 strip", "Onboard LED" };

static const struct { const char *name; uint32_t rgb; } led_colors[] = {
	{ "Red", 0xff0000 }, { "Orange", 0xff5000 }, { "Yellow", 0xffc800 },
	{ "Green", 0x00ff00 }, { "Cyan", 0x00ffff }, { "Blue", 0x0000ff },
	{ "Purple", 0x8000ff }, { "Pink", 0xff0080 }, { "White", 0xffffff },
	{ "Rainbow", 0 },
};
#define N_COLORS	((int)(sizeof(led_colors) / sizeof(led_colors[0])))
#define RAINBOW		(N_COLORS - 1)
#define CUSTOM		N_COLORS	/* led_custom */

static uint32_t led_color_rgb(void)
{
	return B.led_color == CUSTOM ? B.led_custom : led_colors[B.led_color].rgb;
}

/* Logic analyzer: the last capture and its settings. */
struct la {
	int n;				/* samples, from the trigger on */
	unsigned rate;			/* Hz, of the capture */
	int rate_i, size_i;		/* settings: la_rates, la_sizes */
	int trig, falling;		/* trigger pin, -1 none; on the falling edge */
	int zoom;			/* log2 samples per pixel (negative: zoomed in) */
	int pos;			/* first sample on screen */
	char status[96];
	int status_bad;
};

static const unsigned la_rates[] = { 10000, 100000, 1000000, 10000000, 50000000 };
static const int la_sizes[] = { 2048, 8192, 32768 };
#define N_RATES		((int)(sizeof(la_rates) / sizeof(la_rates[0])))
#define LA_X0		(WB_CONTENT_PAD + 44)		/* left of the traces */
#define LA_W		(W - WB_CONTENT_PAD - LA_X0)
#define LA_LANE		42
#define LA_TOP		226

/* Samples per pixel as a shift; negative zooms in. The widest view that
 * shows the whole capture. */
static int la_fit(int n)
{
	int z = 0;

	while ((n >> z) > LA_W)
		z++;
	return z;
}

static uint8_t la_buf[LA_MAX];		/* oldest sample first, bit n = IOn */
static struct la LA = { .rate_i = 2, .size_i = 1, .trig = -1 };

/* "3.3 V" from volts x 10 */
static char *volts(char *p, int dv)
{
	p = utoa(p, (unsigned)dv / 10, 1);
	p = cat(p, ".");
	return utoa(p, (unsigned)dv % 10, 1);
}

/* "10 kHz", "1 MHz" */
static char *hz(char *p, unsigned f)
{
	return f >= 1000000 ? cat(utoa(p, f / 1000000, 1), " MHz") :
	       f >= 1000 ? cat(utoa(p, f / 1000, 1), " kHz") : cat(utoa(p, f, 1), " Hz");
}

/* A duration, "820 ns", "8.19 ms", "-2.04 us" */
static char *dur(char *p, long long ns)
{
	static const char *const unit[] = { " ns", " us", " ms", " s" };
	unsigned long long v = ns < 0 ? (unsigned long long)-ns : (unsigned long long)ns, d = 1;
	int u = 0;

	if (ns < 0)
		p = cat(p, "-");
	while (u < 3 && v >= d * 1000) {
		d *= 1000;
		u++;
	}
	p = utoa(p, (unsigned)(v / d), 1);
	if (u && v / d < 100) {
		p = cat(p, ".");
		p = utoa(p, (unsigned)(v % d * (v / d < 10 ? 100 : 10) / d), v / d < 10 ? 2 : 1);
	}
	return cat(p, unit[u]);
}

static char *hexn(char *p, uint32_t v, int digits)
{
	static const char hx[] = "0123456789ABCDEF";

	while (digits--)
		*p++ = hx[(v >> (4 * digits)) & 15];
	*p = 0;
	return p;
}

#ifndef HOME_PREVIEW
/* LED i of n in the chosen colour (the rainbow spreads its hues over the
 * strip), at the chosen brightness; 0xRRGGBB. */
static uint32_t led_rgb(int i, int n)
{
	uint32_t c = led_color_rgb(), r, g, b;

	if (B.led_color == RAINBOW) {
		int h = n > 1 ? i * 1536 / n : 0, f = h & 255;

		switch (h >> 8) {
		case 0: r = 255; g = f; b = 0; break;
		case 1: r = 255 - f; g = 255; b = 0; break;
		case 2: r = 0; g = 255; b = f; break;
		case 3: r = 0; g = 255 - f; b = 255; break;
		case 4: r = f; g = 0; b = 255; break;
		default: r = 255; g = 0; b = 255 - f; break;
		}
		c = r << 16 | g << 8 | b;
	}
	r = ((c >> 16) & 255) * (unsigned)B.led_pct / 100;
	g = ((c >> 8) & 255) * (unsigned)B.led_pct / 100;
	b = (c & 255) * (unsigned)B.led_pct / 100;
	return r << 16 | g << 8 | b;
}
#endif

/* The pins the current mode uses, "IO0 SDO", or "" */
static void bp_wiring(char *out, int max)
{
	char *p = out;

	*p = 0;
	for (int i = 0; i < BP_PINS; i++) {
		if (!B.pin_f[i][0] || !strcmp(B.pin_f[i], "-") || p - out + 16 >= max)
			continue;
		if (p != out)
			p = cat(p, ", ");
		p = cat(utoa(cat(p, "IO"), (unsigned)i, 1), " ");
		p = cat(p, B.pin_f[i]);
	}
}

#define SD_MNT		"/mnt/sd"
#define USB_MNT		"/media/"	/* + sda1; a tmpfs (fstab), as / is read-only */

/* Where the menu mounts f: the SD card where S15sdcard does, USB drives
 * in /media by partition. */
static void mnt_point(const struct fsinfo *f, char *out)
{
	const struct diskinfo *k = fs_disk(f);

	if (k && k->usb)
		cat(cat(out, USB_MNT), f->dev + 5);
	else
		cat(out, SD_MNT);
}

#ifndef HOME_PREVIEW

#define EVDEV		"/dev/input/event0"
#define CONSOLE_TTY	"/dev/tty1"
#define BL		"/sys/class/backlight/backlight/brightness"
#define BL_MAX		"/sys/class/backlight/backlight/max_brightness"
#define KBD_BL		"/sys/class/backlight/kbd-backlight/brightness"
#define FB_BLANK	"/sys/class/graphics/fb0/blank"
#define WLAN		"/sys/class/net/wlan0/"
#define EVIOCGRAB	0x40045590	/* _IOW('E', 0x90, int) */
#define BL_DEFAULT	76		/* C6 slave's initial duty (sleepd) */
#define KBD_DEFAULT	25

struct ev {				/* 32-bit kernel input_event */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static int fb_fd = -1, ev_fd = -1, tty_fd = -1;

/*
 * Test hook: key codes written to this FIFO ("60 28", decimal, any
 * whitespace) count as key presses, so Tools/usb/hil can drive the menu.
 * Root only (0600), like the keypad device itself.
 */
#define KEYS_FIFO	"/tmp/home.keys"
static int fifo_fd = -1;
static char fifo_buf[64];
static int fifo_len;
static size_t fb_bytes;
static int session;

static void say(const char *s) { write(2, s, strlen(s)); }

static int read_str(const char *path, char *buf, int max)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, max - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = 0;
	return n;
}

static int read_int(const char *path, int fallback)
{
	char buf[16];
	int v = 0;

	if (read_str(path, buf, sizeof(buf)) <= 0)
		return fallback;
	for (char *p = buf; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	return v;
}

static void write_int(const char *path, int v)
{
	char buf[16];
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return;
	write(fd, buf, (size_t)(utoa(buf, (unsigned)v, 1) - buf));
	close(fd);
}

/* pid of a running /usr/bin/<name>, or 0. */
static int find_proc(const char *name)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int pid = 0;

	if (!d)
		return 0;
	while (!pid && (e = readdir(d))) {
		char path[48], cmd[64], *p;

		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		p = cat(path, "/proc/");
		p = cat(p, e->d_name);
		cat(p, "/cmdline");
		if (read_str(path, cmd, sizeof(cmd)) <= 0)
			continue;
		p = strrchr(cmd, '/');
		if (!strcmp(p ? p + 1 : cmd, name))
			for (p = e->d_name; *p; p++)
				pid = pid * 10 + (*p - '0');
	}
	closedir(d);
	return pid;
}

static int strtol_(const char *s)
{
	int v = 0, neg = *s == '-';

	for (s += neg; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (*s - '0');
	return neg ? -v : v;
}

/* IPv4 address of ifname as text, "" if none. */
static void if_ip(const char *ifname, char *out)
{
	struct ifreq ifr;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	out[0] = 0;
	if (s < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
		uint8_t *a = (uint8_t *)&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;

		for (int i = 0; i < 4; i++) {
			out = utoa(out, a[i], 1);
			if (i < 3)
				out = cat(out, ".");
		}
	}
	close(s);
}

static void read_wifi(void)
{
	S.wifi_ip[0] = 0;
	if (read_str(WLAN "operstate", S.wifi_state, sizeof(S.wifi_state)) < 0) {
		cat(S.wifi_state, "not present");
		S.wifi_up = 0;
		S.wifi_mac[0] = 0;
		return;
	}
	S.wifi_up = !strcmp(S.wifi_state, "up");
	read_str(WLAN "address", S.wifi_mac, sizeof(S.wifi_mac));
	if_ip("wlan0", S.wifi_ip);
}

static void read_usb(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;

	S.n_usb = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && S.n_usb < 6) {
		char path[96], v[8], pr[8], *p, *base;

		/* devices are "1-1", "1-1.2"; skip root hubs and interfaces */
		if (!strchr(e->d_name, '-') || strchr(e->d_name, ':'))
			continue;
		base = cat(path, "/sys/bus/usb/devices/");
		base = cat(base, e->d_name);
		cat(base, "/idVendor");
		if (read_str(path, v, sizeof(v)) <= 0)
			continue;
		cat(base, "/idProduct");
		read_str(path, pr, sizeof(pr));
		cat(base, "/product");
		if (read_str(path, S.usb[S.n_usb][0], sizeof(S.usb[0][0])) <= 0)
			cat(S.usb[S.n_usb][0], "USB device");
		p = cat(S.usb[S.n_usb][1], v);
		p = cat(p, ":");
		cat(p, pr);
		S.n_usb++;
	}
	closedir(d);
}

/* ---- disks ---- */

static unsigned le16(const unsigned char *p)
{
	return p[0] | p[1] << 8;
}

static unsigned long le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* File system from the boot sector: the badge only cares about FAT/exFAT. */
static void fs_probe(const char *dev, char *type)
{
	unsigned char bs[512];
	int fd = open(dev, O_RDONLY);

	cat(type, "unknown");
	if (fd < 0)
		return;
	if (read(fd, bs, sizeof(bs)) == (int)sizeof(bs)) {
		/* FAT12/16/32 by the cluster count, as the FAT spec says: the
		 * type label is only a hint, and some write just "FAT". */
		unsigned bps = le16(bs + 11), spc = bs[13], rsvd = le16(bs + 14), nfat = bs[16];
		unsigned root = (le16(bs + 17) * 32 + bps - 1) / (bps ? bps : 1);
		unsigned long fatsz = le16(bs + 22) ? le16(bs + 22) : le32(bs + 36);
		unsigned long tot = le16(bs + 19) ? le16(bs + 19) : le32(bs + 32);
		unsigned long meta = rsvd + nfat * fatsz + root;

		type[0] = 0;
		if (!memcmp(bs + 3, "EXFAT   ", 8))
			cat(type, "exFAT");
		else if (bs[510] == 0x55 && bs[511] == 0xaa && (bps == 512 || bps == 1024 ||
			 bps == 2048 || bps == 4096) && spc && !(spc & (spc - 1)) &&
			 rsvd && nfat && fatsz && tot > meta) {
			unsigned long clusters = (tot - meta) / spc;

			cat(type, clusters < 4085 ? "FAT12" : clusters < 65525 ? "FAT16" : "FAT32");
		} else {
			cat(type, bs[510] == 0x55 && bs[511] == 0xaa ? "unknown" : "empty");
		}
	}
	close(fd);
}

/* Next "device mountpoint" pair of /proc/mounts text; 0 at the end. */
static char *mounts_next(char *line, char *dev, char *mnt, int max)
{
	char *next;
	int i = 0, j = 0;

	if (!line || !*line)
		return 0;
	next = strchr(line, '\n');
	if (next)
		*next++ = 0;
	while (line[i] && line[i] != ' ' && i < max - 1)
		dev[i] = line[i], i++;
	dev[i] = 0;
	while (line[i] == ' ')
		i++;
	while (line[i] && line[i] != ' ' && j < max - 1)
		mnt[j++] = line[i++];
	mnt[j] = 0;
	return next ? next : line + strlen(line);
}

/* Mount point and usage of dev (or of mnt when dev is ""), from /proc/mounts. */
static void fs_usage(struct fsinfo *f)
{
	char buf[2048], dev[48], mnt[48], *line = buf;
	struct statfs st;

	if (read_str("/proc/mounts", buf, sizeof(buf)) <= 0)
		return;
	while ((line = mounts_next(line, dev, mnt, sizeof(dev)))) {
		if (f->dev[0] ? strcmp(dev, f->dev) : strcmp(mnt, f->mnt))
			continue;
		f->mnt[0] = 0;
		strncpy(f->mnt, mnt, sizeof(f->mnt) - 1);
		if (statfs(mnt, &st) == 0) {
			f->size = (unsigned long long)st.f_blocks * st.f_bsize;
			f->used = (unsigned long long)(st.f_blocks - st.f_bfree) * st.f_bsize;
		}
		/* tmpfs on NOMMU has no size of its own (df says 0): it takes
		 * memory, so show the memory, as MemTotal and MemAvailable */
		if (!f->size && !strcmp(f->type, "tmpfs") && read_str("/proc/meminfo", buf, sizeof(buf)) > 0) {
			const char *t = strstr(buf, "MemTotal:"), *a = strstr(buf, "MemAvailable:");

			if (t && a) {
				for (t += 9; *t == ' '; t++)
					;
				for (a += 13; *a == ' '; a++)
					;
				f->size = (unsigned long long)strtol_(t) * 1024;
				f->used = f->size - (unsigned long long)strtol_(a) * 1024;
			}
		}
		return;
	}
	if (f->dev[0])
		f->mnt[0] = 0;
}

/* A drive pulled out while mounted: detach what is left of it in /media. */
static void media_reap(void)
{
	char buf[2048], dev[48], mnt[48], *line = buf;

	if (read_str("/proc/mounts", buf, sizeof(buf)) <= 0)
		return;
	while ((line = mounts_next(line, dev, mnt, sizeof(dev)))) {
		if (!strncmp(mnt, USB_MNT, sizeof(USB_MNT) - 1) && access(dev, F_OK)) {
			umount2(mnt, MNT_DETACH);
			rmdir(mnt);
		}
		/* the card's partition went (the card failed or was pulled) */
		if (!strcmp(mnt, SD_MNT) && !strncmp(dev, "/dev/", 5) && access(dev, F_OK))
			umount2(mnt, MNT_DETACH);
	}
}

static unsigned long long read_ull(const char *path)
{
	char buf[24];
	unsigned long long v = 0;

	if (read_str(path, buf, sizeof(buf)) <= 0)
		return 0;
	for (char *p = buf; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (unsigned)(*p - '0');
	return v;
}

/* Trim and squeeze spaces: SCSI vendor and model strings are padded. */
static void squeeze(char *s)
{
	char *d = s, *start = s;
	int sp = 0;

	for (; *s; s++) {
		if (*s == ' ' || *s == '\t' || *s == '\n') {
			sp = d != start;
			continue;
		}
		if (sp)
			*d++ = ' ';
		sp = 0;
		*d++ = *s;
	}
	*d = 0;
}

static void disk_add(const char *name)
{
	struct diskinfo *k;
	char path[64], v[24], m[24], *p;

	if (D.n_disk == MAX_DISKS || strlen(name) >= sizeof(k->name) ||
	    strstr(name, "boot") || strstr(name, "rpmb"))
		return;
	k = &D.disk[D.n_disk++];
	memset(k, 0, sizeof(*k));
	cat(k->name, name);
	k->usb = name[0] == 's';
	p = cat(cat(path, "/sys/block/"), name);
	cat(p, "/size");
	k->size = read_ull(path) * 512;
	if (k->usb && !k->size) {
		/* After a media change (a card swapped in a reader, the Bus
		 * Pirate re-inserting its drive when a terminal closes) the
		 * kernel only notices it; it reads the capacity and the
		 * partition table again on the next open. */
		char dev[24];
		int fd;

		cat(cat(dev, "/dev/"), name);
		fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
			close(fd);
		k->size = read_ull(path) * 512;
	}
	if (!k->usb) {
		cat(k->label, "SD card");
		return;
	}
	cat(p, "/device/vendor");
	if (read_str(path, v, sizeof(v)) < 0)
		v[0] = 0;
	cat(p, "/device/model");
	if (read_str(path, m, sizeof(m)) < 0)
		m[0] = 0;
	squeeze(v);
	squeeze(m);
	p = cat(k->label, v);
	if (v[0] && m[0])
		p = cat(p, " ");
	cat(p, m);
	if (!k->label[0])
		cat(k->label, "USB drive");
}

/* The partitions of disk di, or the whole disk when it has no table. */
static void parts_add(int di)
{
	const struct diskinfo *k = &D.disk[di];
	size_t len = strlen(k->name);
	char path[40];
	int first = D.n;

	cat(cat(path, "/sys/block/"), k->name);
	for (int pass = 0; pass < 2 && D.n == first; pass++) {
		DIR *d;
		struct dirent *e;

		if (pass) {
			/* the same for a partition table (see disk_add) */
			char dev[24];
			int fd;

			if (!k->size)
				break;
			cat(cat(dev, "/dev/"), k->name);
			fd = open(dev, O_RDONLY | O_NONBLOCK);
			if (fd >= 0)
				close(fd);
		}
		d = opendir(path);
		if (!d)
			return;
		while ((e = readdir(d)) && D.n < MAX_FS - 3) {
			const char *s = e->d_name + len;
			struct fsinfo *f;
			char sz[80];
			int part = 0;

			if (strncmp(e->d_name, k->name, len))
				continue;
			if (*s == 'p')
				s++;
			if (*s < '1' || *s > '9')
				continue;
			while (*s >= '0' && *s <= '9')
				part = part * 10 + (*s++ - '0');
			if (*s || strlen(e->d_name) + 5 >= sizeof(f->dev))
				continue;
			f = &D.fs[D.n++];
			memset(f, 0, sizeof(*f));
			f->disk = di;
			f->part = part;
			cat(cat(f->dev, "/dev/"), e->d_name);
			cat(cat(cat(cat(sz, path), "/"), e->d_name), "/size");
			f->size = read_ull(sz) * 512;
		}
		closedir(d);
	}
	for (int i = first + 1; i < D.n; i++)		/* by partition number */
		for (int j = i; j > first && D.fs[j].part < D.fs[j - 1].part; j--) {
			struct fsinfo t = D.fs[j];

			D.fs[j] = D.fs[j - 1];
			D.fs[j - 1] = t;
		}
	if (D.n == first && k->size && D.n < MAX_FS - 3) {	/* no partition table */
		struct fsinfo *f = &D.fs[D.n++];

		memset(f, 0, sizeof(*f));
		f->disk = di;
		cat(cat(f->dev, "/dev/"), k->name);
		f->size = k->size;
	}
	for (int i = first; i < D.n; i++) {
		struct fsinfo *f = &D.fs[i];

		if (D.n - first > 1)
			utoa(cat(f->label, "Partition "), (unsigned)f->part, 1);
		else
			cat(f->label, k->usb ? "USB drive" : "SD card");
		fs_probe(f->dev, f->type);
		fs_usage(f);
	}
}

static int disk_before(const struct diskinfo *a, const struct diskinfo *b)
{
	if (a->usb != b->usb)
		return !a->usb;
	if (strlen(a->name) != strlen(b->name))		/* sdz before sdaa */
		return strlen(a->name) < strlen(b->name);
	return strcmp(a->name, b->name) < 0;
}

/*
 * What disks_read() saw: the disks, their sizes and partitions, and the
 * mounts. Reading a disk's boot sector or opening it (which makes the
 * kernel read its partition table again) is cheap on a healthy disk, but
 * a failing SD card answers every sector with a timeout and retries: the
 * menu then waited seconds for every key, as it read the disks at every
 * refresh. So the disks are read again only when this changes.
 */
static long now_ms(void);

static int disks_sig(char *sig, int max)
{
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	char *p = sig, buf[2048];
	int again = 0;

	*p = 0;
	while (d && (e = readdir(d)) && p < sig + max - 160) {
		char path[96];
		DIR *pd;
		struct dirent *pe;
		unsigned long long size;

		if (strncmp(e->d_name, "mmcblk", 6) && strncmp(e->d_name, "sd", 2))
			continue;
		cat(cat(path, "/sys/block/"), e->d_name);
		p = cat(cat(p, e->d_name), "=");
		cat(path + strlen(path), "/size");
		size = read_ull(path);
		p = utoa(p, (unsigned)(size >> 11), 1);	/* MB */
		/* a USB drive with no medium: opened at every refresh (disk_add) */
		again |= e->d_name[0] == 's' && !size;
		path[strlen(path) - 5] = 0;
		pd = opendir(path);
		while (pd && (pe = readdir(pd)) && p < sig + max - 80)
			if (!strncmp(pe->d_name, e->d_name, strlen(e->d_name)))
				p = cat(cat(p, " "), pe->d_name);
		if (pd)
			closedir(pd);
		p = cat(p, ";");
	}
	if (d)
		closedir(d);
	if (read_str("/proc/mounts", buf, sizeof(buf)) > 0)
		for (char *l = buf; *l && p < sig + max - 2; l++)
			*p++ = *l;
	*p = 0;
	return again;
}

static void disks_read(void)
{
	static char last[3072];
	static long usage_at;
	char sig[3072];
	DIR *d;
	struct dirent *e;
	struct fsinfo *f;

	media_reap();
	if (!disks_sig(sig, sizeof(sig)) && D.n && !strcmp(sig, last)) {
		if (now_ms() - usage_at >= 5000) {	/* usage only: statfs */
			usage_at = now_ms();
			for (int i = 0; i < D.n; i++)
				if (D.fs[i].mnt[0])
					fs_usage(&D.fs[i]);
		}
		return;
	}
	memcpy(last, sig, sizeof(last));
	usage_at = now_ms();
	d = opendir("/sys/block");
	D.n_disk = D.n = 0;
	D.have_fsck = access("/sbin/fsck.fat", X_OK) == 0;
	if (d) {
		while ((e = readdir(d)))
			if (!strncmp(e->d_name, "mmcblk", 6) || !strncmp(e->d_name, "sd", 2))
				disk_add(e->d_name);
		closedir(d);
	}
	for (int i = 1; i < D.n_disk; i++)
		for (int j = i; j > 0 && disk_before(&D.disk[j], &D.disk[j - 1]); j--) {
			struct diskinfo t = D.disk[j];

			D.disk[j] = D.disk[j - 1];
			D.disk[j - 1] = t;
		}
	for (int i = 0; i < D.n_disk; i++)
		parts_add(i);
	/* the badge's own: flash (/data, with /etc and /root), memory, the image */
	for (int i = 0; i < 3; i++) {
		static const char *const lab[3] = { "Badge storage", "Memory", "System" };
		static const char *const mnt[3] = { "/data", "/tmp", "/" };
		static const char *const type[3] = { "JFFS2", "tmpfs", "squashfs" };

		f = &D.fs[D.n++];
		memset(f, 0, sizeof(*f));
		f->disk = -1;
		cat(f->label, lab[i]);
		cat(f->mnt, mnt[i]);
		cat(f->type, type[i]);
		fs_usage(f);
	}
}

static void disk_notice(const char *a, const char *b, int bad)
{
	D.notice[0] = 0;
	cat(cat(D.notice, a), b);
	D.notice_bad = bad;
}

/*
 * Mount as S15sdcard does: vfat, else exfat. Read-only when the drive is
 * write-protected (a card reader's lock switch, or a Bus Pirate, which
 * protects its drive while a terminal is open). 0 read-write, 1
 * read-only, -1 not FAT or exFAT.
 */
static int fs_mount(const struct fsinfo *f)
{
	char mnt[32];

	mnt_point(f, mnt);
	mkdir(mnt, 0755);
	for (int ro = 0; ro < 2; ro++) {
		unsigned long fl = MS_NOATIME | (ro ? MS_RDONLY : 0);

		if (mount(f->dev, mnt, "vfat", fl, "utf8=1,flush") == 0 ||
		    mount(f->dev, mnt, "exfat", fl, "") == 0)
			return ro;
	}
	if (strcmp(mnt, SD_MNT))
		rmdir(mnt);
	return -1;
}

static int fs_unmount(const struct fsinfo *f)
{
	const struct diskinfo *k = fs_disk(f);

	if (!f->mnt[0])
		return 0;
	if (umount(f->mnt) == 0 || errno == EINVAL) {	/* EINVAL: not mounted */
		if (k && k->usb)
			rmdir(f->mnt);
		return 0;
	}
	disk_notice(errno == EBUSY ? "In use: close what has files open on the "
				   : "Could not unmount the ", disk_noun(f), 1);
	return -1;
}

/* The store finishes its work for a card that comes back: interrupted
 * installs, removals that waited for it (WhyOS-Apps docs/device.md §5.7).
 * In the background; the main loop reaps it. */
static void store_maintain(void)
{
	static char *const argv[] = { "/data/apps/store/store", "maintain", 0 };
	int pid;

	if (access(argv[0], X_OK))
		return;
	pid = vfork();
	if (pid == 0) {
		int fd = open("/tmp/store-maintain.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(argv[0], argv);
		_exit(127);
	}
}

static void disk_mount_toggle(const struct fsinfo *f)
{
	char msg[64];
	int rc;

	if (f->mnt[0]) {
		if (fs_unmount(f) == 0) {
			cat(cat(cat(msg, "Unmounted: the "), disk_noun(f)), " can be removed");
			disk_notice(msg, "", 0);
		}
		return;
	}
	rc = fs_mount(f);
	mnt_point(f, msg);
	if (rc < 0)
		disk_notice("Could not mount: not FAT or exFAT (check or format it)", "", 1);
	else
		disk_notice(rc ? "Mounted read-only at " : "Mounted at ", msg, 0);
	if (rc == 0 && !strcmp(msg, SD_MNT))
		store_maintain();
}

static int run_wait(char *const argv[], const char *log)
{
	int status = -1, pid = vfork();

	if (pid == 0) {
		int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void draw_message(const char *eyebrow_s, const char *msg);
static void blit(void);

/* What a check's exit status means, as the disk screen's notice. */
static void disk_check_result(int rc)
{
	char log[2048];
	int fd = open("/tmp/fsck.log", O_RDONLY), n = fd >= 0 ? read(fd, log, sizeof(log) - 1) : 0;

	if (fd >= 0)
		close(fd);
	log[n > 0 ? n : 0] = 0;
	if (rc == 0)
		disk_notice("No errors found", "", 0);
	else if (rc == 1)
		disk_notice("Errors repaired (log: /tmp/fsck.log)", "", 0);
	else if (strstr(log, "allocate memory"))
		disk_notice("Not enough memory to check it: restart the badge and try again", "", 1);
	else
		disk_notice("Could not repair it: format it (log: /tmp/fsck.log)", "", 1);
}

/*
 * fsck.fat -a: repair what it can without asking. It holds the whole FAT
 * and a word per cluster in memory, in blocks of up to 2 MB for a 16 GB
 * card; with the menu in memory there is no free block that big, and a
 * check that died half-way had already written some of its repairs
 * (2026-10-13). So in the session the check runs in our place, as an app
 * does, and a shell brings us back with --disk-checked=<status> <dev>
 * <was mounted>, which mounts it again and shows the result.
 */
static void disk_check(const struct fsinfo *f)
{
	struct fsinfo g = *f;		/* D is read again while we draw */
	static char cmd[200];
	char dev[24], msg[48];
	char *fat[] = { "/sbin/fsck.fat", "-a", "-w", dev, 0 };
	char *exfat[] = { "/usr/sbin/fsck.exfat", "-y", dev, 0 };
	char *sh[] = { "/bin/sh", "-c", cmd, 0 };
	int was = f->mnt[0] != 0, ex = !strcmp(g.type, "exFAT");

	strcpy(dev, f->dev);
	if (was && fs_unmount(f) < 0)
		return;
	cat(cat(cat(msg, "Checking the "), disk_noun(f)), "\xe2\x80\xa6");
	draw_message("DISKS", msg);
	blit();
	if (session) {
		char *o = cat(cat(cmd, ex ? "/usr/sbin/fsck.exfat -y " : "/sbin/fsck.fat -a -w "), dev);

		o = cat(cat(o, " >/tmp/fsck.log 2>&1; exec /usr/bin/home --session --disk-checked=$? "), dev);
		cat(o, was ? " 1" : " 0");
		execv(sh[0], sh);	/* our fds are close-on-exec */
	}
	disk_check_result(run_wait(ex ? exfat : fat, "/tmp/fsck.log"));
	if (was)
		fs_mount(&g);
}

/*
 * Erase disk di: a fresh MBR with one FAT32-LBA partition from 4 MiB
 * (the SD erase-block alignment) to the end, then a FAT32 file system. A
 * damaged partition table is replaced too, which is the point when a card
 * is in trouble.
 *
 * Big clusters on purpose: fsck.fat holds the whole FAT plus an owner
 * pointer per cluster in memory, about 8 bytes per cluster, out of a
 * 10 MB user pool. 32 KB clusters keep a 32 GB card at ~1M clusters,
 * 64 KB above that. busybox mkfs.vfat cannot choose (16 GB: 8 KB
 * clusters, a card fsck could not check), so dosfstools' mkfs.fat is
 * used when installed.
 */
static void disk_format(int di)
{
	const struct diskinfo *k = di >= 0 && di < D.n_disk ? &D.disk[di] : 0;
	unsigned char mbr[512] = { 0 };
	unsigned long long sectors;
	unsigned start = 8192, count;
	char spc[4] = "64", disk[24], part[24], msg[48];
	char *mkfs_fat[] = { "/sbin/mkfs.fat", "-F", "32", "-s", spc, "-n", "BADGE", part, 0 };
	char *mkfs_bb[] = { "/sbin/mkdosfs", "-n", "BADGE", part, 0 };
	const char *noun;
	struct fsinfo nf;
	int fd, rc;

	if (!k)
		return;
	noun = k->usb ? "USB drive" : "SD card";
	sectors = k->size / 512;
	cat(cat(disk, "/dev/"), k->name);
	cat(cat(part, disk), k->usb ? "1" : "p1");
	if (sectors <= start + 2048) {
		disk_notice("Too small to format: the ", noun, 1);
		return;
	}
	for (int i = 0; i < D.n; i++)
		if (D.fs[i].disk == di && fs_unmount(&D.fs[i]) < 0)
			return;
	cat(cat(cat(msg, "Formatting the "), noun), "\xe2\x80\xa6");
	draw_message("DISKS", msg);
	blit();
	count = sectors - start > 0xffffffffULL ? 0xffffffffU : (unsigned)(sectors - start);
	mbr[446 + 1] = 0xfe;		/* CHS fields: use LBA */
	mbr[446 + 2] = 0xff;
	mbr[446 + 3] = 0xff;
	mbr[446 + 4] = 0x0c;		/* FAT32, LBA */
	mbr[446 + 5] = 0xfe;
	mbr[446 + 6] = 0xff;
	mbr[446 + 7] = 0xff;
	for (int i = 0; i < 4; i++) {
		mbr[446 + 8 + i] = (unsigned char)(start >> (8 * i));
		mbr[446 + 12 + i] = (unsigned char)(count >> (8 * i));
	}
	mbr[440] = (unsigned char)time(0);	/* disk signature */
	mbr[441] = 0xb4;
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	fd = open(disk, O_RDWR);
	if (fd < 0 || write(fd, mbr, sizeof(mbr)) != (int)sizeof(mbr)) {
		if (fd >= 0)
			close(fd);
		disk_notice(errno == EROFS || errno == EACCES ?
			    "Write-protected: could not write the partition table" :
			    "Could not write the partition table", "", 1);
		return;
	}
	fsync(fd);
	ioctl(fd, BLKRRPART, 0);
	close(fd);
	for (int i = 0; i < 30 && access(part, F_OK); i++)
		usleep(100000);
	if (k->size > (32ULL << 30)) {
		spc[0] = 0;
		cat(spc, "128");
	}
	rc = run_wait(access(mkfs_fat[0], X_OK) ? mkfs_bb : mkfs_fat, "/tmp/mkfs.log");
	if (rc != 0) {
		disk_notice("Formatting failed (log: /tmp/mkfs.log)", "", 1);
		return;
	}
	memset(&nf, 0, sizeof(nf));
	nf.disk = di;
	cat(nf.dev, part);
	fs_mount(&nf);
	if (!k->usb)
		mkdir(SD_MNT "/badge", 0755);
	disk_notice("Formatted as FAT32 and mounted", "", 0);
}

/* ---- Bus Pirate: its terminal port and its SUMP port ---- */

static int bp_fd = -1, la_fd = -1;
static int bp_gone;			/* it stopped answering: back to Apps */
static char bp_tty[2][16];		/* its terminal and its binmode port */
static char bp_rx[4096];		/* the last reply, without ANSI or CRs */

static long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* Is it on USB? Also finds its ttys: the two CDC ports' control
 * interfaces are 0 and 2. */
static int bp_find(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	int found = 0;

	bp_tty[0][0] = bp_tty[1][0] = 0;
	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char path[96], v[8], *base;

		if (!strchr(e->d_name, '-') || strchr(e->d_name, ':') || strlen(e->d_name) > 40)
			continue;
		base = cat(cat(path, "/sys/bus/usb/devices/"), e->d_name);
		cat(base, "/idVendor");
		if (read_str(path, v, sizeof(v)) <= 0 || strcmp(v, "1209"))
			continue;
		cat(base, "/idProduct");
		if (read_str(path, v, sizeof(v)) <= 0 || strcmp(v, "7331"))
			continue;
		found = 1;
		for (int i = 0; i < 2; i++) {
			DIR *t;
			struct dirent *te;

			cat(utoa(cat(base, ":1."), (unsigned)(2 * i), 1), "/tty");
			t = opendir(path);
			if (!t)
				continue;
			while ((te = readdir(t)))
				if (te->d_name[0] != '.' && strlen(te->d_name) < sizeof(bp_tty[i])) {
					cat(bp_tty[i], te->d_name);
					break;
				}
			closedir(t);
		}
	}
	closedir(d);
	return found;
}

static int tty_open_raw(const char *name)
{
	char path[32];
	struct termios t;
	int fd;

	cat(cat(path, "/dev/"), name);
	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 || tcgetattr(fd, &t) < 0)
		return fd;
	t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
	t.c_oflag &= ~OPOST;
	t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	t.c_cflag &= ~(CSIZE | PARENB | CRTSCTS);
	t.c_cflag |= CS8 | CLOCAL | CREAD | HUPCL;	/* close drops DTR: a hang-up */
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, B115200);
	cfsetospeed(&t, B115200);
	tcsetattr(fd, TCSANOW, &t);
	return fd;
}

static void bp_close(void)
{
	if (la_fd >= 0)
		close(la_fd);
	if (bp_fd >= 0)
		close(bp_fd);
	la_fd = bp_fd = -1;
}

static void bp_note(const char *a, const char *b, int bad)
{
	B.notice[0] = 0;
	strncat(cat(B.notice, a), b, sizeof(B.notice) - strlen(a) - 1);
	B.notice_bad = bad;
}

static void bp_lost(void)
{
	bp_close();
	bp_gone = 1;
	bp_note("The Bus Pirate stopped answering", "", 1);
}

/*
 * Its "where is the cursor" (ESC[6n, after ESC[999;999H): in colour mode
 * it measures the terminal that way. The terminal is the panel's
 * console, so answer with its size.
 */
static void bp_report_size(void)
{
	struct winsize ws;
	char r[24], *q;

	if (tty_fd < 0 || ioctl(tty_fd, TIOCGWINSZ, &ws) < 0 || !ws.ws_row) {
		ws.ws_row = 45;
		ws.ws_col = 90;
	}
	q = utoa(cat(r, "\033["), ws.ws_row, 1);
	q = cat(utoa(cat(q, ";"), ws.ws_col, 1), "R");
	write(bp_fd, r, (size_t)(q - r));
}

/*
 * Read up to the ETX the Bus Pirate sends after each prompt (its "waiting
 * for input"), into bp_rx without CRs and ANSI sequences. 0, or -1 on a
 * timeout or when it is gone; bp_rx then holds what did arrive (its colour
 * question, for one, has no ETX).
 */
static int bp_wait(int timeout_ms)
{
	struct pollfd p = { .fd = bp_fd, .events = POLLIN };
	long end = now_ms() + timeout_ms;
	int n = 0, esc = 0, arg = 0;

	bp_rx[0] = 0;
	for (;;) {
		char c[256];
		long left = end - now_ms();
		int k;

		bp_rx[n] = 0;
		if (left <= 0)
			return -1;
		k = poll(&p, 1, (int)left);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		k = read(bp_fd, c, sizeof(c));
		if (k < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (k <= 0)
			return -1;
		for (int i = 0; i < k; i++) {
			unsigned char ch = (unsigned char)c[i];

			if (ch == 0x03) {
				bp_rx[n] = 0;
				return 0;
			}
			if (esc == 1) {		/* ESC [ ... starts a CSI */
				esc = ch == '[' ? 2 : 0;
				arg = 0;
				continue;
			}
			if (esc == 2) {
				if (ch >= '0' && ch <= '9')
					arg = arg * 10 + (ch - '0');
				if (ch == 'n' && arg == 6)
					bp_report_size();
				if (ch >= 0x40 && ch <= 0x7e)
					esc = 0;
				continue;
			}
			if (ch == 0x1b)
				esc = 1;
			else if (ch != '\r' && n < (int)sizeof(bp_rx) - 1)
				bp_rx[n++] = (char)ch;
		}
	}
}

/* The reply's last line: its prompt, "DIO> " or a menu's "(Y) > ". */
static const char *bp_last_line(void)
{
	const char *nl = strrchr(bp_rx, '\n');

	return nl ? nl + 1 : bp_rx;
}

/* A mode prompt ("DIO> ", "LED-(WS2812)> ") sets B.mode; menus have spaces. */
static int bp_prompt(void)
{
	const char *l = bp_last_line(), *gt = strchr(l, '>');
	int n = 0;

	if (!gt || gt == l || memchr(l, ' ', (size_t)(gt - l)))
		return 0;
	while (l + n < gt && l[n] != '-' && n < (int)sizeof(B.mode) - 1)
		n++;
	memcpy(B.mode, l, (size_t)n);
	B.mode[n] = 0;
	if (strcmp(B.mode, "LED"))
		B.led_set = -1;
	return 1;
}

/* Type a command and wait for its prompt. */
static int bp_cmd(const char *cmd)
{
	char junk[256];

	if (bp_fd < 0)
		return -1;
	while (read(bp_fd, junk, sizeof(junk)) > 0)
		;		/* stale output */
	if (write(bp_fd, cmd, strlen(cmd)) < 0 || write(bp_fd, "\r", 1) < 0 ||
	    bp_wait(5000) < 0) {
		bp_lost();
		return -1;
	}
	bp_prompt();
	return 0;
}

static int has(const char *l, int n, const char *w)
{
	for (int k = (int)strlen(w), i = 0; i + k <= n; i++)
		if (!memcmp(l + i, w, (size_t)k))
			return 1;
	return 0;
}

/* An error line in the last reply, as the notice; 1 if there was one. */
static int bp_error(void)
{
	for (char *l = strchr(bp_rx, '\n'); l; l = strchr(l, '\n')) {
		char *e = strchr(++l, '\n');
		int n = e ? (int)(e - l) : (int)strlen(l);

		if ((has(l, n, "rror") || has(l, n, "nvalid")) && n < (int)sizeof(B.notice)) {
			memcpy(B.notice, l, (size_t)n);
			B.notice[n] = 0;
			B.notice_bad = 1;
			return 1;
		}
	}
	return 0;
}

/* The number of the menu line (" 2. ANSI color mode") holding w, or 0. */
static int menu_num(const char *w)
{
	const char *l = bp_rx;

	while (l && *l) {
		const char *e;
		int n = 0;

		while (*l == '\n' || *l == ' ')
			l++;
		e = strchr(l, '\n');
		while (*l >= '0' && *l <= '9')
			n = n * 10 + (*l++ - '0');
		if (n && *l == '.' && has(l, e ? (int)(e - l) : (int)strlen(l), w))
			return n;
		l = e;
	}
	return 0;
}

/*
 * Colours and the status bar for the console, through its configuration
 * menu (c): the item, then the option, then x, which leaves the menu and
 * saves its configuration file. Picking colour makes it measure the
 * terminal (bp_report_size).
 */
static void bp_colours(int on)
{
	static const char *const items[2] = { "toolbar", "color mode" };
	char num[8];

	if (bp_cmd("c") < 0)
		return;
	for (int i = 0; i < 2; i++) {
		const char *it = items[on ? 1 - i : i];	/* colour first, toolbar last */
		int n = menu_num(it), o;

		if (!n)
			continue;
		utoa(num, (unsigned)n, 1);
		if (bp_cmd(num) < 0)
			return;
		o = !on ? menu_num("Disable") : it[0] == 't' ? menu_num("Enable") :
		    menu_num("256") ? menu_num("256") : menu_num("Full");
		utoa(num, (unsigned)(o ? o : 0), 1);
		if (bp_cmd(o ? num : "x") < 0 || !o)
			return;
	}
	bp_cmd("x");
}

/* Out of whatever menu it is in, back at its prompt. */
static void bp_to_prompt(void)
{
	for (int i = 0; i < 3 && bp_cmd("") == 0 && !bp_prompt(); i++)
		bp_cmd("x");
}

/* Next tab-separated cell of a `v` row, into out. */
static char *cell(char *p, char *out, int max)
{
	int n = 0;

	while (*p && *p != '\t' && *p != '\n') {
		if (n < max - 1)
			out[n++] = *p;
		p++;
	}
	out[n] = 0;
	return *p == '\t' ? p + 1 : p;
}

/*
 * `v`: a header row (1.Vout 2.IO0 ...), what each pin does in this mode
 * (OFF or the supply's current, then SDO, - ...) and the voltages.
 */
static void bp_measure(void)
{
	char *h, *f, *v;
	int was = B.psu;

	if (bp_cmd("v") < 0 || !(h = strstr(bp_rx, "1.Vout")) ||
	    !(f = strchr(h, '\n')) || !(v = strchr(++f, '\n')))
		return;
	v++;
	f = cell(f, B.iout, sizeof(B.iout));
	v = cell(v, B.vout, sizeof(B.vout));
	for (int i = 0; i < BP_PINS; i++) {
		f = cell(f, B.pin_f[i], sizeof(B.pin_f[i]));
		v = cell(v, B.pin_v[i], sizeof(B.pin_v[i]));
	}
	B.psu = strcmp(B.iout, "OFF") != 0;
	if (was && !B.psu)
		bp_note("The power supply switched off: over the current limit?", "", 1);
}

/* W, P and the LA need a mode other than HiZ: DIO touches no pins. */
static void bp_need_mode(void)
{
	if (strcmp(B.mode, "HiZ"))
		return;
	bp_cmd("m dio");
	B.psu = B.pullups = 0;
}

static void bp_psu(int on)
{
	char cmd[24];

	B.notice[0] = 0;
	bp_need_mode();
	if (!on) {
		bp_cmd("w");
	} else {
		utoa(cat(volts(cat(cmd, "W "), B.dv), " "), (unsigned)B.ma, 1);
		if (bp_cmd(cmd) == 0 && !strstr(bp_rx, "Enabled") && !bp_error())
			bp_note("The power supply did not switch on", "", 1);
	}
	bp_measure();
}

static void bp_pullups(int on)
{
	B.notice[0] = 0;
	bp_need_mode();
	if (bp_cmd(on ? "P" : "p") < 0)
		return;
	if (strstr(bp_rx, on ? "Enabled" : "Disabled"))
		B.pullups = on;
	else
		bp_error();
}

/* "4.6V" -> 46 */
static int dv_of(const char *v)
{
	int d = 0, frac = 0;

	for (; *v >= '0' && *v <= '9'; v++)
		d = d * 10 + (*v - '0');
	if (*v == '.' && v[1] >= '0' && v[1] <= '9')
		frac = v[1] - '0';
	return d * 10 + frac;
}

/*
 * m led, then its two questions: not the previous settings, this type.
 * The mode change switches the power supply off, and the Bus Pirate's IO
 * buffers run on Vout/Vref: without it no data reaches a strip. So the
 * supply goes back on if it was on; for a strip with nothing on Vout (no
 * supply of its own) it is switched on at 5 V.
 */
static int bp_led_mode(void)
{
	char num[4];
	int was = B.psu;

	if (bp_cmd("m led") < 0)
		return -1;
	if (strstr(bp_rx, "y/n") && bp_cmd("n") < 0)
		return -1;
	if (strstr(bp_rx, "Type")) {
		utoa(num, (unsigned)B.led_type + 1, 1);
		if (bp_cmd(num) < 0)
			return -1;
	}
	B.psu = B.pullups = 0;		/* a mode change switches both off */
	if (strcmp(B.mode, "LED")) {
		if (!bp_error())
			bp_note("Could not switch to LED mode", "", 1);
		return -1;
	}
	B.led_set = B.led_type;
	bp_measure();			/* which pins it uses */
	if (was) {
		bp_psu(1);
	} else if (B.led_type != LT_ONBOARD && dv_of(B.vout) < 10) {
		B.dv = 50;
		if (B.ma && B.ma < 300)
			B.ma = 300;
		bp_psu(1);
		if (B.psu)
			bp_note("Power supply switched on at 5 V: the strip runs on Vout", "", 0);
	}
	return bp_fd >= 0 ? 0 : -1;
}

/*
 * Light the LEDs: "[0x..:n]" for one colour, a run per hue for the
 * rainbow. WS2812s take green-red-blue; an APA102 frame is 0xE0 | global
 * brightness, then blue, green, red; the onboard LED is plain RGB (the
 * Bus Pirate lights one of its LEDs with it).
 */
static void bp_leds(int off)
{
	char cmd[200], *p = cmd;
	int n = B.led_type == LT_ONBOARD ? 1 : B.led_n, run;

	B.notice[0] = 0;
	if (B.led_set != B.led_type && bp_led_mode() < 0)
		return;
	if (B.led_type != LT_ONBOARD && dv_of(B.vout) < 10 && !B.notice[0])
		bp_note("No voltage on Vout: switch the power supply on, or connect the strip's supply to Vout", "", 1);
	run = !off && B.led_color == RAINBOW ? (n + 11) / 12 : n;
	*p++ = '[';
	for (int i = 0; i < n; i += run) {
		uint32_t c = off ? 0 : led_rgb(i, n), r = c >> 16, g = (c >> 8) & 255, b = c & 255;
		int k = n - i < run ? n - i : run;

		if (i)
			*p++ = ' ';
		p = cat(p, "0x");
		if (B.led_type == LT_WS2812)
			p = hexn(p, g << 16 | r << 8 | b, 6);
		else if (B.led_type == LT_APA102)
			p = hexn(p, 0xffu << 24 | b << 16 | g << 8 | r, 8);
		else
			p = hexn(p, c, 6);
		if (k > 1)
			p = utoa(cat(p, ":"), (unsigned)k, 1);
	}
	cat(p, "]");
	if (bp_cmd(cmd) == 0)
		bp_error();
}

/* Connect: open its terminal and wait for a prompt. */
static int bp_open(void)
{
	long t0 = now_ms();
	int ok = 0;

	if (bp_fd >= 0)
		return 0;
	B.notice[0] = 0;
	bp_gone = 0;
	if (!bp_find() || !bp_tty[0][0]) {
		bp_note("Connect a Bus Pirate to the bottom port", "", 1);
		return -1;
	}
	draw_message("BUS PIRATE", "Connecting\xe2\x80\xa6");
	blit();
	bp_fd = tty_open_raw(bp_tty[0]);
	if (bp_fd < 0) {
		bp_note("Cannot open /dev/", bp_tty[0], 1);
		return -1;
	}
	/* Opening its terminal makes it eject and re-insert its USB drive,
	 * and it answers once the kernel has noticed both (S05diskevents):
	 * 2-5 s. A new terminal is asked about colours first. */
	while (!ok && now_ms() - t0 < 20000) {
		int got;

		if (write(bp_fd, "\r", 1) < 0)
			break;
		got = bp_wait(1500);
		if (strstr(bp_rx, "(Y/n)"))
			write(bp_fd, "n\r", 2);	/* the colour question, after power-up */
		else if (got < 0)
			continue;
		else if (!(ok = bp_prompt()))
			write(bp_fd, "x\r", 2);	/* left in a menu: out of it */
	}
	if (!ok) {
		bp_close();
		bp_note("The Bus Pirate does not answer: unplug it and plug it in again", "", 1);
		return -1;
	}
	if (bp_cmd("i") == 0) {
		char *l = bp_rx, *e;

		B.n_info = 0;
		B.sump = strstr(bp_rx, "binmode: SUMP") != 0;
		for (l = strchr(l, '\n'); l && *l; l = e) {
			int n;

			e = strchr(++l, '\n');
			n = e ? (int)(e - l) : (int)strlen(l);
			if (!e || n == 0 || !strncmp(l, "This device", 11) || l[0] == '(')
				continue;
			if (n >= (int)sizeof(B.info[0]))
				n = sizeof(B.info[0]) - 1;
			if (!strncmp(l, "Bus Pirate", 10) && n < (int)sizeof(B.board)) {
				memcpy(B.board, l, (size_t)n);
				B.board[n] = 0;
			}
			if (!strncmp(l, "Firmware", 8)) {
				const char *at = memchr(l, '@', (size_t)n);
				int k = at ? n - (int)(at + 2 - l) : 0;

				if (k > 0 && k < (int)sizeof(B.fw)) {
					memcpy(B.fw, at + 2, (size_t)k);
					B.fw[k] = 0;
				}
			}
			if (B.n_info < (int)(sizeof(B.info) / sizeof(B.info[0]))) {
				memcpy(B.info[B.n_info], l, (size_t)n);
				B.info[B.n_info++][n] = 0;
			}
		}
	}
	bp_need_mode();
	bp_measure();
	return bp_fd >= 0 ? 0 : -1;
}

/* ---- logic analyzer (SUMP on the second port) ---- */

static void sump_long(int fd, int cmd, uint32_t v)
{
	unsigned char b[5] = { (unsigned char)cmd, (unsigned char)v, (unsigned char)(v >> 8),
			       (unsigned char)(v >> 16), (unsigned char)(v >> 24) };

	write(fd, b, 5);
}

static void sump_reset(int fd)
{
	static const unsigned char z[5];

	write(fd, z, 5);
}

/* Open the SUMP port; the ID command answers "1ALS". */
static int la_open(void)
{
	unsigned char id[4];
	struct pollfd p;
	long end;
	int n = 0;

	if (la_fd >= 0)
		return 0;
	if (!bp_tty[1][0] || (la_fd = tty_open_raw(bp_tty[1])) < 0)
		return -1;
	sump_reset(la_fd);
	usleep(20000);
	tcflush(la_fd, TCIFLUSH);
	write(la_fd, "\x02", 1);
	p.fd = la_fd;
	p.events = POLLIN;
	for (end = now_ms() + 1500; n < 4 && now_ms() < end; ) {
		int k;

		if (poll(&p, 1, 200) <= 0)
			continue;
		k = read(la_fd, id + n, (size_t)(4 - n));
		if (k > 0)
			n += k;
	}
	if (n == 4 && !memcmp(id, "1ALS", 4))
		return 0;
	close(la_fd);
	la_fd = -1;
	return -1;
}

static void la_status(const char *a, const char *b, int bad)
{
	LA.status[0] = 0;
	cat(cat(LA.status, a), b);
	LA.status_bad = bad;
}

static int wait_key(int timeout_ms);
static void draw_la(void);

/*
 * One capture: 8 channels at the chosen rate, optionally from an edge on
 * one pin (two trigger stages with the same mask and opposite values are
 * how the Bus Pirate's SUMP takes an edge). It starts sampling at the
 * trigger, so there is nothing from before it: all samples come after.
 * They arrive newest first. Esc cancels.
 */
static void la_capture(void)
{
	static uint8_t raw[LA_MAX];
	unsigned rate = la_rates[LA.rate_i];
	int n = la_sizes[LA.size_i], got = 0;
	uint32_t m = LA.trig >= 0 ? 1u << LA.trig : 0;
	long end = now_ms() + (long)((unsigned long long)n * 1000 / rate) + 3000;
	struct pollfd p[2];
	char buf[80], *q;

	bp_need_mode();		/* in HiZ it powers its supply for SUMP */
	if (la_open() < 0) {
		la_status(B.sump ? "The logic analyzer port does not answer"
				 : "Set the Bus Pirate's binmode to SUMP (Console: binmode)", "", 1);
		return;
	}
	sump_reset(la_fd);
	tcflush(la_fd, TCIFLUSH);
	sump_long(la_fd, 0x80, 100000000u / rate - 1);		/* divider of 100 MHz */
	sump_long(la_fd, 0x81, (uint32_t)(n / 4 - 1) << 16 | (uint32_t)(n / 4 - 1));
	sump_long(la_fd, 0x82, 0x38);			/* 8 channels: groups 1-3 off */
	sump_long(la_fd, 0xc0, m);
	sump_long(la_fd, 0xc1, LA.falling ? m : 0);
	sump_long(la_fd, 0xc2, m ? 0x08000000 : 0);	/* stage 0 starts the capture */
	sump_long(la_fd, 0xc4, m);
	sump_long(la_fd, 0xc5, LA.falling ? 0 : m);
	write(la_fd, "\x01", 1);				/* arm */
	if (m) {
		q = utoa(cat(buf, "Waiting for IO"), (unsigned)LA.trig, 1);
		cat(q, LA.falling ? " to fall (Esc cancels)" : " to rise (Esc cancels)");
	} else {
		cat(buf, "Capturing\xe2\x80\xa6");
	}
	la_status(buf, "", 0);
	draw_la();
	blit();
	p[0].fd = la_fd;
	p[0].events = POLLIN;
	p[1].fd = ev_fd;
	p[1].events = POLLIN;
	while (got < n) {
		int k = poll(p, 2, 200);

		if (k > 0 && (p[1].revents & POLLIN) && wait_key(0) == KEY_ESC) {
			sump_reset(la_fd);
			la_status("Cancelled", "", 0);
			return;
		}
		if (k > 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			k = read(la_fd, raw + got, (size_t)(n - got));
			if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
				bp_lost();
				return;
			}
			if (k > 0) {
				got += k;
				end = now_ms() + 3000;
			}
		}
		if (!got && m)
			end = now_ms() + 3000;		/* waiting for the trigger */
		if (now_ms() > end) {
			la_status("No data from the logic analyzer", "", 1);
			return;
		}
	}
	for (int i = 0; i < n; i++)
		la_buf[i] = raw[n - 1 - i];
	LA.n = n;
	LA.rate = rate;
	LA.zoom = la_fit(n);
	LA.pos = 0;
	q = cat(utoa(buf, (unsigned)n, 1), " samples at ");
	q = cat(hz(q, rate), ": ");
	q = dur(q, (long long)n * 1000000000LL / rate);
	if (m)
		cat(utoa(cat(q, ", triggered on IO"), (unsigned)LA.trig, 1),
		    LA.falling ? " falling" : " rising");
	la_status(buf, "", 0);
}

/* The capture as raw bytes, one per sample: sigrok reads it with
 * -I binary:numchannels=8:samplerate=<rate>. */
static void la_save(void)
{
	static const char path[] = "/root/capture.bin";
	char rate[16];
	int fd;

	if (!LA.n) {
		la_status("Nothing captured yet", "", 1);
		return;
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, la_buf, (size_t)LA.n) != LA.n) {
		if (fd >= 0)
			close(fd);
		la_status("Could not write ", path, 1);
		return;
	}
	close(fd);
	hz(rate, LA.rate);
	la_status("Saved to /root/capture.bin: 8 channels, one byte per sample, ", rate, 0);
}

/* ---- network: netd's files, wpa_supplicant's control socket ---- */

#define WPA_CTRL	"/var/run/wpa_supplicant/wlan0"
#define NETD_PID	"/var/run/netd.pid"
static const char *const conf_path[2] = { "/etc/network/wifi.conf", "/etc/network/usb.conf" };

static void conf_load(int kind)
{
	char buf[512];

	if (read_str(conf_path[kind], buf, sizeof(buf)) < 0)
		buf[0] = 0;
	conf_parse(buf, &N.conf[kind]);
}

/* Write the file and have netd apply it. */
static void conf_save(int kind)
{
	char buf[512];
	int n = conf_text(buf, &N.conf[kind], kind), fd, pid;

	mkdir("/etc/network", 0755);
	fd = open(conf_path[kind], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, buf, n);
		close(fd);
	}
	pid = read_int(NETD_PID, 0);
	if (pid > 1)
		kill(pid, SIGHUP);
}

/* One request to wpa_supplicant; reply NUL-terminated. -1 if it is not up.
 * A socket name per request (pid, count): a reply that comes after the
 * timeout must not land on the next request (wbstatus.h). */
static int wpa_cmd(const char *cmd, char *reply, int max, int timeout_ms)
{
	static unsigned seq;
	struct sockaddr_un me = { .sun_family = AF_UNIX }, wpa = { .sun_family = AF_UNIX };
	struct pollfd p;
	int s, n = -1;
	char *q;

	s = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (s < 0)
		return -1;
	q = cat(me.sun_path, "/tmp/home-wpa-");
	q = cat(utoa(q, (unsigned)getpid(), 1), "-");
	utoa(q, seq++ % 1000, 1);
	unlink(me.sun_path);
	cat(wpa.sun_path, WPA_CTRL);
	if (bind(s, (struct sockaddr *)&me, sizeof(me)) == 0 &&
	    connect(s, (struct sockaddr *)&wpa, sizeof(wpa)) == 0 &&
	    send(s, cmd, strlen(cmd), 0) >= 0) {
		p.fd = s;
		p.events = POLLIN;
		if (poll(&p, 1, timeout_ms) > 0)
			n = recv(s, reply, max - 1, 0);
	}
	close(s);
	unlink(me.sun_path);
	if (n < 0)
		return -1;
	reply[n] = 0;
	return n;
}

static int wpa_ok(const char *cmd)
{
	char r[32];

	return wpa_cmd(cmd, r, sizeof(r), 3000) > 0 && !strncmp(r, "OK", 2);
}

static char wpa_buf[8192];	/* SCAN_RESULTS can be long; not on the 16 KB stack */

/* Next tab-separated field of *p (modified in place). */
static char *field(char **p)
{
	char *f = *p, *t;

	if (!f)
		return "";
	t = strchr(f, '\t');
	if (t)
		*t++ = 0;
	*p = t;
	return f;
}

static struct wnet *net_find(const char *ssid)
{
	for (int i = 0; i < N.n; i++)
		if (!strcmp(N.nets[i].ssid, ssid))
			return &N.nets[i];
	return 0;
}

static struct wnet *net_add(const char *ssid)
{
	struct wnet *w = net_find(ssid);

	if (w || N.n == MAX_NETS || !ssid[0])
		return w;
	w = &N.nets[N.n++];
	memset(w, 0, sizeof(*w));
	strncpy(w->ssid, ssid, sizeof(w->ssid) - 1);
	w->signal = -200;
	w->saved_id = -1;
	w->supported = 1;
	return w;
}

static void wpa_read(void)
{
	char *line, *next;

	N.wpa = 0;
	N.n = 0;
	N.state[0] = N.ssid[0] = 0;
	if (!N.conf[NK_WIFI].enabled || wpa_cmd("STATUS", wpa_buf, sizeof(wpa_buf), 1000) <= 0)
		return;
	N.wpa = 1;
	for (line = wpa_buf; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (!strncmp(line, "wpa_state=", 10))
			strncpy(N.state, line + 10, sizeof(N.state) - 1);
		else if (!strncmp(line, "ssid=", 5))
			strncpy(N.ssid, line + 5, sizeof(N.ssid) - 1);
	}
	if (strcmp(N.state, "COMPLETED"))
		N.ssid[0] = 0;

	/* "bssid / frequency / signal level / flags / ssid", one per line */
	if (wpa_cmd("SCAN_RESULTS", wpa_buf, sizeof(wpa_buf), 2000) > 0) {
		for (line = wpa_buf; line; line = next) {
			char *p = line, *flags, *ssid;
			struct wnet *w;
			int sig;

			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			if (line == wpa_buf || !*line)
				continue;		/* header */
			field(&p);
			field(&p);
			sig = strtol_(field(&p));
			flags = field(&p);
			ssid = field(&p);
			w = net_add(ssid);
			if (!w || (w->signal > -200 && w->signal >= sig))
				continue;
			w->signal = sig;
			w->secured = strstr(flags, "WPA") || strstr(flags, "RSN") || strstr(flags, "WEP");
			w->supported = !w->secured || strstr(flags, "PSK");
		}
	}
	/* "network id / ssid / bssid / flags" */
	if (wpa_cmd("LIST_NETWORKS", wpa_buf, sizeof(wpa_buf), 1000) > 0) {
		for (line = wpa_buf; line; line = next) {
			char *p = line, *id;
			struct wnet *w;

			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			if (line == wpa_buf || !*line)
				continue;
			id = field(&p);
			w = net_add(field(&p));
			if (w) {
				w->saved_id = strtol_(id);
				w->supported = 1;
			}
		}
	}
	for (int i = 0; i < N.n; i++)
		N.nets[i].current = N.ssid[0] && !strcmp(N.nets[i].ssid, N.ssid);
	/* current first, then by signal */
	for (int i = 1; i < N.n; i++)
		for (int j = i; j > 0; j--) {
			struct wnet *a = &N.nets[j - 1], *b = &N.nets[j], t;
			int ka = a->current * 1000 + a->signal, kb = b->current * 1000 + b->signal;

			if (ka >= kb)
				break;
			t = *a;
			*a = *b;
			*b = t;
		}
}

static void usb_read(void)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;

	N.usb_if[0] = N.usb_ip[0] = N.usb_mac[0] = 0;
	N.usb_link = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && !N.usb_if[0]) {
		char path[64], link[128], *p;
		int n;

		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || !strcmp(e->d_name, "wlan0"))
			continue;
		p = cat(path, "/sys/class/net/");
		cat(p, e->d_name);
		n = readlink(path, link, sizeof(link) - 1);
		if (n <= 0)
			continue;
		link[n] = 0;
		if (!strstr(link, "/usb"))
			continue;
		strncpy(N.usb_if, e->d_name, sizeof(N.usb_if) - 1);
		cat(cat(cat(path, "/sys/class/net/"), e->d_name), "/carrier");
		N.usb_link = read_int(path, 0) == 1;
		cat(cat(cat(path, "/sys/class/net/"), e->d_name), "/address");
		read_str(path, N.usb_mac, sizeof(N.usb_mac));
		if_ip(N.usb_if, N.usb_ip);
	}
	closedir(d);
}

static void net_read(void)
{
	conf_load(NK_WIFI);
	conf_load(NK_USB);
	wpa_read();
	usb_read();
}

/* wpa_supplicant prints SSIDs printf-escaped; it wants them as hex. */
static void ssid_hex(const char *s, char *out)
{
	static const char hx[] = "0123456789abcdef";

	while (*s) {
		unsigned c = (unsigned char)*s++;

		if (c == '\\' && *s) {
			c = (unsigned char)*s++;
			if (c == 'x' && s[0] && s[1]) {
				char h[3] = { s[0], s[1], 0 };
				int v = 0;

				for (int i = 0; i < 2; i++)
					v = v * 16 + (h[i] <= '9' ? h[i] - '0' : (h[i] | 32) - 'a' + 10);
				c = (unsigned)v;
				s += 2;
			} else if (c == 'n') {
				c = '\n';
			} else if (c == 'r') {
				c = '\r';
			} else if (c == 't') {
				c = '\t';
			} else if (c == 'e') {
				c = 27;
			}
		}
		*out++ = hx[c >> 4];
		*out++ = hx[c & 15];
	}
	*out = 0;
}

static void notice(const char *a, const char *b, int bad)
{
	cat(cat(N.notice, a), b);
	N.notice_bad = bad;
}

static void draw_message(const char *eyebrow_s, const char *msg);
static void blit(void);

/*
 * Join scanned or saved network i (psk 0: open, or already saved). Blocks
 * for up to 20 s with a "Connecting" screen. On success the network is
 * saved in /etc/wpa_supplicant.conf; a new one that fails is dropped again.
 */
static void wifi_connect(int i, const char *psk)
{
	struct wnet w = N.nets[i];
	char cmd[200], r[32], *p;
	int id = w.saved_id, added = 0, ok = 0;

	N.notice[0] = 0;
	if (id < 0) {
		if (wpa_cmd("ADD_NETWORK", r, sizeof(r), 2000) <= 0 || r[0] < '0' || r[0] > '9') {
			notice("Wi-Fi is not ready", "", 1);
			return;
		}
		id = strtol_(r);
		added = 1;
		p = utoa(cat(cmd, "SET_NETWORK "), (unsigned)id, 1);
		ssid_hex(w.ssid, cat(p, " ssid "));
		wpa_ok(cmd);
	}
	if (psk) {
		p = utoa(cat(cmd, "SET_NETWORK "), (unsigned)id, 1);
		cat(cat(cat(p, " psk \""), psk), "\"");
		if (!wpa_ok(cmd)) {
			notice("The password must be 8 to 63 characters", "", 1);
			goto fail;
		}
		p = utoa(cat(cmd, "SET_NETWORK "), (unsigned)id, 1);
		wpa_ok(cat(p, " key_mgmt WPA-PSK"));
	} else if (added) {
		p = utoa(cat(cmd, "SET_NETWORK "), (unsigned)id, 1);
		wpa_ok(cat(p, " key_mgmt NONE"));
	}
	p = utoa(cat(cmd, "SELECT_NETWORK "), (unsigned)id, 1);
	wpa_ok(cmd);
	for (int t = 0; t < 40 && !ok; t++) {
		char msg[96];

		cat(cat(cat(msg, "Connecting to "), w.ssid), "\xe2\x80\xa6");
		draw_message("WI-FI", msg);
		blit();
		usleep(500000);
		if (wpa_cmd("STATUS", wpa_buf, sizeof(wpa_buf), 1000) > 0)
			ok = strstr(wpa_buf, "wpa_state=COMPLETED") != 0;
	}
	/* SELECT_NETWORK disabled the other saved networks; keep them */
	wpa_ok("ENABLE_NETWORK all");
	if (ok) {
		wpa_ok("SAVE_CONFIG");
		notice("Connected to ", w.ssid, 0);
		return;
	}
	notice("No connection to ", w.ssid, 1);
	if (w.secured)
		cat(N.notice, " (wrong password?)");
fail:
	if (added) {
		p = utoa(cat(cmd, "REMOVE_NETWORK "), (unsigned)id, 1);
		wpa_ok(cmd);
		wpa_ok("ENABLE_NETWORK all");
	}
}

static void wifi_forget(int i)
{
	char cmd[40];

	if (N.nets[i].saved_id < 0)
		return;
	utoa(cat(cmd, "REMOVE_NETWORK "), (unsigned)N.nets[i].saved_id, 1);
	wpa_ok(cmd);
	wpa_ok("SAVE_CONFIG");
	notice("Forgot ", N.nets[i].ssid, 0);
}

static void wifi_scan(void)
{
	wpa_ok("SCAN");		/* results arrive in a few seconds; we redraw */
}

static void wifi_enable(int on)
{
	N.conf[NK_WIFI].enabled = on;
	N.notice[0] = 0;
	conf_save(NK_WIFI);
}

/* The badge's own (the file viewers, /usr/share/badge/apps) after the others. */
static const char *const app_roots[] = { "/mnt/sd/badge/apps", "/data/apps", "/usr/share/badge/apps" };

/* One app.conf (KEY=value lines, as netd's files) into ap. */
/*
 * The app store's placeholders in an app's ARGS and FILES (WhyOS-Apps
 * docs/package.md §2.3): %D its data folder on the card, %C its config
 * folder in $HOME, %T its cache, %d its directory, %% a %. Others stay.
 */
#define SD_ROOT	"/mnt/sd"

static char *app_place(const struct app *ap, char which, char *out, size_t n)
{
	const char *home = getenv("HOME");

	if (which == 'D')
		snprintf(out, n, SD_ROOT "/badge/appdata/%s", ap->id);
	else if (which == 'C')
		snprintf(out, n, "%s/.whyos/config/%s", home ? home : "/root", ap->id);
	else if (which == 'T' && access(SD_ROOT "/badge", W_OK) == 0)
		snprintf(out, n, SD_ROOT "/badge/appdata/%s/.cache", ap->id);
	else if (which == 'T')
		snprintf(out, n, "/tmp/cache/%s", ap->id);
	else
		snprintf(out, n, "%s", ap->dir);
	return out;
}

static void app_expand(const struct app *ap, const char *in, char *out, size_t n)
{
	size_t o = 0;

	for (; *in && o + 1 < n; in++) {
		char place[160];
		const char *v = 0;

		if (in[0] == '%' && strchr("DCTd", in[1]))
			v = app_place(ap, in[1], place, sizeof(place));
		else if (in[0] == '%' && in[1] == '%')
			v = "%";
		if (!v) {
			out[o++] = *in;
			continue;
		}
		in++;
		while (*v && o + 1 < n)
			out[o++] = *v++;
	}
	out[o] = 0;
}

static int app_parse(struct app *ap, const char *dir)
{
	char path[96], buf[2048], cat_[12] = "apps", rot[8] = "";

	memset(ap, 0, sizeof(*ap));
	if (strlen(dir) >= sizeof(ap->dir))
		return -1;
	cat(ap->dir, dir);
	cat(cat(path, dir), "/app.conf");
	if (read_str(path, buf, sizeof(buf)) <= 0)
		return -1;
	for (const char *l = buf; l && *l; ) {
		const char *nl = strchr(l, '\n');
		static const struct { const char *k; size_t off, size; } keys[] = {
			{ "NAME=", offsetof(struct app, name), sizeof(((struct app *)0)->name) },
			{ "INFO=", offsetof(struct app, info), sizeof(((struct app *)0)->info) },
			{ "EXEC=", offsetof(struct app, exec), sizeof(((struct app *)0)->exec) },
			{ "ARGS=", offsetof(struct app, args), sizeof(((struct app *)0)->args) },
			{ "FILES=", offsetof(struct app, files), sizeof(((struct app *)0)->files) },
			{ "TYPES=", offsetof(struct app, types), sizeof(((struct app *)0)->types) },
			{ "ICON=", offsetof(struct app, icon), sizeof(((struct app *)0)->icon) },
			{ "ASK=", offsetof(struct app, ask), sizeof(((struct app *)0)->ask) },
			{ "ASK_DEFAULT=", offsetof(struct app, ask_default),
			  sizeof(((struct app *)0)->ask_default) },
			{ "OPENS=", offsetof(struct app, opens), sizeof(((struct app *)0)->opens) },
			{ "OPENS_ALSO=", offsetof(struct app, opens_also),
			  sizeof(((struct app *)0)->opens_also) },
		};

		for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
			if (!strncmp(l, keys[i].k, strlen(keys[i].k)))
				conf_value((char *)ap + keys[i].off, (int)keys[i].size, l + strlen(keys[i].k));
		if (!strncmp(l, "CATEGORY=", 9))
			conf_value(cat_, sizeof(cat_), l + 9);
		if (!strncmp(l, "TERMINAL=", 9)) {
			char v[8] = "";

			conf_value(v, sizeof(v), l + 9);
			ap->terminal = v[0] == 'c' ? 2 : v[0] == 's' ? 3 :
				       v[0] == '1' || v[0] == 'y' || v[0] == 't';
		}
		if (!strncmp(l, "ESC=", 4)) {
			char v[8] = "";

			conf_value(v, sizeof(v), l + 4);
			ap->esc = !strcmp(v, "quit");
		}
		if (!strncmp(l, "ID=", 3))
			conf_value(ap->id, sizeof(ap->id), l + 3);
		if (!strncmp(l, "TYPE=", 5))
			conf_value(ap->type, sizeof(ap->type), l + 5);
		if (!strncmp(l, "ROTATION=", 9))
			conf_value(rot, sizeof(rot), l + 9);
		l = nl ? nl + 1 : 0;
	}
	ap->rotation_lock = !strcmp(rot, "locked");
	if (!ap->id[0]) {
		const char *b = strrchr(ap->dir, '/');

		strncpy(ap->id, b ? b + 1 : ap->dir, sizeof(ap->id) - 1);
	}
	if (strchr(ap->files, '%')) {
		char f[sizeof(ap->files)];

		app_expand(ap, ap->files, f, sizeof(f));
		memcpy(ap->files, f, sizeof(f));
	}
	ap->cat = !strcmp(cat_, "games") ? CAT_GAMES : !strcmp(cat_, "tools") ? CAT_TOOLS :
		  !strcmp(cat_, "none") ? CAT_NONE : CAT_APPS;
	if (!ap->name[0] || !ap->exec[0])
		return -1;
	if (ap->exec[0] == '/')
		cat(path, ap->exec);
	else
		cat(cat(cat(path, ap->dir), "/"), ap->exec);
	return access(path, X_OK) == 0 ? 0 : -1;
}

/* All installed apps, by name. */
static void apps_read(void)
{
	n_apps = 0;
	for (unsigned r = 0; r < sizeof(app_roots) / sizeof(app_roots[0]); r++) {
		DIR *d = opendir(app_roots[r]);
		struct dirent *e;

		while (d && (e = readdir(d)) && n_apps < MAX_APPS) {
			char dir[64];
			int i;

			if (e->d_name[0] == '.' || strlen(app_roots[r]) + strlen(e->d_name) + 2 > sizeof(dir))
				continue;
			cat(cat(cat(dir, app_roots[r]), "/"), e->d_name);
			if (app_parse(&A[n_apps], dir) < 0)
				continue;
			for (i = n_apps; i > 0 && strcasecmp(A[i - 1].name, A[n_apps].name) > 0; i--)
				;
			if (i < n_apps) {
				struct app t = A[n_apps];

				memmove(&A[i + 1], &A[i], sizeof(A[0]) * (size_t)(n_apps - i));
				A[i] = t;
			}
			n_apps++;
		}
		if (d)
			closedir(d);
	}
}

/* Is name one of the extensions in types ("gb gbc"), or is types empty? */
static int type_ok(const char *types, const char *name)
{
	const char *dot = strrchr(name, '.');
	size_t n;

	if (!types[0])
		return 1;
	if (!dot)
		return 0;
	n = strlen(++dot);
	for (const char *t = types; *t; ) {
		size_t k = 0;

		while (t[k] && t[k] != ' ' && t[k] != ',')
			k++;
		if (k == n && !strncasecmp(t, dot, n))
			return 1;
		t += k;
		while (*t == ' ' || *t == ',')
			t++;
	}
	return 0;
}

/* The files the chooser of app F.app offers, by name. */
static void files_read(void)
{
	const struct app *ap = &A[F.app];
	DIR *d = opendir(ap->files);
	struct dirent *e;

	F.n = F.more = 0;
	while (d && (e = readdir(d))) {
		const char *dot = strrchr(e->d_name, '.');
		size_t n = dot && dot != e->d_name ? (size_t)(dot - e->d_name) : strlen(e->d_name);
		int i;

		if (e->d_name[0] == '.' || !type_ok(ap->types, e->d_name) ||
		    strlen(e->d_name) >= sizeof(F.file[0]))
			continue;
		if (F.n == MAX_FILES) {
			F.more++;
			continue;
		}
		for (i = F.n; i > 0 && strcasecmp(F.file[i - 1], e->d_name) > 0; i--) {
			memcpy(F.file[i], F.file[i - 1], sizeof(F.file[0]));
			memcpy(F.name[i], F.name[i - 1], sizeof(F.name[0]));
		}
		memset(F.file[i], 0, sizeof(F.file[0]));
		memset(F.name[i], 0, sizeof(F.name[0]));
		cat(F.file[i], e->d_name);
		memcpy(F.name[i], e->d_name, n < sizeof(F.name[0]) - 1 ? n : sizeof(F.name[0]) - 1);
		F.n++;
	}
	if (d)
		closedir(d);
}

/*
 * The home screen's welcome: the user's name (setting "name",
 * docs/SETTINGS.md) and the temperature from the BME680, read at most
 * every 30 s: each read starts a measurement over the bit-banged I2C.
 */
static void welcome_read(void)
{
	static struct settings ws;
	static time_t temp_at;
	static char temp_path[64];
	time_t now = time(0);
	struct setting *n;

	settings_load(&ws);
	memset(S.user, 0, sizeof(S.user));
	if ((n = settings_find(&ws, "name")) && n->value[0]) {
		strncpy(S.user, n->value, sizeof(S.user) - 1);
		if (S.user[0] >= 'a' && S.user[0] <= 'z')
			S.user[0] = (char)(S.user[0] - 'a' + 'A');
	}
	if (temp_at && now - temp_at < 30 && now >= temp_at)
		return;
	temp_at = now;
	if (!temp_path[0]) {
		char name[16];

		for (int i = 0; i < 8; i++) {
			char *p = cat(temp_path, "/sys/bus/iio/devices/iio:device");

			utoa(p, (unsigned)i, 1);
			p = temp_path + strlen(temp_path);
			cat(p, "/name");
			if (read_str(temp_path, name, sizeof(name)) >= 0 && !strncmp(name, "bme680", 6)) {
				cat(p, "/in_temp_input");
				break;
			}
			temp_path[0] = 0;
		}
	}
	S.temp[0] = 0;
	if (temp_path[0]) {
		char v[16];
		long mc = 0;	/* millidegrees */
		int neg;
		const char *q = v;

		if (read_str(temp_path, v, sizeof(v)) < 0)
			return;
		neg = *q == '-';
		q += neg;
		if (*q < '0' || *q > '9')
			return;
		while (*q >= '0' && *q <= '9')
			mc = mc * 10 + (*q++ - '0');
		mc = (mc + 50) / 100;	/* tenths, rounded */
		utoa(cat(utoa(cat(S.temp, neg && mc ? "-" : ""), (unsigned)(mc / 10), 1), "."),
		     (unsigned)(mc % 10), 1);
		cat(S.temp + strlen(S.temp), "\xc2\xb0" "C");
	}
}

static void sys_read(void)
{
	fill_clock(time(0));
	welcome_read();
	read_wifi();
	S.bl_max = read_int(BL_MAX, 255);
	S.bl = read_int(BL, BL_DEFAULT);
	S.kbd = read_int(KBD_BL, 0);
	S.ble_pid = find_proc("ble_adv");
	S.lora = lora_on();
	S.wifi_bars = wifi_bars();
	apps_read();
	if (F.app >= n_apps)
		F.app = -1;
	if (F.app >= 0)
		files_read();
	read_usb();
	B.present = HAS_BP && bp_find();
	net_read();
	disks_read();
}

#else /* HOME_PREVIEW: a fixed, plausible badge */

static void sys_read(void)
{
	struct tm tm = { .tm_year = 126, .tm_mon = 9, .tm_mday = 9,
			 .tm_hour = 14, .tm_min = 32, .tm_isdst = -1 };

	fill_clock(mktime(&tm));
	cat(S.user, "John");
	cat(S.temp, "21.4\xc2\xb0" "C");
	S.wifi_up = 1;
	S.wifi_bars = 2;
	cat(S.wifi_state, "up");
	cat(S.wifi_ip, "192.168.88.36");
	cat(S.wifi_mac, "a0:85:e3:12:34:56");
	S.bl_max = 255;
	S.bl = 153;
	S.kbd = 0;
	S.ble_pid = 0;
	S.lora = 1;
	{
		static const struct { const char *name, *info, *icon, *files; int cat; } apps[] = {
			{ "Game Boy", "gnuboy", "gameboy", "/mnt/sd/roms", CAT_GAMES },
			{ "NetSurf", "web browser", "globe", "", CAT_APPS },
		};
		static const char *const games[] = { "dmg-acid2", "Tobu Tobu Girl", "ucity" };

		n_apps = 2;
		for (int i = 0; i < n_apps; i++) {
			cat(A[i].name, apps[i].name);
			cat(A[i].info, apps[i].info);
			cat(A[i].icon, apps[i].icon);
			cat(A[i].files, apps[i].files);
			cat(A[i].types, i ? "" : "gb gbc");
			A[i].cat = apps[i].cat;
		}
		F.app = 0;
		F.n = 3;
		for (int i = 0; i < F.n; i++)
			cat(F.name[i], games[i]);
	}
	S.n_usb = 1;
	cat(S.usb[0][0], "USB2.0-Serial");
	cat(S.usb[0][1], "1a86:7523");

	static const struct { const char *ssid; int sig, sec, sup, saved, cur; } nets[] = {
		{ "why2025", -48, 1, 1, 0, 1 }, { "why2025-open", -55, 0, 1, -1, 0 },
		{ "Nordic Village", -63, 1, 1, -1, 0 }, { "eduroam", -70, 1, 0, -1, 0 },
		{ "Home", -200, 1, 1, 1, 0 },
	};
	N.conf[NK_WIFI].enabled = 1;
	N.conf[NK_USB].is_static = 1;
	cat(N.conf[NK_USB].address, "10.0.0.2/24");
	cat(N.conf[NK_USB].gateway, "10.0.0.1");
	cat(N.conf[NK_USB].dns, "10.0.0.1");
	N.wpa = 1;
	cat(N.state, "COMPLETED");
	cat(N.ssid, "why2025");
	N.n = 5;
	for (int i = 0; i < N.n; i++) {
		cat(N.nets[i].ssid, nets[i].ssid);
		N.nets[i].signal = nets[i].sig;
		N.nets[i].secured = nets[i].sec;
		N.nets[i].supported = nets[i].sup;
		N.nets[i].saved_id = nets[i].saved;
		N.nets[i].current = nets[i].cur;
	}
	cat(N.usb_if, "eth0");
	N.usb_link = 1;
	cat(N.usb_ip, "10.0.0.2");
	cat(N.usb_mac, "00:e0:4c:68:01:23");
	cat(N.notice, "Connected to why2025");

	/* the SD card, and two USB drives through a hub: a stick and the
	 * Bus Pirate's own drive */
	static const struct { const char *name, *label; unsigned long long size; } dk[] = {
		{ "mmcblk0", "SD card", 15728640000ULL },
		{ "sda", "SanDisk Cruzer Blade", 8004304896ULL },
		{ "sdb", "Bus Pirate 6", 100663296ULL },
	};
	static const struct { int disk; const char *dev, *mnt, *type, *label;
			      unsigned long long size, used; } fs[] = {
		{ 0, "/dev/mmcblk0p1", "/mnt/sd", "FAT32", "SD card", 15715221504ULL, 5505024ULL },
		{ 1, "/dev/sda1", "/media/sda1", "exFAT", "USB drive", 8002732032ULL, 3221225472ULL },
		{ 2, "/dev/sdb1", "", "FAT16", "USB drive", 100597760ULL, 0 },
		{ -1, "", "/data", "JFFS2", "Badge storage", 4587520ULL, 2293760ULL },
		{ -1, "", "/tmp", "tmpfs", "Memory", 9437184ULL, 1048576ULL },
		{ -1, "", "/", "squashfs", "System", 2752512ULL, 2752512ULL },
	};

	D.have_fsck = 1;
	D.n_disk = 3;
	for (int i = 0; i < D.n_disk; i++) {
		cat(D.disk[i].name, dk[i].name);
		cat(D.disk[i].label, dk[i].label);
		D.disk[i].usb = i > 0;
		D.disk[i].size = dk[i].size;
	}
	D.n = 6;
	for (int i = 0; i < D.n; i++) {
		struct fsinfo *f = &D.fs[i];

		f->disk = fs[i].disk;
		f->part = fs[i].disk >= 0;
		cat(f->dev, fs[i].dev);
		cat(f->mnt, fs[i].mnt);
		cat(f->type, fs[i].type);
		cat(f->label, fs[i].label);
		f->size = fs[i].size;
		f->used = fs[i].used;
	}
	cat(D.notice, "No errors found");

	/* a Bus Pirate 6 in DIO mode, its supply on with the pull-ups */
	static const char *const pv[BP_PINS] = { "3.3V", "0.0V", "3.2V", "3.3V", "1.6V", "3.3V", "3.3V", "0.0V" };
	static const char *const info[] = {
		"Bus Pirate 6", "https://BusPirate.com/",
		"Firmware main branch @ de50049 (Oct  9 2025 10:33:57)",
		"RP2350B with 512KB RAM, 128Mbit FLASH", "S/N: 844A3CC0D5088C86",
		"Storage:   0.10GB (FAT16 File System)", "Configuration file: Not Detected",
		"Active binmode: SUMP logic analyzer",
		"Available modes: HiZ 1WIRE UART HDUART I2C SPI 2WIRE 3WIRE DIO LED INFRARED JTAG",
		"Active mode: DIO", "Display format: Auto",
	};
	B.present = 1;
	cat(B.board, "Bus Pirate 6");
	cat(B.mode, "DIO");
	cat(B.fw, "de50049 (Oct  9 2025 10:33:57)");
	B.sump = 1;
	B.psu = 1;
	B.pullups = 1;
	cat(B.vout, "3.3V");
	cat(B.iout, "1.4mA");
	for (int i = 0; i < BP_PINS; i++) {
		cat(B.pin_v[i], pv[i]);
		cat(B.pin_f[i], "-");
	}
	B.n_info = (int)(sizeof(info) / sizeof(info[0]));
	for (int i = 0; i < B.n_info; i++)
		cat(B.info[i], info[i]);

	/* a capture: an SPI-like burst triggered on IO2 falling (CS) */
	LA.n = 8192;
	LA.rate = 1000000;
	LA.trig = 2;
	LA.falling = 1;
	LA.zoom = la_fit(LA.n);
	for (int i = 0; i < LA.n; i++) {
		int t = i - 300, cs = t >= 0 && t < 4000, clk = cs && t > 20 && (t / 8) % 2;
		int bit = (t * 7 / 64 + t / 300) % 3 == 0;

		la_buf[i] = (uint8_t)(clk << 0 | (cs && bit) << 1 | !cs << 2 | (i / 1500 % 2) << 3 |
				      1 << 5 | ((i % 997) < 40) << 6);
	}
	cat(LA.status, "8192 samples at 1 MHz: 8.19 ms, triggered on IO2 falling");
}

#endif

/* ------------------------------------------------------------------ */
/* Chrome: status bar and hint bar                                     */

/*
 * WhyOS on the left ("OS" in cyan); on the right LoRa while the radio is on
 * (pink, docs/LORA.md), Wi-Fi by signal strength (none when not connected:
 * wbstatus.h) and, rightmost, the clock (design/canvas).
 * The design sets "LoRa" in regular 16 px mono; the font set has the
 * clock's semibold at that size, used here. No battery gauge: the IP5306
 * has no data interface.
 */
static void status_bar(void)
{
	const struct wb_font *n = &f_ui18b;
	int cy = WB_STATUS_BAR_H / 2;

	text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
	     text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
	status_right(S.hhmm, S.clock_ok, S.wifi_bars, S.lora);
}

static const char *fit(const struct wb_font *f, const char *s, int max_w, char *buf, int size);

/* ------------------------------------------------------------------ */
/* Home                                                                */

struct tile {
	const char *label;
	const struct wb_icon *ic, *key;
	uint32_t color;
};

static const struct tile tiles[6] = {
	{ "Apps",     &i_apps,     &k_square,   WB_COLOR_KEY_SQUARE },
	{ "Games",    &i_games,    &k_triangle, WB_COLOR_KEY_TRIANGLE },
	{ "Files",    &i_files,    &k_cross,    WB_COLOR_KEY_CROSS },
	{ "Terminal", &i_terminal, &k_circle,   WB_COLOR_KEY_CIRCLE },
	{ "Settings", &i_settings, &k_cloud,    WB_COLOR_KEY_CLOUD },
	{ "Power",    &i_power,    &k_diamond,  WB_COLOR_KEY_DIAMOND },
};

#define TILE_W ((W - 2 * WB_CONTENT_PAD - 2 * WB_HOME_TILE_GAP) / 3)
/* The header above the tiles: an eyebrow (18) + 6 + a line (26), or the
 * home screen's welcome (40) + 6 + the date (26) */
#define HEAD_EYEBROW	(18 + 6 + 26)
#define HEAD_WELCOME	(40 + 6 + 26)

/* The home screen's header: "Welcome <name>", the date, the temperature. */
static void welcome(int top)
{
	const struct wb_font *t = &f_ui36b, *d = &f_ui20;
	const struct wb_icon *th = &i_thermo20;
	int dy = top + 40 + 6 + 13;	/* centre of the date line */
	int right = W - WB_CONTENT_PAD - 6, x;
	char buf[40];

	if (S.temp[0]) {
		int tw = text_w(d, S.temp, 0);

		text(d, right - tw, text_top(d, dy), S.temp, WB_COLOR_TEXT_MID, 0);
		icon(th, right - tw - 6 - th->w, dy - th->h / 2, WB_COLOR_TEXT_MID, 255);
		right -= tw + 6 + th->w;
	}
	x = text(t, WB_CONTENT_PAD, text_top(t, top + 20), S.user[0] ? "Welcome " : "Welcome",
		 WB_COLOR_TEXT_HI, -12);
	/* the name gives way to the temperature (24 px apart), with an ellipsis */
	if (S.user[0])
		text(t, x, text_top(t, top + 20),
		     fit(t, S.user, right - 24 - x, buf, sizeof(buf)), WB_COLOR_CYAN, -12);
	text(d, WB_CONTENT_PAD, text_top(d, dy), S.date, WB_COLOR_TEXT_MID, 0);
}

static int popup;	/* not the session menu: Esc and the logo key close it */

/*
 * Six tiles under a header, as the home screen. sub, when given, is a
 * status line under each label (the Bus Pirate's tiles), in sub_c.
 */
static void draw_tiles(const char *eb, const char *line, const struct tile *ts,
		       const char *const *sub, const uint32_t *sub_c, int focus,
		       const struct hint *hs, int nh)
{
	/* header + 36 + two rows, centred between the bars as the design's
	 * flex column does; eb 0: the home screen's welcome */
	int head = eb ? HEAD_EYEBROW : HEAD_WELCOME;
	int top = WB_STATUS_BAR_H + (H - WB_STATUS_BAR_H - WB_HINT_BAR_H -
				     (head + 36 + 2 * WB_HOME_TILE_H + WB_HOME_TILE_GAP)) / 2;
	int grid = top + head + 36;

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	if (eb) {
		eyebrow(WB_CONTENT_PAD, top, eb);
		text(&f_ui20, WB_CONTENT_PAD, top + 24, line, WB_COLOR_TEXT_MID, 0);
	} else {
		welcome(top);
	}

	for (int i = 0; i < 6; i++) {
		const struct tile *t = &ts[i];
		int x = WB_CONTENT_PAD + (i % 3) * (TILE_W + WB_HOME_TILE_GAP);
		int y = grid + (i / 3) * (WB_HOME_TILE_H + WB_HOME_TILE_GAP);
		int on = i == focus;
		const struct wb_font *lf = on ? &f_ui18m : &f_ui18;
		/* icon 44 + gap 14 + label line 23 (+ 2 + status 20), centred */
		int block = sub ? 44 + 12 + 23 + 2 + 20 : 44 + 14 + 23;
		int iy = y + (WB_HOME_TILE_H - block) / 2;
		int ly = iy + 44 + (sub ? 12 : 14) + 23 / 2;
		char buf[40];

		if (on)
			box_focus(x, y, TILE_W, WB_HOME_TILE_H, WB_RADIUS_LG, &GLOW_TILE);
		else
			box_plain(x, y, TILE_W, WB_HOME_TILE_H, WB_RADIUS_LG, WB_COLOR_SURFACE1);
		if (t->key)	/* the symbol key that opens it, if one does */
			icon(t->key, x + TILE_W - 14 - t->key->w, y + 14, t->color, 178);
		icon(t->ic, x + (TILE_W - t->ic->w) / 2, iy, t->color, 255);
		text(lf, x + (TILE_W - text_w(lf, t->label, 0)) / 2,
		     text_top(lf, ly), t->label, WB_COLOR_TEXT_HI, 0);
		if (sub && sub[i][0]) {
			const char *v = fit(&f_ui16, sub[i], TILE_W - 24, buf, sizeof(buf));

			text(&f_ui16, x + (TILE_W - text_w(&f_ui16, v, 0)) / 2,
			     text_top(&f_ui16, ly + 23 / 2 + 2 + 10), v, sub_c[i], 0);
		}
	}
	hint_bar(hs, nh);
}

/* 6.8: on the home screen the six symbol keys open their tiles; Esc does
 * nothing there, except close the menu when it is a pop-up. */
static void draw_home(int focus)
{
	static const struct hint session_hints[] = {
		{ "SYMS", "open", 0 }, { "Enter", "open", 0 },
	};
	static const struct hint popup_hints[] = {
		{ "SYMS", "open", 0 }, { "Enter", "open", 0 }, { "Esc", "close", 1 },
	};

	if (popup)
		draw_tiles(0, 0, tiles, 0, 0, focus, popup_hints, 3);
	else
		draw_tiles(0, 0, tiles, 0, 0, focus, session_hints, 2);
}

#ifndef HOME_PREVIEW
/* Arrows, Enter and the symbol keys on a tile screen: the tile to open,
 * or -1. */
static int tiles_key(int key, int *focus)
{
	if (key >= KEY_F1 && key <= KEY_F6)
		return *focus = key - KEY_F1;
	if (key == KEY_LEFT)
		*focus = *focus % 3 ? *focus - 1 : *focus + 2;
	else if (key == KEY_RIGHT)
		*focus = *focus % 3 == 2 ? *focus - 2 : *focus + 1;
	else if (key == KEY_UP || key == KEY_DOWN)
		*focus = (*focus + 3) % 6;
	else if (key == KEY_ENTER)
		return *focus;
	return -1;
}
#endif

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */

/* Text cut to max_w px with an ellipsis, into buf. */
static const char *fit(const struct wb_font *f, const char *s, int max_w, char *buf, int size)
{
	int n;

	if (text_w(f, s, 0) <= max_w)
		return s;
	n = (int)strlen(s);
	if (n > size - 4)
		n = size - 4;
	while (n > 0) {
		memcpy(buf, s, (size_t)n);
		cat(buf + n, "\xe2\x80\xa6");
		if (text_w(f, buf, 0) <= max_w)
			return buf;
		n--;
	}
	return "";
}

#define DLG_X	72
#define DLG_W	(W - 2 * DLG_X)

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
	int h = 32 + 18 + 6 + 36 + 24 + body_h + gap + (n ? n * 68 + (n - 1) * 12 : 0) + 32;
	int y = (H - h) / 2, body = y + 32 + 18 + 6 + 36 + 24, by = body + body_h + gap;
	char tbuf[96];

	dim(WB_COLOR_CONSOLE, 184);	/* rgba(6,6,13,0.72) */
	box_plain(DLG_X, y, DLG_W, h, WB_RADIUS_LG, WB_COLOR_SURFACE2);
	eyebrow(DLG_X + 32, y + 32, eb);
	text(&f_ui28b, DLG_X + 32, y + 32 + 18 + 6 + 2,
	     fit(&f_ui28b, title, DLG_W - 64, tbuf, sizeof(tbuf)), WB_COLOR_TEXT_HI, 0);
	for (int i = 0; i < n; i++, by += 68 + 12) {
		int on = i == focus;

		if (on)
			box_focus(DLG_X + 32, by, DLG_W - 64, 68, WB_RADIUS_MD, &GLOW_ROW);
		else
			box_plain(DLG_X + 32, by, DLG_W - 64, 68, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		/* the destructive choice in red (6.5: Dialog) */
		icon(ics[i], DLG_X + 32 + 20, by + (68 - 26) / 2,
		     i == danger ? WB_COLOR_ERROR : on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255);
		text(&f_ui22, DLG_X + 32 + 20 + 26 + 16, text_top(&f_ui22, by + 34),
		     labels[i], i == danger ? WB_COLOR_ERROR : WB_COLOR_TEXT_HI, 0);
	}
	return body;
}

static const struct hint choice_hints[] = {
	{ "\xe2\x86\x91\xe2\x86\x93", "select", 0 }, { "Enter", "confirm", 0 },
	{ "Esc", "cancel", 1 },
};

static void draw_power(int home_focus, int focus)
{
	static const char *const labels[2] = { "Restart", "Shut down" };
	static const struct wb_icon *const ics[2] = { &r_restart, &r_power };

	draw_home(home_focus);
	dialog("POWER", "Turn off the badge?", 0, labels, ics, 2, focus, -1);
	hint_bar(choice_hints, 3);
}

/* A full-screen one-liner while the badge goes down. */
static void draw_message(const char *eyebrow_s, const char *msg)
{
	int w = text_w(&f_ui28b, msg, 0);

	fill(0, 0, W, H, WB_COLOR_VOID);
	eyebrow((W - text_w(&f_mono14, eyebrow_s, 22)) / 2, H / 2 - 40, eyebrow_s);
	text(&f_ui28b, (W - w) / 2, H / 2 - 12, msg, WB_COLOR_TEXT_HI, 0);
}

/* ------------------------------------------------------------------ */
/* Lists                                                               */

enum kind { R_GROUP, R_ACTION, R_TOGGLE, R_SLIDER, R_INFO, R_NETWORK, R_USAGE,
	    R_CHOICE,		/* R_CHOICE: one of a list, chosen in a pop-up (Enter) */
	    R_FILE,		/* R_FILE: a file in Files: its size, no chevron */
	    R_DRIVE };		/* R_DRIVE: Files' Drives: path, free space, usage */
enum act {
	A_NONE, A_BLE, A_USB, A_BRIGHT, A_KBD,
	A_NET_WIFI, A_NET_USB, A_WIFI_ON, A_SCAN, A_NETWORK, A_IP, A_DHCP,
	A_ADDR, A_GW, A_DNS, A_DISKS, A_DISK, A_MOUNT, A_FSCK, A_FORMAT,
	A_BP, A_PSU, A_VOLT, A_LIMIT, A_PULL, A_LTYPE, A_LCOLOR, A_LBRIGHT,
	A_LCOUNT, A_LOFF, A_APP, A_FILE, A_ASK,
	A_BROWSE, A_OPENWITH, A_FILEOPS, A_RENAME, A_MKDIR, A_SETTING,
	A_STORE, A_MYAPPS, A_LORA, A_SUB, A_LORA_PRESET, A_LORA_POWER, A_TZ,
	A_DRIVE, A_HIDDEN, A_NETOPS, A_LA_SET,
	A_LA_RATE, A_LA_TRIG, A_LA_EDGE, A_LA_SIZE,
};
enum list_id { L_APPS, L_GAMES, L_BROWSE, L_SETTINGS, L_USB,
	       L_NET_WIFI, L_NET_USB, L_IP_WIFI, L_IP_USB, L_DISKS, L_DISK,
	       L_BP_PSU, L_BP_LED, L_FILES,
	       /* Settings' submenus (A_SUB: idx is the list) */
	       L_SET_GENERAL, L_SET_CONN, L_SET_DISPLAY, L_SET_STORAGE, L_LORA,
	       L_FILES_SET };	/* Files' own settings (the cloud key) */

struct row {
	enum kind kind;
	const char *label;
	const struct wb_icon *ic;
	char value[40];
	int on;			/* toggle state, slider percent */
	int idx;		/* R_NETWORK: index into N.nets */
	int lock, dim;		/* R_NETWORK: secured; cannot be joined */
	char text[40];		/* label storage when it is built here */
	enum act act;
	int swatch;		/* R_CHOICE: 1 a colour swatch (sw_c), 2 a rainbow */
	uint32_t sw_c;
	int compact;		/* Files: 56 px instead of 68 */
};

#define LIST_MAX	96		/* Apps with many store apps, in groups */

struct list {
	enum list_id id;
	const char *eyebrow, *title;
	const char *title_r;		/* right of the title (Files: free space) */
	const char *path, *path_tag;	/* under the title in mono; a pink label */
	const char *notice;
	int notice_bad;
	struct row rows[LIST_MAX];
	int n;
	int sb_total, sb_first;		/* Files: the scroll bar over all entries, in px */
};

static time_t scanning_until;	/* "Scanning..." on the Wi-Fi screen */
static const char *games_notice;	/* how the last game ended */
static const char *tools_notice;	/* an app in Settings > Tools did not start */
static struct settings SET;		/* general settings, read for Settings */

/* Is LoRa on in Settings? (The radio itself: S.lora.) */
static int lora_wanted(void)
{
	struct setting *v = settings_find(&SET, "lora");

	return v && !strcmp(v->value, "on");
}
/* The radio settings (lora_presets.h): lora.radio, lora.power. */
static void lora_radio(struct lora_radio *r, int *dbm)
{
	struct setting *v = settings_find(&SET, "lora.radio");
	struct setting *pw = settings_find(&SET, "lora.power");

	if (!v || lora_radio_parse(v->value, r))
		lora_radio_parse(LORA_RADIO_DEFAULT, r);
	*dbm = pw && pw->value[0] ? atoi(pw->value) : LORA_POWER_DEFAULT;
}

/* The time zones on offer (/usr/share/badge/zones, from overlay-src/zones.py):
 * their names, read once. */
#define ZONES_FILE	"/usr/share/badge/zones"
#define ZONES_MAX	96
static char zone_names[ZONES_MAX][32];
static char zone_city[ZONES_MAX][32];	/* "St Johns": the part after the last / */
static char zone_region[ZONES_MAX][16];	/* "America"; "" for UTC */
static char zone_off[ZONES_MAX][12];	/* "UTC-3:30" */
static int n_zones = -1;

static void tz_offset(char *out, const char *rule);

static void zones_read(void)
{
	static char buf[4096];
	char *p, *e;

	if (n_zones >= 0)
		return;
	n_zones = 0;
#ifdef HOME_PREVIEW
	strcpy(buf, "UTC\tUTC0\nEurope/London\tGMT0BST,M3.5.0/1,M10.5.0\n"
		    "Europe/Amsterdam\tCET-1CEST,M3.5.0,M10.5.0/3\nEurope/Brussels\tCET-1CEST,M3.5.0,M10.5.0/3\n"
		    "Europe/Helsinki\tEET-2EEST,M3.5.0/3,M10.5.0/4\nAsia/Kolkata\tIST-5:30\n"
		    "America/St_Johns\tNST3:30NDT,M3.2.0,M11.1.0\nAmerica/New_York\tEST5EDT,M3.2.0,M11.1.0\n"
		    "America/Sao_Paulo\t<-03>3\nAsia/Tokyo\tJST-9\nAustralia/Sydney\tAEST-10AEDT,M10.1.0,M4.1.0/3\n");
#else
	if (read_str(ZONES_FILE, buf, sizeof(buf)) < 0)
		return;
#endif
	for (p = buf; *p && n_zones < ZONES_MAX; p = *e ? e + 1 : e) {
		char *tab = strchr(p, '\t');

		e = strchr(p, '\n');
		if (!e)			/* read_str drops the last newline */
			e = p + strlen(p);
		if (*p != '#' && tab && tab < e && tab - p < (int)sizeof(zone_names[0])) {
			char *z = zone_names[n_zones], *slash, *c;

			memcpy(z, p, (size_t)(tab - p));
			z[tab - p] = 0;
			tz_offset(zone_off[n_zones], tab + 1);	/* stops at the rule's end */
			slash = strrchr(z, '/');
			if (slash && slash - z < (int)sizeof(zone_region[0])) {
				memcpy(zone_region[n_zones], z, (size_t)(slash - z));
				zone_region[n_zones][slash - z] = 0;
			}
			cat(zone_city[n_zones], slash ? slash + 1 : z);
			for (c = zone_city[n_zones]; *c; c++)
				*c = *c == '_' ? ' ' : *c;
			n_zones++;
		}
	}
}

/* the setting timezone: "auto" or a zone; and the one found for auto */
static const char *tz_setting(const char *key)
{
	struct setting *v = settings_find(&SET, key);

	return v ? v->value : "";
}

/* changed while the radio runs: the daemon restarts with them when the
 * LoRa screen is left, not at every arrow press */
static int lora_dirty;

static char app_notice[96];		/* how the last app ended */
static const char *apps_notice;		/* the Bus Pirate did not answer */

static char disk_sel[24];		/* the disk screen's partition (D.fs[].dev) */
static char disk_title[40];
static char led_wiring[64];

static struct row *add(struct list *l, enum kind k, const char *label,
		       const struct wb_icon *ic, enum act a)
{
	static struct row spare;	/* past the end: drawn nowhere */
	struct row *r = l->n < LIST_MAX ? &l->rows[l->n++] : &spare;

	memset(r, 0, sizeof(*r));
	r->kind = k;
	r->label = label;
	r->ic = ic;
	r->act = a;
	return r;
}

static const struct wb_icon *signal_icon(int dbm)
{
	return dbm >= -55 ? &r_sig3 : dbm >= -67 ? &r_sig2 : dbm >= -78 ? &r_sig1 : &r_sig0;
}

static const char *ip_mode(int kind)
{
	return N.conf[kind].is_static ? "Static" : "Automatic";
}

/* An app's ICON, as one of the menu's row icons; the Apps icon if unknown. */
static const struct wb_icon *app_icon(const struct app *ap)
{
	static const struct { const char *n; const struct wb_icon *ic; } icons[] = {
		{ "globe", &r_globe }, { "gameboy", &r_gameboy }, { "games", &r_games },
		{ "terminal", &r_ip }, { "sensors", &r_sensors }, { "bluetooth", &r_bluetooth },
		{ "usb", &r_usb }, { "chip", &r_chip }, { "wifi", &r_wifi }, { "info", &r_info },
		{ "led", &r_led }, { "disk", &r_drive }, { "sdcard", &r_sdcard },
		{ "ethernet", &r_ethernet }, { "palette", &r_palette }, { "power", &r_power },
		{ "bolt", &r_bolt }, { "keyboard", &r_keyboard }, { "sun", &r_sun },
		{ "lora", &r_lora },
	};

	for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
		if (!strcmp(ap->icon, icons[i].n))
			return icons[i].ic;
	return &r_count;
}

/*
 * The installed apps of a category, as list rows, by name. The store's
 * categories (SUBCATEGORY in app.conf) are for browsing the store only.
 */
static void add_apps(struct list *l, int category)
{
	for (int i = 0; i < n_apps; i++)
		if (A[i].cat == category && strcmp(A[i].id, "store")) {
			struct row *r = add(l, R_ACTION, A[i].name, app_icon(&A[i]), A_APP);

			r->idx = i;
			cat(r->value, A[i].info);
		}
}

/* the store (WhyOS-Apps), when installed: its index in A, or -1 */
static int store_app(void)
{
	for (int i = 0; i < n_apps; i++)
		if (!strcmp(A[i].id, "store"))
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Files: a file browser in the lists' style (docs/HOME.md)             */

#define BR_MAX		256
#define BR_SHOWN	20		/* entries in the list at a time */

struct br_ent {
	char name[64];			/* the roots: their label */
	char path[40];			/* the roots: their mount point */
	unsigned long long size;	/* bytes */
	int dir;
	int reg;			/* a regular file: only those open */
	const struct wb_icon *ic;
};

static struct {
	char dir[256];			/* absolute; "" is Drives, above "/" */
	struct br_ent e[BR_MAX];
	int n, more;			/* entries; left out for want of room */
	int sel, off;			/* focused entry; first one in the list */
	int stale;			/* read dir again before showing it */
	char want[64];			/* after reading, focus this name */
	char notice[160];
	int notice_bad;
	char clip[256];			/* Copy/Cut: waiting for Paste */
	int clip_cut;
	char title[64], where[96], free_s[24], tag[96];
	int focus_row;			/* row of e[sel] in the list built last */
	int hidden;			/* show .names (Files' settings: files.hidden) */
	int drive;			/* Drives: the focused row's D.fs index */
} BR;

static int br_drives(void)
{
	return !BR.dir[0];
}

static const char *br_ext(const char *name)
{
	const char *dot = strrchr(name, '.');

	return dot && dot != name ? dot + 1 : "";
}

/* Is ext ("JPG") one of the words in list ("jpg jpeg"); "*" matches all. */
static int ext_in(const char *list, const char *ext)
{
	size_t n = strlen(ext);

	for (const char *t = list; *t; ) {
		size_t k = 0;

		while (t[k] && t[k] != ' ' && t[k] != ',')
			k++;
		if ((k == 1 && *t == '*') || (n && k == n && !strncasecmp(t, ext, n)))
			return 1;
		t += k;
		while (*t == ' ' || *t == ',')
			t++;
	}
	return 0;
}

static const struct wb_icon *br_icon(const char *name, int dir)
{
	const char *x = br_ext(name);

	if (dir)
		return &r_folder;
	if (ext_in("jpg jpeg png gif bmp webp", x))
		return &r_image;
	if (ext_in("html htm svg xhtml", x))
		return &r_globe;
	if (ext_in("pdf epub", x))
		return &r_doc;
	if (ext_in("gb gbc", x))
		return &r_gameboy;
	if (ext_in("txt md conf cfg ini log sh py c h json csv xml yml yaml rc toml lua js css", x))
		return &r_text;
	return &r_file;
}

static int br_cmp(const void *a, const void *b)
{
	const struct br_ent *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;		/* folders first */
	return strcasecmp(x->name, y->name);
}

/* dir/name into out (size out_n); 0 if it does not fit */
static int br_join(char *out, size_t out_n, const char *dir, const char *name)
{
	size_t a = strlen(dir), b = strlen(name);

	if (a == 1 && dir[0] == '/')	/* "/" + "bin", not "//bin" */
		a = 0;
	if (a + 1 + b + 1 > out_n)
		return 0;
	memcpy(out, dir, a);
	out[a] = '/';
	memcpy(out + a + 1, name, b + 1);
	return 1;
}

#ifdef HOME_PREVIEW
/* a plausible folder on the card */
static int br_list_dir(void)
{
	static const struct { const char *n; int dir; unsigned long long size; } fake[] = {
		{ "photos", 1, 0 }, { "roms", 1, 0 }, { "badge", 1, 0 }, { "notes.txt", 0, 1830 },
		{ "Badge schematics.pdf", 0, 2411724 }, { "IMG_2041.jpg", 0, 3874133 },
		{ "index.html", 0, 5120 }, { "camp-map.png", 0, 812345 }, { "config.json", 0, 912 },
		{ "firmware.bin", 0, 1048576 },
	};

	BR.n = BR.more = 0;
	for (unsigned i = 0; i < sizeof(fake) / sizeof(fake[0]); i++) {
		struct br_ent *e = &BR.e[BR.n++];

		memset(e, 0, sizeof(*e));
		cat(e->name, fake[i].n);
		e->dir = fake[i].dir;
		e->reg = !fake[i].dir;
		e->size = fake[i].size;
		e->ic = br_icon(e->name, e->dir);
	}
	qsort(BR.e, (size_t)BR.n, sizeof(BR.e[0]), br_cmp);
	return 0;
}
#else
static int br_list_dir(void)
{
	DIR *d = opendir(BR.dir);
	struct dirent *de;

	BR.n = BR.more = 0;
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		struct br_ent *e = &BR.e[BR.n];
		char path[320];
		struct stat st;

		if ((de->d_name[0] == '.' && (!BR.hidden || !de->d_name[1] ||
					      (de->d_name[1] == '.' && !de->d_name[2]))) ||
		    strlen(de->d_name) >= sizeof(e->name))
			continue;
		if (BR.n == BR_MAX) {
			BR.more++;
			continue;
		}
		memset(e, 0, sizeof(*e));
		cat(e->name, de->d_name);
		if (br_join(path, sizeof(path), BR.dir, de->d_name) && stat(path, &st) == 0) {
			e->dir = S_ISDIR(st.st_mode);
			e->reg = S_ISREG(st.st_mode);
			e->size = (unsigned long long)st.st_size;
		}
		e->ic = br_icon(e->name, e->dir);
		BR.n++;
	}
	closedir(d);
	qsort(BR.e, (size_t)BR.n, sizeof(BR.e[0]), br_cmp);
	return 0;
}
#endif

static void br_read(void)
{
	BR.stale = 0;
	if (br_drives()) {
		BR.n = BR.more = 0;
		return;
	}
	if (br_list_dir() < 0) {
		BR.notice[0] = 0;
		cat(cat(BR.notice, "Cannot open "), BR.dir);
		BR.notice_bad = 1;
	}
	if (BR.want[0]) {
		for (int i = 0; i < BR.n; i++)
			if (!strcmp(BR.e[i].name, BR.want))
				BR.sel = i;
		BR.want[0] = 0;
	}
	if (BR.sel >= BR.n)
		BR.sel = BR.n - 1;
	if (BR.sel < 0)
		BR.sel = 0;
}

/* A file system's name in Files: its disk's ("SD card", "SanDisk Ultra"),
 * with the partition when the disk has more than one. */
static char *fs_name(char *out, const struct fsinfo *f)
{
	const struct diskinfo *dk = fs_disk(f);
	int parts = 0;

	if (!dk)
		return cat(out, f->label);
	for (int i = 0; i < D.n; i++)
		parts += D.fs[i].disk == f->disk;
	out = cat(out, dk->label);
	return parts > 1 ? cat(cat(out, ", "), f->label) : out;
}

/* The mounted file system dir is in (the longest mount point), or 0. */
static const struct fsinfo *br_fs_of(const char *dir)
{
	const struct fsinfo *best = 0;
	size_t bn = 0;

	for (int i = 0; i < D.n; i++) {
		size_t n = strlen(D.fs[i].mnt);

		if (n == 1)	/* "/": everything is in it */
			n = 0;
		if (D.fs[i].mnt[0] && (n > bn || !best) && !strncmp(dir, D.fs[i].mnt, n) &&
		    (dir[n] == '/' || !dir[n])) {
			best = &D.fs[i];
			bn = n;
		}
	}
	return best;
}

/* Go into dir (absolute; "" Drives), focusing want if given. */
static void br_go(const char *dir, const char *want)
{
	memset(BR.dir, 0, sizeof(BR.dir));
	strncpy(BR.dir, dir, sizeof(BR.dir) - 1);
	memset(BR.want, 0, sizeof(BR.want));
	if (want)
		strncpy(BR.want, want, sizeof(BR.want) - 1);
	BR.sel = BR.off = 0;
	BR.drive = -1;
	BR.stale = 1;
}

/* Esc: one level up, on the folder we came from; from "/" to Drives, on
 * the system. 0 when already at Drives. */
static int br_up(void)
{
	char *slash = strrchr(BR.dir, '/');
	char name[64] = "";

	if (br_drives())
		return 0;
	if (!BR.dir[1] || !slash) {
		br_go("", 0);
		for (int i = 0; i < D.n; i++)
			if (!strcmp(D.fs[i].mnt, "/"))
				BR.drive = i;
		return 1;
	}
	strncpy(name, slash + 1, sizeof(name) - 1);
	*slash = 0;
	br_go(BR.dir[0] ? BR.dir : "/", name);
	return 1;
}

/* The mounted file system at path, if path is its mount point. */
static const struct fsinfo *br_mount_at(const char *path)
{
	for (int i = 0; i < D.n; i++)
		if (D.fs[i].mnt[0] && !strcmp(D.fs[i].mnt, path))
			return &D.fs[i];
	return 0;
}

/* Drives (6.10): every disk's file systems, then the badge's own. */
static void br_build_drives(struct list *l)
{
	struct row *r;

	l->title = "Drives";
	add(l, R_GROUP, "REMOVABLE", 0, A_NONE);
	if (!D.n_disk || D.disk[0].usb)
		add(l, R_INFO, "No SD card", &r_sdcard, A_NONE);
	for (int k = 0; k < D.n_disk; k++) {
		const struct diskinfo *dk = &D.disk[k];
		int any = 0;

		for (int i = 0; i < D.n; i++) {
			if (D.fs[i].disk != k)
				continue;
			r = add(l, R_DRIVE, 0, dk->usb ? &r_usbstick : &r_sdcard, A_DRIVE);
			fs_name(r->text, &D.fs[i]);
			r->label = r->text;
			r->idx = i;
			any = 1;
		}
		if (!any) {
			r = add(l, R_INFO, dk->label, dk->usb ? &r_usbstick : &r_sdcard, A_NONE);
			cat(r->value, dk->size ? "No partitions" : "No medium");
		}
	}
	add(l, R_GROUP, "BADGE", 0, A_NONE);
	for (int i = 0; i < D.n; i++)
		if (D.fs[i].disk < 0) {
			r = add(l, R_DRIVE, D.fs[i].label, !strcmp(D.fs[i].mnt, "/tmp") ? &r_chip : &r_drive,
				A_DRIVE);
			r->idx = i;
		}
	if (BR.drive < 0 || BR.drive >= D.n)
		BR.drive = D.n_disk && D.fs[0].disk == 0 ? 0 : D.n - 3;
	BR.focus_row = -1;
	for (int i = 0; i < l->n; i++)
		if (l->rows[i].kind == R_DRIVE && l->rows[i].idx == BR.drive)
			BR.focus_row = i;
}

static void br_build(struct list *l)
{
	struct row *r;
	const struct fsinfo *fs;
	int last;

	if (BR.stale)
		br_read();
	l->eyebrow = "FILES";
	if (BR.notice[0]) {
		l->notice = BR.notice;
		l->notice_bad = BR.notice_bad;
	}
	if (br_drives()) {
		br_build_drives(l);
		return;
	}
	if (BR.clip[0]) {	/* the clipboard, in pink next to the path */
		const char *base = strrchr(BR.clip, '/');

		BR.tag[0] = 0;
		cat(cat(BR.tag, BR.clip_cut ? "Cut: " : "Copied: "), base ? base + 1 : BR.clip);
		l->path_tag = BR.tag;
	}
	{
		const char *base = strrchr(BR.dir, '/');
		char t[64] = "";
		size_t tn, wn = strlen(BR.dir);

		/* a drive's top by its name ("SD card"), else the folder's */
		fs = br_fs_of(BR.dir);
		if (fs && !strcmp(fs->mnt, BR.dir))
			fs_name(t, fs);
		else
			strncpy(t, base && base[1] ? base + 1 : BR.dir, sizeof(t) - 1);
		tn = strlen(t);
		memcpy(BR.title, t, tn + 1);
		/* long paths keep their end */
		if (wn < sizeof(BR.where)) {
			memcpy(BR.where, BR.dir, wn + 1);
		} else {
			memcpy(BR.where, "\xe2\x80\xa6", 3);
			memcpy(BR.where + 3, BR.dir + wn - (sizeof(BR.where) - 4), sizeof(BR.where) - 3);
		}
		l->title = BR.title;
		l->path = BR.where;
		BR.free_s[0] = 0;
		if (fs && fs->size >= fs->used && strcmp(fs->type, "squashfs"))
			cat(human(BR.free_s, fs->size - fs->used), " free");
		else if (fs)
			cat(BR.free_s, "read-only");
		l->title_r = BR.free_s;
	}
	if (BR.more) {
		static char more[64];

		more[0] = 0;
		cat(utoa(more, (unsigned)BR.more, 1), " more not shown: the list holds 256");
		if (!l->notice) {
			l->notice = more;
			l->notice_bad = 1;
		}
	}
	if (BR.n == 0) {
		add(l, R_INFO, "Empty folder", &r_info, A_NONE);
		BR.focus_row = -1;
		return;
	}
	if (BR.sel < BR.off || BR.sel >= BR.off + BR_SHOWN)
		BR.off = BR.sel - BR_SHOWN / 2;
	if (BR.off > BR.n - BR_SHOWN)
		BR.off = BR.n - BR_SHOWN;
	if (BR.off < 0)
		BR.off = 0;
	last = BR.off + BR_SHOWN < BR.n ? BR.off + BR_SHOWN : BR.n;
	/* the scroll bar stands for all entries, not the 20 in the list */
	l->sb_total = BR.n * (56 + 10) - 10;
	l->sb_first = BR.off * (56 + 10);
	for (int i = BR.off; i < last; i++) {
		const struct br_ent *e = &BR.e[i];

		r = add(l, e->dir ? R_ACTION : R_FILE, e->name, e->ic, A_BROWSE);
		r->compact = 1;
		r->idx = i;
		if (i == BR.sel)
			BR.focus_row = l->n - 1;
		if (e->dir) {		/* a mount point: its free space */
			char path[320];
			const struct fsinfo *f = br_join(path, sizeof(path), BR.dir, e->name) ?
						 br_mount_at(path) : 0;

			if (f && f->size > f->used)
				cat(human(r->value, f->size - f->used), " free");
		} else if (e->reg) {
			human(r->value, e->size);
		}
	}
}

/* "SD card, 1 USB drive" and so on */
static void disks_value(char *p)
{
	int n_usb = 0;

	for (int i = 0; i < D.n_disk; i++)
		n_usb += D.disk[i].usb;
	if (D.n_disk > n_usb)
		p = cat(p, n_usb ? "SD card, " : "SD card");
	if (n_usb)
		cat(utoa(p, (unsigned)n_usb, 1), n_usb > 1 ? " USB drives" : " USB drive");
	if (!D.n_disk)
		cat(p, "no SD card");
}

static void build(struct list *l, enum list_id id)
{
	struct ipconf *c;
	struct row *r;
	char *p;

	l->n = 0;
	l->id = id;
	l->notice = 0;
	l->notice_bad = 0;
	l->title_r = l->path = l->path_tag = 0;
	l->sb_total = l->sb_first = 0;
	switch (id) {
	case L_APPS:
		l->eyebrow = "APPS";
		l->title = "Apps";
		l->notice = apps_notice;
		l->notice_bad = 1;
		if (!APP_MODE && store_app() >= 0) {
			r = add(l, R_ACTION, "App store", &r_store, A_STORE);
			cat(r->value, "install apps and games");
		}
		if (HAS_BP) {
			r = add(l, R_ACTION, "Bus Pirate", &r_chip, A_BP);
			cat(r->value, B.present ? "connected" : "not connected");
		}
		if (APP_MODE) {		/* only the Bus Pirate: nothing connected yet */
			l->title = "Bus Pirate";
			break;
		}
		add_apps(l, CAT_APPS);
		break;
	case L_FILES: {
		const struct app *ap = F.app >= 0 && F.app < n_apps ? &A[F.app] : 0;

		l->eyebrow = ap && ap->cat == CAT_GAMES ? "GAMES" : ap && ap->cat == CAT_TOOLS ? "TOOLS" : "APPS";
		l->title = ap ? ap->name : "Files";
		if (app_notice[0]) {
			l->notice = app_notice;
			l->notice_bad = 1;
		}
		if (!ap)
			break;
		if (!F.n) {
			r = add(l, R_INFO, "Nothing to open yet", app_icon(ap), A_NONE);
			r = add(l, R_GROUP, 0, 0, A_NONE);
			cat(cat(cat(r->text, "PUT "), ap->types[0] ? ap->types : "FILES"), " FILES IN");
			for (char *q = r->text; *q; q++)
				*q = *q >= 'a' && *q <= 'z' ? *q - 32 : *q;
			r->label = r->text;
			add(l, R_INFO, ap->files, 0, A_NONE);
		}
		for (int i = 0; i < F.n; i++) {
			r = add(l, R_ACTION, F.name[i], app_icon(ap), A_FILE);
			r->idx = i;
		}
		if (F.more) {
			r = add(l, R_INFO, "More files", &r_info, A_NONE);
			cat(utoa(r->value, (unsigned)F.more, 1), " not shown");
		}
		break;
	}
	case L_BP_PSU:
		l->eyebrow = "BUS PIRATE";
		l->title = "Power supply";
		if (B.notice[0]) {
			l->notice = B.notice;
			l->notice_bad = B.notice_bad;
		}
		r = add(l, R_INFO, "Measured", &r_sensors, A_NONE);
		if (B.psu)
			cat(cat(cat(cat(r->value, B.vout), " out, "), B.iout), " drawn");
		else
			cat(r->value, "Off");
		r = add(l, R_TOGGLE, "Power supply", &r_power, A_PSU);
		r->on = B.psu;
		r = add(l, R_SLIDER, "Voltage", &r_bolt, A_VOLT);
		r->on = (B.dv - 8) * 100 / 42;
		cat(volts(r->value, B.dv), " V");
		r = add(l, R_SLIDER, "Current limit", &r_fuse, A_LIMIT);
		r->on = B.ma * 100 / 500;
		if (B.ma)
			cat(utoa(r->value, (unsigned)B.ma, 1), " mA");
		else
			cat(r->value, "No limit");
		r = add(l, R_TOGGLE, "Pull-up resistors (10k)", &r_pullup, A_PULL);
		r->on = B.pullups;
		break;
	case L_BP_LED: {
		char wires[40];

		l->eyebrow = "BUS PIRATE";
		l->title = "LEDs";
		bp_wiring(wires, sizeof(wires));
		if (B.notice[0]) {
			l->notice = B.notice;
			l->notice_bad = B.notice_bad;
		} else if (B.led_set >= 0 && B.led_set != LT_ONBOARD && wires[0]) {
			cat(cat(led_wiring, "Connect the strip to "), wires);
			l->notice = led_wiring;
		}
		r = add(l, R_ACTION, "Type", &r_led, A_LTYPE);
		cat(r->value, led_types[B.led_type]);
		r = add(l, R_ACTION, "Colour", &r_palette, A_LCOLOR);
		if (B.led_color == CUSTOM)
			hexn(cat(r->value, "#"), B.led_custom, 6);
		else
			cat(r->value, led_colors[B.led_color].name);
		r->swatch = B.led_color == RAINBOW ? 2 : 1;
		r->sw_c = led_color_rgb();
		r = add(l, R_ACTION, "Brightness", &r_sun, A_LBRIGHT);
		cat(utoa(r->value, (unsigned)B.led_pct, 1), "%");
		if (B.led_type != LT_ONBOARD) {
			r = add(l, R_ACTION, "LEDs on the strip", &r_count, A_LCOUNT);
			utoa(r->value, (unsigned)B.led_n, 1);
				r = add(l, R_TOGGLE, "Power supply", &r_power, A_PSU);
			r->on = B.psu;
			}
		r = add(l, R_ACTION, "All off", &r_trash, A_LOFF);
		break;
	}
	case L_GAMES:
		l->eyebrow = "GAMES";
		l->title = "Games";
		l->notice = games_notice;
		l->notice_bad = 1;
		add_apps(l, CAT_GAMES);
		if (l->n == 0)
			add(l, R_INFO, "No games installed", &r_info, A_NONE);
		break;
	case L_BROWSE:
		br_build(l);
		break;
	case L_DISKS:
		l->eyebrow = "SETTINGS";
		l->title = "Disks";
		if (!D.n_disk || D.disk[0].usb) {
			add(l, R_GROUP, "SD CARD", 0, A_NONE);
			add(l, R_INFO, "No SD card", &r_sdcard, A_NONE);
		}
		for (int k = 0; k < D.n_disk; k++) {
			const struct diskinfo *dk = &D.disk[k];
			int any = 0;

			r = add(l, R_GROUP, 0, 0, A_NONE);
			if (dk->usb) {
				p = cat(r->text, "USB: ");
				for (const char *q = dk->label; *q && p < r->text + sizeof(r->text) - 1; q++)
					*p++ = *q >= 'a' && *q <= 'z' ? *q - 32 : *q;
				*p = 0;
			} else {
				cat(r->text, "SD CARD");
			}
			r->label = r->text;
			for (int i = 0; i < D.n; i++) {
				struct fsinfo *f = &D.fs[i];

				if (f->disk != k)
					continue;
				r = add(l, R_USAGE, 0, dk->usb ? &r_usbstick : &r_sdcard, A_DISK);
				cat(cat(cat(cat(r->text, f->label), " ("), f->type), ")");
				r->label = r->text;
				r->idx = i;
				any = 1;
			}
			if (!any)
				add(l, R_INFO, dk->size ? "No partitions" : "No medium",
				    dk->usb ? &r_usbstick : &r_sdcard, A_NONE);
		}
		add(l, R_GROUP, "BADGE", 0, A_NONE);
		for (int i = 0; i < D.n; i++) {
			if (D.fs[i].disk >= 0)
				continue;
			r = add(l, R_USAGE, 0, &r_drive, A_NONE);
			cat(cat(cat(r->text, D.fs[i].label), " "), D.fs[i].mnt);
			r->label = r->text;
			r->idx = i;
		}
		break;
	case L_DISK: {
		int i = fs_find(disk_sel), fat;
		struct fsinfo *f = &D.fs[i < 0 ? 0 : i];
		const struct diskinfo *dk = i < 0 ? 0 : fs_disk(f);

		l->eyebrow = "DISKS";
		l->title = disk_title;
		if (D.notice[0]) {
			l->notice = D.notice;
			l->notice_bad = D.notice_bad;
		}
		if (!dk) {
			add(l, R_INFO, "Removed", &r_info, A_NONE);
			break;
		}
		fat = !strncmp(f->type, "FAT", 3) || !strcmp(f->type, "exFAT");
		r = add(l, R_USAGE, "Space", dk->usb ? &r_usbstick : &r_sdcard, A_NONE);
		r->idx = i;
		r = add(l, R_INFO, "File system", &r_info, A_NONE);
		cat(cat(cat(r->value, f->type), ", "), f->dev + 5);
		r = add(l, R_TOGGLE, 0, &r_drive, A_MOUNT);
		p = cat(r->text, "Mounted at ");
		if (f->mnt[0])
			cat(p, f->mnt);
		else
			mnt_point(f, p);
		r->label = r->text;
		r->on = f->mnt[0] != 0;
		if (D.have_fsck && fat) {
			add(l, R_ACTION, "Check and repair", &r_check, A_FSCK);
		} else {
			r = add(l, R_INFO, "Check and repair", &r_check, A_NONE);
			cat(r->value, fat ? "not installed" : "not FAT");
		}
		r = add(l, R_ACTION, "Format as FAT32", &r_trash, A_FORMAT);
		cat(r->value, dk->usb ? "erases the drive" : "erases the card");
		break;
	}
	case L_SETTINGS:
		l->eyebrow = "SETTINGS";
		l->title = "Settings";
		l->notice = tools_notice;
		l->notice_bad = 1;
		settings_load(&SET);
		r = add(l, R_ACTION, "General", &r_user, A_SUB);
		r->idx = L_SET_GENERAL;
		{
			struct setting *v = settings_find(&SET, "name");

			cat(r->value, v && v->value[0] ? v->value : "Name not set");
		}
		r = add(l, R_ACTION, "Connection", &r_wifi, A_SUB);
		r->idx = L_SET_CONN;
		p = cat(r->value, !N.conf[NK_WIFI].enabled ? "Wi-Fi off" : N.ssid[0] ? N.ssid : "Not connected");
		if (lora_wanted())
			cat(p, ", LoRa");
		r = add(l, R_ACTION, "Display & keyboard", &r_sun, A_SUB);
		r->idx = L_SET_DISPLAY;
		cat(utoa(cat(r->value, "Brightness "), S.bl_max ? (unsigned)((S.bl * 100 + S.bl_max / 2) / S.bl_max) : 0, 1),
		    "%");
		r = add(l, R_ACTION, "Storage & devices", &r_sdcard, A_SUB);
		r->idx = L_SET_STORAGE;
		disks_value(r->value);
		for (int i = 0; i < n_apps; i++)
			if (A[i].cat == CAT_TOOLS) {
				add(l, R_GROUP, "TOOLS", 0, A_NONE);
				add_apps(l, CAT_TOOLS);
				break;
			}
		break;
	case L_SET_GENERAL:
		l->eyebrow = "SETTINGS";
		l->title = "General";
		/* general settings with the menu flag (settings.h); Enter edits */
		settings_load(&SET);
		for (int i = 0; i < SET.n; i++) {
			if (!SET.s[i].menu)
				continue;
			r = add(l, R_ACTION, SET.s[i].label,
				!strcmp(SET.s[i].key, "name") ? &r_user : &r_info, A_SETTING);
			r->idx = i;
			strncpy(r->value, SET.s[i].value[0] ? SET.s[i].value : "Not set",
				sizeof(r->value) - 1);
		}
		{
			const char *tz = tz_setting("timezone"), *found = tz_setting("timezone.auto");

			r = add(l, R_CHOICE, "Time zone", &r_globe, A_TZ);
			if (!tz[0] || !strcmp(tz, "auto"))
				cat(cat(r->value, "Automatic, "), found[0] ? found : "not known yet");
			else
				for (int k = 0; tz[k] && k < (int)sizeof(r->value) - 1; k++)
					r->value[k] = tz[k];	/* zones fit: the longest is 30 */
		}
		break;
	case L_SET_CONN:
		l->eyebrow = "SETTINGS";
		l->title = "Connection";
		settings_load(&SET);
		r = add(l, R_ACTION, "Wi-Fi", &r_wifi, A_NET_WIFI);
		cat(r->value, !N.conf[NK_WIFI].enabled ? "Off" : N.ssid[0] ? N.ssid : "Not connected");
		r = add(l, R_ACTION, "Wired (USB)", &r_ethernet, A_NET_USB);
		cat(r->value, !N.usb_if[0] ? "No adapter" : N.usb_ip[0] ? N.usb_ip :
			      N.usb_link ? "No address" : "No cable");
		r = add(l, R_TOGGLE, "Bluetooth beacon", &r_bluetooth, A_BLE);
		r->on = S.ble_pid > 0;
		r = add(l, R_ACTION, "LoRa", &r_lora, A_SUB);
		r->idx = L_LORA;
		if (lora_wanted()) {
			struct lora_radio lr;
			int dbm, pi;

			lora_radio(&lr, &dbm);
			pi = lora_preset_of(lr.freq_khz, lr.bw_hz, lr.sf, lr.cr);
			cat(utoa(cat(cat(r->value, pi >= 0 ? lora_presets[pi].name : "Custom"), ", "),
				 (unsigned)abs(dbm), 1), " dBm");
		} else {
			cat(r->value, "Off");
		}
		break;
	case L_LORA: {
		struct lora_radio lr;
		int dbm, pi;

		l->eyebrow = "CONNECTION";
		l->title = "LoRa";
		settings_load(&SET);
		lora_radio(&lr, &dbm);
		pi = lora_preset_of(lr.freq_khz, lr.bw_hz, lr.sf, lr.cr);
		if (lora_dirty)
			l->notice = "The radio takes the new settings when you leave this screen";
		else if (dbm > 14 && lr.freq_khz >= 863000 && lr.freq_khz <= 870000 &&
			 (lr.freq_khz < 869400 || lr.freq_khz > 869650))
			l->notice = "Europe allows 14 dBm here (more only at 869.4-869.65 MHz)";
		r = add(l, R_TOGGLE, "LoRa radio", &r_lora, A_LORA);
		r->on = lora_wanted();
		add(l, R_GROUP, "RADIO", 0, A_NONE);
		r = add(l, R_CHOICE, "Preset", &r_sig3, A_LORA_PRESET);
		cat(r->value, pi >= 0 ? lora_presets[pi].name : "Custom");
		r = add(l, R_INFO, "Radio", &r_info, A_NONE);
		p = utoa(r->value, lr.freq_khz / 1000, 1);
		*p++ = '.';
		*p++ = '0' + lr.freq_khz % 1000 / 100;
		*p++ = '0' + lr.freq_khz % 100 / 10;
		*p++ = '0' + lr.freq_khz % 10;
		*p = 0;
		p = utoa(cat(p, " MHz, SF"), (unsigned)lr.sf, 1);
		p = utoa(cat(p, ", BW "), lr.bw_hz / 1000, 1);
		if (lr.bw_hz % 1000)
			p = utoa(cat(p, "."), lr.bw_hz % 1000 / 100, 1);
		utoa(cat(p, ", CR 4/"), (unsigned)lr.cr, 1);
		r = add(l, R_SLIDER, "Transmit power", &r_bolt, A_LORA_POWER);
		r->on = (dbm - LORA_POWER_MIN) * 100 / (LORA_POWER_MAX - LORA_POWER_MIN);
		if (dbm < 0)
			cat(r->value, "-");
		cat(utoa(r->value + strlen(r->value), (unsigned)abs(dbm), 1), " dBm");
		break;
	}
	case L_FILES_SET:
		l->eyebrow = "FILES";
		l->title = "Files settings";
		r = add(l, R_TOGGLE, "Show hidden files", &r_file, A_HIDDEN);
		r->on = BR.hidden;
		r = add(l, R_INFO, "Names that start with a dot", &r_info, A_NONE);
		break;
	case L_SET_DISPLAY:
		l->eyebrow = "SETTINGS";
		l->title = "Display & keyboard";
		r = add(l, R_SLIDER, "Brightness", &r_sun, A_BRIGHT);
		r->on = S.bl_max ? (S.bl * 100 + S.bl_max / 2) / S.bl_max : 0;
		p = utoa(r->value, (unsigned)r->on, 1);
		cat(p, "%");
		r = add(l, R_TOGGLE, "Keyboard light", &r_keyboard, A_KBD);
		r->on = S.kbd > 0;
		break;
	case L_SET_STORAGE:
		l->eyebrow = "SETTINGS";
		l->title = "Storage & devices";
		r = add(l, R_ACTION, "Disks", &r_sdcard, A_DISKS);
		disks_value(r->value);
		if (store_app() >= 0)
			add(l, R_ACTION, "My apps", &r_store, A_MYAPPS);
		r = add(l, R_ACTION, "USB devices", &r_usb, A_USB);
		p = utoa(r->value, (unsigned)S.n_usb, 1);
		cat(p, " connected");
		break;
	case L_NET_WIFI:
		l->eyebrow = "SETTINGS";
		l->title = "Wi-Fi";
		if (time(0) < scanning_until) {
			l->notice = "Scanning\xe2\x80\xa6";
		} else if (N.notice[0]) {
			l->notice = N.notice;
			l->notice_bad = N.notice_bad;
		}
		r = add(l, R_TOGGLE, "Wi-Fi", &r_wifi, A_WIFI_ON);
		r->on = N.conf[NK_WIFI].enabled;
		if (r->on) {
			r = add(l, R_INFO, "Address", &r_info, A_NONE);
			cat(r->value, S.wifi_ip[0] ? S.wifi_ip : "none");
		}
		if (S.wifi_mac[0]) {
			r = add(l, R_INFO, "MAC address", &r_info, A_NONE);
			cat(r->value, S.wifi_mac);
		}
		if (!N.conf[NK_WIFI].enabled)
			break;
		r = add(l, R_ACTION, "IP settings", &r_ip, A_IP);
		cat(r->value, ip_mode(NK_WIFI));
		if (!N.wpa) {
			add(l, R_INFO, "Starting Wi-Fi\xe2\x80\xa6", &r_info, A_NONE);
			break;
		}
		add(l, R_GROUP, "NETWORKS", 0, A_NONE);
		add(l, R_ACTION, "Scan for networks", &r_refresh, A_SCAN);
		for (int i = 0; i < N.n; i++) {
			const struct wnet *w = &N.nets[i];

			r = add(l, R_NETWORK, w->ssid, signal_icon(w->signal), A_NETWORK);
			r->idx = i;
			r->lock = w->secured;
			r->dim = !w->supported && w->saved_id < 0;
			cat(r->value, w->current ? "Connected" :
				      w->saved_id >= 0 ? (w->signal <= -200 ? "Saved, out of range" : "Saved") :
				      r->dim ? "Not supported" : "");
		}
		break;
	case L_NET_USB:
		l->eyebrow = "SETTINGS";
		l->title = "Wired (USB)";
		r = add(l, R_INFO, "Adapter", &r_ethernet, A_NONE);
		cat(r->value, N.usb_if[0] ? N.usb_if : "None on the bottom port");
		if (N.usb_if[0]) {
			r = add(l, R_INFO, "Cable", &r_info, A_NONE);
			cat(r->value, N.usb_link ? "Connected" : "Not connected");
			r = add(l, R_INFO, "Address", &r_info, A_NONE);
			cat(r->value, N.usb_ip[0] ? N.usb_ip : "none");
			if (N.usb_mac[0]) {
				r = add(l, R_INFO, "MAC address", &r_info, A_NONE);
				cat(r->value, N.usb_mac);
			}
		}
		r = add(l, R_ACTION, "IP settings", &r_ip, A_IP);
		cat(r->value, ip_mode(NK_USB));
		break;
	case L_IP_WIFI:
	case L_IP_USB:
		c = &N.conf[id == L_IP_WIFI ? NK_WIFI : NK_USB];
		l->eyebrow = id == L_IP_WIFI ? "WI-FI" : "WIRED (USB)";
		l->title = "IP settings";
		r = add(l, R_TOGGLE, "Automatic (DHCP)", &r_ip, A_DHCP);
		r->on = !c->is_static;
		if (!c->is_static)
			break;
		if (!c->address[0])
			l->notice = "Set an address; until then DHCP is used";
		add(l, R_GROUP, "STATIC", 0, A_NONE);
		r = add(l, R_ACTION, "Address", &r_info, A_ADDR);
		cat(r->value, c->address[0] ? c->address : "not set");
		r = add(l, R_ACTION, "Gateway", &r_info, A_GW);
		cat(r->value, c->gateway[0] ? c->gateway : "none");
		r = add(l, R_ACTION, "DNS servers", &r_info, A_DNS);
		cat(r->value, c->dns[0] ? c->dns : "none");
		break;
	case L_USB:
		l->eyebrow = "SETTINGS";
		l->title = "USB devices";
		if (!S.n_usb)
			add(l, R_INFO, "Nothing on the bottom port", &r_usb, A_NONE);
		for (int i = 0; i < S.n_usb; i++) {
			r = add(l, R_INFO, S.usb[i][0], &r_usb, A_NONE);
			cat(r->value, S.usb[i][1]);
		}
		break;
	}
}

static int focusable(const struct row *r)
{
	return r->kind == R_ACTION || r->kind == R_TOGGLE || r->kind == R_SLIDER ||
	       r->kind == R_NETWORK || r->kind == R_CHOICE || r->kind == R_FILE ||
	       r->kind == R_DRIVE || (r->kind == R_USAGE && r->act != A_NONE);
}

static int row_h(const struct row *r)
{
	return r->kind == R_GROUP ? 17 : r->kind == R_SLIDER || r->kind == R_USAGE ? 98 :
	       r->kind == R_DRIVE ? 104 : r->compact ? 56 : 68;
}

static void toggle(int x, int cy, int on)
{
	int y = cy - 16;

	box(x, y, 56, 32, 16, on ? WB_COLOR_PINK : WB_COLOR_LINE, 255, 0, 0, 0);
	box(on ? x + 56 - 3 - 26 : x + 3, y + 3, 26, 26, 13,
	    on ? WB_COLOR_VOID : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
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

static void draw_row(const struct row *r, int y, int on)
{
	int x = WB_CONTENT_PAD, w = W - 2 * WB_CONTENT_PAD, h = row_h(r);
	int top = r->kind == R_SLIDER || r->kind == R_USAGE || r->kind == R_DRIVE ? y + 18 + 16 : y + h / 2;
	int rx = x + w - 20, lx = x + 20 + (r->ic ? 26 + 16 : 0);
	uint32_t ic_c = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
	char buf[80];

	if (r->kind == R_GROUP) {
		text(&f_mono13, x, y, r->label, WB_COLOR_TEXT_LOW, 21);
		return;
	}
	if (on)
		box_focus(x, y, w, h, WB_RADIUS_MD, &GLOW_ROW);
	else
		box_plain(x, y, w, h, WB_RADIUS_MD, WB_COLOR_SURFACE1);
	if (r->kind == R_NETWORK)	/* the full fan, dimmed, under the lit part */
		icon(&r_wifi, x + 20, top - 13, WB_COLOR_LINE, 255);
	if (r->ic)
		icon(r->ic, x + 20, top - 13, ic_c, 255);

	/* right-hand side first, so the label knows how much room it has */
	switch (r->kind) {
	case R_ACTION:
		icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 20 + 16;
		/* fall through */
	case R_INFO:
	case R_NETWORK:
	case R_FILE:
		if (r->value[0]) {
			rx -= text_w(&f_ui18, r->value, 0);
			text(&f_ui18, rx, text_top(&f_ui18, top), r->value,
			     r->kind == R_NETWORK && !strcmp(r->value, "Connected") ?
			     WB_COLOR_OK : WB_COLOR_TEXT_MID, 0);
			rx -= 16;
		}
		if (r->swatch) {
			swatch(rx - 22, top, r->swatch, r->sw_c);
			rx -= 22 + 14;
		}
		if (r->kind == R_NETWORK && r->lock) {
			icon(&r_lock, rx - 18, top - 9, WB_COLOR_TEXT_LOW, 255);
			rx -= 18 + 12;
		}
		break;
	case R_TOGGLE:
		toggle(rx - 56, top, r->on);
		rx -= 56 + 16;
		break;
	case R_CHOICE:		/* Enter opens the choices: the chevron says so */
		icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 20 + 16;
		rx -= text_w(&f_ui18, r->value, 0);
		text(&f_ui18, rx, text_top(&f_ui18, top), r->value,
		     on ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
		rx -= 12;
		if (r->swatch) {
			swatch(rx - 22, top, r->swatch, r->sw_c);
			rx -= 22 + 14;
		}
		break;
	case R_SLIDER: {
		int tx = x + 24, tw = w - 48, ty = y + 18 + 32 + 18;
		int kx = tx + tw * r->on / 100;
		uint32_t c = on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;

		rx -= text_w(&f_mono18, r->value, 0);
		text(&f_mono18, rx, text_top(&f_mono18, top), r->value, c, 0);
		box(tx, ty, tw, 8, 4, WB_COLOR_LINE, 255, 0, 0, 0);
		box(tx, ty, kx - tx, 8, 4, c, 255, 0, 0, 0);
		if (on)
			box(kx - 12, ty - 8, 24, 24, 12, WB_COLOR_TEXT_HI, 255,
			    WB_COLOR_CYAN, 3, &GLOW_ROW);
		else
			box(kx - 12, ty - 8, 24, 24, 12, WB_COLOR_TEXT_MID, 255, 0, 0, 0);
		rx -= 16;
		break;
	}
	case R_USAGE: {
		const struct fsinfo *f = &D.fs[r->idx];
		int tx = x + 24, tw = w - 48, ty = y + 18 + 32 + 18;
		int pct = f->size ? (int)(f->used * 100 / f->size) : 0;
		/* nearly full is a warning, except for the read-only image */
		uint32_t c = pct >= 90 && strcmp(f->type, "squashfs") ? WB_COLOR_WARN :
			     on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID;
		char v[48], *q = v;

		if (r->act != A_NONE) {
			icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
			rx -= 20 + 16;
		}
		if (f->mnt[0] && f->size) {
			q = human(v, f->used);
			q = cat(q, " of ");
			human(q, f->size);
		} else {
			cat(human(v, f->size), ", not mounted");
		}
		rx -= text_w(&f_ui18, v, 0);
		text(&f_ui18, rx, text_top(&f_ui18, top), v, WB_COLOR_TEXT_MID, 0);
		rx -= 16;
		box(tx, ty, tw, 8, 4, WB_COLOR_LINE, 255, 0, 0, 0);
		if (f->mnt[0] && pct)
			box(tx, ty, tw * pct / 100 < 8 ? 8 : tw * pct / 100, 8, 4, c, 255, 0, 0, 0);
		break;
	}
	case R_DRIVE: {		/* name, path and file system, free space, usage */
		const struct fsinfo *f = &D.fs[r->idx];
		int tx = x + 24, tw = w - 48, ty = y + 104 - 18 - 8;
		int pct = f->size ? (int)(f->used * 100 / f->size) : 0;
		int full = pct >= 90 && strcmp(f->type, "squashfs");
		char v[48];

		icon(&r_chevron, rx - 20, top - 10, WB_COLOR_TEXT_LOW, 255);
		rx -= 20 + 16;
		if (!f->mnt[0])
			cat(v, "Not mounted");
		else if (!strcmp(f->type, "squashfs"))
			cat(v, "Read-only");
		else
			cat(human(v, f->size > f->used ? f->size - f->used : 0), " free");
		rx -= text_w(&f_ui18, v, 0);
		text(&f_ui18, rx, text_top(&f_ui18, top), v, full ? WB_COLOR_WARN : WB_COLOR_TEXT_MID, 0);
		rx -= 16;
		v[0] = 0;
		if (f->mnt[0])
			cat(cat(cat(v, f->mnt), "  "), f->type);
		else
			cat(cat(cat(v, f->dev), "  "), f->type);
		text(&f_mono13, lx, top + 16, v, WB_COLOR_TEXT_LOW, 0);
		box(tx, ty, tw, 8, 4, WB_COLOR_LINE, 255, 0, 0, 0);
		if (f->mnt[0] && pct)
			box(tx, ty, tw * pct / 100 < 8 ? 8 : tw * pct / 100, 8, 4,
			    full ? WB_COLOR_WARN : on ? WB_COLOR_CYAN : WB_COLOR_TEXT_MID, 255, 0, 0, 0);
		break;
	}
	default:
		break;
	}

	if (r->kind == R_INFO && !r->ic)	/* a command to type: data */
		text(&f_mono18, x + 20, text_top(&f_mono18, top), r->label,
		     WB_COLOR_TEXT_HI, 0);
	else
		text(&f_ui22, lx, text_top(&f_ui22, top),
		     fit(&f_ui22, r->label, rx - lx, buf, sizeof(buf)),
		     r->dim ? WB_COLOR_TEXT_LOW : WB_COLOR_TEXT_HI, 0);
}

/* What Enter does on a row, for the hint bar; 0 nothing. */
static const char *enter_word(const struct row *r)
{
	switch (r->kind) {
	case R_TOGGLE:
		return "switch";
	case R_SLIDER:
		return 0;
	case R_CHOICE:
		return "choose";
	case R_NETWORK:
		return N.nets[r->idx].current ? 0 : "connect";
	default:
		break;
	}
	switch (r->act) {
	case A_SCAN:
		return "scan";
	case A_FSCK:
		return "check";
	case A_FORMAT:
		return "format";
	case A_LOFF:
		return "turn off";
	case A_LTYPE: case A_LCOLOR: case A_LBRIGHT: case A_LCOUNT:
		return "change";
	case A_APP: case A_FILE: case A_STORE: case A_MYAPPS: case A_BP:
		return "start";
	default:
		return "open";
	}
}

/* Does the square key open a context menu here? (6.6) */
static int list_menu(const struct list *l, const struct row *r)
{
	if (l->id == L_BROWSE)
		return !br_drives();
	return r && r->kind == R_NETWORK;
}

/*
 * The hint bar (6.8): only what differs per screen. Enter with its word,
 * the square key's options and the triangle and cross the screen uses on
 * the left; Esc on the right. No up/down and none of the circle, cloud
 * and diamond keys: they do the same everywhere, and the help lists them.
 */
static void list_hints(const struct list *l, int focus)
{
	const struct row *r = focus >= 0 ? &l->rows[focus] : 0;
	struct hint h[6];
	int n = 0;
	const char *w = r ? enter_word(r) : 0;

	if (r && r->kind == R_SLIDER)
		h[n++] = (struct hint){ "\xe2\x86\x90 \xe2\x86\x92", "adjust", 0 };
	if (w)
		h[n++] = (struct hint){ "Enter", w, 0 };
	if (list_menu(l, r))
		h[n++] = (struct hint){ "#1", "options", 0 };
	if (l->id == L_BROWSE) {
		h[n++] = (struct hint){ "#2", "/", 0 };
		h[n++] = (struct hint){ "#3", "home", 0 };
	} else if (HAS_BP && l->id == L_BP_PSU) {
		h[n++] = (struct hint){ "#2", B.psu ? "power off" : "power on", 0 };
	}
	h[n++] = (struct hint){ "Esc", l->id == L_BROWSE && !br_drives() ? "up" :
				APP_MODE && l->id == L_APPS ? "close" : "back", 1 };
	hint_bar(h, n);
}

static void draw_list(const struct list *l, int focus)
{
	int ys[LIST_MAX], hs[LIST_MAX], y = 0, top, bottom = H - WB_HINT_BAR_H - 8, scroll, cut;
	char buf[96];

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	top = WB_STATUS_BAR_H + 20;
	eyebrow(WB_CONTENT_PAD, top, l->eyebrow);
	{
		int rw = l->title_r && l->title_r[0] ? text_w(&f_ui20, l->title_r, 0) : 0;

		if (rw)
			text(&f_ui20, W - WB_CONTENT_PAD - rw, top + 18 + 6 + 12, l->title_r,
			     WB_COLOR_TEXT_MID, 0);
		text(&f_ui36b, WB_CONTENT_PAD, top + 18 + 6,
		     fit(&f_ui36b, l->title, W - 2 * WB_CONTENT_PAD - (rw ? rw + 24 : 0), buf, sizeof(buf)),
		     WB_COLOR_TEXT_HI, -11);
	}
	top += 18 + 6 + 44;
	if (l->path) {		/* the full path in mono, the clipboard after it */
		int tw = l->path_tag ? text_w(&f_ui16, l->path_tag, 0) + 24 : 0;
		int x = text(&f_mono14, WB_CONTENT_PAD, top + 6,
			     fit(&f_mono14, l->path, W - 2 * WB_CONTENT_PAD - (tw ? tw + 16 : 0), buf, sizeof(buf)),
			     WB_COLOR_TEXT_LOW, 0);

		if (tw) {
			box(x + 16, top, tw, 28, 14, WB_COLOR_PINK, 36, WB_COLOR_PINK, 1, 0);
			text(&f_ui16, x + 16 + 12, text_top(&f_ui16, top + 14), l->path_tag, WB_COLOR_PINK, 0);
		}
		top += 28;
	}
	if (l->notice) {
		text(&f_ui18, WB_CONTENT_PAD, top + 8,
		     fit(&f_ui18, l->notice, W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     l->notice_bad ? WB_COLOR_ERROR : WB_COLOR_OK, 0);
		top += 8 + 24;
	}
	top += 24;

	/* Lay out, then scroll so the focused row is fully visible. */
	for (int i = 0; i < l->n; i++) {
		if (i && l->rows[i].kind == R_GROUP)
			y += 24 - 10;		/* group gap replaces row gap */
		ys[i] = y;
		hs[i] = row_h(&l->rows[i]);
		y += hs[i] + 10;
	}
	scroll = list_scroll(ys, hs, l->n, focus, focus, bottom - top, &cut);
	/* cards cut off at either end show only their part inside the list */
	clip_rows(scroll ? top : 0, l->n && y - 10 - scroll > bottom - top ? top + cut : H);
	for (int i = 0; i < l->n; i++) {
		int ry = top + ys[i] - scroll;

		if (ry + hs[i] <= top && scroll)
			continue;
		if (ry >= top + cut)
			break;
		draw_row(&l->rows[i], ry, i == focus);
	}
	clip_end();
	/* a narrow scroll bar right of a list that does not fit */
	if (l->sb_total)
		scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, l->sb_total + 10, bottom - top,
			  l->sb_first + scroll);
	else
		scrollbar(W - WB_CONTENT_PAD + 18, top, bottom - top, y, bottom - top, scroll);
	list_hints(l, focus);
}

static int first_focus(const struct list *l)
{
	for (int i = 0; i < l->n; i++)
		if (focusable(&l->rows[i]))
			return i;
	return -1;
}

static int move_focus(const struct list *l, int focus, int dir)
{
	for (int i = focus + dir; i >= 0 && i < l->n; i += dir)
		if (focusable(&l->rows[i]))
			return i;
	return focus;
}

/* ------------------------------------------------------------------ */
/* Text input (Wi-Fi password, addresses) on the badge's keyboard      */

struct input {
	enum act act;		/* what the text is for */
	int kind;		/* NK_WIFI / NK_USB for addresses */
	char ssid[64];		/* A_NETWORK: which network */
	const char *eb;
	char title[96];
	char buf[128];
	int len, secret, show;
	int cur;		/* the cursor, before buf[cur]; the arrows move it */
	int fresh;		/* the value shown is replaced by the first key typed */
	const char *err;
};

/* Rename: where the extension starts (it is grey), or len. */
static int input_ext(const struct input *in)
{
	const char *dot = strrchr(in->buf, '.');

	return in->act == A_RENAME && dot && dot != in->buf ? (int)(dot - in->buf) : in->len;
}

static void draw_input(const struct input *in)
{
	static const struct hint plain[] = {
		{ "Enter", "save", 0 }, { "Esc", "cancel", 1 },
	};
	static const struct hint start[] = {
		{ "Enter", "start", 0 }, { "Esc", "cancel", 1 },
	};
	static const struct hint secret[] = {
		{ "Enter", "connect", 0 }, { "Tab", "show", 0 }, { "Esc", "cancel", 1 },
	};
	static const struct hint secret_shown[] = {
		{ "Enter", "connect", 0 }, { "Tab", "hide", 0 }, { "Esc", "cancel", 1 },
	};
	int fx = DLG_X + 32, fw = DLG_W - 64, fy, tx, max = fw - 40, ext = input_ext(in), from = 0, cx;
	char shown[3 * sizeof(in->buf) + 1], *p = shown;
	int unit = in->secret && !in->show ? 3 : 1;	/* bytes per character shown */

	fy = dialog(in->eb, in->title, 56 + (in->err ? 34 : 0), 0, 0, 0, 0, -1);
	box_focus(fx, fy, fw, 56, WB_RADIUS_MD, &GLOW_ROW);
	if (unit == 3) {
		for (int i = 0; i < in->len; i++)
			p = cat(p, "\xe2\x80\xa2");
	} else {
		cat(shown, in->buf);
	}
	/* scroll so the cursor shows: drop characters off the front */
	for (;;) {
		char c = shown[in->cur * unit];

		shown[in->cur * unit] = 0;
		cx = text_w(&f_mono18, shown + from * unit, 0);
		shown[in->cur * unit] = c;
		if (cx <= max || from >= in->cur)
			break;
		from++;
	}
	/* the name, then the extension in grey (Rename) */
	{
		char c = shown[ext * unit];
		int e = ext > from ? ext : from;

		c = shown[e * unit];
		shown[e * unit] = 0;
		tx = text(&f_mono18, fx + 20, text_top(&f_mono18, fy + 28), shown + from * unit, WB_COLOR_TEXT_HI, 0);
		shown[e * unit] = c;
		text(&f_mono18, tx, text_top(&f_mono18, fy + 28), shown + e * unit, WB_COLOR_TEXT_LOW, 0);
	}
	fill(fx + 20 + cx + 1, fy + 16, 2, 24, WB_COLOR_CYAN);
	if (in->err)
		text(&f_ui18, fx, fy + 56 + 10, in->err, WB_COLOR_ERROR, 0);
	if (in->secret)
		hint_bar(in->show ? secret_shown : secret, 3);
	else
		hint_bar(in->act == A_ASK ? start : plain, 2);
}

/*
 * A pop-up list (6.6): the square key's context menu, or the choices of an
 * enumeration (LED type and colour, time zone, LoRa preset), drawn by
 * wbdraw.h's menu_popup. A long one filters as you type.
 */
#define PICK_MAX	(ZONES_MAX + 1)

struct pick {
	enum act act;			/* what is chosen */
	const char *eb, *title;
	int n, focus, cur;		/* focus, cur: in the shown (filtered) list */
	struct menu_item it[PICK_MAX];
	int map[PICK_MAX], shown;	/* the items that match the filter */
	int cur_item;			/* the present choice, an index into it[]; -1 none */
	int filterable;
	char filter[32];
	char count[24];
};

static struct pick P;

/* Is needle in hay, ignoring case? */
static int has_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return !n;
}

/* Apply the filter; the focus stays on the same item when it still shows. */
static void pick_filter(void)
{
	int was = P.shown && P.focus >= 0 && P.focus < P.shown ? P.map[P.focus] : P.cur_item;

	P.shown = 0;
	P.focus = P.cur = -1;
	for (int i = 0; i < P.n; i++) {
		const struct menu_item *it = &P.it[i];

		/* the label, or the small word after it (a zone's region); not
		 * Automatic's, which names the zone it found */
		if (P.filter[0] && !has_ci(it->label, P.filter) &&
		    !(it->sub && !(P.act == A_TZ && i == 0) && has_ci(it->sub, P.filter)))
			continue;
		if (i == was)
			P.focus = P.shown;
		if (i == P.cur_item)
			P.cur = P.shown;
		P.map[P.shown++] = i;
	}
	if (P.focus < 0)
		P.focus = 0;
}

/* The item chosen in the pop-up: an index into P.it, or -1. */
static int pick_item(void)
{
	return P.focus >= 0 && P.focus < P.shown ? P.map[P.focus] : -1;
}

/* Start a pop-up: n items in P.it, cur the present one (-1 none). */
static void pick_open(enum act a, const char *eb, const char *title, int cur)
{
	P.act = a;
	P.eb = eb;
	P.title = title;
	P.cur_item = cur;
	P.filter[0] = 0;
	P.shown = 0;
	P.focus = -1;
	P.count[0] = 0;
	P.filterable = P.n > 8;		/* "more than about eight": typing filters */
	if (P.filterable)
		cat(utoa(P.count, (unsigned)P.n, 1), " options");
	pick_filter();
}

static struct menu_item *pick_add(const char *label)
{
	struct menu_item *it = &P.it[P.n < PICK_MAX ? P.n++ : PICK_MAX - 1];

	memset(it, 0, sizeof(*it));
	it->label = label;
	return it;
}

static void pick_reset(void)
{
	memset(&P, 0, sizeof(P));
}

static void draw_pick(void)
{
	static const struct hint menu_hints[] = {
		{ "Enter", "choose", 0 }, { "Esc", "cancel", 1 },
	};
	static const struct hint filter_hints[] = {
		{ "Enter", "choose", 0 }, { "Backspace", "erase", 0 }, { "Esc", "cancel", 1 },
	};

	menu_popup(P.eb, P.title, P.count[0] ? P.count : 0, P.it, P.map, P.shown, P.focus, P.cur,
		   P.filterable ? P.filter : 0, P.act == A_TZ ? "Type a city to filter" : "Type to filter",
		   P.filterable ? filter_hints : menu_hints, P.filterable ? 3 : 2);
}

/* Fill P for the LED type or colour. */
static void pick_led(enum act a)
{
	pick_reset();
	if (a == A_LTYPE) {
		for (int i = 0; i < 3; i++)
			pick_add(led_types[i]);
		pick_open(a, "LEDS", "LED type", B.led_type);
		return;
	}
	for (int i = 0; i < N_COLORS; i++) {
		struct menu_item *it = pick_add(led_colors[i].name);

		it->swatch = i == RAINBOW ? 2 : 1;
		it->sw_c = led_colors[i].rgb;
	}
	pick_add("Custom (RRGGBB)\xe2\x80\xa6")->swatch = 1;
	P.it[CUSTOM].sw_c = B.led_custom;
	pick_open(a, "LEDS", "Colour", B.led_color);
	P.filterable = 0;
	P.count[0] = 0;
}

/* "UTC+5:30" from a POSIX TZ rule's standard offset ("IST-5:30", "<-03>3"). */
static void tz_offset(char *out, const char *rule)
{
	const char *p = rule;
	int sign = 1, h = 0, m = 0;

	if (*p == '<') {
		while (*p && *p != '>')
			p++;
		if (*p)
			p++;
	} else {
		while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
			p++;
	}
	if (*p == '-' || *p == '+')
		sign = *p++ == '-' ? -1 : 1;
	while (*p >= '0' && *p <= '9')
		h = h * 10 + *p++ - '0';
	if (*p == ':')
		for (p++; *p >= '0' && *p <= '9'; p++)
			m = m * 10 + *p - '0';
	/* POSIX counts west as positive: UTC+1 is "CET-1" */
	out = cat(out, "UTC");
	if (!h && !m)
		return;
	*out++ = sign < 0 ? '+' : '-';
	out = utoa(out, (unsigned)h, 1);
	if (m)
		utoa(cat(out, ":"), (unsigned)m, 2);
}

/* Time zone: Automatic, then the zones; each with its offset from UTC. */
static void pick_tz(void)
{
	static char found[40];
	const char *tz;
	int cur = 0;
	struct menu_item *it;

	settings_load(&SET);
	zones_read();
	tz = tz_setting("timezone");
	pick_reset();
	it = pick_add("Automatic");
	found[0] = 0;
	cat(found, tz_setting("timezone.auto")[0] ? tz_setting("timezone.auto") : "from the network");
	it->sub = found;
	for (int k = 0; k < n_zones; k++) {
		it = pick_add(zone_city[k]);
		it->sub = zone_region[k][0] ? zone_region[k] : 0;
		cat(it->value, zone_off[k]);
		if (!strcmp(zone_names[k], tz))
			cur = k + 1;
	}
	pick_open(A_TZ, "GENERAL", "Time zone", cur);
}

/* LoRa preset: the presets of lora_presets.h, with their frequency. */
static void pick_preset(void)
{
	struct lora_radio lr;
	int dbm;

	settings_load(&SET);
	lora_radio(&lr, &dbm);
	pick_reset();
	for (int i = 0; i < LORA_N_PRESETS; i++) {
		struct menu_item *it = pick_add(lora_presets[i].name);
		char *p = utoa(it->value, lora_presets[i].freq_khz / 1000, 1);

		*p++ = '.';
		utoa(p, lora_presets[i].freq_khz % 1000, 3);
		it->sub = 0;
	}
	pick_open(A_LORA_PRESET, "LORA", "Preset", lora_preset_of(lr.freq_khz, lr.bw_hz, lr.sf, lr.cr));
}

/*
 * Turning LoRa on: sending without an antenna can damage the radio for good
 * (Docs/03-veiligheid.md 3.5), so ask first. Cancel has the focus by
 * default. Turning it off asks nothing.
 */
static void draw_lora_confirm(int focus)
{
	static const char *const labels[2] = { "Cancel", "Antenna connected, turn on" };
	static const struct wb_icon *const ics[2] = { &r_chevron, &r_lora };
	static const char *const lines[] = {
		"Once on, the radio can transmit at any time.",
		"Without an antenna on the SMA connector",
		"that can damage the LoRa chip for good.",
	};
	int y = dialog("LORA", "Is the antenna connected?", 3 * 30, labels, ics, 2, focus, -1);

	for (int i = 0; i < 3; i++)
		text(&f_ui20, DLG_X + 32, y + i * 30, lines[i], WB_COLOR_TEXT_MID, 0);
	hint_bar(choice_hints, 3);
}

/* Formatting erases the disk: Cancel has the focus by default. */
static void draw_format_confirm(int focus)
{
	static const char *const labels[2] = { "Cancel", "Erase and format" };
	static const struct wb_icon *const ics[2] = { &r_chevron, &r_trash };
	int i = fs_find(disk_sel);
	const struct diskinfo *dk = i < 0 ? 0 : fs_disk(&D.fs[i]);
	int usb = dk && dk->usb;

	dialog("DISKS", usb ? "Erase the USB drive?" : "Erase the SD card?", 0, labels, ics, 2, focus, 1);
	hint_bar(choice_hints, 3);
}

/* ------------------------------------------------------------------ */
/* Bus Pirate screens                                                  */

/*
 * The Bus Pirate's tiles follow the keys' rules (6.6): the triangle and
 * cross for the screens used most, the circle for its console (its log),
 * the cloud for Pins (its configuration); LEDs and Info open with Enter.
 * Tiles without a key are drawn in the neutral colour.
 */
static const struct tile bp_tiles[6] = {
	{ "Power supply",   &i_bolt,     &k_triangle, WB_COLOR_KEY_TRIANGLE },
	{ "Logic analyzer", &i_logic,    &k_cross,    WB_COLOR_KEY_CROSS },
	{ "LEDs",           &i_led,      0,           WB_COLOR_TEXT_MID },
	{ "Console",        &i_terminal, &k_circle,   WB_COLOR_KEY_CIRCLE },
	{ "Pins",           &i_gauge,    &k_cloud,    WB_COLOR_KEY_CLOUD },
	{ "Info",           &i_info,     0,           WB_COLOR_TEXT_MID },
};

static void draw_bp(int focus)
{
	char sub[6][32], line[64], *p;
	const char *subs[6];
	uint32_t c[6];

	for (int i = 0; i < 6; i++) {
		subs[i] = sub[i];
		sub[i][0] = 0;
		c[i] = WB_COLOR_TEXT_LOW;
	}
	if (B.psu) {
		cat(cat(cat(sub[0], B.vout), ", "), B.iout);
		c[0] = WB_COLOR_PINK;
	} else {
		cat(sub[0], "Off");
	}
	if (LA.n)
		cat(utoa(sub[1], (unsigned)LA.n, 1), " samples");
	else
		cat(sub[1], B.sump ? "8 channels" : "binmode is not SUMP");
	cat(sub[2], B.led_set >= 0 ? led_types[B.led_set] : "Off");
	if (B.led_set >= 0)
		c[2] = WB_COLOR_PINK;
	cat(cat(sub[3], B.mode), " mode");
	cat(sub[4], B.pullups ? "Pull-ups on" : "Voltages");
	if (B.pullups)
		c[4] = WB_COLOR_PINK;
	p = cat(sub[5], "Firmware ");
	for (const char *q = B.fw; *q && *q != ' ' && p < sub[5] + 30; q++)
		*p++ = *q;
	*p = 0;
	cat(cat(line, B.board[0] ? B.board : "Bus Pirate"), " on the bottom port");
	{
		static const struct hint hints[] = { { "Enter", "open", 0 }, { "Esc", "close", 1 } };
		static const struct hint back[] = { { "Enter", "open", 0 }, { "Esc", "back", 1 } };

		draw_tiles("BUS PIRATE", line, bp_tiles, subs, c, focus, APP_MODE ? hints : back, 2);
	}
}

/* Title block of the Bus Pirate's own screens; returns the y below it. */
static int bp_header(const char *title)
{
	int top = WB_STATUS_BAR_H + 20;

	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	eyebrow(WB_CONTENT_PAD, top, "BUS PIRATE");
	text(&f_ui36b, WB_CONTENT_PAD, top + 18 + 6, title, WB_COLOR_TEXT_HI, -11);
	return top + 18 + 6 + 44;
}

/* A setting as a chip, with the symbol key that acts on it (0: none);
 * returns its right edge. */
static int chip(int x, int y, int key, const char *s, int lit)
{
	int w = (key ? 14 + 8 : 0) + text_w(&f_ui16, s, 0) + 24;

	box(x, y, w, 36, WB_RADIUS_SM, lit ? WB_COLOR_SURFACE2 : WB_COLOR_SURFACE1, 255,
	    lit ? WB_COLOR_PINK : WB_COLOR_LINE, 1, 0);
	if (key)
		icon(tiles[key - 1].key, x + 12, y + 11, tiles[key - 1].color, 255);
	text(&f_ui16, x + 12 + (key ? 14 + 8 : 0), text_top(&f_ui16, y + 18), s, WB_COLOR_TEXT_HI, 0);
	return x + w;
}

static int la_span(void)	/* samples on screen */
{
	return LA.zoom >= 0 ? LA_W << LA.zoom : LA_W >> -LA.zoom;
}

/* Time of sample s, from the start (the trigger, if any). */
static long long la_ns(int s)
{
	return LA.rate ? (long long)s * 1000000000LL / (long long)LA.rate : 0;
}

static void draw_la(void)
{
	/* 6.6: triangle the main action, cross the second; the capture
	 * settings are options (square); the arrows scroll and zoom */
	static const struct hint hints[] = {
		{ "#2", "capture", 0 }, { "#3", "save", 0 }, { "#1", "options", 0 },
		{ "\xe2\x86\x90\xe2\x86\x92", "scroll", 0 }, { "\xe2\x86\x91\xe2\x86\x93", "zoom", 0 },
		{ "Esc", "back", 1 },
	};
	int y = bp_header("Logic analyzer") + 4, x = WB_CONTENT_PAD;
	char buf[48], *p;

	x = chip(x, y, 2, "Capture", 0) + 8;
	x = chip(x, y, 3, "Save", 0) + 8;
	hz(buf, la_rates[LA.rate_i]);
	x = chip(x, y, 0, buf, 0) + 8;
	buf[0] = 0;
	if (LA.trig < 0)
		cat(buf, "No trigger");
	else
		utoa(cat(buf, "IO"), (unsigned)LA.trig, 1);
	x = chip(x, y, 0, buf, LA.trig >= 0) + 8;
	x = chip(x, y, 0, LA.falling ? "Falling" : "Rising", 0) + 8;
	p = utoa(buf, (unsigned)la_sizes[LA.size_i] / 1024, 1);
	cat(p, "k");
	chip(x, y, 0, buf, 0);

	if (LA.status[0])
		text(&f_ui16, WB_CONTENT_PAD, y + 36 + 12, LA.status,
		     LA.status_bad ? WB_COLOR_ERROR : WB_COLOR_TEXT_MID, 0);

	for (int ch = 0; ch < BP_PINS; ch++) {
		int ly = LA_TOP + ch * LA_LANE, yh = ly + 8, yl = ly + LA_LANE - 12;
		int prev = -1;
		char name[8];

		utoa(cat(name, "IO"), (unsigned)ch, 1);
		text(&f_mono13, WB_CONTENT_PAD, text_top(&f_mono13, (yh + yl) / 2), name,
		     ch == LA.trig ? WB_COLOR_PINK : WB_COLOR_TEXT_MID, 0);
		for (int px = 0; px < LA_W && LA.n; px++) {
			int s0 = LA.pos + (LA.zoom >= 0 ? px << LA.zoom : px >> -LA.zoom);
			int s1 = LA.zoom >= 0 ? s0 + (1 << LA.zoom) : s0 + 1;
			int hi = 0, lo = 0, first;

			if (s0 >= LA.n)
				break;
			if (s1 > LA.n)
				s1 = LA.n;
			first = la_buf[s0] >> ch & 1;
			for (int i = s0; i < s1; i++) {
				if (la_buf[i] >> ch & 1)
					hi = 1;
				else
					lo = 1;
			}
			if ((hi && lo) || (prev >= 0 && prev != first))
				fill(LA_X0 + px, yh, 1, yl - yh + 2, WB_COLOR_CYAN);
			if (hi)
				fill(LA_X0 + px, yh, 1, 2, WB_COLOR_CYAN);
			if (lo)
				fill(LA_X0 + px, yl, 1, 2, WB_COLOR_CYAN);
			prev = la_buf[s1 - 1] >> ch & 1;
		}
	}
	if (!LA.n) {
		text(&f_ui18, LA_X0 + 16, text_top(&f_ui18, LA_TOP + 4 * LA_LANE - LA_LANE / 2),
		     B.sump ? "No capture yet" : "Set the Bus Pirate's binmode to SUMP (Console: binmode)",
		     WB_COLOR_TEXT_LOW, 0);
	} else {
		int span = la_span(), ay = LA_TOP + BP_PINS * LA_LANE + 6;

		dur(buf, la_ns(LA.pos));
		text(&f_mono13, LA_X0, ay, buf, WB_COLOR_TEXT_MID, 0);
		dur(buf, la_ns(LA.pos + span < LA.n ? LA.pos + span : LA.n));
		text(&f_mono13, W - WB_CONTENT_PAD - text_w(&f_mono13, buf, 0), ay, buf,
		     WB_COLOR_TEXT_MID, 0);
		p = buf;
		if (LA.zoom >= 0)
			p = cat(utoa(p, 1u << LA.zoom, 1), LA.zoom ? " samples/px" : " sample/px");
		else
			p = cat(utoa(p, 1u << -LA.zoom, 1), " px/sample");
		text(&f_mono13, (W - text_w(&f_mono13, buf, 0)) / 2, ay, buf, WB_COLOR_TEXT_LOW, 0);
	}
	hint_bar(hints, 6);
}

static void draw_pins(void)
{
	static const struct hint hints[] = {
		{ "#2", "power supply", 0 }, { "#3", "pull-ups", 0 }, { "Esc", "back", 1 },
	};
	int top = bp_header("Pins"), cw = (W - 2 * WB_CONTENT_PAD - 2 * 16) / 3, ch = 118;
	char line[96], *p;

	p = cat(cat(cat(line, B.mode), " mode, power supply "), B.psu ? "on" : "off");
	cat(cat(p, ", pull-ups "), B.pullups ? "on" : "off");
	text(&f_ui18, WB_CONTENT_PAD, top + 8, line, WB_COLOR_TEXT_MID, 0);
	top += 8 + 24 + 20;
	for (int i = 0; i < 1 + BP_PINS; i++) {
		int x = WB_CONTENT_PAD + (i % 3) * (cw + 16), y = top + (i / 3) * (ch + 14);
		const char *v = i ? B.pin_v[i - 1] : B.vout;
		const char *f = i ? B.pin_f[i - 1] : B.psu ? B.iout : "off";
		char name[8], val[12], *q = val;

		box_plain(x, y, cw, ch, WB_RADIUS_MD, WB_COLOR_SURFACE1);
		if (i)
			utoa(cat(name, "IO"), (unsigned)(i - 1), 1);
		else
			cat(name, "VOUT");
		text(&f_mono14, x + 18, y + 16, name, WB_COLOR_CYAN, 22);
		/* "3.3V" -> "3.3 V" */
		for (const char *s = v; *s && q < val + 9; s++) {
			if (*s == 'V' && q > val)
				*q++ = ' ';
			*q++ = *s;
		}
		*q = 0;
		text(&f_ui36b, x + 18, y + 38, val[0] ? val : "\xe2\x80\xa6",
		     i == 0 && B.psu ? WB_COLOR_PINK : WB_COLOR_TEXT_HI, -11);
		if (f[0] && strcmp(f, "-"))
			text(&f_ui16, x + 18, y + 88, f, WB_COLOR_TEXT_LOW, 0);
	}
	hint_bar(hints, 3);
}

static void draw_bpinfo(void)
{
	static const struct hint hints[] = { { "Esc", "back", 1 } };
	int y = bp_header("Info") + 8;
	char buf[80];

	for (int i = 0; i < B.n_info && y < H - WB_HINT_BAR_H - 30; i++, y += 30)
		text(&f_ui18, WB_CONTENT_PAD, y,
		     fit(&f_ui18, B.info[i], W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     i == 0 ? WB_COLOR_TEXT_HI : WB_COLOR_TEXT_MID, 0);
	hint_bar(hints, 1);
}

enum screen { SC_HOME, SC_POWER, SC_LIST, SC_INPUT, SC_FORMAT,
	      SC_BP, SC_LA, SC_PINS, SC_BPINFO,	/* the Bus Pirate's tiles and screens */
	      SC_PICK,			/* a pop-up list (struct pick) */
	      SC_CONFIRM,		/* Files: delete? */
	      SC_LORA,			/* LoRa on: antenna connected? */
	      SC_LOG,			/* the circle key: a log */
	      SC_HELP };		/* the diamond key: help over the screen */

/* ---- the circle key: a log; the diamond key: help (6.6) ---------------- */

#define LOG_LINES	200
#define LOG_ROW		24
#define LOG_TOP		(WB_STATUS_BAR_H + 20 + 18 + 6 + 44 + 24)
#define LOG_ROWS	((H - WB_HINT_BAR_H - 8 - LOG_TOP) / LOG_ROW)
static char log_buf[16384];
static const char *log_line[LOG_LINES];
static int log_n, log_top;		/* lines; the first one shown */
static const char *log_eb, *log_title;
static const char *log_none = "No log yet.";	/* the line when there is none */

/* Read the end of path for the log screen; no file or an empty one
 * shows "No log yet." */
static void log_open(const char *path, const char *eb, const char *title)
{
	int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1, n = 0;
	off_t end = 0;
	char *p;

	log_eb = eb;
	log_title = title;
	log_none = "No log yet.";
	log_n = log_top = 0;
	log_buf[0] = 0;
	if (fd >= 0) {
		end = lseek(fd, 0, SEEK_END);
		lseek(fd, end > (off_t)sizeof(log_buf) - 1 ? end - (off_t)sizeof(log_buf) + 1 : 0, SEEK_SET);
		n = (int)read(fd, log_buf, sizeof(log_buf) - 1);
		close(fd);
		log_buf[n > 0 ? n : 0] = 0;
	}
	p = log_buf;
	if (n > 0 && end > n)		/* started mid-line: drop that part */
		while (*p && *p != '\n')
			p++;
	while (*p) {
		char *e = p;

		while (*e && *e != '\n')
			e++;
		if (e > p) {
			for (char *q = p; q < e; q++)	/* tabs, ANSI and such */
				if ((unsigned char)*q < 0x20)
					*q = ' ';
			if (log_n == LOG_LINES) {	/* keep the last ones */
				memmove(log_line, log_line + 1, sizeof(log_line) - sizeof(log_line[0]));
				log_n--;
			}
			log_line[log_n++] = p;
		}
		if (!*e)
			break;
		*e = 0;
		p = e + 1;
	}
	log_top = log_n > LOG_ROWS ? log_n - LOG_ROWS : 0;	/* the newest in view */
}

static void draw_log(void)
{
	static const struct hint hints[] = {
		{ "\xe2\x86\x91\xe2\x86\x93", "scroll", 0 }, { "Esc", "back", 1 },
	};
	char buf[120];

	if (!log_n) {
		empty_screen(log_eb, log_title, log_none);
		status_bar();
		return;
	}
	fill(0, 0, W, H, WB_COLOR_VOID);
	status_bar();
	eyebrow(WB_CONTENT_PAD, WB_STATUS_BAR_H + 20, log_eb);
	text(&f_ui36b, WB_CONTENT_PAD, WB_STATUS_BAR_H + 20 + 18 + 6, log_title, WB_COLOR_TEXT_HI, -11);
	for (int i = 0; i < LOG_ROWS && log_top + i < log_n; i++)
		text(&f_mono14, WB_CONTENT_PAD, LOG_TOP + i * LOG_ROW,
		     fit(&f_mono14, log_line[log_top + i], W - 2 * WB_CONTENT_PAD, buf, sizeof(buf)),
		     WB_COLOR_TEXT_MID, 0);
	scrollbar(W - WB_CONTENT_PAD + 18, LOG_TOP, LOG_ROWS * LOG_ROW, log_n, LOG_ROWS, log_top);
	if (log_n > LOG_ROWS)
		hint_bar(hints, 2);
	else
		hint_bar(hints + 1, 1);
}

static char last_app[64];	/* the directory of the app that ran last */

/* The log behind the circle key on a menu screen. */
static void log_for(enum list_id id)
{
	static char app_log[96], title[48];

	switch (id) {
	case L_BROWSE:
	case L_FILES_SET:
		log_open("/tmp/files.log", "FILES", "Copy, move and delete");
		break;
	case L_DISKS:
	case L_DISK:
		log_open("/tmp/fsck.log", "DISKS", "Check and repair");
		break;
	case L_LORA:
		log_open("/tmp/lora.log", "LORA", "LoRa radio");
		break;
	case L_APPS:
	case L_GAMES:
	case L_FILES:
		if (!last_app[0]) {
			log_open(0, id == L_GAMES ? "GAMES" : "APPS", "No app has run yet");
			break;
		}
		app_log[0] = title[0] = 0;
		cat(cat(cat(app_log, "/tmp/app-"), last_app), ".log");
		for (int i = 0; i < n_apps; i++)
			if (strrchr(A[i].dir, '/') && !strcmp(strrchr(A[i].dir, '/') + 1, last_app))
				strncpy(title, A[i].name, sizeof(title) - 1);
		log_open(app_log, "LAST APP", title[0] ? title : last_app);
		break;
	default:
		log_open(0, "SETTINGS", "Log");
		break;
	}
}

/* Help (the diamond key) for a menu screen: what it is for and its keys. */
static struct help_key help_keys[14];
static int help_n;

static void hk(const char *keys, const char *what)
{
	if (help_n < (int)(sizeof(help_keys) / sizeof(help_keys[0])))
		help_keys[help_n++] = (struct help_key){ keys, what };
}

static const char *list_about(enum list_id id)
{
	switch (id) {
	case L_APPS: return "The apps on the badge and the SD card. Get more in the App store.";
	case L_GAMES: return "The games on the badge and the SD card.";
	case L_FILES: return "The files this app opens, from the folder it looks in.";
	case L_BROWSE: return br_drives() ? "All storage: the SD card, USB drives and the badge's own. "
					    "Enter mounts a drive when needed and opens it." :
				       "Every file on the badge, as it is. Enter opens a folder, or a file "
				       "with the app that opens it.";
	case L_FILES_SET: return "How Files shows folders.";
	case L_SETTINGS: return "The badge's settings, by subject.";
	case L_SET_GENERAL: return "Your name, shown on the home screen, and the time zone of the clock.";
	case L_SET_CONN: return "Wi-Fi, a network adapter on the bottom port, the Bluetooth beacon and LoRa.";
	case L_SET_DISPLAY: return "The screen's brightness and the keyboard light.";
	case L_SET_STORAGE: return "Disks, the apps from the store and what is on the bottom USB port.";
	case L_NET_WIFI: return "Wi-Fi networks in range. Enter connects; the options forget a saved one.";
	case L_NET_USB: return "A USB network adapter on the bottom port.";
	case L_IP_WIFI:
	case L_IP_USB: return "How this connection gets its address: automatically (DHCP) or fixed.";
	case L_DISKS: return "The SD card, USB drives and the badge's own storage, and how full they are.";
	case L_DISK: return "Mount, check or format this drive. Formatting erases everything on it.";
	case L_USB: return "What is connected to the bottom USB port.";
	case L_LORA: return "The LoRa radio. Turn it on only with the antenna connected.";
	case L_BP_PSU: return "The Bus Pirate's power supply on Vout, its current limit and pull-ups.";
	case L_BP_LED: return "An LED strip on the Bus Pirate. Every change is sent at once.";
	}
	return 0;
}

/* The keys every screen has, after its own. */
static void hk_fixed(const char *esc)
{
	hk("#4", "Log of this screen or app");
	hk("#5", "Settings");
	hk("#6", "This help");
	hk("Esc", esc);
	hk("Logo", "Home screen");
}

static void help_list(const struct list *l, int focus)
{
	const struct row *r = focus >= 0 ? &l->rows[focus] : 0;
	int slider = 0;

	help_n = 0;
	for (int i = 0; i < l->n; i++)
		slider |= l->rows[i].kind == R_SLIDER;
	if (r) {
		hk("\xe2\x86\x91\xe2\x86\x93", "Choose a row");
		hk("Enter", l->id == L_BROWSE ? "Open" : "Open, switch or choose: what the row says");
		if (!(l->id == L_BROWSE && br_drives()))
			hk("\xe2\x86\x90\xe2\x86\x92", slider ? "Adjust a slider; elsewhere a page up or down" :
							      "A page up or down");
	}
	if (list_menu(l, r))
		hk("#1", l->id == L_BROWSE ? "Options: open with, rename, copy, delete\xe2\x80\xa6" :
					     "Options: connect, forget");
	if (l->id == L_BROWSE) {
		hk("#2", "Go to /");
		hk("#3", "Go to your home folder, /root");
	}
	if (HAS_BP && l->id == L_BP_PSU)
		hk("#2", "Power supply on or off");
	hk_fixed(l->id == L_BROWSE && !br_drives() ? "Up a folder; from / to Drives" : "Back");
	help_popup(l->title, list_about(l->id), help_keys, help_n);
}

static void help_bp(enum screen sc)
{
	const char *title = "Bus Pirate", *about = "A Bus Pirate 6 on the bottom USB port.";

	help_n = 0;
	switch (sc) {
	case SC_LA:
		title = "Logic analyzer";
		about = "Captures IO0-IO7 with the Bus Pirate's logic analyzer. Sampling starts at the trigger.";
		hk("#2", "Capture");
		hk("#3", "Save to /root/capture.bin");
		hk("#1", "Options: sample rate, trigger, edge, samples");
		hk("\xe2\x86\x90\xe2\x86\x92", "Scroll");
		hk("\xe2\x86\x91\xe2\x86\x93", "Zoom");
		hk("Esc", "Cancel a capture that waits, or back");
		break;
	case SC_PINS:
		title = "Pins";
		about = "The voltage on Vout and every IO pin, and what the mode uses each for.";
		hk("#2", "Power supply on or off");
		hk("#3", "Pull-up resistors on or off");
		hk("Esc", "Back");
		break;
	case SC_BPINFO:
		title = "Info";
		about = "What the Bus Pirate says about itself.";
		hk("Esc", "Back");
		break;
	default:
		hk("\xe2\x86\x90\xe2\x86\x91\xe2\x86\x93\xe2\x86\x92", "Choose a tile");
		hk("Enter", "Open it");
		hk("#2", "Power supply");
		hk("#3", "Logic analyzer");
		hk("Esc", APP_MODE ? "Close the app" : "Back");
		break;
	}
	hk("#4", "Console: the Bus Pirate's own terminal");
	hk("#5", "Pins and pull-ups");
	hk("#6", "This help");
	hk("Logo", "Home screen");
	help_popup(title, about, help_keys, help_n);
}

static void help_log(void)
{
	help_n = 0;
	hk("\xe2\x86\x91\xe2\x86\x93", "Scroll");
	hk_fixed("Back");
	help_popup(log_title, "The end of the log, newest at the bottom.", help_keys, help_n);
}

/* ------------------------------------------------------------------ */
#ifdef HOME_PREVIEW

#include <stdio.h>
#include <stdlib.h>

static void save(const char *dir, const char *name)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t d = fb[i];
		unsigned char px[3] = {
			(unsigned char)(((d >> 11) << 3) | (d >> 13)),
			(unsigned char)((((d >> 5) & 63) << 2) | ((d >> 9) & 3)),
			(unsigned char)(((d & 31) << 3) | ((d >> 2) & 7)),
		};
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

/* Host checks of the pure helpers; preview.sh stops on a failure. */
static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "selftest: %s\n", what);
		failures++;
	}
}

static void selftest(void)
{
	struct ipconf c = { .enabled = 0, .is_static = 1, .address = "10.0.0.2/24",
			    .gateway = "10.0.0.1", .dns = "1.1.1.1 9.9.9.9" }, d;
	char buf[512], a[24];

	conf_text(buf, &c, NK_WIFI);
	conf_parse(buf, &d);
	check(!d.enabled && d.is_static && !strcmp(d.address, c.address) &&
	      !strcmp(d.gateway, c.gateway) && !strcmp(d.dns, c.dns), "conf round trip");
	conf_parse("", &d);
	check(d.enabled && !d.is_static, "conf defaults: Wi-Fi on, DHCP");
	conf_parse("MODE=static\nADDRESS='192.168.1.5/24'\n", &d);
	check(d.is_static && !strcmp(d.address, "192.168.1.5/24"), "conf quoting");

	strcpy(a, "192.168.1.20");
	check(ip_ok(a, 1, sizeof(a)) && !strcmp(a, "192.168.1.20/24"), "address gets /24");
	strcpy(a, "10.1.2.3/8");
	check(ip_ok(a, 1, sizeof(a)), "address with length");
	strcpy(a, "10.1.2.3/33");
	check(!ip_ok(a, 1, sizeof(a)), "length over 32");
	strcpy(a, "256.1.1.1");
	check(!ip_ok(a, 0, 0), "octet over 255");
	strcpy(a, "1.2.3");
	check(!ip_ok(a, 0, 0), "three octets");
	strcpy(buf, "1.1.1.1, 9.9.9.9");
	check(dns_ok(buf) && !strcmp(buf, "1.1.1.1  9.9.9.9"), "dns list with comma");
	strcpy(buf, "1.1.1.1 dns.example");
	check(!dns_ok(buf), "dns names are not addresses");

	check(key_char(30, 0) == 'a' && key_char(30, 1) == 'A', "a/A");
	check(key_char(2, 0) == '1' && key_char(2, 1) == '!', "1/!");
	check(key_char(11, 0) == '0' && key_char(13, 1) == '+', "0, +");
	check(key_char(43, 0) == '\\' && key_char(53, 1) == '?', "backslash, ?");
	check(key_char(57, 0) == ' ' && key_char(28, 0) == 0 && key_char(42, 0) == 0,
	      "space; Enter and Shift type nothing");
}

static void shot(const char *dir, const char *name, enum list_id id, int focus_step)
{
	static struct list l;
	int f;

	build(&l, id);
	f = first_focus(&l);
	for (int i = 0; i < focus_step; i++)
		f = move_focus(&l, f, 1);
	draw_list(&l, f);
	save(dir, name);
}

/* A list with help over it, as the diamond key shows it. */
static void help_shot(const char *dir, const char *name, enum list_id id, int focus_step)
{
	static struct list l;
	int f;

	build(&l, id);
	f = id == L_BROWSE ? BR.focus_row : first_focus(&l);
	for (int i = 0; i < focus_step; i++)
		f = move_focus(&l, f, 1);
	draw_list(&l, f);
	help_list(&l, f);
	save(dir, name);
}

int main(int argc, char **argv)
{
	static uint16_t buf[W * H];
	const char *dir = argc > 1 ? argv[1] : ".";
	struct input in = { .act = A_NETWORK, .eb = "WI-FI", .title = "Password for Nordic Village",
			    .buf = "hunter22", .len = 8, .cur = 8, .secret = 1 };
	static struct list l;

	selftest();
	if (failures)
		return 1;
	fb = buf;
	sys_read();
	draw_home(0);
	save(dir, "home");
	draw_home(5);
	save(dir, "home-power-focused");
	popup = 1;
	draw_home(3);
	save(dir, "home-popup");
	popup = 0;
	draw_power(5, 0);
	save(dir, "power");
	shot(dir, "settings", L_SETTINGS, 0);
	help_shot(dir, "settings-help", L_SETTINGS, 0);
	shot(dir, "settings-general", L_SET_GENERAL, 0);
	build(&l, L_SET_GENERAL);
	draw_list(&l, 1);
	pick_tz();
	P.focus = P.cur = 3;	/* Amsterdam */
	draw_pick();
	save(dir, "settings-tz");
	strcpy(P.filter, "am");
	pick_filter();
	draw_list(&l, 1);
	draw_pick();
	save(dir, "settings-tz-filter");
	shot(dir, "settings-conn", L_SET_CONN, 3);
	shot(dir, "settings-lora", L_LORA, 1);
	shot(dir, "settings-lora-power", L_LORA, 2);
	build(&l, L_LORA);
	draw_list(&l, 2);
	pick_preset();
	draw_pick();
	save(dir, "settings-lora-preset");
	shot(dir, "settings-slider", L_SET_DISPLAY, 0);
	shot(dir, "settings-storage", L_SET_STORAGE, 0);
	shot(dir, "apps", L_APPS, 0);
	shot(dir, "games", L_GAMES, 1);
	shot(dir, "files", L_FILES, 0);
	br_go("", 0);
	shot(dir, "browse-drives", L_BROWSE, 0);
	br_go("/", 0);
	shot(dir, "browse-root", L_BROWSE, 0);
	br_go("/mnt/sd", 0);
	BR.sel = 4;
	cat(BR.clip, "/mnt/sd/notes.txt");
	shot(dir, "browse-dir", L_BROWSE, 0);
	help_shot(dir, "browse-help", L_BROWSE, 0);
	build(&l, L_BROWSE);	/* the square key's options */
	draw_list(&l, BR.focus_row);
	pick_reset();
	{
		static const char *const ops[] = { "Open with\xe2\x80\xa6", "Rename", "Copy", "Cut",
						   "Paste here", "New folder", "Delete" };

		for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
			struct menu_item *it = pick_add(ops[i]);

			it->sep = i == 4 || i == 6;
			it->danger = i == 6;
		}
		P.it[4].sub = "notes.txt";
	}
	pick_open(A_FILEOPS, "FILES", BR.e[BR.sel].name, -1);
	P.filterable = 0;
	draw_pick();
	save(dir, "browse-options");
	{
		static const char *const labels[2] = { "Cancel", "Delete" };
		static const struct wb_icon *const ics[2] = { &r_chevron, &r_trash };

		draw_list(&l, BR.focus_row);
		dialog("FILES", "Delete IMG_2041.jpg?", 0, labels, ics, 2, 0, 1);
		hint_bar(choice_hints, 3);
		save(dir, "browse-delete");
	}
	memset(&in, 0, sizeof(in));
	in.act = A_RENAME;
	in.eb = "FILES";
	cat(in.title, "Rename");
	cat(in.buf, "IMG_2041.jpg");
	in.len = (int)strlen(in.buf);
	in.cur = input_ext(&in);
	draw_list(&l, BR.focus_row);
	draw_input(&in);
	save(dir, "browse-rename");
	BR.clip[0] = 0;
	shot(dir, "files-settings", L_FILES_SET, 0);
	log_open(0, "FILES", "Copy, move and delete");
	draw_log();
	save(dir, "log-empty");
	shot(dir, "net-wifi", L_NET_WIFI, 3);
	shot(dir, "net-usb", L_NET_USB, 0);
	shot(dir, "ip-usb", L_IP_USB, 1);
	shot(dir, "disks", L_DISKS, 0);
	cat(disk_sel, "/dev/mmcblk0p1");
	cat(disk_title, "SD card");
	shot(dir, "disk", L_DISK, 1);
	memset(disk_sel, 0, sizeof(disk_sel));
	cat(disk_sel, "/dev/sdb1");
	memset(disk_title, 0, sizeof(disk_title));
	cat(disk_title, "Bus Pirate 6");
	shot(dir, "disk-usb", L_DISK, 0);
	build(&l, L_DISK);
	draw_list(&l, 4);
	draw_format_confirm(0);
	save(dir, "format-confirm");
	build(&l, L_LORA);
	draw_list(&l, first_focus(&l));
	draw_lora_confirm(0);
	save(dir, "lora-confirm");
	memset(&in, 0, sizeof(in));
	in.act = A_NETWORK;
	in.eb = "WI-FI";
	cat(in.title, "Password for Nordic Village");
	cat(in.buf, "hunter22");
	in.len = in.cur = 8;
	in.secret = 1;
	build(&l, L_NET_WIFI);
	draw_list(&l, 5);
	draw_input(&in);
	save(dir, "input-password");
	in.secret = 0;
	in.act = A_ADDR;
	in.title[0] = 0;
	cat(in.title, "Address");
	in.buf[0] = 0;
	cat(in.buf, "10.0.0.");
	in.len = in.cur = 7;
	in.err = "Use a.b.c.d/len, for example 192.168.1.20/24";
	build(&l, L_IP_USB);
	draw_list(&l, 2);
	draw_input(&in);
	save(dir, "input-address");
	in.act = A_ASK;		/* an app's ASK (nmap) */
	in.eb = "TOOLS";
	in.err = 0;
	memset(in.title, 0, sizeof(in.title));
	cat(in.title, "nmap arguments");
	memset(in.buf, 0, sizeof(in.buf));
	cat(in.buf, "-F 192.168.1.1");
	in.len = in.cur = (int)strlen(in.buf);
	build(&l, L_SETTINGS);
	draw_list(&l, 0);
	draw_input(&in);
	save(dir, "input-ask");
	draw_message("POWER", "Shutting down\xe2\x80\xa6");
	save(dir, "message");
	draw_bp(0);
	save(dir, "bp");
	draw_bp(0);
	help_bp(SC_BP);
	save(dir, "bp-help");
	shot(dir, "bp-psu", L_BP_PSU, 1);
	B.led_set = LT_WS2812;
	B.led_color = RAINBOW;
	B.pin_f[0][0] = 0;
	cat(B.pin_f[0], "SDO");
	shot(dir, "bp-led", L_BP_LED, 1);
	build(&l, L_BP_LED);
	draw_list(&l, 1);
	pick_led(A_LCOLOR);
	P.focus = 8;
	draw_pick();
	save(dir, "bp-led-colour");
	draw_list(&l, 0);
	pick_led(A_LTYPE);
	draw_pick();
	save(dir, "bp-led-type");
	memset(&in, 0, sizeof(in));
	in.act = A_LBRIGHT;
	in.eb = "LEDS";
	cat(in.title, "Brightness, 1 to 100 %");
	cat(in.buf, "25");
	in.len = in.cur = 2;
	draw_list(&l, 2);
	draw_input(&in);
	save(dir, "bp-led-bright");
	draw_la();
	save(dir, "bp-la");
	LA.zoom = 0;
	LA.pos = 200;
	draw_la();
	save(dir, "bp-la-zoom");
	draw_pins();
	save(dir, "bp-pins");
	draw_bpinfo();
	save(dir, "bp-info");
	return 0;
}

#else /* the badge */

static void blit(void)
{
	pwrite(fb_fd, fb, fb_bytes, 0);
}

static void console_graphics(int on)
{
	if (tty_fd >= 0)
		ioctl(tty_fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
}

static int spawn(char *const argv[])
{
	int pid = vfork();

	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static int fn_down, shift_down;
static int want_repeat;		/* text input: auto-repeat counts as presses */

#define KEY_TAB		15
#define KEY_LEFTSHIFT	42
#define KEY_RIGHTSHIFT	54

/* Next key code from the test FIFO, 0 if none buffered. */
static int fifo_key(void)
{
	int i = 0, v = 0, got = 0;

	while (i < fifo_len && (fifo_buf[i] < '0' || fifo_buf[i] > '9'))
		i++;
	while (i < fifo_len && fifo_buf[i] >= '0' && fifo_buf[i] <= '9') {
		v = v * 10 + fifo_buf[i++] - '0';
		got = 1;
	}
	if (!got || i == fifo_len)	/* need the separator after a number */
		return 0;
	memmove(fifo_buf, fifo_buf + i, (size_t)(fifo_len - i));
	fifo_len -= i;
	return v;
}

static int wait_key(int timeout_ms)
{
	struct pollfd p[2] = { { .fd = ev_fd, .events = POLLIN },
			       { .fd = fifo_fd, .events = POLLIN } };
	struct ev e;
	int k;

	for (;;) {
		int r;

		if ((k = fifo_key()))
			return k;
		r = poll(p, fifo_fd >= 0 ? 2 : 1, timeout_ms);
		if (r == 0)
			return 0;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (fifo_fd >= 0 && (p[1].revents & POLLIN)) {
			int n = read(fifo_fd, fifo_buf + fifo_len,
				     sizeof(fifo_buf) - 1 - (size_t)fifo_len);

			if (n > 0)
				fifo_len += n;
			if (fifo_len >= (int)sizeof(fifo_buf) - 1)
				fifo_len = 0;	/* garbage: drop */
			continue;
		}
		if (!(p[0].revents & POLLIN))
			continue;
		if (read(ev_fd, &e, sizeof(e)) != sizeof(e)) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (e.type != 1)
			continue;
		if (e.code == KEY_FN) {
			fn_down = e.value != 0;
			continue;
		}
		if (e.code == KEY_LEFTSHIFT || e.code == KEY_RIGHTSHIFT) {
			shift_down = e.value != 0;
			continue;
		}
		if (e.value == 1 || (e.value == 2 && want_repeat))
			return e.code;
	}
}

/* Fn+Esc: screen and both backlights off until any key (as sleepd). */
static void screen_off(void)
{
	int bl = read_int(BL, BL_DEFAULT), kbd = read_int(KBD_BL, KBD_DEFAULT);

	write_int(BL, 0);
	write_int(KBD_BL, 0);
	write_int(FB_BLANK, 4);
	while (wait_key(-1) == 0)
		;
	write_int(FB_BLANK, 0);
	write_int(BL, bl ? bl : BL_DEFAULT);
	write_int(KBD_BL, kbd);
	fn_down = 0;
}

static const char *const cat_eyebrow[4] = { "APPS", "GAMES", "TOOLS", "FILES" };

/* /tmp/app-<directory><ext>: its log, and the last answer to ASK */
static void app_tmp(const struct app *ap, char *out, const char *ext)
{
	const char *base = strrchr(ap->dir, '/');

	cat(cat(cat(out, "/tmp/app-"), base ? base + 1 : "app"), ext);
}

/*
 * App i, in our place: apprun (overlay-src/apprun.c) runs it, ends it on
 * the logo key or when memory runs out, and execs the menu again with
 * --app=<its dir>, so we come back to its list. ARGS is split at spaces;
 * %d is the app's directory, %f the file chosen (or nothing), %a the
 * answer to ASK (an argument of its own: one per word), %u the file as a
 * file:// URL, %% a %. An argument with %f or %u is left out when there is
 * no file. With TERMINAL=1 the program runs in appterm's window; with
 * TERMINAL=shell a shell follows it there (shell_wrap). With ESC=quit,
 * apprun ends it on Esc. Started from Files (run_back), the menu comes
 * back there, also after the logo key. Returns only on failure.
 */
static const char *run_back;	/* "--browse=<file>" when Files started it */
static const char *run_extra;	/* one more argument after ARGS (store --my-apps) */

/* file as a file:// URL, with what a URL cannot hold %-escaped */
static char *file_url(char *o, const char *end, const char *file)
{
	static const char hx[] = "0123456789ABCDEF";

	for (const char *p = "file://"; *p && o < end; )
		*o++ = *p++;
	for (const unsigned char *f = (const unsigned char *)file; *f && o < end - 3; f++) {
		if (*f <= ' ' || *f >= 0x7f || strchr("%#?\"<>", *f)) {
			*o++ = '%';
			*o++ = hx[*f >> 4];
			*o++ = hx[*f & 15];
		} else {
			*o++ = (char)*f;
		}
	}
	return o;
}

/*
 * TERMINAL=shell: the program, then a shell to run it again. The shell
 * finds it as /tmp/app-bin/<name>, a script that adds the ARGS it needs
 * (nmap: --datadir, --unprivileged, ...), so "nmap -F host" works as
 * typed. prog is the program and its arguments; they become sh -c with
 * cmd (its buffer). exit, or the logo key, closes the window.
 */
static void shell_wrap(char **prog, char *cmd)
{
	const char *name = strrchr(prog[0], '/');
	char path[96], *o;
	int fd;

	name = name ? name + 1 : prog[0];
	mkdir("/tmp/app-bin", 0755);
	cat(cat(path, "/tmp/app-bin/"), name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (fd >= 0) {
		char line[600];

		o = cat(line, "#!/bin/sh\nexec");
		for (char **a = prog; *a && o < line + sizeof(line) - 80; a++) {
			o = cat(o, " '");
			for (const char *c = *a; *c && o < line + sizeof(line) - 80; c++)
				o = *c == '\'' ? cat(o, "'\\''") : (*o++ = *c, o);
			o = cat(o, "'");
		}
		o = cat(o, " \"$@\"\n");
		write(fd, line, (size_t)(o - line));
		close(fd);
	}
	setenv("PATH", "/tmp/app-bin:/root/.whyos/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
	o = cat(cmd, name);
	cat(cat(cat(o, "; echo; echo \"Type "), name),
	    " ... to run it again; exit or the logo key closes this.\"; exec /bin/sh");
	prog[0] = "/bin/sh";
	prog[1] = "-c";
	prog[2] = cmd;
	prog[3] = 0;
}

static void run_app(int i, const char *file, const char *answer)
{
	static char exe[112], back[300], log[64], args[768], cmd[192], idopt[40], diropt[80];
	char *argv[48];
	const struct app *ap = &A[i];
	char *o = args, *end = args + sizeof(args) - 1;
	int n = 0, prog;

	argv[n++] = "/usr/bin/apprun";
	argv[n++] = session ? "--session" : "--popup";
	if (ap->esc)
		argv[n++] = "--esc";
	/* who it is, for its APP_* environment (apprun.c) */
	snprintf(idopt, sizeof(idopt), "--id=%s", ap->id);
	snprintf(diropt, sizeof(diropt), "--dir=%s", ap->dir);
	argv[n++] = idopt;
	argv[n++] = diropt;
	if (ap->rotation_lock)
		argv[n++] = "--rotation-lock";
	argv[n++] = back;
	argv[n++] = log;

	exe[0] = back[0] = log[0] = 0;
	if (ap->exec[0] == '/')
		cat(exe, ap->exec);
	else
		cat(cat(cat(exe, ap->dir), "/"), ap->exec);
	if (run_back)
		strncpy(back, run_back, sizeof(back) - 1);
	else
		cat(cat(back, "--app="), ap->dir);
	app_tmp(ap, log, ".log");
	if (ap->terminal) {
		argv[n++] = "/usr/bin/appterm";
		argv[n++] = "--title";
		argv[n++] = (char *)ap->name;
		argv[n++] = "--eyebrow";
		argv[n++] = (char *)cat_eyebrow[ap->cat];
		if (ap->terminal >= 2)
			argv[n++] = "--close";
	}
	prog = n;
	argv[n++] = exe;
	for (const char *q = ap->args; *q && n < 46; ) {
		while (*q == ' ')
			q++;
		if (!*q)
			break;
		if (q[0] == '%' && q[1] == 'a' && (q[2] == ' ' || !q[2])) {
			for (const char *w = answer ? answer : ""; *w && n < 46 && o < end - 1; ) {
				while (*w == ' ')
					w++;
				if (!*w)
					break;
				argv[n++] = o;
				while (*w && *w != ' ' && o < end - 1)
					*o++ = *w++;
				*o++ = 0;
			}
			q += 2;
			continue;
		}
		{
			int no_file = 0;	/* %f or %u in it, but no file: left out */

			for (const char *t = q; *t && *t != ' '; t++)
				no_file |= *t == '%' && (t[1] == 'f' || t[1] == 'u') && !file;
			if (no_file) {
				while (*q && *q != ' ')
					q++;
				continue;
			}
		}
		argv[n++] = o;
		for (; *q && *q != ' ' && o < end - 1; q++) {
			const char *sub = 0;

			if (*q == '%' && q[1] == 'u') {
				o = file_url(o, end - 1, file);
				q++;
				continue;
			}
			if (*q == '%' && (q[1] == 'D' || q[1] == 'C' || q[1] == 'T')) {
				char place[160];

				for (const char *v = app_place(ap, q[1], place, sizeof(place));
				     *v && o < end - 1; )
					*o++ = *v++;
				q++;
				continue;
			}
			if (*q == '%' && (q[1] == 'd' || q[1] == 'f' || q[1] == 'a'))
				sub = q[1] == 'd' ? ap->dir : q[1] == 'f' ? (file ? file : "") :
				      answer ? answer : "";
			if (sub) {
				while (*sub && o < end - 1)
					*o++ = *sub++;
				q++;
			} else if (*q == '%' && q[1] == '%') {
				*o++ = '%';
				q++;
			} else {
				*o++ = *q;
			}
		}
		*o++ = 0;
	}
	if (run_extra && n < 46)
		argv[n++] = (char *)run_extra;
	argv[n] = 0;
	if (ap->terminal == 3)
		shell_wrap(argv + prog, cmd);
	fill(0, 0, W, H, 0);
	blit();
	execv(argv[0], argv);	/* our fds are close-on-exec, grab included */
}

/*
 * The Bus Pirate's own terminal on the panel, for everything the tiles do
 * not cover: the panel's VT in raw mode, bytes copied both ways, the logo
 * key (read from the ungrabbed keypad) back to the home screen.
 *
 * The menu's connection said no to "VT100 compatible color mode?" (plain
 * replies to parse), and in that mode the Bus Pirate assumes a 24-row
 * terminal: its help stops every 20 rows. It asks that question only
 * once after power-up, so the console switches colours and the status
 * bar on through its configuration menu instead, and off again when you
 * leave. Switching colours on makes it measure the terminal; we answer
 * with the panel console's size, so it pages to 45 rows. As a pop-up over
 * the panel's shell, that shell's foreground job is stopped meanwhile, or
 * it would read half the keys (as for DOOM).
 */
static void bp_console(void)
{
	static const char banner[] = "\033[2J\033[H\033[1mBus Pirate\033[0m: switching it to colour "
				     "mode\xe2\x80\xa6 The logo key goes back to the home screen.\r\n";
	static const char reset[] = "\033[r\033[0m\033[2J\033[H";	/* its scroll region too */
	struct termios old, raw;
	struct pollfd p[4];
	char buf[512];
	int have = tcgetattr(tty_fd, &old) == 0, fg = 0, done = 0;

	if (tty_fd < 0 || bp_fd < 0)
		return;
	ioctl(ev_fd, EVIOCGRAB, 0);
	fill(0, 0, W, H, 0);
	blit();
	console_graphics(0);
	if (have) {
		raw = old;
		raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
		raw.c_oflag &= ~OPOST;
		raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(tty_fd, TCSANOW, &raw);
	}
	if (popup && ioctl(tty_fd, TIOCGPGRP, &fg) == 0 && fg > 1)
		kill(-fg, SIGSTOP);
	else
		fg = 0;
	write(tty_fd, banner, sizeof(banner) - 1);
	bp_to_prompt();
	bp_colours(1);
	if (bp_fd >= 0)
		write(bp_fd, "cls\r", 4);	/* a clean, coloured screen */
	p[0].fd = tty_fd;
	p[1].fd = bp_fd;
	p[2].fd = ev_fd;
	p[3].fd = fifo_fd;
	for (int i = 0; i < 4; i++)
		p[i].events = POLLIN;
	while (!done && bp_fd >= 0) {
		int k = poll(p, fifo_fd >= 0 ? 4 : 3, -1);

		if (k < 0 && errno != EINTR)
			break;
		if (k <= 0)
			continue;
		if (p[0].revents & POLLIN) {
			k = read(tty_fd, buf, sizeof(buf));
			if (k > 0)
				write(bp_fd, buf, (size_t)k);
		}
		if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			k = read(bp_fd, buf, sizeof(buf));
			if (k > 0)
				write(tty_fd, buf, (size_t)k);
			else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
				done = 1;
			}
		}
		if (p[2].revents & POLLIN) {
			struct ev e;

			if (read(ev_fd, &e, sizeof(e)) == sizeof(e) && e.type == 1 &&
			    e.code == KEY_LEFTMETA && e.value == 1)
				done = 1;
		}
		if (fifo_fd >= 0 && (p[3].revents & POLLIN)) {	/* the test hook */
			k = read(fifo_fd, fifo_buf + fifo_len, sizeof(fifo_buf) - 1 - (size_t)fifo_len);
			if (k > 0)
				fifo_len += k;
			while ((k = fifo_key()))
				if (k == KEY_LEFTMETA)
					done = 1;
			if (fifo_len >= (int)sizeof(fifo_buf) - 1)
				fifo_len = 0;
		}
	}
	if (bp_fd >= 0) {	/* plain again, for the menu's own replies */
		bp_to_prompt();
		bp_colours(0);
	}
	write(tty_fd, reset, sizeof(reset) - 1);
	if (have)
		tcsetattr(tty_fd, TCSANOW, &old);
	tcflush(tty_fd, TCIFLUSH);
	if (fg)
		kill(-fg, SIGCONT);
	console_graphics(1);
	ioctl(ev_fd, EVIOCGRAB, 1);
}

/* Remember the choice (it survives a restart: S60lora reads it at boot) and
 * start or stop the mesh daemon, which holds the radio. */
static void lora_set(int on)
{
	char *argv[] = { "/etc/init.d/S60lora", on ? "start" : "stop", 0 };

	settings_load(&SET);
	if (settings_set(&SET, "lora", on ? "on" : "off") || settings_save(&SET))
		return;
	run_wait(argv, "/tmp/lora-toggle.log");
	S.lora = lora_on();
	lora_dirty = 0;		/* it started with the settings as they are */
}

/* Time zone i of the pop-up (-1: Automatic). /etc/TZ follows at once (tz
 * apply); Automatic with nothing found yet asks (tz detect, in the
 * background). */
static void tz_set(int i)
{
	char *apply[] = { "/usr/bin/tz", "apply", 0 }, *detect[] = { "/usr/bin/tz", "detect", 0 };

	settings_load(&SET);
	zones_read();
	if (i >= n_zones)
		return;
	if (settings_set(&SET, "timezone", i < 0 ? "auto" : zone_names[i]) || settings_save(&SET))
		return;
	run_wait(apply, "/tmp/tz.log");
	if (i < 0 && !tz_setting("timezone.auto")[0])
		spawn(detect);
}

/* Preset dir (A_LORA_PRESET: its index), or the transmit power a dBm up or
 * down. Kept in the settings; a running daemon gets them when the LoRa
 * screen is left (lora_apply). */
static void lora_change(enum act a, int dir)
{
	struct lora_radio lr;
	char v[40], *p;
	int dbm, i;

	settings_load(&SET);
	lora_radio(&lr, &dbm);
	if (a == A_LORA_PRESET) {
		i = dir;
		if (i < 0 || i >= LORA_N_PRESETS)
			return;
		v[0] = 0;
		p = utoa(v, lora_presets[i].freq_khz, 1);
		p = utoa(cat(p, " "), lora_presets[i].bw_hz, 1);
		p = utoa(cat(p, " "), lora_presets[i].sf, 1);
		utoa(cat(p, " "), lora_presets[i].cr, 1);
		if (settings_set(&SET, "lora.radio", v))
			return;
	} else {
		dbm += dir;
		if (dbm < LORA_POWER_MIN || dbm > LORA_POWER_MAX)
			return;
		v[0] = 0;
		utoa(v, (unsigned)dbm, 1);
		if (settings_set(&SET, "lora.power", v))
			return;
	}
	if (settings_save(&SET))
		return;
	if (lora_on())
		lora_dirty = 1;
}

static void lora_apply(void)
{
	char *argv[] = { "/etc/init.d/S60lora", "restart", 0 };

	lora_dirty = 0;
	if (lora_wanted())
		run_wait(argv, "/tmp/lora-toggle.log");
	S.lora = lora_on();
}

static void ble_toggle(void)
{
	char *argv[] = { "/usr/bin/ble_adv", 0 };

	if (S.ble_pid > 0)
		kill(S.ble_pid, SIGTERM);
	else
		spawn(argv);	/* keeps running after the menu */
}

static void brightness_step(int dir)
{
	int pct = S.bl_max ? (S.bl * 100 + S.bl_max / 2) / S.bl_max : 0;

	pct += dir * 10;
	pct = pct < 10 ? 10 : pct > 100 ? 100 : pct;
	write_int(BL, S.bl_max * pct / 100);
}

static void kbd_toggle(void)
{
	static int last_on = KBD_DEFAULT;

	if (S.kbd > 0) {
		last_on = S.kbd;
		write_int(KBD_BL, 0);
	} else {
		write_int(KBD_BL, last_on);
	}
}

static void leave(void)
{
	ioctl(ev_fd, EVIOCGRAB, 0);
	fill(0, 0, W, H, 0);
	blit();
	console_graphics(0);
}

/*
 * The panel's shell: the console getty, logged in as root without a
 * prompt (/usr/bin/autologin runs `login -f root`). Root has no password
 * and the panel is only usable by whoever holds the badge.
 */
static char *const panel_getty[] = { "/sbin/getty", "-n", "-l", "/usr/bin/autologin",
				     "-L", "console", "0", "linux", 0 };

/* Terminal: in a session, become the panel's shell; init starts the menu
 * again when it exits. As a pop-up, just close: the shell is underneath. */
static void terminal(void)
{
	leave();
	if (!session)
		_exit(0);
	close(ev_fd);
	close(fb_fd);
	close(tty_fd);
	execv(panel_getty[0], panel_getty);
	_exit(127);
}

static void power(int restart)
{
	char *rb[] = { "/sbin/reboot", 0 }, *po[] = { "/sbin/poweroff", 0 };

	draw_message("POWER", restart ? "Restarting\xe2\x80\xa6" : "Shutting down\xe2\x80\xa6");
	blit();
	spawn(restart ? rb : po);
	for (;;)
		pause();	/* init's shutdown takes us down */
}

static int setup(void)
{
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi) < 0) {
		say("home: no /dev/fb0\n");
		return -1;
	}
	if (vi.xres != W || vi.yres != H || vi.bits_per_pixel != 16 ||
	    fi.line_length != W * 2) {
		say("home: expected a 720x720 RGB565 panel\n");
		return -1;
	}
	fb_bytes = (size_t)W * H * 2;
	/* Back from a big app (the store, a browser), its memory may not be
	 * free yet the moment we start: try again for a few seconds before
	 * giving the panel to a shell. */
	for (int t = 0; t < 10; t++) {
		fb = mmap(0, fb_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (fb != MAP_FAILED)
			break;
		usleep(300000);
	}
	if (fb == MAP_FAILED) {
		say("home: no memory for the shadow buffer\n");
		return -1;
	}
	ev_fd = open(EVDEV, O_RDONLY);
	if (ev_fd < 0) {
		say("home: no keypad\n");
		return -1;
	}
	ioctl(ev_fd, EVIOCGRAB, 1);
	tty_fd = open(CONSOLE_TTY, O_RDWR | O_NOCTTY);
	console_graphics(1);
	/* children (DOOM, sensorpanel, ble_adv) get none of these */
	fcntl(fb_fd, F_SETFD, FD_CLOEXEC);
	fcntl(ev_fd, F_SETFD, FD_CLOEXEC);
	fcntl(tty_fd, F_SETFD, FD_CLOEXEC);
	if (session) {		/* one menu owns the hook; a pop-up does not */
		unlink(KEYS_FIFO);
		if (mkfifo(KEYS_FIFO, 0600) == 0)
			fifo_fd = open(KEYS_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	}
	return 0;
}

/*
 * A symbol key, an arrow or Enter on a Bus Pirate list row: how is -1
 * (left), 1 (right, Enter) or 0 (its symbol key: the next preset).
 */
static void bp_row(enum act a, int how)
{
	static const int volt_presets[] = { 18, 25, 33, 50 };
	static const int ma_presets[] = { 50, 100, 300, 500, 0 };
	int i;

	switch (a) {
	case A_PSU:
		bp_psu(!B.psu);
		break;
	case A_PULL:
		bp_pullups(!B.pullups);
		break;
	case A_VOLT:
		if (how) {
			B.dv += how;
		} else {
			for (i = 0; i < 4 && volt_presets[i] <= B.dv; i++)
				;
			B.dv = volt_presets[i % 4];
		}
		B.dv = B.dv < 8 ? 8 : B.dv > 50 ? 50 : B.dv;
		if (B.psu)
			bp_psu(1);
		break;
	case A_LIMIT:
		if (how) {
			B.ma += how * 10;
		} else {
			for (i = 0; i < 5 && ma_presets[i] != B.ma; i++)
				;
			B.ma = ma_presets[(i + 1) % 5];
		}
		B.ma = B.ma < 0 ? 0 : B.ma > 500 ? 500 : B.ma;
		if (B.psu)
			bp_psu(1);
		break;
	case A_LOFF:
		bp_leds(1);
		break;
	default:
		break;
	}
}

static int bp_list(enum list_id id)
{
	return HAS_BP && (id == L_BP_PSU || id == L_BP_LED);
}

/* Start a text input; prefilled with the current value. */
static void input_start(struct input *in, enum act act, int kind, const char *eb,
			const char *title, const char *value, int secret)
{
	memset(in, 0, sizeof(*in));
	in->act = act;
	in->kind = kind;
	in->eb = eb;
	strncpy(in->title, title, sizeof(in->title) - 1);
	strncpy(in->buf, value, sizeof(in->buf) - 1);
	in->len = (int)strlen(in->buf);
	in->secret = secret;
	in->cur = in->len;
	if (act == A_RENAME)		/* before the extension (6.10) */
		in->cur = input_ext(in);
}

/*
 * Start app i (with the file chosen, if any): straight away, or with ASK
 * first through the text dialog, offering the last answer (kept in /tmp
 * across the menu's exec) or ASK_DEFAULT. A failed start leaves a notice.
 */
static char ask_file[sizeof(((struct app *)0)->files) + 50];

static void app_start(int i, const char *file, struct input *in, enum screen *sc)
{
	const struct app *ap = &A[i];
	char path[80] = "", last[sizeof(in->buf)];

	ask_file[0] = 0;
	if (ap->ask[0]) {
		if (file)
			strncpy(ask_file, file, sizeof(ask_file) - 1);
		app_tmp(ap, path, ".ask");
		if (read_str(path, last, sizeof(last)) < 0)
			cat(last, ap->ask_default);
		input_start(in, A_ASK, i, cat_eyebrow[ap->cat], ap->ask, last, 0);
		*sc = SC_INPUT;
		return;
	}
	run_app(i, file, 0);
	cat(cat(app_notice, "Could not start "), ap->name);
}

/* ---- Files: opening and changing files -------------------------------- */

static char br_target[320];		/* the entry acted on */
static int br_cand[MAX_APPS], br_ncand;
enum br_op { OP_OPENWITH, OP_RENAME, OP_COPY, OP_CUT, OP_PASTE, OP_MKDIR, OP_DELETE };
static enum br_op br_ops[8];

static void br_say(const char *a, const char *b, int bad)
{
	BR.notice[0] = 0;
	strncat(BR.notice, a, sizeof(BR.notice) - 1);
	strncat(BR.notice, b, sizeof(BR.notice) - 1 - strlen(BR.notice));
	BR.notice_bad = bad;
}

/* the focused entry's path into br_target; 0 if there is none */
static int br_sel_path(void)
{
	const struct br_ent *e = BR.n ? &BR.e[BR.sel] : 0;

	br_target[0] = 0;
	if (!e)
		return 0;
	return br_join(br_target, sizeof(br_target), BR.dir, e->name);
}

static void br_start(int i, struct input *in, enum screen *sc)
{
	static char back[sizeof(br_target) + 10];

	back[0] = 0;
	cat(cat(back, "--browse="), br_target);
	run_back = back;
	app_notice[0] = 0;
	app_start(i, br_target, in, sc);	/* returns on failure, or to ASK */
	if (*sc != SC_INPUT) {
		run_back = 0;
		br_say(app_notice, "", 1);
	}
}

/* Does the file start like text (no NUL, mostly printable)? For files
 * without a known extension: /proc/cpuinfo, README, rc scripts. */
static int br_textlike(const char *path)
{
	unsigned char b[512];
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC), n, odd = 0;

	if (fd < 0)
		return 0;
	n = (int)read(fd, b, sizeof(b));
	close(fd);
	if (n < 0)
		return 0;
	for (int i = 0; i < n; i++) {
		if (!b[i])
			return 0;
		odd += b[i] < 0x20 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && b[i] != 0x1b;
	}
	return odd * 20 <= n;
}

/* The apps for the focused file: those whose OPENS has its extension, or
 * with all, those whose OPENS_ALSO has it too. */
static int br_candidates(int all)
{
	const char *x = br_ext(BR.e[BR.sel].name);

	br_ncand = 0;
	for (int i = 0; i < n_apps; i++)
		if (ext_in(A[i].opens, x) || (all && ext_in(A[i].opens_also, x)))
			br_cand[br_ncand++] = i;
	return br_ncand;
}

static void br_pick_apps(void)
{
	pick_reset();
	for (int i = 0; i < br_ncand && i < PICK_MAX; i++)
		pick_add(A[br_cand[i]].name);
	pick_open(A_OPENWITH, "FILES", "Open with", -1);
	P.filterable = 0;
	P.count[0] = 0;
}

/* Enter on an entry: into a folder, or open a file with its app. */
static void br_open(struct input *in, enum screen *sc)
{
	const struct br_ent *e;

	BR.notice[0] = 0;
	if (!BR.n || !br_sel_path())
		return;
	e = &BR.e[BR.sel];
	if (e->dir) {
		br_go(br_target, 0);
		return;
	}
	if (!e->reg) {		/* a device, pipe or socket: opening it can do things */
		br_say("Not a regular file: ", e->name, 1);
		return;
	}
	if (!br_candidates(0)) {
		/* no app for its extension: text goes to what opens .txt */
		if (br_textlike(br_target)) {
			for (int i = 0; i < n_apps; i++)
				if (ext_in(A[i].opens, "txt") && !ext_in(A[i].opens, "*")) {
					br_start(i, in, sc);
					return;
				}
		}
		if (!br_candidates(1)) {
			br_say("No app opens this kind of file: ", e->name, 1);
			return;
		}
	}
	if (br_ncand == 1) {
		br_start(br_cand[0], in, sc);
		return;
	}
	br_pick_apps();
	*sc = SC_PICK;
}

/* The options pop-up (the square key). */
static void br_options(enum screen *sc)
{
	static const char *const names[] = {
		"Open with\xe2\x80\xa6", "Rename", "Copy", "Cut", "Paste here", "New folder", "Delete",
	};
	static char paste[64];
	int have = BR.n > 0, n = 0;

	/* 6.6: first the item's options, under a line the screen's own
	 * (Paste here, New folder), and Delete last, in red */
	BR.notice[0] = 0;
	pick_reset();
	if (have && BR.e[BR.sel].reg)
		br_ops[n++] = OP_OPENWITH;
	if (have) {
		br_ops[n++] = OP_RENAME;
		br_ops[n++] = OP_COPY;
		br_ops[n++] = OP_CUT;
	}
	if (BR.clip[0])
		br_ops[n++] = OP_PASTE;
	br_ops[n++] = OP_MKDIR;
	if (have)
		br_ops[n++] = OP_DELETE;
	for (int i = 0; i < n; i++) {
		struct menu_item *it = pick_add(names[br_ops[i]]);

		it->sep = i && (br_ops[i] == OP_DELETE || ((br_ops[i] == OP_PASTE || br_ops[i] == OP_MKDIR) &&
							  br_ops[i - 1] != OP_PASTE));
		it->danger = br_ops[i] == OP_DELETE;
		if (br_ops[i] == OP_PASTE) {	/* what would be pasted */
			const char *base = strrchr(BR.clip, '/');

			paste[0] = 0;
			strncat(paste, base ? base + 1 : BR.clip, sizeof(paste) - 1);
			it->sub = paste;
		}
	}
	pick_open(A_FILEOPS, "FILES", have ? BR.e[BR.sel].name : BR.title, -1);
	P.filterable = 0;
	*sc = SC_PICK;
}

/* A file or folder name from the text dialog: no slash, not . or .. */
static int br_name_ok(const char *s)
{
	return s[0] && !strchr(s, '/') && strcmp(s, ".") && strcmp(s, "..");
}

static int br_busy(const char *what, char *const argv[])
{
	draw_message("FILES", what);
	blit();
	return run_wait(argv, "/tmp/files.log");
}

static void br_paste(void)
{
	const char *base = strrchr(BR.clip, '/');
	char dst[320];
	size_t n = strlen(BR.clip);
	int rc;

	base = base ? base + 1 : BR.clip;
	if (!br_join(dst, sizeof(dst), BR.dir, base))
		return;
	if (access(dst, F_OK) == 0) {
		br_say("Already here: ", base, 1);
		return;
	}
	if (!strncmp(BR.dir, BR.clip, n) && (BR.dir[n] == '/' || !BR.dir[n])) {
		br_say("Cannot put a folder inside itself: ", base, 1);
		return;
	}
	{
		char *cp[] = { "/bin/cp", "-a", BR.clip, dst, 0 };
		char *mv[] = { "/bin/mv", BR.clip, dst, 0 };

		rc = br_busy(BR.clip_cut ? "Moving\xe2\x80\xa6" : "Copying\xe2\x80\xa6",
			     BR.clip_cut ? mv : cp);
	}
	if (rc) {
		br_say(BR.clip_cut ? "Could not move " : "Could not copy ", base, 1);
		return;
	}
	memset(BR.want, 0, sizeof(BR.want));
	strncpy(BR.want, base, sizeof(BR.want) - 1);
	br_say(BR.clip_cut ? "Moved " : "Copied ", base, 0);
	if (BR.clip_cut)
		BR.clip[0] = 0;
	BR.stale = 1;
}

static void br_delete(void)
{
	char *rm[] = { "/bin/rm", "-rf", br_target, 0 };
	const char *base = strrchr(br_target, '/');

	if (!br_sel_path())
		return;
	base = strrchr(br_target, '/');
	base = base ? base + 1 : br_target;
	if (br_busy("Deleting\xe2\x80\xa6", rm) || access(br_target, F_OK) == 0)
		br_say("Could not delete ", base, 1);
	else
		br_say("Deleted ", base, 0);
	if (!strcmp(BR.clip, br_target))
		BR.clip[0] = 0;
	BR.stale = 1;
}

static void br_pick_done(struct input *in, enum screen *sc)
{
	*sc = SC_LIST;
	if (P.act == A_OPENWITH) {
		if (pick_item() >= 0 && pick_item() < br_ncand && br_sel_path())
			br_start(br_cand[pick_item()], in, sc);
		return;
	}
	if (pick_item() < 0)
		return;
	switch (br_ops[pick_item()]) {
	case OP_OPENWITH:
		if (!br_candidates(1)) {
			br_say("No app opens this kind of file: ", BR.e[BR.sel].name, 1);
			break;
		}
		br_pick_apps();
		*sc = SC_PICK;
		break;
	case OP_RENAME:
		input_start(in, A_RENAME, 0, "FILES", "Rename", BR.e[BR.sel].name, 0);
		*sc = SC_INPUT;
		break;
	case OP_COPY:
	case OP_CUT:
		if (br_sel_path()) {
			memset(BR.clip, 0, sizeof(BR.clip));
			strncpy(BR.clip, br_target, sizeof(BR.clip) - 1);
			BR.clip_cut = br_ops[pick_item()] == OP_CUT;
		}
		break;
	case OP_PASTE:
		br_paste();
		break;
	case OP_MKDIR:
		input_start(in, A_MKDIR, 0, "FILES", "New folder", "New folder", 0);
		in->fresh = 1;
		*sc = SC_INPUT;
		break;
	case OP_DELETE:
		*sc = SC_CONFIRM;
		break;
	}
}

/* Rename / New folder from the text dialog: 0 done, 1 stay (in->err). */
static int br_input_done(struct input *in)
{
	char path[320], old[320];

	if (!br_name_ok(in->buf)) {
		in->err = "A name without /";
		return 1;
	}
	if (!br_join(path, sizeof(path), BR.dir, in->buf)) {
		in->err = "Too long";
		return 1;
	}
	if (access(path, F_OK) == 0) {
		in->err = "That name is taken here";
		return 1;
	}
	if (in->act == A_MKDIR) {
		if (mkdir(path, 0755) < 0)
			br_say("Could not make folder ", in->buf, 1);
	} else if (br_join(old, sizeof(old), BR.dir, BR.e[BR.sel].name) && rename(old, path) < 0) {
		br_say("Could not rename to ", in->buf, 1);
	}
	memset(BR.want, 0, sizeof(BR.want));
	strncpy(BR.want, in->buf, sizeof(BR.want) - 1);
	BR.stale = 1;
	return 0;
}

static void draw_delete_confirm(int focus)
{
	static const char *const labels[2] = { "Cancel", "Delete" };
	static const struct wb_icon *const ics[2] = { &r_chevron, &r_trash };
	char t[96] = "Delete ";

	strncat(t, BR.n ? BR.e[BR.sel].name : "", sizeof(t) - 9);
	strcat(t, "?");
	dialog("FILES", t, 0, labels, ics, 2, focus, 1);
	hint_bar(choice_hints, 3);
}

/*
 * The LED rows open pop-ups: a list for the type and the colour, a
 * number for the brightness and strip length. 1 if a has one.
 */
static int bp_popup(enum act a, struct input *in, enum screen *sc)
{
	char v[8];

	switch (a) {
	case A_LTYPE:
	case A_LCOLOR:
		pick_led(a);
		*sc = SC_PICK;
		return 1;
	case A_LBRIGHT:
		utoa(v, (unsigned)B.led_pct, 1);
		input_start(in, a, 0, "LEDS", "Brightness, 1 to 100 %", v, 0);
		in->fresh = 1;
		*sc = SC_INPUT;
		return 1;
	case A_LCOUNT:
		utoa(v, (unsigned)B.led_n, 1);
		input_start(in, a, 0, "LEDS", "LEDs on the strip, 1 to 300", v, 0);
		in->fresh = 1;
		*sc = SC_INPUT;
		return 1;
	default:
		return 0;
	}
}

/* Wi-Fi: the square key on a network: Connect, Forget (6.6). */
static char net_ssid[64];

static void net_options(const struct wnet *w)
{
	pick_reset();
	memset(net_ssid, 0, sizeof(net_ssid));
	strncpy(net_ssid, w->ssid, sizeof(net_ssid) - 1);
	if (!w->current)
		pick_add("Connect");
	if (w->saved_id >= 0) {
		struct menu_item *it = pick_add("Forget");

		it->danger = 1;
		it->sep = !w->current;
	}
	pick_open(A_NETOPS, "WI-FI", net_ssid, -1);
}

/* The logic analyzer's options (the square key), and each one's choices. */
static const char *const la_edges[2] = { "Rising edge", "Falling edge" };

static void la_options(void)
{
	static char rate[16], trig[8], size[8];
	struct menu_item *it;

	pick_reset();
	it = pick_add("Sample rate");
	rate[0] = 0;
	hz(rate, la_rates[LA.rate_i]);
	cat(it->value, rate);
	it = pick_add("Trigger");
	trig[0] = 0;
	if (LA.trig < 0)
		cat(trig, "none");
	else
		utoa(cat(trig, "IO"), (unsigned)LA.trig, 1);
	cat(it->value, trig);
	it = pick_add("Edge");
	cat(it->value, LA.falling ? "falling" : "rising");
	it = pick_add("Samples");
	size[0] = 0;
	cat(utoa(size, (unsigned)la_sizes[LA.size_i] / 1024, 1), "k");
	cat(it->value, size);
	pick_open(A_LA_SET, "LOGIC ANALYZER", "Capture", -1);
	P.filterable = 0;
}

static void la_choices(int which)
{
	static char names[N_RATES > 9 ? N_RATES : 9][16];

	pick_reset();
	switch (which) {
	case 0:
		for (int i = 0; i < N_RATES; i++) {
			names[i][0] = 0;
			hz(names[i], la_rates[i]);
			pick_add(names[i]);
		}
		pick_open(A_LA_RATE, "LOGIC ANALYZER", "Sample rate", LA.rate_i);
		break;
	case 1:
		pick_add("None");
		for (int i = 0; i < BP_PINS && i < 8; i++) {
			names[i][0] = 0;
			utoa(cat(names[i], "IO"), (unsigned)i, 1);
			pick_add(names[i]);
		}
		pick_open(A_LA_TRIG, "LOGIC ANALYZER", "Trigger", LA.trig + 1);
		break;
	case 2:
		pick_add(la_edges[0]);
		pick_add(la_edges[1]);
		pick_open(A_LA_EDGE, "LOGIC ANALYZER", "Edge", LA.falling);
		break;
	default:
		for (int i = 0; i < 3; i++) {
			names[i][0] = 0;
			cat(utoa(names[i], (unsigned)la_sizes[i] / 1024, 1), "k samples");
			pick_add(names[i]);
		}
		pick_open(A_LA_SIZE, "LOGIC ANALYZER", "Samples", LA.size_i);
		break;
	}
	P.filterable = 0;
	P.count[0] = 0;
}

/* Enter in a pick list. */
static void pick_done(struct input *in, enum screen *sc)
{
	char v[8];
	int i = pick_item();
	struct wnet *w;

	if (P.act == A_OPENWITH || P.act == A_FILEOPS) {
		br_pick_done(in, sc);
		return;
	}
	*sc = SC_LIST;
	if (i < 0)
		return;
	switch (P.act) {
	case A_TZ:
		tz_set(i - 1);		/* 0 is Automatic */
		break;
	case A_LORA_PRESET:
		lora_change(A_LORA_PRESET, i);
		break;
	case A_NETOPS:
		if (!(w = net_find(net_ssid)))
			break;
		if (!strcmp(P.it[i].label, "Connect"))
			wifi_connect((int)(w - N.nets), 0);
		else
			wifi_forget((int)(w - N.nets));
		break;
	case A_LA_SET:
		la_choices(i);
		*sc = SC_PICK;
		break;
	case A_LA_RATE:
		LA.rate_i = i;
		*sc = SC_LA;
		break;
	case A_LA_TRIG:
		LA.trig = i - 1;
		*sc = SC_LA;
		break;
	case A_LA_EDGE:
		LA.falling = i;
		*sc = SC_LA;
		break;
	case A_LA_SIZE:
		LA.size_i = i;
		*sc = SC_LA;
		break;
	case A_LTYPE:
		B.led_type = i;
		bp_leds(0);
		break;
	default:		/* A_LCOLOR */
		if (i == CUSTOM) {
			hexn(v, B.led_custom, 6);
			input_start(in, A_LCOLOR, 0, "LEDS", "Colour as RRGGBB (hex)", v, 0);
			in->fresh = 1;
			*sc = SC_INPUT;
		} else {
			B.led_color = i;
			bp_leds(0);
		}
		break;
	}
}

/* Enter in the input dialog: 0 when done, 1 to stay (in->err set). */
static int input_done(struct input *in)
{
	struct ipconf *c = &N.conf[in->kind];
	struct wnet *w;

	in->err = 0;
	switch (in->act) {
	case A_LBRIGHT:
	case A_LCOUNT: {
		int max = in->act == A_LBRIGHT ? 100 : 300, v = 0;
		const char *q = in->buf;

		while (*q >= '0' && *q <= '9' && v <= max)
			v = v * 10 + (*q++ - '0');
		if (*q || v < 1 || v > max) {
			in->err = max == 100 ? "A number from 1 to 100" : "A number from 1 to 300";
			return 1;
		}
		if (in->act == A_LBRIGHT)
			B.led_pct = v;
		else
			B.led_n = v;
		bp_leds(0);
		return 0;
	}
	case A_LCOLOR: {
		const char *q = in->buf;
		uint32_t v = 0;
		int n = 0;

		if (*q == '#')
			q++;
		else if (q[0] == '0' && (q[1] | 32) == 'x')
			q += 2;
		for (; *q && n < 7; q++, n++) {
			int d = *q >= '0' && *q <= '9' ? *q - '0' :
				(*q | 32) >= 'a' && (*q | 32) <= 'f' ? (*q | 32) - 'a' + 10 : -1;

			if (d < 0)
				break;
			v = v << 4 | (uint32_t)d;
		}
		if (*q || n != 6) {
			in->err = "Six hex digits, RRGGBB: FF8000 is orange";
			return 1;
		}
		B.led_custom = v;
		B.led_color = CUSTOM;
		bp_leds(0);
		return 0;
	}
	case A_ASK: {
		char path[80] = "";
		int fd;

		app_tmp(&A[in->kind], path, ".ask");
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd >= 0) {
			write(fd, in->buf, (size_t)in->len);
			close(fd);
		}
		run_app(in->kind, ask_file[0] ? ask_file : 0, in->buf);
		app_notice[0] = 0;
		cat(cat(app_notice, "Could not start "), A[in->kind].name);
		return 0;
	}
	case A_RENAME:
	case A_MKDIR:
		return br_input_done(in);
	case A_SETTING: {
		char key[sizeof(SET.s[0].key)];

		if (in->kind < 0 || in->kind >= SET.n)
			return 0;
		strcpy(key, SET.s[in->kind].key);
		settings_load(&SET);	/* the setting command may have changed others */
		if (settings_set(&SET, key, in->buf) || settings_save(&SET)) {
			in->err = "Could not save it";
			return 1;
		}
		return 0;
	}
	case A_NETWORK:
		if (in->len < 8) {
			in->err = "At least 8 characters";
			return 1;
		}
		w = net_find(in->ssid);
		if (w)
			wifi_connect((int)(w - N.nets), in->buf);
		return 0;
	case A_ADDR:
		if (!ip_ok(in->buf, 1, sizeof(in->buf)) || strlen(in->buf) >= sizeof(c->address)) {
			in->err = "Use a.b.c.d/len, for example 192.168.1.20/24";
			return 1;
		}
		memset(c->address, 0, sizeof(c->address));
		strncpy(c->address, in->buf, sizeof(c->address) - 1);
		break;
	case A_GW:
		if (in->len && (!ip_ok(in->buf, 0, 0) || in->len >= (int)sizeof(c->gateway))) {
			in->err = "Use a.b.c.d, or leave it empty";
			return 1;
		}
		memset(c->gateway, 0, sizeof(c->gateway));
		strncpy(c->gateway, in->buf, sizeof(c->gateway) - 1);
		break;
	case A_DNS:
		if (!dns_ok(in->buf) || in->len >= (int)sizeof(c->dns)) {
			in->err = "Addresses a.b.c.d, separated by spaces";
			return 1;
		}
		memset(c->dns, 0, sizeof(c->dns));
		strncpy(c->dns, in->buf, sizeof(c->dns) - 1);
		break;
	default:
		return 0;
	}
	conf_save(in->kind);
	return 0;
}

static void input_key(struct input *in, int key)
{
	char ch;

	in->err = 0;
	if (in->fresh && key != KEY_BACKSPACE && key_char(key, shift_down)) {
		in->len = in->cur = 0;
		in->buf[0] = 0;
	}
	in->fresh = 0;
	if (key == KEY_BACKSPACE) {
		if (in->cur) {
			memmove(in->buf + in->cur - 1, in->buf + in->cur, (size_t)(in->len - in->cur + 1));
			in->cur--;
			in->len--;
		}
	} else if (key == KEY_LEFT || key == KEY_RIGHT) {	/* the cursor (6.6) */
		in->cur += key == KEY_LEFT ? (in->cur > 0 ? -1 : 0) : (in->cur < in->len ? 1 : 0);
	} else if (key == KEY_TAB) {
		in->show = !in->show;
	} else if ((ch = key_char(key, shift_down)) && in->len < (int)sizeof(in->buf) - 1) {
		memmove(in->buf + in->cur + 1, in->buf + in->cur, (size_t)(in->len - in->cur + 1));
		in->buf[in->cur++] = ch;
		in->len++;
	}
}

/* A screen of the Bus Pirate's (not its lists). */
static int bp_screen(enum screen sc)
{
	return HAS_BP && (sc == SC_BP || sc == SC_LA || sc == SC_PINS || sc == SC_BPINFO);
}

/* What a pop-up (help, a pick list) is drawn over: the screen it came from. */
static enum screen help_back = SC_LIST, pick_back = SC_LIST, log_back = SC_LIST;

static enum screen under(enum screen sc)
{
	return sc == SC_HELP ? help_back : sc == SC_PICK ? pick_back : sc;
}

/* Draw screen sc (a pop-up's own part is drawn by the caller). */
static void draw_sc(enum screen sc, const struct list *l, int lfocus, const struct input *in,
		    int hfocus, int pfocus, int cfocus, int bfocus)
{
	switch (sc) {
	case SC_HOME:
		draw_home(hfocus);
		break;
	case SC_POWER:
		draw_power(hfocus, pfocus);
		break;
	case SC_LIST:
	case SC_HELP:
	case SC_PICK:
		draw_list(l, lfocus);
		break;
	case SC_INPUT:
		draw_list(l, lfocus);
		draw_input(in);
		break;
	case SC_CONFIRM:
		draw_list(l, lfocus);
		draw_delete_confirm(cfocus);
		break;
	case SC_FORMAT:
		draw_list(l, lfocus);
		draw_format_confirm(cfocus);
		break;
	case SC_LORA:
		draw_list(l, lfocus);
		draw_lora_confirm(cfocus);
		break;
	case SC_LOG:
		draw_log();
		break;
	case SC_BP:
		if (HAS_BP)
			draw_bp(bfocus);
		break;
	case SC_LA:
		if (HAS_BP)
			draw_la();
		break;
	case SC_PINS:
		if (HAS_BP)
			draw_pins();
		break;
	case SC_BPINFO:
		if (HAS_BP)
			draw_bpinfo();
		break;
	}
}

int main(int argc, char **argv)
{
	enum screen sc = SC_HOME;
	enum list_id stack[6];
	int focus_stack[6], depth = 0;

#define LID	stack[depth - 1]
#define LFOCUS	focus_stack[depth - 1]
#define PUSH(id)	do { stack[depth] = (id); focus_stack[depth] = -1; depth++; } while (0)

	static struct list l;	/* 96 rows: too big for our 16 KB stack */
	struct input in;
	int hfocus = 0, pfocus = 0, cfocus = 0, bfocus = 0, open_after = -1;
	long measured = 0;
	const char *checked_dev = 0;	/* --disk-checked: the disk that was checked */

	session = !APP_MODE && argc > 1 && !strcmp(argv[1], "--session");
	/* init starts us with HOME=/; apps (APP_CONFIG, the store) want root's */
	if (!getenv("HOME") || !strcmp(getenv("HOME"), "/")) {
		struct passwd *pw = getpwuid(getuid());
		setenv("HOME", pw && pw->pw_dir ? pw->pw_dir : "/root", 1);
	}
	popup = !session && !APP_MODE;
	/* back from doomrun or apprun: where to continue */
	if (argc > 2 && !strncmp(argv[2], "--app=", 6)) {
		const char *how = argc > 3 ? argv[3] : "";

		apps_read();
		if (strrchr(argv[2], '/'))	/* its log: the circle key */
			strncpy(last_app, strrchr(argv[2], '/') + 1, sizeof(last_app) - 1);
		for (int i = 0; i < n_apps; i++) {
			if (strcmp(A[i].dir, argv[2] + 6))
				continue;
			/* the store's "Open": the app it names, once we are set up */
			if (!strcmp(A[i].id, "store")) {
				char want[40];

				if (read_str("/tmp/store-open", want, sizeof(want)) > 0)
					for (int j = 0; j < n_apps; j++)
						if (!strcmp(A[j].id, want))
							open_after = j;
				unlink("/tmp/store-open");
			}
			PUSH(A[i].cat == CAT_GAMES ? L_GAMES : A[i].cat == CAT_TOOLS ? L_SETTINGS : L_APPS);
			if (A[i].files[0]) {
				F.app = i;
				PUSH(L_FILES);
			}
			sc = SC_LIST;
			if (!strcmp(how, "--app-failed"))
				cat(cat(cat(cat(app_notice, A[i].name), " did not start: see /tmp/app-"),
					strrchr(A[i].dir, '/') + 1), ".log");
			else if (!strcmp(how, "--app-oom"))
				cat(cat(app_notice, A[i].name), " was stopped: out of memory");
			else if (!strcmp(how, "--app-error"))
				cat(cat(cat(cat(app_notice, A[i].name), " stopped with an error: see /tmp/app-"),
					strrchr(A[i].dir, '/') + 1), ".log");
		}
	} else if (argc > 2 && !strncmp(argv[2], "--browse=", 9)) {
		const char *how = argc > 3 ? argv[3] : "";
		char *slash = strrchr(argv[2] + 9, '/');

		if (slash) {		/* back in the folder, on the file */
			*slash = 0;
			br_go(argv[2][9] ? argv[2] + 9 : "/", slash + 1);
		}
		PUSH(L_BROWSE);
		sc = SC_LIST;
		if (!strcmp(how, "--app-failed"))
			br_say("The app did not start: see its log in /tmp", "", 1);
		else if (!strcmp(how, "--app-oom"))
			br_say("The app was stopped: out of memory", "", 1);
		else if (!strcmp(how, "--app-error"))
			br_say("The app stopped with an error: see its log in /tmp", "", 1);
	} else if (argc > 4 && !strncmp(argv[2], "--disk-checked=", 15)) {
		/* back from a check (disk_check): its disk's screen, after setup() */
		checked_dev = argv[3];
		PUSH(L_SETTINGS);
		PUSH(L_SET_STORAGE);
		PUSH(L_DISKS);
		PUSH(L_DISK);
		sc = SC_LIST;
	} else if (argc > 2 && strcmp(argv[2], "--home")) {
		PUSH(L_GAMES);
		sc = SC_LIST;
	}
	if (app_notice[0])
		apps_notice = games_notice = tools_notice = app_notice;
	{			/* Files' settings (the cloud key in Files) */
		struct setting *v;

		settings_load(&SET);
		v = settings_find(&SET, "files.hidden");
		BR.hidden = v && !strcmp(v->value, "on");
	}
	if (setup() < 0) {
		/* In a session, never leave the panel without a shell:
		 * a respawn loop is fork churn, the kernel's wedge trigger. */
		if (session)
			execv(panel_getty[0], panel_getty);
		return 1;
	}
	if (checked_dev) {
		int i, rc = atoi(argv[2] + 15);

		disks_read();
		strncpy(disk_sel, checked_dev, sizeof(disk_sel) - 1);
		if ((i = fs_find(checked_dev)) >= 0) {
			const struct diskinfo *dk = fs_disk(&D.fs[i]);

			strncpy(disk_title, dk ? dk->label : "Disk", sizeof(disk_title) - 1);
			if (!strcmp(argv[4], "1") && fs_mount(&D.fs[i]) == 0 && dk && !dk->usb)
				store_maintain();
			disks_read();
		}
		disk_check_result(rc);
	}
	if (APP_MODE) {		/* the Bus Pirate app: straight to its screens */
		depth = 0;
		PUSH(L_APPS);
		sc = SC_LIST;
		if (bp_open() == 0) {
			bfocus = 0;
			sc = SC_BP;
		} else {
			apps_notice = B.notice;
		}
	}
	if (open_after >= 0) {
		int j = open_after;

		depth = 0;
		PUSH(A[j].cat == CAT_GAMES ? L_GAMES : A[j].cat == CAT_TOOLS ? L_SETTINGS : L_APPS);
		sc = SC_LIST;
		if (A[j].files[0]) {		/* its files first, as from the list */
			F.app = j;
			files_read();
			PUSH(L_FILES);
		} else {
			app_start(j, 0, &in, &sc);	/* returns only when it did not start */
		}
	}

	for (;;) {
		time_t now;
		int key, timeout;

		/* the app ends where the menu would go home (Esc, logo key) */
		if (APP_MODE && (sc == SC_HOME || sc == SC_POWER)) {
			bp_close();
			leave();
			return 0;
		}

		while (waitpid(-1, 0, WNOHANG) > 0)	/* ble_adv exiting */
			;
		sys_read();
		/* The Bus Pirate's connection lives as long as its screens. */
		if (HAS_BP && (bp_screen(under(sc)) || (depth && bp_list(LID)))) {
			if (bp_fd >= 0 && !B.present)
				bp_lost();
			if (bp_gone) {
				bp_gone = 0;
				apps_notice = B.notice;
				depth = 1;		/* Apps */
				sc = SC_LIST;
			} else if ((under(sc) == SC_PINS || (depth && LID == L_BP_PSU)) &&
				   now_ms() - measured >= 900) {
				bp_measure();
				measured = now_ms();
				continue;	/* it may have gone meanwhile */
			}
		} else if (HAS_BP && bp_fd >= 0) {
			bp_close();
		}
		/* the LoRa screen was left: the radio takes its new settings */
		if (lora_dirty && !(under(sc) != SC_HOME && under(sc) != SC_POWER && depth && LID == L_LORA))
			lora_apply();
		if (under(sc) != SC_HOME && under(sc) != SC_POWER && depth) {
			build(&l, LID);
			if (LID == L_BROWSE)
				LFOCUS = BR.focus_row;	/* Files keeps its own (BR.sel) */
			else if (LFOCUS < 0 || LFOCUS >= l.n || !focusable(&l.rows[LFOCUS]))
				LFOCUS = first_focus(&l);
		}
		draw_sc(under(sc), &l, depth ? LFOCUS : -1, &in, hfocus, pfocus, cfocus, bfocus);
		if (sc == SC_HELP) {
			if (bp_screen(help_back))
				help_bp(help_back);
			else if (help_back == SC_LOG)
				help_log();
			else
				help_list(&l, depth ? LFOCUS : -1);
		} else if (sc == SC_PICK) {
			draw_pick();
		}
		blit();

		/* Network screens follow wpa_supplicant and netd; the rest
		 * only needs the clock. */
		now = time(0);
		timeout = (int)(60 - now % 60) * 1000;
		if (timeout > 5000)
			timeout = 5000;	/* the status bar's Wi-Fi follows within seconds */
		if (sc == SC_LIST && (LID == L_NET_WIFI || LID == L_NET_USB || LID == L_SETTINGS ||
				      LID == L_SET_CONN || LID == L_SET_STORAGE || LID == L_DISKS || LID == L_DISK ||
				      (LID == L_BROWSE && br_drives())))
			timeout = 1500;	/* and disks come and go */
		if (sc == SC_PINS || (sc == SC_LIST && LID == L_BP_PSU))
			timeout = 1000;
		want_repeat = sc == SC_INPUT || (sc == SC_PICK && P.filterable);
		key = wait_key(timeout);
		if (key < 0) {
			say("home: keypad gone\n");
			leave();
			if (session)
				terminal();
			return 1;
		}
		if (key == 0)
			continue;
		if (key == KEY_ESC && fn_down) {
			screen_off();
			continue;
		}
		if (key == KEY_LEFTMETA) {
			if (sc == SC_HOME && popup) {
				leave();
				return 0;
			}
			sc = SC_HOME;
			depth = 0;
			continue;
		}

		/*
		 * The right three symbol keys do the same on every screen but the
		 * home screen and pop-ups (6.6): circle the log, cloud the
		 * settings, diamond this screen's help.
		 */
		if ((sc == SC_LIST || sc == SC_LOG || bp_screen(sc)) &&
		    (key == KEY_SYM_LOG || key == KEY_SYM_SETTINGS || key == KEY_SYM_HELP)) {
			int bp = HAS_BP && (bp_screen(sc) || (sc == SC_LIST && depth && bp_list(LID)));

			if (key == KEY_SYM_HELP) {
				help_back = sc;
				sc = SC_HELP;
			} else if (APP_MODE && !bp && key != KEY_SYM_HELP) {
				/* the Bus Pirate app with none connected: its own,
				 * empty screens, not the menu's */
				log_open(0, "BUS PIRATE", key == KEY_SYM_LOG ? "Console" : "Pins");
				log_none = "No Bus Pirate on the bottom port.";
				log_back = SC_LIST;
				sc = SC_LOG;
			} else if (key == KEY_SYM_LOG && bp) {
				bp_console();		/* the Bus Pirate's log is its console */
				sc = SC_HOME;		/* the logo key's place */
				depth = 0;
			} else if (key == KEY_SYM_LOG && sc != SC_LOG) {
				log_for(depth ? LID : L_SETTINGS);
				log_back = sc;
				sc = SC_LOG;
			} else if (key == KEY_SYM_SETTINGS && bp) {
				sc = SC_PINS;
				measured = 0;
			} else if (key == KEY_SYM_SETTINGS && sc == SC_LIST && LID == L_BROWSE) {
				if (depth < 6) {
					BR.notice[0] = 0;
					PUSH(L_FILES_SET);
				}
			} else if (key == KEY_SYM_SETTINGS && !(sc == SC_LIST && LID == L_FILES_SET)) {
				depth = 0;		/* the menu's settings are Settings */
				PUSH(L_SETTINGS);
				sc = SC_LIST;
			}
			continue;
		}

		if (sc == SC_HELP) {
			if (key == KEY_ESC || key == KEY_SYM_HELP)
				sc = help_back;
		} else if (sc == SC_LOG) {
			if (key == KEY_ESC)
				sc = log_back;
			else if (key == KEY_UP && log_top > 0)
				log_top--;
			else if (key == KEY_DOWN && log_top + LOG_ROWS < log_n)
				log_top++;
		} else if (sc == SC_HOME) {
			int open_tile = tiles_key(key, &hfocus);

			if (key == KEY_ESC && !session) {
				leave();
				return 0;
			}
			switch (open_tile) {
			case 0: case 1: case 2: case 4:
				depth = 0;
				if (open_tile == 2) {
					BR.notice[0] = 0;
					br_go("/", 0);
				}
				PUSH(open_tile == 0 ? L_APPS : open_tile == 1 ? L_GAMES :
				     open_tile == 2 ? L_BROWSE : L_SETTINGS);
				sc = SC_LIST;
				break;
			case 3:
				terminal();
				break;
			case 5:
				pfocus = 0;	/* the safe choice */
				sc = SC_POWER;
				break;
			}
		} else if (HAS_BP && sc == SC_BP) {
			int open_tile = -1;

			if (key == KEY_ESC) {
				bp_close();
				sc = APP_MODE ? SC_HOME : SC_LIST;	/* Apps */
				continue;
			}
			if (key == KEY_SYM_MAIN)
				open_tile = bfocus = 0;
			else if (key == KEY_SYM_SECOND)
				open_tile = bfocus = 1;
			else if (key < KEY_F1 || key > KEY_F6)
				open_tile = tiles_key(key, &bfocus);
			switch (open_tile) {
			case 0:
			case 2:
				if (depth < 6) {
					B.notice[0] = 0;
					PUSH(bfocus == 0 ? L_BP_PSU : L_BP_LED);
					sc = SC_LIST;
				}
				break;
			case 1:
				sc = SC_LA;
				break;
			case 3:
				bp_console();
				sc = SC_HOME;		/* the logo key's place */
				depth = 0;
				break;
			case 4:
				sc = SC_PINS;
				break;
			case 5:
				sc = SC_BPINFO;
				break;
			}
		} else if (HAS_BP && sc == SC_LA) {
			int span = la_span();

			if (key == KEY_ESC)
				sc = SC_BP;
			else if (key == KEY_SYM_MAIN)
				la_capture();
			else if (key == KEY_SYM_SECOND)
				la_save();
			else if (key == KEY_SYM_MENU) {
				la_options();
				pick_back = SC_LA;
				sc = SC_PICK;
			} else if (key == KEY_LEFT || key == KEY_RIGHT)
				LA.pos += (key == KEY_LEFT ? -span : span) / 4;
			else if (key == KEY_UP && LA.zoom > -3)
				LA.zoom--, LA.pos += span / 4;		/* about the middle */
			else if (key == KEY_DOWN && LA.zoom < la_fit(LA.n))
				LA.zoom++, LA.pos -= span / 2;
			span = la_span();
			if (LA.pos > LA.n - span)
				LA.pos = LA.n - span;
			if (LA.pos < 0)
				LA.pos = 0;
		} else if (HAS_BP && sc == SC_PINS) {
			if (key == KEY_ESC)
				sc = SC_BP;
			else if (key == KEY_SYM_MAIN)
				bp_psu(!B.psu);
			else if (key == KEY_SYM_SECOND)
				bp_pullups(!B.pullups);
			measured = 0;
		} else if (sc == SC_PICK) {
			char ch;

			if (key == KEY_UP || key == KEY_DOWN) {
				if (P.shown)
					P.focus = (P.focus + (key == KEY_UP ? P.shown - 1 : 1)) % P.shown;
			} else if (key == KEY_ESC) {
				sc = pick_back;
			} else if (key == KEY_ENTER) {
				enum screen back = pick_back;

				sc = SC_LIST;
				pick_done(&in, &sc);
				if (sc == SC_LIST && back != SC_LIST)
					sc = back;	/* the logic analyzer */
				if (sc == SC_PICK)
					pick_back = back;
				cfocus = 0;	/* Files: delete? starts on Cancel */
			} else if (P.filterable && key == KEY_BACKSPACE) {
				if (P.filter[0]) {
					P.filter[strlen(P.filter) - 1] = 0;
					pick_filter();
				}
			} else if (P.filterable && (ch = key_char(key, shift_down)) &&
				   strlen(P.filter) < sizeof(P.filter) - 1) {
				size_t n = strlen(P.filter);

				P.filter[n] = ch;
				P.filter[n + 1] = 0;
				pick_filter();
			}
		} else if (HAS_BP && sc == SC_BPINFO) {
			if (key == KEY_ESC)
				sc = SC_BP;
		} else if (sc == SC_POWER) {
			if (key == KEY_UP || key == KEY_DOWN)
				pfocus ^= 1;
			else if (key == KEY_ESC)
				sc = SC_HOME;
			else if (key == KEY_ENTER)
				power(pfocus == 0);
		} else if (sc == SC_CONFIRM) {
			if (key == KEY_UP || key == KEY_DOWN)
				cfocus ^= 1;
			else if (key == KEY_ESC)
				sc = SC_LIST;
			else if (key == KEY_ENTER) {
				sc = SC_LIST;
				if (cfocus == 1)
					br_delete();
			}
		} else if (sc == SC_LIST && depth && LID == L_BROWSE && br_drives()) {
			if (key == KEY_UP || key == KEY_DOWN) {
				int f = move_focus(&l, LFOCUS, key == KEY_UP ? -1 : 1);

				if (f >= 0 && l.rows[f].kind == R_DRIVE)
					BR.drive = l.rows[f].idx;
				BR.notice[0] = 0;
			} else if (key == KEY_ENTER && LFOCUS >= 0 && l.rows[LFOCUS].kind == R_DRIVE) {
				struct fsinfo *f = &D.fs[l.rows[LFOCUS].idx];

				/* not mounted: mount it, then open it (6.10) */
				BR.notice[0] = 0;
				if (!f->mnt[0]) {
					const struct diskinfo *dk = fs_disk(f);

					if (fs_mount(f) == 0 && dk && !dk->usb)
						store_maintain();
					disks_read();
				}
				if (f->mnt[0])
					br_go(f->mnt, 0);
				else
					br_say(D.notice[0] ? D.notice : "Could not mount it", "", 1);
			} else if (key == KEY_SYM_MAIN) {
				br_go("/", 0);
			} else if (key == KEY_SYM_SECOND) {
				br_go("/root", 0);
			} else if (key == KEY_ESC) {
				BR.notice[0] = 0;
				if (--depth == 0)
					sc = SC_HOME;
			}
		} else if (sc == SC_LIST && depth && LID == L_BROWSE) {
			if (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT) {
				/* the arrows across: a page */
				BR.sel += key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : key == KEY_LEFT ? -7 : 7;
				BR.sel = BR.sel < 0 ? 0 : BR.sel >= BR.n ? BR.n - 1 : BR.sel;
				BR.notice[0] = 0;
			} else if (key == KEY_ENTER) {
				br_open(&in, &sc);
				pick_back = SC_LIST;
			} else if (key == KEY_SYM_MENU) {
				br_options(&sc);
				pick_back = SC_LIST;
			} else if (key == KEY_SYM_MAIN) {
				BR.notice[0] = 0;
				br_go("/", 0);
			} else if (key == KEY_SYM_SECOND) {
				BR.notice[0] = 0;
				br_go("/root", 0);
			} else if (key == KEY_ESC) {
				BR.notice[0] = 0;
				br_up();
			}
		} else if (sc == SC_LORA) {
			if (key == KEY_UP || key == KEY_DOWN)
				cfocus ^= 1;
			else if (key == KEY_ESC)
				sc = SC_LIST;
			else if (key == KEY_ENTER) {
				sc = SC_LIST;
				if (cfocus == 1)
					lora_set(1);
				build(&l, LID);
			}
		} else if (sc == SC_FORMAT) {
			if (key == KEY_UP || key == KEY_DOWN)
				cfocus ^= 1;
			else if (key == KEY_ESC)
				sc = SC_LIST;
			else if (key == KEY_ENTER) {
				sc = SC_LIST;
				if (cfocus == 1 && fs_find(disk_sel) >= 0)
					disk_format(D.fs[fs_find(disk_sel)].disk);
			}
		} else if (sc == SC_INPUT) {
			if (key == KEY_ESC)
				sc = SC_LIST;
			else if (key == KEY_ENTER) {
				if (!input_done(&in))
					sc = SC_LIST;
			} else {
				input_key(&in, key);
			}
		} else if (sc == SC_LIST && depth) {
			struct row *r = LFOCUS >= 0 ? &l.rows[LFOCUS] : 0;

			if (key == KEY_ESC) {
				int was_bp = bp_list(LID);

				if (--depth == 0)
					sc = SC_HOME;
				else if (was_bp)
					sc = SC_BP;
			} else if (key == KEY_UP || key == KEY_DOWN) {
				LFOCUS = move_focus(&l, LFOCUS, key == KEY_UP ? -1 : 1);
			} else if ((key == KEY_LEFT || key == KEY_RIGHT) && !(r && r->kind == R_SLIDER)) {
				/* 6.6: sideways is a page in a list, never a value */
				for (int i = 0; i < 5; i++)
					LFOCUS = move_focus(&l, LFOCUS, key == KEY_LEFT ? -1 : 1);
			} else if (key == KEY_SYM_MENU) {
				if (r && r->kind == R_NETWORK) {
					net_options(&N.nets[r->idx]);
					pick_back = SC_LIST;
					sc = SC_PICK;
				}
			} else if (key == KEY_SYM_MAIN && HAS_BP && LID == L_BP_PSU) {
				bp_psu(!B.psu);
				measured = now_ms();
			} else if (r && bp_list(LID) && (key == KEY_ENTER || key == KEY_LEFT || key == KEY_RIGHT)) {
				if (key == KEY_ENTER && r->kind == R_SLIDER)
					continue;	/* the arrows change these */
				if (key == KEY_ENTER && bp_popup(r->act, &in, &sc)) {
					pick_back = SC_LIST;
					continue;
				}
				bp_row(r->act, key == KEY_LEFT ? -1 : 1);
				measured = now_ms();
			} else if (r && key == KEY_ENTER) {
				int kind, i;
				struct wnet *w;

				switch (r->act) {
				case A_BP:
					if (!HAS_BP)
						break;
					apps_notice = 0;
					if (bp_open() == 0) {
						bfocus = 0;
						sc = SC_BP;
					} else {
						apps_notice = B.notice;
					}
					break;
				case A_STORE:
				case A_MYAPPS:
					if ((i = store_app()) < 0)
						break;
					app_notice[0] = 0;
					run_back = 0;
					run_extra = r->act == A_MYAPPS ? "--my-apps" : 0;
					run_app(i, 0, 0);
					run_extra = 0;
					break;
				case A_APP:
					if (r->idx >= n_apps)
						break;
					app_notice[0] = 0;
					run_back = 0;
					if (A[r->idx].files[0] && depth < 6) {
						F.app = r->idx;
						files_read();
						PUSH(L_FILES);
						break;
					}
					app_start(r->idx, 0, &in, &sc);
					break;
				case A_FILE: {
					static char file[sizeof(A[0].files) + 50];

					if (F.app < 0 || r->idx >= F.n)
						break;
					run_back = 0;
					file[0] = 0;
					cat(cat(cat(file, A[F.app].files), "/"), F.file[r->idx]);
					app_start(F.app, file, &in, &sc);
					break;
				}
				case A_BLE:
					ble_toggle();
					break;
				case A_SUB:
					if (depth < 6)
						PUSH((enum list_id)r->idx);
					break;
				case A_LORA_PRESET:
					pick_preset();
					pick_back = SC_LIST;
					sc = SC_PICK;
					break;
				case A_TZ:
					pick_tz();
					pick_back = SC_LIST;
					sc = SC_PICK;
					break;
				case A_LORA:
					if (lora_wanted()) {
						lora_set(0);
					} else {
						cfocus = 0;	/* Cancel */
						sc = SC_LORA;
					}
					break;
				case A_KBD:
					kbd_toggle();
					break;
				case A_HIDDEN:
					BR.hidden = !BR.hidden;
					settings_load(&SET);
					if (!settings_set(&SET, "files.hidden", BR.hidden ? "on" : "off"))
						settings_save(&SET);
					BR.stale = 1;
					break;
				case A_USB:
				case A_NET_WIFI:
				case A_NET_USB:
					if (depth == 6)
						break;
					PUSH(r->act == A_USB ? L_USB :
					     r->act == A_NET_WIFI ? L_NET_WIFI : L_NET_USB);
					if (r->act == A_NET_WIFI) {
						N.notice[0] = 0;
						wifi_scan();
						scanning_until = time(0) + 5;
					}
					break;
				case A_IP:
					if (depth < 6)
						PUSH(LID == L_NET_WIFI ? L_IP_WIFI : L_IP_USB);
					break;
				case A_WIFI_ON:
					wifi_enable(!N.conf[NK_WIFI].enabled);
					break;
				case A_DHCP:
					kind = LID == L_IP_WIFI ? NK_WIFI : NK_USB;
					N.conf[kind].is_static = !N.conf[kind].is_static;
					conf_save(kind);
					break;
				case A_SCAN:
					N.notice[0] = 0;
					wifi_scan();
					scanning_until = time(0) + 5;
					break;
				case A_NETWORK:
					/* Enter connects; Forget is under the square key */
					w = &N.nets[r->idx];
					if (w->current) {
						break;
					} else if (w->saved_id >= 0) {
						wifi_connect(r->idx, 0);
					} else if (!w->supported) {
						notice(w->ssid, ": enterprise or WPA3-only, not supported", 1);
					} else if (w->secured) {
						char title[96];

						cat(cat(title, "Password for "), w->ssid);
						input_start(&in, A_NETWORK, NK_WIFI, "WI-FI", title, "", 1);
						strncpy(in.ssid, w->ssid, sizeof(in.ssid) - 1);
						sc = SC_INPUT;
					} else {
						wifi_connect(r->idx, 0);
					}
					break;
				case A_DISKS:
					if (depth < 6) {
						D.notice[0] = 0;
						PUSH(L_DISKS);
					}
					break;
				case A_DISK:
					if (depth < 6) {
						struct fsinfo *f = &D.fs[r->idx];
						const struct diskinfo *dk = fs_disk(f);

						memset(disk_sel, 0, sizeof(disk_sel));
						strncpy(disk_sel, f->dev, sizeof(disk_sel) - 1);
						memset(disk_title, 0, sizeof(disk_title));
						strncpy(disk_title, dk ? dk->label : "Disk", sizeof(disk_title) - 1);
						D.notice[0] = 0;
						PUSH(L_DISK);
					}
					break;
				case A_MOUNT:
					if ((i = fs_find(disk_sel)) >= 0)
						disk_mount_toggle(&D.fs[i]);
					break;
				case A_FSCK:
					if ((i = fs_find(disk_sel)) >= 0)
						disk_check(&D.fs[i]);
					break;
				case A_FORMAT:
					cfocus = 0;	/* Cancel */
					sc = SC_FORMAT;
					break;
				case A_SETTING:
					if (r->idx < 0 || r->idx >= SET.n)
						break;
					input_start(&in, A_SETTING, r->idx, "SETTINGS", SET.s[r->idx].label,
						    SET.s[r->idx].value, 0);
					sc = SC_INPUT;
					break;
				case A_ADDR:
				case A_GW:
				case A_DNS:
					kind = LID == L_IP_WIFI ? NK_WIFI : NK_USB;
					input_start(&in, r->act, kind, kind == NK_WIFI ? "WI-FI" : "WIRED (USB)",
						    r->label,
						    r->act == A_ADDR ? N.conf[kind].address :
						    r->act == A_GW ? N.conf[kind].gateway : N.conf[kind].dns, 0);
					sc = SC_INPUT;
					break;
				default:
					break;
				}
			} else if (r && r->kind == R_SLIDER && (key == KEY_LEFT || key == KEY_RIGHT)) {
				int dir = key == KEY_LEFT ? -1 : 1;

				if (r->act == A_BRIGHT)
					brightness_step(dir);
				else if (r->act == A_LORA_POWER)
					lora_change(A_LORA_POWER, dir);
			}
		}
	}
}

#endif
