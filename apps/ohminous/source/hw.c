// SPDX-License-Identifier: MIT
/*
 * hw.c — OHMinous' own pins: the badge's J3 and J7 headers (hw.h).
 *
 * The six header pins are one line request on /dev/gpiochip0 (GPIO uAPI
 * v2): a mode is a line's flags (GPIO_V2_LINE_SET_CONFIG_IOCTL with an
 * attribute per mode), the levels are one GET_VALUES for all of them. The
 * logic analyzer reads that request in a loop, timed by CLOCK_MONOTONIC:
 * what rate it reaches depends on the ioctl's cost and on interrupts; it
 * reports the rate it got. I2C on J7 is SDA and SCL as open-drain lines,
 * bit-banged. The UART sends by setting IO3 bit by bit against the clock
 * (so only slow rates hold), and receives from IO4's edge events, whose
 * timestamps the kernel takes when the edge happens: the bits come from
 * the times between edges, however late they are read.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hw.h"

#ifndef OHM_HOST
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif

struct hw_pin hw_pins[HW_PINS] = {
	{ "IO0", "J3\xc2\xb7" "3 \xc2\xb7 GPIO7", 7, PM_IN, 0, 0 },
	{ "IO1", "J3\xc2\xb7" "4 \xc2\xb7 GPIO5", 5, PM_IN, 0, 0 },
	{ "IO2", "J3\xc2\xb7" "5 \xc2\xb7 GPIO3", 3, PM_IN, 0, 0 },
	{ "IO3", "J3\xc2\xb7" "6 \xc2\xb7 GPIO8", 8, PM_IN, 0, 0 },
	{ "IO4", "J3\xc2\xb7" "7 \xc2\xb7 GPIO10", 10, PM_IN, 0, 0 },
	{ "STROBE", "J7\xc2\xb7" "2 \xc2\xb7 GPIO6", 6, PM_IN, 0, 0 },
};

const char *const hw_mode_names[N_PMODES] = { "Input", "Input, pull-up", "Input, pull-down", "Output" };

char hw_err[96];

#define SDA_GPIO	22
#define SCL_GPIO	9
#define TX_PIN		3		/* IO3 */
#define RX_PIN		4		/* IO4 */

static long long now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

#ifndef OHM_HOST
static void spin_until(long long t)
{
	while (now_ns() < t)
		;
}
#endif

/* ---- UART reception from edges: shared by the badge and the host --------- */

static struct {
	unsigned baud;
	long long edge_t[512];		/* edges after a start bit, oldest first */
	int edge_v[512], n;
	char rx[256];
	int rx_n;
} U;

/* Decode the frames whose ten bits have passed; the bytes into U.rx. */
static void uart_decode(long long now)
{
	long long bit = 1000000000LL / U.baud;

	for (;;) {
		int i = 0, level, byte = 0, k;

		while (i < U.n && U.edge_v[i] != 0)	/* to a falling edge: a start bit */
			i++;
		if (i) {
			memmove(U.edge_t, U.edge_t + i, sizeof(U.edge_t[0]) * (size_t)(U.n - i));
			memmove(U.edge_v, U.edge_v + i, sizeof(U.edge_v[0]) * (size_t)(U.n - i));
			U.n -= i;
		}
		if (!U.n || now < U.edge_t[0] + 10 * bit)
			return;
		/* the level in the middle of each bit: after the last edge before it */
		for (int b = 0; b < 9; b++) {
			long long t = U.edge_t[0] + bit + bit / 2 + b * bit;

			level = 0;
			for (k = 0; k < U.n && U.edge_t[k] <= t; k++)
				level = U.edge_v[k];
			if (b < 8)
				byte |= level << b;
			else if (!level)
				byte = -1;		/* no stop bit: noise */
		}
		/* drop the edges of this frame (up to its stop bit's middle) */
		for (k = 0; k < U.n && U.edge_t[k] < U.edge_t[0] + 9 * bit + bit / 2; k++)
			;
		memmove(U.edge_t, U.edge_t + k, sizeof(U.edge_t[0]) * (size_t)(U.n - k));
		memmove(U.edge_v, U.edge_v + k, sizeof(U.edge_v[0]) * (size_t)(U.n - k));
		U.n -= k;
		if (byte >= 0 && U.rx_n < (int)sizeof(U.rx))
			U.rx[U.rx_n++] = (char)byte;
	}
}

