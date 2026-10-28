// SPDX-License-Identifier: GPL-2.0-only
/*
 * sensorpanel — RV32 NOMMU FLAT program that draws live BME680 + BMI270
 * readings + a temperature graph at ~3 Hz into /dev/fb0 (RGB565). Built
 * for the WHY2025 badge running native Linux 6.18.35 LTS on the ESP32-P4
 * HP core. The "look, it's Linux!" demo for FOSDEM 2027.
 *
 * Drawn with the menu's library (libwb, the canvas "Menu's met
 * frontpaneel", Docs/06-design-system.md): the frontpanel's figures
 * behind, the status bar, "APP" / "Sensor panel", the readings on glass
 * cards (temperature, humidity, pressure; motion; the temperature of the
 * last 20 samples as a graph) and the hint bar. It reads no keys: Esc and
 * the logo key are apprun's (app.conf: ESC=quit).
 *
 * Design constraints (all learned the hard way; see CLAUDE.md / memory):
 * - Raw write(2)/read(2) only for its own I/O: uClibc stdio FILE* once
 *   dragged in pthread/locale globals whose R_RISCV_32 relocs elf2flt
 *   dropped on the floor → SIGSEGV at low addresses on the first
 *   FILE-touching call. libwb has no stdio either.
 * - No u64/u64 division in this file: the helper hung on the buildroot
 *   gcc 13.x + uClibc combo (observed on 13.3.0). libwb's glass divides
 *   64-bit values by constants, as the menu does on the badge.
 * - DON'T mmap /dev/fb0. drm_fbdev_dma's fb_mmap path doesn't work on
 *   this NOMMU build (returns -EINVAL or -ENOMEM); witnessed via
 *   "sensorpanel: mmap failed" in /tmp/sp.log. Use the same pattern
 *   fbdoom uses: anonymous-mmap an off-screen shadow (goes through the
 *   nommu_userspace_pool reservation in DT), draw into it (libwb's fb),
 *   then a single pwrite() per frame blits to /dev/fb0. drm_fbdev_dma's
 *   write_iter handler does the dirty notification → pipe_update
 *   rotates + scans out.
 * - Match IIO devices by name, not by iio:device0/1 ordering — probe
 *   order depends on which device wins the i2c-gpio race per boot.
 *
 * Built by sdapps/build.sh (sensorpanel), with -I libwb. With
 * -DSENSORPANEL_HOST it draws two frames of made-up readings as PPM
 * instead (sensorpanel/test).
 */

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <linux/fb.h>

#include "wb_tokens.h"

#define W	WB_SCREEN_W
#define H	WB_SCREEN_H
#include "wb.h"
#include "wbstatus.h"

static int      fb_fd = -1;     /* open fd to /dev/fb0 */
static size_t   fb_bytes;       /* line_length * yres */

/* ---- I/O helpers (no FILE*) ---- */

/*
 * Read a sysfs file into the supplied buffer. Returns bytes read or -1.
 * Trims the trailing newline so the caller can treat it as a C string.
 */
static int slurp(const char *path, char *buf, int max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0) return -1;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) n--;
    buf[n] = 0;
    return n;
}

/* Parse a possibly-signed integer from str, stop at first non-digit. */
static int parse_int(const char *s, int *out)
{
    int sign = 1, v = 0, any = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++; any = 1;
    }
    if (!any) return -1;
    *out = sign * v;
    return 0;
}

/*
 * Parse "INT.FRAC" into integer*scale (e.g. 3 decimal digits → milli-).
 * Returns 0 on success, -1 on parse failure. Output is signed micro/milli
 * units depending on scale_digits. Truncates extra fractional digits.
 */
static int parse_fixed(const char *s, int scale_digits, int *out)
{
    int sign = 1, ip = 0, fp = 0, fdigits = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        ip = ip * 10 + (*s - '0');
        s++; any = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && fdigits < scale_digits) {
            fp = fp * 10 + (*s - '0');
            s++; fdigits++;
        }
        while (*s >= '0' && *s <= '9') s++;     /* skip extra precision */
    }
    while (fdigits < scale_digits) { fp *= 10; fdigits++; }
    if (!any) return -1;
    int scale = 1;
    for (int i = 0; i < scale_digits; i++) scale *= 10;
    *out = sign * (ip * scale + fp);
    return 0;
}

