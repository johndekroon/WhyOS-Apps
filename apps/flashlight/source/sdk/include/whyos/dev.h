/* SPDX-License-Identifier: MIT */
/*
 * whyos/dev.h: the badge's sensors, as an app reads them. The BME680
 * (temperature, humidity, pressure) and the BMI270 (acceleration) are IIO
 * devices; their number (iio:device0, 1, ...) depends on which probed
 * first, so they are found by name. Reading them needs no permission.
 * Part of the WhyOS SDK: include whyos.h.
 */
#pragma once

#define WOS_IIO		"/sys/bus/iio/devices/iio:device"

/* A sysfs value as text, without its newline; its length, or -1. */
static int wos_sysfs(const char *dir, const char *attr, char *buf, int size)
{
	char path[128] = "";
	int n;

	wos_add(wos_add(wos_add(path, dir, sizeof(path)), "/", sizeof(path)), attr, sizeof(path));
	n = wos_read_file(path, buf, size);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return n;
}

/* The directory of the IIO device called name ("bme680", "bmi270") into
 * dir; 0 when it is there. */
static int wos_iio_find(const char *name, char *dir, int size)
{
	char b[32];

	for (int i = 0; i < 10; i++) {
		dir[0] = 0;
		wos_add_int(wos_copy(dir, WOS_IIO, size), i, size);
		if (wos_sysfs(dir, "name", b, sizeof(b)) > 0 && !strcmp(b, name))
			return 0;
	}
	dir[0] = 0;
	return -1;
}

/* "INT.FRAC" as an integer with digits decimals (24.5 with 2: 2450); the
 * extra digits are cut off. 0 when it is a number. */
static int wos_fixed(const char *s, int digits, long *out)
{
	long ip = 0, fp = 0, scale = 1;
	int neg = 0, any = 0, got = 0;

	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	while (*s >= '0' && *s <= '9') {
		ip = ip * 10 + (*s++ - '0');
		any = 1;
	}
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++)
			if (got < digits) {
				fp = fp * 10 + (*s - '0');
				got++;
			}
	for (; got < digits; got++)
		fp *= 10;
	for (int i = 0; i < digits; i++)
		scale *= 10;
	if (!any)
		return -1;
	*out = neg ? -(ip * scale + fp) : ip * scale + fp;
	return 0;
}

/* v (with digits decimals) as text added to the end of d: 3198, 2: "31.98" */
static char *wos_add_fixed(char *d, long v, int digits, int size)
{
	long scale = 1;
	char frac[12] = "";

	for (int i = 0; i < digits; i++)
		scale *= 10;
	if (v < 0) {
		wos_add(d, "-", size);
		v = -v;
	}
	wos_add_int(d, v / scale, size);
	if (!digits)
		return d;
	wos_add_int(frac, scale + v % scale, sizeof(frac));	/* "1" and the digits with their zeros */
	wos_add(d, ".", size);
	return wos_add(d, frac + 1, size);
}

/* The environment from the BME680: centi-°C, centi-% relative humidity and
 * deci-kPa; 0 when it was read. */
struct wos_env { long temp_c100, humidity_100, pressure_kpa10; };

static int wos_env_read(struct wos_env *e)
{
	static char dir[48];
	char b[48];
	long v;

	if (!dir[0] && wos_iio_find("bme680", dir, sizeof(dir)))
		return -1;
	if (wos_sysfs(dir, "in_temp_input", b, sizeof(b)) <= 0 || wos_fixed(b, 0, &v)) {
		dir[0] = 0;	/* gone, or not there yet: look again next time */
		return -1;
	}
	e->temp_c100 = v / 10;		/* milli-°C */
	e->humidity_100 = wos_sysfs(dir, "in_humidityrelative_input", b, sizeof(b)) > 0 && !wos_fixed(b, 2, &v) ? v : 0;
	e->pressure_kpa10 = wos_sysfs(dir, "in_pressure_input", b, sizeof(b)) > 0 && !wos_fixed(b, 1, &v) ? v : 0;
	return 0;
}

/* The acceleration from the BMI270 in milli-g on the badge's axes; 0
 * when it was read. */
struct wos_accel { long x_mg, y_mg, z_mg; };

static int wos_accel_read(struct wos_accel *a)
{
	static char dir[48];
	static long scale_n;		/* m/s² per count, in 1e-9 */
	char b[48];
	long v[3];
	static const char *const axis[3] = { "in_accel_x_raw", "in_accel_y_raw", "in_accel_z_raw" };

	if (!dir[0] && wos_iio_find("bmi270", dir, sizeof(dir)))
		return -1;
	if (!scale_n && (wos_sysfs(dir, "in_accel_scale", b, sizeof(b)) <= 0 || wos_fixed(b, 9, &scale_n) || !scale_n))
		scale_n = 598624;	/* ±2 g */
	for (int i = 0; i < 3; i++)
		if (wos_sysfs(dir, axis[i], b, sizeof(b)) <= 0 || wos_fixed(b, 0, &v[i])) {
			dir[0] = 0;
			return -1;
		}
	/* mg = raw * scale / 9.80665, in 32 bits: raw < 2^15, scale / 1000 < 2^11 */
	a->x_mg = v[0] * (scale_n / 1000) / 9807;
	a->y_mg = v[1] * (scale_n / 1000) / 9807;
	a->z_mg = v[2] * (scale_n / 1000) / 9807;
	return 0;
}