static void uart_edge(long long t, int v)
{
	if (U.n == (int)(sizeof(U.edge_t) / sizeof(U.edge_t[0]))) {	/* overrun: start over */
		U.n = 0;
		return;
	}
	U.edge_t[U.n] = t;
	U.edge_v[U.n++] = v;
}

static int uart_take(char *buf, int max)
{
	int n = U.rx_n < max ? U.rx_n : max;

	memcpy(buf, U.rx, (size_t)n);
	memmove(U.rx, U.rx + n, (size_t)(U.rx_n - n));
	U.rx_n -= n;
	return n;
}

#ifndef OHM_HOST

/* ---- the badge ---------------------------------------------------------- */

static int chip_fd = -1, pins_fd = -1, i2c_fd = -1;
static unsigned rate_max;

static uint64_t mode_flags(int mode)
{
	switch (mode) {
	case PM_OUT:
		return GPIO_V2_LINE_FLAG_OUTPUT;
	case PM_PULLUP:
		return GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
	case PM_PULLDOWN:
		return GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
	default:
		return GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_DISABLED;
	}
}

/* every pin's flags and output level as one configuration */
static void pins_config(struct gpio_v2_line_config *c)
{
	uint64_t outs = 0, outmask = 0;

	memset(c, 0, sizeof(*c));
	c->flags = mode_flags(PM_IN);
	for (int m = PM_PULLUP; m < N_PMODES; m++) {
		uint64_t mask = 0;

		for (int i = 0; i < HW_PINS; i++)
			if (hw_pins[i].mode == m)
				mask |= 1ull << i;
		if (!mask)
			continue;
		c->attrs[c->num_attrs].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
		c->attrs[c->num_attrs].attr.flags = mode_flags(m);
		if (m == PM_OUT)
			outmask = mask;
		c->attrs[c->num_attrs++].mask = mask;
	}
	for (int i = 0; i < HW_PINS; i++)
		if (hw_pins[i].level)
			outs |= 1ull << i;
	if (outmask) {
		c->attrs[c->num_attrs].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
		c->attrs[c->num_attrs].attr.values = outs;
		c->attrs[c->num_attrs++].mask = outmask;
	}
	/* the UART's RX: IO4's edges, while it runs */
	if (U.baud) {
		c->attrs[c->num_attrs].attr.id = GPIO_V2_LINE_ATTR_ID_FLAGS;
		c->attrs[c->num_attrs].attr.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP |
						    GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
		c->attrs[c->num_attrs++].mask = 1ull << RX_PIN;
	}
}