/* ---- Number formatting ---- */

/*
 * Render a fixed-point integer into buf as decimal with `decimals`
 * places (e.g. value=3198, scale=2 → "31.98"). value is signed.
 * Returns number of characters written (excluding the NUL).
 */
static int fmt_fixed(char *buf, int max, int value, int decimals, int width)
{
    int neg = 0;
    if (value < 0) { neg = 1; value = -value; }
    int scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    int ip = value / scale;
    int fp = value % scale;
    /* Build into a temporary then space-pad-left for alignment. */
    char tmp[32]; int n = 0;
    /* fractional part (LSD-first) */
    int fpd = decimals;
    while (fpd--) { tmp[n++] = '0' + (fp % 10); fp /= 10; }
    if (decimals > 0) tmp[n++] = '.';
    /* integer part (LSD-first) */
    if (ip == 0) tmp[n++] = '0';
    while (ip > 0) { tmp[n++] = '0' + (ip % 10); ip /= 10; }
    if (neg) tmp[n++] = '-';
    /* reverse + pad */
    int pad = (width > n) ? (width - n) : 0;
    int out = 0;
    while (pad-- && out < max - 1) buf[out++] = ' ';
    while (n-- && out < max - 1) buf[out++] = tmp[n];
    buf[out] = 0;
    return out;
}

/* ---- IIO device discovery ---- */

#define BME680_PATH_LEN 64
static char bme680_dir[BME680_PATH_LEN];   /* e.g. /sys/bus/iio/devices/iio:device0 */
static char bmi270_dir[BME680_PATH_LEN];

static int find_iio_devs(void)
{
    bme680_dir[0] = bmi270_dir[0] = 0;
    char namepath[BME680_PATH_LEN + 8];
    char namebuf[32];
    /* iio:device0..7 is the possible range; ten is overkill but cheap. */
    for (int i = 0; i < 10; i++) {
        int n = 0;
        const char *p = "/sys/bus/iio/devices/iio:device";
        while (*p && n < BME680_PATH_LEN - 1) bme680_dir[n++] = *p++;
        bme680_dir[n++] = '0' + i;
        bme680_dir[n] = 0;
        /* probe by reading $dir/name */
        for (int j = 0; bme680_dir[j]; j++) namepath[j] = bme680_dir[j];
        int dl = 0; while (bme680_dir[dl]) dl++;
        namepath[dl++] = '/';
        const char *nm = "name";
        while (*nm) namepath[dl++] = *nm++;
        namepath[dl] = 0;
        if (slurp(namepath, namebuf, sizeof namebuf) < 0) {
            bme680_dir[0] = 0;
            continue;
        }
        if (strcmp(namebuf, "bme680") == 0) {
            /* keep current bme680_dir */
        } else if (strcmp(namebuf, "bmi270") == 0) {
            int k = 0; while (bme680_dir[k]) { bmi270_dir[k] = bme680_dir[k]; k++; }
            bmi270_dir[k] = 0;
            bme680_dir[0] = 0;     /* clear so the next match for bme680 wins */
        } else {
            bme680_dir[0] = 0;
        }
    }
    /* Re-scan once to find bme680 even after we cleared its slot. */
    if (bme680_dir[0] == 0) {
        for (int i = 0; i < 10; i++) {
            int n = 0;
            const char *p = "/sys/bus/iio/devices/iio:device";
            while (*p && n < BME680_PATH_LEN - 1) bme680_dir[n++] = *p++;
            bme680_dir[n++] = '0' + i;
            bme680_dir[n] = 0;
            for (int j = 0; bme680_dir[j]; j++) namepath[j] = bme680_dir[j];
            int dl = 0; while (bme680_dir[dl]) dl++;
            namepath[dl++] = '/';
            const char *nm = "name";
            while (*nm) namepath[dl++] = *nm++;
            namepath[dl] = 0;
            if (slurp(namepath, namebuf, sizeof namebuf) < 0) { bme680_dir[0] = 0; continue; }
            if (strcmp(namebuf, "bme680") == 0) break;
            bme680_dir[0] = 0;
        }
    }
    return (bme680_dir[0] != 0) || (bmi270_dir[0] != 0) ? 0 : -1;
}

