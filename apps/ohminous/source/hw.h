/* SPDX-License-Identifier: MIT */
/*
 * hw.h — OHMinous' own pins: the badge's J3 and J7 headers (hw.c).
 *
 * Through the GPIO character device (/dev/gpiochip0, GPIO uAPI v2): IO0 to
 * IO4 on J3 (P4 GPIO 7, 5, 3, 8, 10) and STROBE on J7 (GPIO 6) as inputs
 * or outputs; a logic analyzer that samples them in software; I2C on J7's
 * SDA and SCL (the badge's second bus, GPIO 22 and 9, which WhyOS has no
 * driver for) bit-banged; a UART on IO3 (TX) and IO4 (RX), also in
 * software; and the WS2812 LEDs that kernel patch 0032 drives on J7 pin 4.
 * Built with -DOHM_HOST, all of it is simulated (hw_sim_*), for the host
 * tests.
 */
#pragma once

#include <stdint.h>

#define HW_PINS		6		/* IO0-IO4, STROBE */
#define HW_LA_CH	5		/* the logic analyzer's: IO0-IO4 */
#define HW_LA_MAX	32768

enum { PM_IN, PM_PULLUP, PM_PULLDOWN, PM_OUT, N_PMODES };

struct hw_pin {
	const char *name;		/* "IO0" */
	const char *where;		/* "J3·3 · GPIO7" */
	int gpio;
	int mode, level;		/* PM_..., what is read or driven */
	int busy;			/* used by the UART or I2C just now */
};

extern struct hw_pin hw_pins[HW_PINS];
extern const char *const hw_mode_names[N_PMODES];
extern char hw_err[96];

int hw_open(void);			/* 0, or -1 with hw_err: no gpiochip */
void hw_close(void);			/* every pin back to an input */
int hw_set_mode(int pin, int mode);
int hw_set_level(int pin, int level);
int hw_read_all(void);			/* refresh the inputs' levels */

/* the logic analyzer: n samples of IO0-IO4 at rate Hz (bit c = IOc), after
 * an edge on trig (-1: at once); 0, or -1 with hw_err; *got samples, and
 * the rate it managed */
int hw_capture(uint8_t *buf, int n, unsigned rate, int trig, int falling, int *got,
	       unsigned *rate_got, int (*cancel)(void));
unsigned hw_rate_max(void);

/* I2C on J7: a scan (found[a] for 0x08-0x77), register reads and writes */
int hw_i2c_scan(uint8_t found[128]);
int hw_i2c_read(int addr, int reg, uint8_t *buf, int n);
int hw_i2c_write(int addr, int reg, const uint8_t *buf, int n);
unsigned hw_i2c_khz(void);

/* the UART on IO3 (TX) and IO4 (RX), 8N1 */
int hw_uart_open(unsigned baud);
void hw_uart_close(void);
int hw_uart_send(const char *s, int n);
int hw_uart_fd(void);			/* to poll; then hw_uart_recv */
int hw_uart_recv(char *buf, int max);

/* the WS2812 LEDs on J7 pin 4 (/sys/class/leds/rgb:indicator-N) */
int hw_leds(void);			/* how many there are */
int hw_led_set(int i, uint32_t rgb, int pct);

#ifdef OHM_HOST
void hw_sim_i2c(int addr, int on);	/* a device on the simulated bus */
void hw_sim_level(int pin, int level);	/* what a simulated input reads */
void hw_sim_uart(const char *rx);	/* what the simulated device sends */
extern char hw_sim_uart_tx[1024];	/* what was sent to it */
extern uint32_t hw_sim_led[8];
#endif