static int pins_apply(void)
{
	struct gpio_v2_line_config c;

	pins_config(&c);
	if (ioctl(pins_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &c) < 0) {
		snprintf(hw_err, sizeof(hw_err), "The pins' setting failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

static unsigned measure_rate(void)
{
	struct gpio_v2_line_values v = { 0, (1ull << HW_LA_CH) - 1 };
	long long t0 = now_ns();
	int n = 0;

	while (now_ns() - t0 < 20000000LL) {
		ioctl(pins_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v);
		n++;
	}
	return (unsigned)(n * 50ll);
}

int hw_open(void)
{
	struct gpio_v2_line_request r;

	if (pins_fd >= 0)
		return 0;
	chip_fd = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
	if (chip_fd < 0) {
		snprintf(hw_err, sizeof(hw_err), "No GPIO here (/dev/gpiochip0): %s", strerror(errno));
		return -1;
	}
	memset(&r, 0, sizeof(r));
	for (int i = 0; i < HW_PINS; i++)
		r.offsets[i] = (uint32_t)hw_pins[i].gpio;
	r.num_lines = HW_PINS;
	strcpy(r.consumer, "ohminous");
	pins_config(&r.config);
	r.event_buffer_size = 512;
	if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
		snprintf(hw_err, sizeof(hw_err), "The header pins are taken: %s", strerror(errno));
		close(chip_fd);
		chip_fd = -1;
		return -1;
	}
	pins_fd = r.fd;
	rate_max = measure_rate();
	return hw_read_all();
}

void hw_close(void)
{
	hw_uart_close();
	if (i2c_fd >= 0)
		close(i2c_fd);
	if (pins_fd >= 0)
		close(pins_fd);		/* released lines go back to inputs */
	if (chip_fd >= 0)
		close(chip_fd);
	i2c_fd = pins_fd = chip_fd = -1;
}

int hw_set_mode(int pin, int mode)
{
	int was = hw_pins[pin].mode;

	hw_pins[pin].mode = mode;
	if (pins_fd >= 0 && pins_apply() < 0) {
		hw_pins[pin].mode = was;
		return -1;
	}
	return hw_read_all();
}

int hw_set_level(int pin, int level)
{
	struct gpio_v2_line_values v = { (uint64_t)!!level << pin, 1ull << pin };

	hw_pins[pin].level = !!level;
	if (pins_fd < 0 || hw_pins[pin].mode != PM_OUT)
		return -1;
	return ioctl(pins_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

int hw_read_all(void)
{
	struct gpio_v2_line_values v = { 0, (1ull << HW_PINS) - 1 };

	if (pins_fd < 0 || ioctl(pins_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
		return -1;
	for (int i = 0; i < HW_PINS; i++)
		hw_pins[i].level = (int)(v.bits >> i & 1);
	return 0;
}

unsigned hw_rate_max(void)
{
	return rate_max;
}

int hw_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int *got, unsigned *rate_got,
	       int (*cancel)(void))
{
	struct gpio_v2_line_values v = { 0, (1ull << HW_LA_CH) - 1 };
	long long step = 1000000000LL / rate, t, t0;
	int last = -1, checked = 0;

	*got = 0;
	if (pins_fd < 0)
		return -1;
	if (trig >= 0) {	/* wait for the edge; a key may cancel */
		for (;;) {
			int b;

			if (ioctl(pins_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0)
				return -1;
			b = (int)(v.bits >> trig & 1);
			if (last >= 0 && b != last && b == !falling)
				break;
			last = b;
			if (++checked % 1024 == 0 && cancel && cancel())
				return 1;
		}
	}
	t0 = t = now_ns();
	for (int i = 0; i < n; i++) {
		spin_until(t);
		ioctl(pins_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v);
		buf[i] = (uint8_t)v.bits;
		t += step;
	}
	*got = n;
	*rate_got = (unsigned)((long long)n * 1000000000LL / (now_ns() - t0 + 1));
	return 0;
}

/* ---- I2C on J7, bit-banged: SDA bit 0, SCL bit 1 of the request ---------- */

#define HALF_NS		5000		/* a quarter of a 50 kHz clock, and more for the ioctls */

static void od_set(int sda, int scl)
{
	struct gpio_v2_line_values v = { (uint64_t)(sda | scl << 1), 3 };

	ioctl(i2c_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
	spin_until(now_ns() + HALF_NS);
}

static int sda_read(void)
{
	struct gpio_v2_line_values v = { 0, 1 };

	ioctl(i2c_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v);
	return (int)(v.bits & 1);
}

static int i2c_open(void)
{
	struct gpio_v2_line_request r;

	if (i2c_fd >= 0)
		return 0;
	if (chip_fd < 0 && hw_open() < 0)
		return -1;
	memset(&r, 0, sizeof(r));
	r.offsets[0] = SDA_GPIO;
	r.offsets[1] = SCL_GPIO;
	r.num_lines = 2;
	strcpy(r.consumer, "ohminous-i2c");
	r.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_OPEN_DRAIN;
	r.config.num_attrs = 1;
	r.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	r.config.attrs[0].attr.values = 3;	/* both released */
	r.config.attrs[0].mask = 3;
	if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
		snprintf(hw_err, sizeof(hw_err), "J7's SDA and SCL are taken: %s", strerror(errno));
		return -1;
	}
	i2c_fd = r.fd;
	return 0;
}

static void i2c_start(void)
{
	od_set(1, 1);
	od_set(0, 1);
	od_set(0, 0);
}

static void i2c_stop(void)
{
	od_set(0, 0);
	od_set(0, 1);
	od_set(1, 1);
}

/* a byte out; 1 when the device acknowledged it */
static int i2c_out(int byte)
{
	int ack;

	for (int b = 7; b >= 0; b--) {
		int bit = byte >> b & 1;

		od_set(bit, 0);
		od_set(bit, 1);
		od_set(bit, 0);
	}
	od_set(1, 0);
	od_set(1, 1);
	ack = !sda_read();
	od_set(1, 0);
	return ack;
}

static int i2c_in(int ack)
{
	int byte = 0;

	for (int b = 7; b >= 0; b--) {
		od_set(1, 0);
		od_set(1, 1);
		byte |= sda_read() << b;
	}
	od_set(!ack, 0);
	od_set(!ack, 1);
	od_set(!ack, 0);
	od_set(1, 0);
	return byte;
}

int hw_i2c_scan(uint8_t found[128])
{
	int n = 0;

	memset(found, 0, 128);
	if (i2c_open() < 0)
		return -1;
	if (!sda_read()) {
		snprintf(hw_err, sizeof(hw_err), "SDA is held low: nothing on the bus can answer");
		return -1;
	}
	for (int a = 0x08; a < 0x78; a++) {
		i2c_start();
		found[a] = (uint8_t)i2c_out(a << 1);
		i2c_stop();
		n += found[a];
	}
	return n;
}

int hw_i2c_read(int addr, int reg, uint8_t *buf, int n)
{
	if (i2c_open() < 0)
		return -1;
	i2c_start();
	if (!i2c_out(addr << 1) || !i2c_out(reg)) {
		i2c_stop();
		snprintf(hw_err, sizeof(hw_err), "No answer from 0x%02X", addr);
		return -1;
	}
	i2c_start();				/* a repeated start */
	if (!i2c_out(addr << 1 | 1)) {
		i2c_stop();
		snprintf(hw_err, sizeof(hw_err), "No answer from 0x%02X", addr);
		return -1;
	}
	for (int i = 0; i < n; i++)
		buf[i] = (uint8_t)i2c_in(i + 1 < n);
	i2c_stop();
	return n;
}

int hw_i2c_write(int addr, int reg, const uint8_t *buf, int n)
{
	int ok;

	if (i2c_open() < 0)
		return -1;
	i2c_start();
	ok = i2c_out(addr << 1) && i2c_out(reg);
	for (int i = 0; ok && i < n; i++)
		ok = i2c_out(buf[i]);
	i2c_stop();
	if (!ok)
		snprintf(hw_err, sizeof(hw_err), "No answer from 0x%02X", addr);
	return ok ? n : -1;
}

unsigned hw_i2c_khz(void)
{
	return 1000000 / (4 * HALF_NS / 1000 + 20);
}

/* ---- the UART ------------------------------------------------------------ */

int hw_uart_open(unsigned baud)
{
	if (pins_fd < 0 && hw_open() < 0)
		return -1;
	U.baud = baud;
	U.n = U.rx_n = 0;
	hw_pins[TX_PIN].mode = PM_OUT;
	hw_pins[TX_PIN].level = 1;		/* idle high */
	hw_pins[RX_PIN].mode = PM_PULLUP;
	hw_pins[TX_PIN].busy = hw_pins[RX_PIN].busy = 1;
	if (pins_apply() < 0) {
		U.baud = 0;
		return -1;
	}
	return 0;
}

void hw_uart_close(void)
{
	if (!U.baud)
		return;
	U.baud = 0;
	hw_pins[TX_PIN].busy = hw_pins[RX_PIN].busy = 0;
	hw_pins[TX_PIN].mode = hw_pins[RX_PIN].mode = PM_IN;
	if (pins_fd >= 0)
		pins_apply();
}

int hw_uart_send(const char *s, int n)
{
	long long bit = 1000000000LL / U.baud, t = now_ns() + bit;

	for (int i = 0; i < n; i++) {
		int frame = 0x200 | (unsigned char)s[i] << 1;	/* start 0, data, stop 1 */

		for (int b = 0; b < 10; b++) {
			struct gpio_v2_line_values v = { (uint64_t)(frame >> b & 1) << TX_PIN, 1ull << TX_PIN };

			spin_until(t);
			ioctl(pins_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
			t += bit;
		}
	}
	return n;
}

int hw_uart_fd(void)
{
	return U.baud ? pins_fd : -1;
}

int hw_uart_recv(char *buf, int max)
{
	struct gpio_v2_line_event ev[32];
	struct pollfd p = { pins_fd, POLLIN, 0 };

	while (U.baud && poll(&p, 1, 0) > 0) {
		int k = (int)read(pins_fd, ev, sizeof(ev));

		if (k <= 0)
			break;
		for (int i = 0; i < k / (int)sizeof(ev[0]); i++)
			if (ev[i].offset == (uint32_t)hw_pins[RX_PIN].gpio)
				uart_edge((long long)ev[i].timestamp_ns, ev[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE);
	}
	uart_decode(now_ns());
	return uart_take(buf, max);
}

/* ---- the LEDs (kernel patch 0032) --------------------------------------- */

int hw_leds(void)
{
	int n = 0;

	for (; n < 8; n++) {
		char path[80];

		snprintf(path, sizeof(path), "/sys/class/leds/rgb:indicator-%d/brightness", n);
		if (access(path, W_OK))
			break;
	}
	return n;
}

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), ok;

	if (fd < 0)
		return -1;
	ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
	close(fd);
	return ok ? 0 : -1;
}

int hw_led_set(int i, uint32_t rgb, int pct)
{
	char path[96], v[32];

	snprintf(path, sizeof(path), "/sys/class/leds/rgb:indicator-%d/multi_intensity", i);
	snprintf(v, sizeof(v), "%u %u %u", (unsigned)(rgb >> 16), (unsigned)(rgb >> 8 & 255), (unsigned)(rgb & 255));
	if (put(path, v) < 0)
		return -1;
	snprintf(path, sizeof(path), "/sys/class/leds/rgb:indicator-%d/brightness", i);
	snprintf(v, sizeof(v), "%d", rgb ? pct * 255 / 100 : 0);
	return put(path, v);
}

#else

/* ---- the host: all of it simulated ---------------------------------------- */

static uint8_t sim_i2c[128];
static int sim_level[HW_PINS], sim_open;
static char sim_rx[512];
char hw_sim_uart_tx[1024];
uint32_t hw_sim_led[8];

void hw_sim_i2c(int addr, int on)
{
	sim_i2c[addr & 127] = (uint8_t)on;
}

void hw_sim_level(int pin, int level)
{
	sim_level[pin] = level;
}

void hw_sim_uart(const char *rx)
{
	strncat(sim_rx, rx, sizeof(sim_rx) - strlen(sim_rx) - 1);
}

int hw_open(void)
{
	if (getenv("OHM_NO_GPIO")) {
		snprintf(hw_err, sizeof(hw_err), "No GPIO here (/dev/gpiochip0): No such file or directory");
		return -1;
	}
	sim_open = 1;
	return hw_read_all();
}

void hw_close(void)
{
	hw_uart_close();
	sim_open = 0;
}

int hw_set_mode(int pin, int mode)
{
	hw_pins[pin].mode = mode;
	return hw_read_all();
}

int hw_set_level(int pin, int level)
{
	hw_pins[pin].level = !!level;
	return hw_pins[pin].mode == PM_OUT ? 0 : -1;
}

int hw_read_all(void)
{
	for (int i = 0; i < HW_PINS; i++)
		if (hw_pins[i].mode != PM_OUT)
			hw_pins[i].level = hw_pins[i].mode == PM_PULLUP ? 1 : hw_pins[i].mode == PM_PULLDOWN ? 0 :
					   sim_level[i];
	return sim_open ? 0 : -1;
}

unsigned hw_rate_max(void)
{
	return 100000;
}

/* a simulated bus: IO0 a clock, IO1 data, IO2 a select, IO3 and IO4 slower */
int hw_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int *got, unsigned *rate_got,
	       int (*cancel)(void))
{
	(void)trig;
	(void)falling;
	(void)cancel;
	for (int i = 0; i < n; i++) {
		int clk = i / 32 % 2, sel = i % 1024 < 900, data = (i / 64 * 7 / 4) % 5 < 2, slow = i / 300 % 2;

		buf[i] = (uint8_t)(clk | data << 1 | sel << 2 | slow << 3 | (i / 700 % 2) << 4);
	}
	*got = n;
	*rate_got = rate;
	return 0;
}

int hw_i2c_scan(uint8_t found[128])
{
	int n = 0;

	for (int a = 0; a < 128; a++)
		n += found[a] = a >= 8 && a < 0x78 && sim_i2c[a];
	return n;
}

int hw_i2c_read(int addr, int reg, uint8_t *buf, int n)
{
	if (!sim_i2c[addr & 127]) {
		snprintf(hw_err, sizeof(hw_err), "No answer from 0x%02X", addr);
		return -1;
	}
	for (int i = 0; i < n; i++)
		buf[i] = (uint8_t)(reg + i + addr);
	return n;
}

int hw_i2c_write(int addr, int reg, const uint8_t *buf, int n)
{
	(void)reg;
	(void)buf;
	return sim_i2c[addr & 127] ? n : -1;
}

unsigned hw_i2c_khz(void)
{
	return 40;
}

int hw_uart_open(unsigned baud)
{
	U.baud = baud;
	hw_pins[TX_PIN].busy = hw_pins[RX_PIN].busy = 1;
	return 0;
}

void hw_uart_close(void)
{
	U.baud = 0;
	hw_pins[TX_PIN].busy = hw_pins[RX_PIN].busy = 0;
}

int hw_uart_send(const char *s, int n)
{
	strncat(hw_sim_uart_tx, s, (size_t)n < sizeof(hw_sim_uart_tx) - strlen(hw_sim_uart_tx) - 1 ? (size_t)n :
		sizeof(hw_sim_uart_tx) - strlen(hw_sim_uart_tx) - 1);
	return n;
}

int hw_uart_fd(void)
{
	return -1;
}

/* the simulated device's bytes go through the edge decoder, as edges */
int hw_uart_recv(char *buf, int max)
{
	long long bit = 1000000000LL / (U.baud ? U.baud : 9600), t = 1000000;

	for (const char *s = sim_rx; *s; s++) {
		int frame = 0x200 | (unsigned char)*s << 1, prev = 1;

		for (int b = 0; b < 10; b++, t += bit) {
			int v = frame >> b & 1;

			if (v != prev)
				uart_edge(t, v);
			prev = v;
		}
		t += 3 * bit;
	}
	sim_rx[0] = 0;
	uart_decode(t + 20 * bit);
	return uart_take(buf, max);
}

int hw_leds(void)
{
	return 4;
}

int hw_led_set(int i, uint32_t rgb, int pct)
{
	hw_sim_led[i & 7] = pct ? rgb : 0;
	return 0;
}
#endif