static int read_attr(const char *dir, const char *attr, char *buf, int max)
{
    char path[96];
    int n = 0;
    while (*dir && n < (int)sizeof(path) - 1) path[n++] = *dir++;
    if (n < (int)sizeof(path) - 1) path[n++] = '/';
    while (*attr && n < (int)sizeof(path) - 1) path[n++] = *attr++;
    path[n] = 0;
    return slurp(path, buf, max);
}

/* ---- App state ---- */

#define SPARK_W   600       /* pixels of sparkline area */
#define SPARK_H    80
#define SPARK_N    20       /* sample count (downsampled to SPARK_W) */
static int temp_hist[SPARK_N];   /* in centi-degC */
static int temp_hist_n;
static int temp_hist_min, temp_hist_max;

static void hist_push(int v)
{
    if (temp_hist_n < SPARK_N) {
        temp_hist[temp_hist_n++] = v;
    } else {
        for (int i = 1; i < SPARK_N; i++) temp_hist[i-1] = temp_hist[i];
        temp_hist[SPARK_N - 1] = v;
    }
    /* recompute extents */
    temp_hist_min = temp_hist_max = temp_hist[0];
    for (int i = 1; i < temp_hist_n; i++) {
        if (temp_hist[i] < temp_hist_min) temp_hist_min = temp_hist[i];
        if (temp_hist[i] > temp_hist_max) temp_hist_max = temp_hist[i];
    }
    /* widen if values clustered to keep the sparkline non-degenerate */
    if (temp_hist_max - temp_hist_min < 40) {
        int mid = (temp_hist_max + temp_hist_min) / 2;
        temp_hist_min = mid - 20;
        temp_hist_max = mid + 20;
    }
}
/* ---- Drawing (libwb) ---- */

#define CARD_X   WB_CONTENT_PAD
#define CARD_W   (W - 2 * WB_CONTENT_PAD)
#define CARD_GAP 16
#define ROW1_Y   WB_ROWS_TOP            /* temperature, humidity, pressure */
#define ROW1_H   132
#define ROW2_Y   (ROW1_Y + ROW1_H + CARD_GAP)   /* motion */
#define ROW2_H   112
#define ROW3_Y   (ROW2_Y + ROW2_H + CARD_GAP)   /* the graph */
#define ROW3_H   (H - WB_HINT_BAR_H - CARD_GAP - ROW3_Y)

static struct {                 /* the status bar */
    char hhmm[6];
    int clock_ok, wifi, lora;
} S;

static void status_read(void)
{
    time_t t = time(0);
    struct tm tm;

    localtime_r(&t, &tm);
    S.clock_ok = tm.tm_year >= 124;
    S.hhmm[0] = (char)('0' + tm.tm_hour / 10);
    S.hhmm[1] = (char)('0' + tm.tm_hour % 10);
    S.hhmm[2] = ':';
    S.hhmm[3] = (char)('0' + tm.tm_min / 10);
    S.hhmm[4] = (char)('0' + tm.tm_min % 10);
    S.hhmm[5] = 0;
    S.wifi = wifi_bars();       /* asks wpa_supplicant at most every 5 s */
    S.lora = lora_on();
}

/* WhyOS left; LoRa, Wi-Fi and the clock right (as the menu's). */
static void status_bar(void)
{
    const struct wb_font *n = &f_ui18b;
    int cy = WB_STATUS_BAR_H / 2;

    text(n, text(n, WB_CHROME_PAD, text_top(n, cy), "Why", WB_COLOR_TEXT_HI, -3),
         text_top(n, cy), "OS", WB_COLOR_CYAN, -3);
    status_right(S.hhmm, S.clock_ok, S.wifi, S.lora);
}

/* A card's caption: mono, upper case, tracked, in text mid. */
static void caption(int x, int y, const char *s)
{
    text(&f_mono13, x, y, s, WB_COLOR_TEXT_MID, 21);
}

/* A big value with its unit after it on the same baseline; "–" and the
 * sensor's name when it is not there. */
static void reading(int x, int y, const char *v, const char *unit, int have, const char *sensor)
{
    if (!have) {
        int e = text(&f_ui36b, x, y, "\xe2\x80\x93", WB_COLOR_TEXT_LOW, -11);

        text(&f_ui16, e + 12, y + f_ui36b.ascent - f_ui16.ascent, sensor, WB_COLOR_TEXT_LOW, 0);
        return;
    }
    x = text(&f_ui36b, x, y, v, WB_COLOR_TEXT_HI, -11);
    text(&f_ui20, x + 6, y + f_ui36b.ascent - f_ui20.ascent, unit, WB_COLOR_TEXT_MID, 0);
}

/* One of the three cards of the first row. */
static void env_card(int x, int w, const char *label, const char *v, const char *unit, int have)
{
    wb_card(x, ROW1_Y, w, ROW1_H, WB_RADIUS_MD, 0, 0);
    caption(x + 20, ROW1_Y + 22, label);
    reading(x + 20, ROW1_Y + ROW1_H - 24 - f_ui36b.ascent, v, unit, have, "no BME680");
}

/* The temperature of the last samples (centi-°C): a cyan line over a
 * faint fill, anti-aliased down the columns, the samples as squares. */
static void graph(int x0, int y0, int w, int h)
{
    int range = temp_hist_max - temp_hist_min, prev16 = 0;

    for (int i = 1; i < 4; i++)         /* a faint grid */
        for (int x = x0; x < x0 + w; x++)
            put(x, y0 + i * h / 4, 0xFFFFFF, 18);
    if (temp_hist_n < 2) {
        const char *m = "Collecting samples\xe2\x80\xa6";

        text(&f_ui18, x0 + (w - text_w(&f_ui18, m, 0)) / 2, text_top(&f_ui18, y0 + h / 2), m,
             WB_COLOR_TEXT_MID, 0);
        return;
    }
    if (range < 1)
        range = 1;
    for (int x = 0; x < w; x++) {
        /* sample position of this column, in 1/256 */
        int p = x * (SPARK_N - 1) * 256 / (w - 1), i = p >> 8, f = p & 255;
        int a, b, v, y16, top16, bot16;

        if (i >= temp_hist_n - 1) {     /* the history is not full yet */
            if (i > temp_hist_n - 1 || f)
                break;
            f = 0;
        }
        a = temp_hist[i];
        b = i + 1 < temp_hist_n ? temp_hist[i + 1] : a;
        v = a + (b - a) * f / 256;
        y16 = (y0 + h - 6) * 16 - (v - temp_hist_min) * (h - 12) * 16 / range;
        if (!x)
            prev16 = y16;
        top16 = (y16 < prev16 ? y16 : prev16) - 16;
        bot16 = (y16 > prev16 ? y16 : prev16) + 16;
        prev16 = y16;
        for (int py = top16 / 16; py <= bot16 / 16; py++) {
            int lo = py * 16 > top16 ? py * 16 : top16, hi = py * 16 + 16 < bot16 ? py * 16 + 16 : bot16;

            if (hi > lo)
                put(x0 + x, py, WB_COLOR_CYAN, (unsigned)(hi - lo) * 255 / 16);
        }
        for (int py = bot16 / 16 + 1; py < y0 + h; py++)        /* the fill under it */
            put(x0 + x, py, WB_COLOR_CYAN, (unsigned)(40 * (y0 + h - py) / (y0 + h - bot16 / 16)));
    }
    for (int i = 0; i < temp_hist_n; i++) {     /* the samples */
        int x = x0 + i * (w - 1) / (SPARK_N - 1);
        int y = y0 + h - 6 - (temp_hist[i] - temp_hist_min) * (h - 12) / range;
        int s = i == temp_hist_n - 1 ? 10 : 6;

        box(x - s / 2, y - s / 2, s, s, 2, i == temp_hist_n - 1 ? WB_COLOR_PINK : WB_COLOR_CYAN, 255, 0, 0, 0);
    }
}

static void render_frame(int frame, int have_bme, int have_bmi,
                         int t_centi, int rh_centi, int p_pa_div10,
                         int ax_mg, int ay_mg, int az_mg)
{
    static const struct hint hints[] = { { "Esc", "close", 1 } };
    char buf[64], foot[96];
    int w3 = (CARD_W - 2 * CARD_GAP) / 3;

    wb_scene();
    status_bar();
    wb_header("APP", "Sensor panel", 0);

    /* --- Environment: temperature, humidity, pressure (BME680) --- */
    fmt_fixed(buf, sizeof buf, t_centi, 2, 0);          /* 3198 -> 31.98 */
    env_card(CARD_X, w3, "TEMPERATURE", buf, "\xc2\xb0" "C", have_bme);
    fmt_fixed(buf, sizeof buf, rh_centi, 2, 0);
    env_card(CARD_X + w3 + CARD_GAP, w3, "HUMIDITY", buf, "%", have_bme);
    fmt_fixed(buf, sizeof buf, p_pa_div10, 1, 0);       /* deci-kPa: 1843 -> 184.3 */
    env_card(CARD_X + CARD_W - w3, w3, "PRESSURE", buf, "kPa", have_bme);

    /* --- Motion: the acceleration on three axes (BMI270) --- */
    wb_card(CARD_X, ROW2_Y, CARD_W, ROW2_H, WB_RADIUS_MD, 0, 0);
    caption(CARD_X + 20, ROW2_Y + 22, "MOTION");
    if (have_bmi) {
        static const char *const axis[3] = { "X", "Y", "Z" };
        int vals[3] = { ax_mg, ay_mg, az_mg };
        int cw = (CARD_W - 40) / 3;

        for (int i = 0; i < 3; i++) {
            int x = CARD_X + 20 + i * cw, y = ROW2_Y + ROW2_H - 22 - f_ui28b.ascent;

            buf[0] = vals[i] < 0 ? '-' : '+';
            fmt_fixed(buf + 1, sizeof buf - 1, vals[i] < 0 ? -vals[i] : vals[i], 3, 0);
            x = text(&f_mono16b, x, y + f_ui28b.ascent - f_mono16b.ascent, axis[i], WB_COLOR_CYAN, 0);
            x = text(&f_ui28b, x + 12, y, buf, WB_COLOR_TEXT_HI, -4);
            text(&f_ui18, x + 6, y + f_ui28b.ascent - f_ui18.ascent, "g", WB_COLOR_TEXT_MID, 0);
        }
    } else {
        reading(CARD_X + 20, ROW2_Y + ROW2_H - 22 - f_ui36b.ascent, 0, 0, 0, "no BMI270");
    }

    /* --- The temperature graph --- */
    wb_card(CARD_X, ROW3_Y, CARD_W, ROW3_H, WB_RADIUS_MD, 0, 0);
    caption(CARD_X + 20, ROW3_Y + 22, "TEMPERATURE, LAST 20 SAMPLES");
    if (temp_hist_n) {              /* the samples' range, right */
        int lo = temp_hist[0], hi = temp_hist[0], n;

        for (int i = 1; i < temp_hist_n; i++) {
            lo = temp_hist[i] < lo ? temp_hist[i] : lo;
            hi = temp_hist[i] > hi ? temp_hist[i] : hi;
        }
        n = fmt_fixed(foot, sizeof foot, lo, 2, 0);
        memcpy(foot + n, " \xe2\x80\x93 ", 5);
        n += 5;
        n += fmt_fixed(foot + n, (int)sizeof foot - n, hi, 2, 0);
        memcpy(foot + n, " \xc2\xb0" "C", 5);
        text(&f_mono14, CARD_X + CARD_W - 20 - text_w(&f_mono14, foot, 0), ROW3_Y + 21, foot,
             WB_COLOR_TEXT_MID, 0);
    }
    graph(CARD_X + 20, ROW3_Y + 52, CARD_W - 40, ROW3_H - 52 - 20);

    /* --- The hint bar: Esc; left of it the system and a heartbeat --- */
    hint_bar(hints, 1);
    {
        struct utsname u;
        int n = 0;
        const char *p;

        p = "Linux ";
        while (*p) foot[n++] = *p++;
        if (uname(&u) == 0)
            for (p = u.release; *p && n < 30; ) foot[n++] = *p++;
        p = " \xc2\xb7 RV32 NOMMU \xc2\xb7 frame ";
        while (*p) foot[n++] = *p++;
        fmt_fixed(foot + n, (int)sizeof foot - n, frame, 0, 0);
        text(&f_mono14, WB_CHROME_PAD, text_top(&f_mono14, H - WB_HINT_BAR_H / 2), foot,
             WB_COLOR_TEXT_LOW, 0);
    }
}


#ifdef SENSORPANEL_HOST
/*
 * sensorpanel-host OUT.ppm MISSING.ppm: a frame with made-up readings and
 * a full history, and the first frame of a badge without its sensors.
 */
static void ppm(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static unsigned char row[W * 3];
    const char *head = "P6\n720 720\n255\n";

    write(fd, head, strlen(head));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t p = fb[y * W + x];

            row[3 * x] = (unsigned char)((p >> 11) << 3);
            row[3 * x + 1] = (unsigned char)(((p >> 5) & 63) << 2);
            row[3 * x + 2] = (unsigned char)((p & 31) << 3);
        }
        write(fd, row, sizeof row);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    static uint16_t shadow[W * H];

    if (argc < 3)
        return 2;
    fb = shadow;
    fb_y0 = 0;
    fb_y1 = H;
    wb_init();
    status_read();
    for (int i = 0; i < SPARK_N; i++)
        hist_push(2140 + (i * 7) % 23 + i * 3);
    render_frame(1234, 1, 1, 2203, 4187, 2351, 12, -998, 31);
    ppm(argv[1]);
    temp_hist_n = 0;
    render_frame(0, 0, 0, 0, 0, 0, 0, 0, 0);
    ppm(argv[2]);
    return 0;
}
#else
int main(void)
{
    write(1, "sensorpanel: start\n", 19);

    /* --- Open framebuffer (no mmap on /dev/fb0 — see header comment) --- */
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        write(2, "sensorpanel: open /dev/fb0 failed\n", 34);
        return 1;
    }
    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        write(2, "sensorpanel: FBIOGET_*SCREENINFO failed\n", 40);
        return 1;
    }
    /* libwb draws a panel of W x H pixels, W to a line */
    if (vi.bits_per_pixel != 16 || vi.xres != W || vi.yres != H || fi.line_length != W * 2) {
        write(2, "sensorpanel: not a 720x720 RGB565 panel\n", 40);
        return 1;
    }

    /* Allocate the shadow as an anonymous mmap. On this NOMMU kernel that
     * routes through nommu_userspace_pool (10 MB DT-reserved at 0x49600000),
     * giving us one contiguous block. Drawing into the shadow then a single
     * pwrite() per frame blits the whole panel atomically. */
    fb_bytes = (size_t)fi.line_length * vi.yres;
    fb = (uint16_t *)mmap(NULL, fb_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fb == (uint16_t *)-1) {
        write(2, "sensorpanel: shadow mmap failed\n", 32);
        return 1;
    }
    fb_y0 = 0;
    fb_y1 = H;
    wb_init();
    write(1, "sensorpanel: fb + shadow ready\n", 31);

    /* --- Discover IIO devices, with retry --- */
    /* BMI270 enumerates a couple seconds *after* init starts because of
     * S05bmi270 (post-rootfs-mount sysfs rebind). Retry every 250 ms for
     * up to 5 s so we don't have to coordinate startup ordering. */
    {
        int attempts = 20;
        while (attempts--) {
            find_iio_devs();
            if (bme680_dir[0] && bmi270_dir[0]) break;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 250 * 1000 * 1000 };
            nanosleep(&ts, 0);
        }
    }
    write(1, "sensorpanel: iio scan: bme=", 27);
    write(1, bme680_dir[0] ? "y" : "n", 1);
    write(1, " bmi=", 5);
    write(1, bmi270_dir[0] ? "y" : "n", 1);
    write(1, "\n", 1);

    /* --- Read BMI270 scale ONCE (constant per chip range). --- */
    /* in_accel_scale is m/s² per LSB, e.g. "0.000598624" at ±2 g. */
    int accel_scale_micro = 598;     /* 0.000598624 → 598 µ(m/s²)/LSB */
    if (bmi270_dir[0]) {
        char tmp[64];
        if (read_attr(bmi270_dir, "in_accel_scale", tmp, sizeof tmp) > 0) {
            int v;
            if (parse_fixed(tmp, 9, &v) == 0) {
                /* v is now scale × 1e9. We want micro, so / 1000. */
                accel_scale_micro = v / 1000;
                if (accel_scale_micro < 1) accel_scale_micro = 1;
            }
        }
    }

    int frame = 0;
    struct timespec next;

    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        char buf[64];

        int t_centi = 0, rh_centi = 0, p_milli = 0;
        int have_bme = 0;
        if (bme680_dir[0]) {
            if (read_attr(bme680_dir, "in_temp_input", buf, sizeof buf) > 0) {
                int v;
                if (parse_int(buf, &v) == 0) {
                    /* milli-°C → centi-°C: divide by 10. */
                    t_centi = v / 10;
                    have_bme = 1;
                }
            }
            if (read_attr(bme680_dir, "in_humidityrelative_input", buf, sizeof buf) > 0) {
                int v;
                /* Driver emits "24.023000000". 2 decimals → centi-%. */
                if (parse_fixed(buf, 2, &v) == 0) {
                    rh_centi = v;
                }
            }
            if (read_attr(bme680_dir, "in_pressure_input", buf, sizeof buf) > 0) {
                int v;
                /* Driver emits kPa with 9-digit fraction; we keep 1 decimal
                 * place so render_frame's 1-decimal formatter prints "100.3"
                 * for typical sea-level. (This BME680 family chip on the
                 * WHY2025 reads ~225-250 kPa instead of ~100 kPa — a known
                 * calibration coefficient mismatch between BME680 vs
                 * BME688/690, not a unit bug; see HARDWARE.md.) */
                if (parse_fixed(buf, 1, &v) == 0) {
                    p_milli = v;            /* deci-kPa */
                }
            }
        }

        int ax_raw = 0, ay_raw = 0, az_raw = 0;
        int have_bmi = 0;
        if (bmi270_dir[0]) {
            int v;
            if (read_attr(bmi270_dir, "in_accel_x_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { ax_raw = v; have_bmi = 1; }
            if (read_attr(bmi270_dir, "in_accel_y_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { ay_raw = v; }
            if (read_attr(bmi270_dir, "in_accel_z_raw", buf, sizeof buf) > 0 &&
                parse_int(buf, &v) == 0) { az_raw = v; }
        }

        /* Convert raw → milli-g.
         *   m_per_s2 = raw * accel_scale_micro * 1e-6
         *   g       = m_per_s2 / 9.80665
         *   mg      = g * 1000 = raw * accel_scale_micro / 9806.65
         *   ≈ raw * accel_scale_micro / 9807   (one in 0.04 % error)
         * All math fits in 32-bit signed: raw in [-32768,32767],
         * scale_micro typically 598, product ~2e7. */
        int ax_mg = (ax_raw * accel_scale_micro) / 9807;
        int ay_mg = (ay_raw * accel_scale_micro) / 9807;
        int az_mg = (az_raw * accel_scale_micro) / 9807;

        if (have_bme) hist_push(t_centi);

        status_read();
        render_frame(frame, have_bme, have_bmi,
                     t_centi, rh_centi, p_milli,
                     ax_mg, ay_mg, az_mg);

        /* Blit shadow → /dev/fb0 in one shot. drm_fbdev_dma's write
         * helper marks the affected pages dirty; pipe_update then
         * rotates + scans out. */
        ssize_t r = pwrite(fb_fd, fb, fb_bytes, 0);
        if (r != (ssize_t)fb_bytes) {
            write(2, "sensorpanel: short pwrite\n", 26);
        }

        if ((frame & 0x1f) == 0) {
            /* Heartbeat every ~10 s so we can tell the loop is alive
             * via /tmp/sp.log without flooding it. */
            write(1, "sensorpanel: tick\n", 18);
        }

        frame++;

        /* ~3 Hz: the next frame 333 ms after the last one started,
         * drawing included (the scene and the glass take a while) */
        next.tv_nsec += 333 * 1000 * 1000;
        if (next.tv_nsec >= 1000 * 1000 * 1000) {
            next.tv_nsec -= 1000 * 1000 * 1000;
            next.tv_sec++;
        }
        {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > next.tv_sec ||
                (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec)) {
                next = now;     /* fell behind: from now on */
            } else {
                struct timespec ts;

                ts.tv_sec = next.tv_sec - now.tv_sec;
                ts.tv_nsec = next.tv_nsec - now.tv_nsec;
                if (ts.tv_nsec < 0) {
                    ts.tv_nsec += 1000 * 1000 * 1000;
                    ts.tv_sec--;
                }
                nanosleep(&ts, 0);
            }
        }
    }

    return 0;       /* unreached */
}
#endif
