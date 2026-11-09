// SPDX-License-Identifier: MIT
/*
 * The sandbox on any computer (sbsim.h). Set by the environment:
 *
 *   SBSIM_ROOT     the badge's file system, a directory (sbsim.py makes it)
 *   SBSIM_OUT      where the report and the screens go
 *   SBSIM_END      the virtual ms at which the run ends
 *   SBSIM_KEYS     "ms:code ...": a key pressed for 100 ms
 *   SBSIM_TAPS     "ms:code ...": a press and its release in one look, as the keypad's poll can bring them
 *   SBSIM_FIFO     "path ms:text ...": text written to that FIFO at ms
 *   SBSIM_SHOTS    "ms ...": the panel saved as <ms>.ppm
 *   SBSIM_TERM     the ms at which SIGTERM comes, as from apprun
 *   SBSIM_FRAME_MS what a frame through /dev/ppa takes, for a game that never waits
 *   SBSIM_STANDBY  "from-to ...": standby by the user
 *   SBSIM_SCREEN   sleepd's power.screen in seconds (default 120, 0 never)
 *   SBSIM_NO_PPA   there is no /dev/ppa
 *   SBSIM_PPA_OFF  "from-to ...": /dev/ppa says EAGAIN, as with the screen off
 *   SBSIM_REALTIME the clock no faster than the computer's, for a thread that keeps real time
 *   PERMISSIONS    what app.conf says
 *
 * Paths are the badge's; APP_DIR, APP_CONFIG and so on as apprun sets them.
 */
#define SBSIM_IMPL
#include "sbsim.h"

/* -include has already brought them here too */
#undef open
#undef close
#undef read
#undef write
#undef pwrite
#undef lseek
#undef getppid
#undef ioctl
#undef poll
#undef access
#undef mkfifo
#undef mkdir
#undef unlink
#undef rename
#undef stat
#undef fsync
#undef fopen
#undef opendir
#undef openat
#undef clock_gettime
#undef nanosleep
#undef usleep
#undef malloc
#undef calloc
#undef realloc
#undef free

#include <stdint.h>
#include <sys/mman.h>

#include "policy.h"
#include "wbppa.h"

/* as in c1urun.c; test_sbsim checks these against it */
#define FRAME_MIN	(64 * 1024UL)	/* more than a band of 40 rows: an app reuses that at once */
#define PPA_MAX_SIDE	2048

#define PANEL		720
#define MAX_KEYS	4096
#define MAX_SPANS	16
#define MAX_REFUSED	64
#define MAX_FDS		256
#define MAX_LIGHT	4096
#define ARENA		(256u << 20)

enum { F_NONE, F_FB, F_EV, F_PPA, F_LIGHT, F_ACTIVE };

static struct policy pol;
static char root[256], outdir[256], cwd[256] = "/";
static uint8_t fd_kind[MAX_FDS], fd_nonblock[MAX_FDS];
static uint16_t panel[PANEL * PANEL];

/* the clock */
static uint64_t now_us;
static long frame_us;		/* SBSIM_FRAME_MS: what drawing a frame takes, for a game that never waits */
static long end_ms = -1, term_ms = -1;
static int realtime;
static uint64_t real_start_us;
static volatile int interrupted;

/* what is to come */
static struct key { long ms; int code, value; } keys[MAX_KEYS];
static int n_keys, key_next;
static long shots[256];
static int n_shots, shot_next;
static struct span { long from, to; } standby_spans[MAX_SPANS], ppa_off[MAX_SPANS];
static int n_standby, n_ppa_off;
static struct fifo_write { long ms; char text[32]; } fifo_writes[64];
static int n_fifo, fifo_next;
static char fifo_path[256];

/* sleepd */
static long screen_ms = 120000, last_active_ms;
static int standby, idle_standby, woken, slept;

/* what the run did */
static long fb_writes, fb_rows, ppa_frames, ppa_again, ppa_bad, torn, ioctls_refused;
static long touches, touches_in_standby, lights_in_standby;
static char first_torn[96];
static struct { char path[POLICY_PATH]; int write, n; } refused[MAX_REFUSED];
static int n_refused;
static struct { long ms; int value; } lights[MAX_LIGHT];
static int n_lights;
static size_t heap_live, heap_peak;
static char *stack_top;
static size_t stack_peak;

/* a frame or a scaling c1urun has answered but not finished */
static struct {
	int busy;
	const uint8_t *p;
	size_t n;
	off_t off;
	uint32_t hash;
} frame;
static struct {
	int busy;
	struct wb_ppa_scale s;
	const uint8_t *p;
	uint32_t hash;
} ppa;

static long ms(void)
{
	return (long)(now_us / 1000);
}

static uint32_t hash(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;

	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

static void note_torn(const char *what)
{
	if (!torn++)
		snprintf(first_torn, sizeof(first_torn), "%s at %ld ms", what, ms());
}

static void frame_finish(void)
{
	if (!frame.busy)
		return;
	frame.busy = 0;
	if (hash(frame.p, frame.n) != frame.hash)
		note_torn("fb0");
	if (frame.off + (off_t)frame.n <= (off_t)sizeof(panel))
		memcpy((uint8_t *)panel + frame.off, frame.p, frame.n);
}

static void ppa_finish(void)
{
	const struct wb_ppa_scale *s = &ppa.s;

	if (!ppa.busy)
		return;
	ppa.busy = 0;
	if (hash(ppa.p, (size_t)s->stride * s->h * 2) != ppa.hash)
		note_torn("ppa");
	for (uint32_t y = 0; y < s->h * s->qy / 16; y++)
		for (uint32_t x = 0; x < s->w * s->q / 16; x++)
			panel[(s->y + y) * PANEL + s->x + x] =
				((const uint16_t *)ppa.p)[y * 16 / s->qy * s->stride + x * 16 / s->q];
}

/* c1urun's frame_can_wait: only these go past a frame */
static void before_call(int passes_frame, int passes_ppa)
{
	if (!passes_frame)
		frame_finish();
	if (!passes_ppa)
		ppa_finish();
}

static void shot(long at)
{
	char path[320];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%ld.ppm", outdir, at);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n720 720\n255\n");
	for (int i = 0; i < PANEL * PANEL; i++) {
		uint16_t c = panel[i];
		uint8_t rgb[3] = { (uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
				   (uint8_t)((c & 31) * 255 / 31) };

		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void standby_set(int on)
{
	char path[320];

	snprintf(path, sizeof(path), "%s/run/standby", root);
	standby = on;
	if (on)
		fclose(fopen(path, "w"));
	else
		unlink(path);
}

static int in_spans(const struct span *s, int n, long t)
{
	for (int i = 0; i < n; i++)
		if (t >= s[i].from && t < s[i].to)
			return 1;
	return 0;
}

static long next_edge(const struct span *s, int n, long t, long best)
{
	for (int i = 0; i < n; i++) {
		if (s[i].from > t && s[i].from < best)
			best = s[i].from;
		if (s[i].to > t && s[i].to < best)
			best = s[i].to;
	}
	return best;
}

static uint64_t real_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

/* SBSIM_REALTIME: a thread beside the app, such as one that plays sound, keeps the
 * computer's time and calls nothing of this; the app waits for it to catch up */
static void pace(void)
{
	uint64_t real = real_us() - real_start_us;

	if (now_us > real + 1000) {
		struct timespec t = { (time_t)((now_us - real) / 1000000), (long)((now_us - real) % 1000000) * 1000 };

		nanosleep(&t, 0);
	}
}

static void fire_events(void)
{
	long t = ms();
	int want = in_spans(standby_spans, n_standby, t);

	if (realtime)
		pace();

	if (want != (standby && !idle_standby)) {
		if (want || !idle_standby)
			standby_set(want);
		if (!want)
			last_active_ms = t;
	}
	if (screen_ms && !standby && t - last_active_ms >= screen_ms) {
		idle_standby = 1;
		slept++;
		standby_set(1);
	}
	while (shot_next < n_shots && shots[shot_next] <= t)
		shot(shots[shot_next++]);
	while (fifo_next < n_fifo && fifo_writes[fifo_next].ms <= t) {
		char path[320];
		int fd;

		snprintf(path, sizeof(path), "%s%s", root, fifo_path);
		fd = open(path, O_WRONLY | O_NONBLOCK);
		if (fd >= 0) {
			if (write(fd, fifo_writes[fifo_next].text, strlen(fifo_writes[fifo_next].text)) < 0)
				fd_kind[0] = 0;
			close(fd);
		}
		fifo_next++;
	}
	if (term_ms >= 0 && t >= term_ms) {
		term_ms = -1;
		interrupted = 1;
		raise(SIGTERM);
	}
	if (end_ms >= 0 && t >= end_ms)
		exit(0);
}

/* the clock to the next thing that happens, at most to `to` */
static long next_event(long to)
{
	long t = ms(), best = to;

	if (key_next < n_keys && keys[key_next].ms > t && keys[key_next].ms < best)
		best = keys[key_next].ms;
	if (shot_next < n_shots && shots[shot_next] > t && shots[shot_next] < best)
		best = shots[shot_next];
	if (fifo_next < n_fifo && fifo_writes[fifo_next].ms > t && fifo_writes[fifo_next].ms < best)
		best = fifo_writes[fifo_next].ms;
	if (term_ms > t && term_ms < best)
		best = term_ms;
	if (end_ms > t && end_ms < best)
		best = end_ms;
	if (screen_ms && !standby && last_active_ms + screen_ms > t && last_active_ms + screen_ms < best)
		best = last_active_ms + screen_ms;
	return next_edge(standby_spans, n_standby, t, best);
}

static void advance_to(uint64_t us)
{
	while (now_us < us && !interrupted) {
		long stop = next_event((long)(us / 1000) + 1);
		uint64_t step = (uint64_t)stop * 1000;

		now_us = step < us ? step : us;
		fire_events();
	}
}

/* a call takes a little time, so a program that polls the clock gets on */
/* How deep the stack is at this call: the badge's is as small as the program's header says. */
static void stack_depth(void)
{
	char *here = __builtin_frame_address(0);

	if (!stack_top || here > stack_top)
		stack_top = here;
	if ((size_t)(stack_top - here) > stack_peak)
		stack_peak = stack_top - here;
}

static void tick(void)
{
	stack_depth();
	now_us += 20;
	fire_events();
}

/* ---- paths --------------------------------------------------------------- */

static void refuse(const char *path, int write)
{
	for (int i = 0; i < n_refused; i++)
		if (!strcmp(refused[i].path, path) && refused[i].write == write) {
			refused[i].n++;
			return;
		}
	if (n_refused < MAX_REFUSED) {
		snprintf(refused[n_refused].path, POLICY_PATH, "%s", path);
		refused[n_refused].write = write;
		refused[n_refused++].n = 1;
	}
}

/* The badge's path as policy.h has it, in out; 0 when refused. */
static int allowed(const char *path, int write, char *out)
{
	if (policy_path(&pol, cwd, path, write, out, POLICY_PATH))
		return 1;
	refuse(path, write);
	errno = EACCES;
	return 0;
}

static void host_path(char *to, size_t size, const char *badge)
{
	snprintf(to, size, "%s%s", root, badge);
}

static int fd_set_kind(int fd, int kind, int flags)
{
	if (fd >= 0 && fd < MAX_FDS) {
		fd_kind[fd] = (uint8_t)kind;
		fd_nonblock[fd] = !!(flags & O_NONBLOCK);
	}
	return fd;
}

int sim_open(const char *path, int flags, ...)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];
	int write = (flags & O_ACCMODE) != O_RDONLY || (flags & O_CREAT), mode = 0, kind = F_NONE, fd;
	va_list ap;

	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = va_arg(ap, int);
	va_end(ap);
	before_call(0, 0);
	tick();
	if (!allowed(path, write, out))
		return -1;
	if (!strcmp(out, "/dev/fb0"))
		kind = F_FB;
	else if (!strcmp(out, "/dev/input/event0"))
		kind = F_EV;
	else if (!strcmp(out, WB_PPA_DEV))
		kind = F_PPA;
	if (kind == F_PPA && getenv("SBSIM_NO_PPA")) {
		errno = ENOENT;
		return -1;
	}
	if (kind)
		return fd_set_kind(open("/dev/null", O_RDWR), kind, flags);
	if (!strcmp(out, "/sys/class/backlight/backlight/brightness"))
		kind = F_LIGHT;
	else if (!strcmp(out, "/run/active"))
		kind = F_ACTIVE;
	host_path(full, sizeof(full), out);
	fd = open(full, flags, mode);
	return fd_set_kind(fd, write ? kind : F_NONE, flags);
}

int sim_close(int fd)
{
	before_call(0, 0);
	tick();
	if (fd >= 0 && fd < MAX_FDS)
		fd_kind[fd] = F_NONE;
	return close(fd);
}

int sim_access(const char *path, int mode)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, (mode & W_OK) != 0, out))
		return -1;
	if (!strcmp(out, "/dev/fb0") || !strcmp(out, "/dev/input/event0"))
		return 0;
	host_path(full, sizeof(full), out);
	return access(full, mode);
}

int sim_mkfifo(const char *path, mode_t mode)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, 1, out))
		return -1;
	host_path(full, sizeof(full), out);
	return mkfifo(full, mode);
}

int sim_mkdir(const char *path, mode_t mode)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, 1, out))
		return -1;
	host_path(full, sizeof(full), out);
	return mkdir(full, mode);
}

int sim_unlink(const char *path)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, 1, out))
		return -1;
	host_path(full, sizeof(full), out);
	return unlink(full);
}

int sim_rename(const char *from, const char *to)
{
	char a[POLICY_PATH], b[POLICY_PATH], fa[POLICY_PATH + 256], fb[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(from, 1, a) || !allowed(to, 1, b))
		return -1;
	host_path(fa, sizeof(fa), a);
	host_path(fb, sizeof(fb), b);
	return rename(fa, fb);
}

int sim_stat(const char *path, struct stat *st)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, 0, out))
		return -1;
	host_path(full, sizeof(full), out);
	return stat(full, st);
}

FILE *sim_fopen(const char *path, const char *mode)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, mode[0] != 'r' || strchr(mode, '+'), out))
		return 0;
	host_path(full, sizeof(full), out);
	return fopen(full, mode);
}

DIR *sim_opendir(const char *path)
{
	char out[POLICY_PATH], full[POLICY_PATH + 256];

	before_call(0, 0);
	tick();
	if (!allowed(path, 0, out))
		return 0;
	host_path(full, sizeof(full), out);
	return opendir(full);
}

/* c1urun takes paths from the working directory only: an app has no directory fds */
int sim_openat(int dirfd, const char *path, int flags, ...)
{
	int mode = 0;
	va_list ap;

	if (dirfd >= 0) {
		errno = EFAULT;
		return -1;
	}
	va_start(ap, flags);
	if (flags & O_CREAT)
		mode = va_arg(ap, int);
	va_end(ap);
	return sim_open(path, flags, mode);
}

/* ---- calls --------------------------------------------------------------- */

struct ev32 {				/* the kernel's input_event on the badge */
	unsigned int sec, usec;
	unsigned short type, code;
	int value;
};

static int key_due(void)
{
	while (key_next < n_keys && keys[key_next].ms <= ms() && standby)
		key_next++;		/* sleepd has them: "the next key, which is eaten" */
	return key_next < n_keys && keys[key_next].ms <= ms();
}

ssize_t sim_read(int fd, void *buf, size_t n)
{
	int kind = fd >= 0 && fd < MAX_FDS ? fd_kind[fd] : F_NONE;
	int nonblock = fd >= 0 && fd < MAX_FDS && fd_nonblock[fd];
	ssize_t got = 0;

	before_call(nonblock && kind != F_FB, nonblock && kind != F_FB);
	tick();
	if (kind != F_EV)
		return read(fd, buf, n);
	while (!key_due() && !nonblock && key_next < n_keys)
		advance_to((uint64_t)keys[key_next].ms * 1000);
	while (key_due() && (size_t)got + sizeof(struct ev32) <= n) {
		struct ev32 e = { 0, 0, 1, (unsigned short)keys[key_next].code, keys[key_next].value };

		memcpy((char *)buf + got, &e, sizeof(e));
		got += (ssize_t)sizeof(e);
		key_next++;
		last_active_ms = ms();
	}
	if (!got) {
		errno = EAGAIN;
		return -1;
	}
	return got;
}

static void start_frame(const void *buf, size_t n, off_t off)
{
	frame.busy = 1;
	frame.p = buf;
	frame.n = n;
	frame.off = off;
	frame.hash = hash(buf, n);
}

ssize_t sim_write(int fd, const void *buf, size_t n)
{
	int kind = fd >= 0 && fd < MAX_FDS ? fd_kind[fd] : F_NONE;

	before_call(0, 0);
	tick();
	if (kind == F_LIGHT) {
		if (n_lights < MAX_LIGHT) {
			lights[n_lights].ms = ms();
			lights[n_lights++].value = atoi(buf);
		}
		if (standby)
			lights_in_standby++;
	}
	if (kind == F_ACTIVE) {
		touches++;
		if (standby) {		/* sleepd: someone else's key ends it */
			touches_in_standby++;
			woken++;
			idle_standby = 0;
			standby_set(0);
		}
		last_active_ms = ms();
	}
	if (kind == F_FB) {
		off_t off = lseek(fd, 0, SEEK_CUR);

		fb_writes++;
		fb_rows += (long)(n / (PANEL * 2));
		if (n >= FRAME_MIN) {
			start_frame(buf, n, off);
		} else if (off + (off_t)n <= (off_t)sizeof(panel)) {
			memcpy((uint8_t *)panel + off, buf, n);
		}
		return (ssize_t)n;
	}
	return write(fd, buf, n);
}

ssize_t sim_pwrite(int fd, const void *buf, size_t n, off_t off)
{
	int kind = fd >= 0 && fd < MAX_FDS ? fd_kind[fd] : F_NONE;

	before_call(0, 0);
	tick();
	if (kind != F_FB)
		return pwrite(fd, buf, n, off);
	fb_writes++;
	fb_rows += (long)(n / (PANEL * 2));
	if (n >= FRAME_MIN)
		start_frame(buf, n, off);
	else if (off + (off_t)n <= (off_t)sizeof(panel))
		memcpy((uint8_t *)panel + off, buf, n);
	return (ssize_t)n;
}

off_t sim_lseek(int fd, off_t off, int whence)
{
	before_call(0, 0);
	tick();
	return lseek(fd, off, whence);
}

pid_t sim_getppid(void)
{
	before_call(0, 0);
	tick();
	return getppid();
}

int sim_fsync(int fd)
{
	before_call(0, 0);
	tick();
	return fsync(fd);
}

/* the arena's addresses: the app's memory is 32 bits wide on the badge */
static uint8_t *arena;
static size_t arena_used;

static void *app_pointer(uint32_t p)
{
	uintptr_t high = (uintptr_t)arena & ~(uintptr_t)0xffffffffu;

	return (void *)(high | p);
}

static int mine(const void *p, size_t n)
{
	return (const uint8_t *)p >= arena && (const uint8_t *)p + n <= arena + arena_used;
}

int sim_ioctl(int fd, unsigned long req, ...)
{
	const struct wb_ppa_scale *s;
	int kind = fd >= 0 && fd < MAX_FDS ? fd_kind[fd] : F_NONE;
	const uint8_t *src;
	uint32_t ow, oh;
	va_list ap;

	va_start(ap, req);
	s = va_arg(ap, const struct wb_ppa_scale *);
	va_end(ap);
	before_call(0, 0);
	tick();
	if (kind != F_PPA || req != WB_PPA_SCALE) {
		ioctls_refused++;
		errno = ENOTTY;
		return -1;
	}
	/* c1urun's ppa_check */
	src = app_pointer(s->src);
	if (s->stride > PPA_MAX_SIDE || s->h > PPA_MAX_SIDE || !mine(src, (size_t)s->stride * s->h * 2)) {
		ppa_bad++;
		errno = EFAULT;
		return -1;
	}
	/* the kernel's checks in patch 0072 */
	ow = s->w * s->q / 16;
	oh = s->h * s->qy / 16;
	if (s->q < 16 || s->q >= 256 * 16 || s->qy < 16 || s->qy >= 256 * 16 || !s->w || !s->h ||
	    s->w > 2048 || s->w > s->stride || (s->src & 1) || s->x + ow > PANEL || s->y + oh > PANEL) {
		ppa_bad++;
		errno = EINVAL;
		return -1;
	}
	if (standby || in_spans(ppa_off, n_ppa_off, ms())) {
		ppa_again++;
		errno = EAGAIN;
		return -1;
	}
	ppa_frames++;
	ppa.busy = 1;
	ppa.s = *s;
	ppa.p = src;
	ppa.hash = hash(src, (size_t)s->stride * s->h * 2);
	now_us += frame_us;
	return 0;
}

int sim_poll(struct pollfd *p, nfds_t n, int timeout)
{
	uint64_t until = timeout < 0 ? UINT64_MAX : now_us + (uint64_t)timeout * 1000;

	before_call(0, 0);
	tick();
	for (;;) {
		int ready = 0;

		for (nfds_t i = 0; i < n; i++) {
			int kind = p[i].fd >= 0 && p[i].fd < MAX_FDS ? fd_kind[p[i].fd] : F_NONE;

			p[i].revents = 0;
			if (kind == F_EV && key_due())
				p[i].revents = POLLIN;
			else if (kind == F_NONE && p[i].fd >= 0) {
				struct pollfd one = p[i];

				if (poll(&one, 1, 0) > 0)
					p[i].revents = one.revents;
			}
			ready += !!p[i].revents;
		}
		if (ready)
			return ready;
		if (interrupted) {
			interrupted = 0;
			errno = EINTR;
			return -1;
		}
		if (now_us >= until)
			return 0;
		advance_to(until == UINT64_MAX ? now_us + 1000000 : until);
	}
}

int sim_clock_gettime(clockid_t id, struct timespec *t)
{
	(void)id;
	tick();
	t->tv_sec = (time_t)(now_us / 1000000 + 1000);
	t->tv_nsec = (long)(now_us % 1000000) * 1000;
	return 0;
}

int sim_nanosleep(const struct timespec *t, struct timespec *left)
{
	(void)left;
	before_call(0, 1);		/* a sleep may go past a scaling, not past a frame */
	tick();
	advance_to(now_us + (uint64_t)t->tv_sec * 1000000 + (uint64_t)t->tv_nsec / 1000);
	if (interrupted) {
		interrupted = 0;
		errno = EINTR;
		return -1;
	}
	return 0;
}

int sim_usleep(unsigned us)
{
	struct timespec t = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };

	return sim_nanosleep(&t, 0);
}

/* ---- memory: one arena, so that a 32-bit address can be found again ------- */

struct block {
	size_t size;
	struct block *next;
};
static struct block *free_blocks;

void *sim_malloc(size_t n)
{
	struct block **b = &free_blocks, *got = 0;

	n = (n + 15) & ~(size_t)15;
	for (; *b; b = &(*b)->next)
		if ((*b)->size >= n && (*b)->size <= n + n / 4) {
			got = *b;
			*b = got->next;
			break;
		}
	if (!got) {
		if (arena_used + n + 16 > ARENA) {
			errno = ENOMEM;
			return 0;
		}
		got = (struct block *)(arena + arena_used);
		got->size = n;
		arena_used += n + 16;
	}
	heap_live += got->size;
	if (heap_live > heap_peak)
		heap_peak = heap_live;
	stack_depth();
	return (uint8_t *)got + 16;
}

void sim_free(void *p)
{
	struct block *b;

	if (!p)
		return;
	if ((uint8_t *)p < arena || (uint8_t *)p >= arena + ARENA) {
		free(p);		/* from libc: strdup, getline */
		return;
	}
	b = (struct block *)((uint8_t *)p - 16);
	heap_live -= b->size;
	b->next = free_blocks;
	free_blocks = b;
}

void *sim_calloc(size_t n, size_t size)
{
	void *p = sim_malloc(n * size);

	if (p)
		memset(p, 0, n * size);
	return p;
}

void *sim_realloc(void *p, size_t n)
{
	struct block *b;
	void *q;

	if (!p)
		return sim_malloc(n);
	if ((uint8_t *)p < arena || (uint8_t *)p >= arena + ARENA)
		return realloc(p, n);
	b = (struct block *)((uint8_t *)p - 16);
	if (b->size >= n)
		return p;
	q = sim_malloc(n);
	if (q) {
		memcpy(q, p, b->size);
		sim_free(p);
	}
	return q;
}

/* ---- start and end ------------------------------------------------------- */

static void report(void)
{
	char path[320];
	FILE *f;

	frame_finish();
	ppa_finish();
	snprintf(path, sizeof(path), "%s/report", outdir);
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "end_ms %ld\nfb_writes %ld\nfb_rows %ld\nppa_frames %ld\nppa_again %ld\nppa_bad %ld\n"
		"torn %ld\nfirst_torn %s\nioctls_refused %ld\ntouches %ld\ntouches_in_standby %ld\n"
		"lights_in_standby %ld\nslept %d\nwoken %d\nheap_peak %zu\nstack_peak %zu\n",
		ms(), fb_writes, fb_rows, ppa_frames, ppa_again, ppa_bad, torn, first_torn, ioctls_refused,
		touches, touches_in_standby, lights_in_standby, slept, woken, heap_peak, stack_peak);
	for (int i = 0; i < n_refused; i++)
		fprintf(f, "refused %s %s %d\n", refused[i].write ? "write" : "read", refused[i].path, refused[i].n);
	for (int i = 0; i < n_lights; i++)
		fprintf(f, "light %ld %d\n", lights[i].ms, lights[i].value);
	fclose(f);
}

static void add_key(long at, int code, int value)
{
	int i = n_keys++;

	while (i > 0 && keys[i - 1].ms > at) {
		keys[i] = keys[i - 1];
		i--;
	}
	keys[i] = (struct key){ at, code, value };
}

static int spans(const char *text, struct span *s)
{
	int n = 0;

	while (text && *text && n < MAX_SPANS) {
		char *e;

		s[n].from = strtol(text, &e, 10);
		s[n].to = strtol(e + 1, &e, 10);
		n++;
		text = *e ? e + 1 : e;
	}
	return n;
}

static void set_place(char *to, const char *env)
{
	const char *v = getenv(env);

	if (v)
		snprintf(to, POLICY_PATH, "%s", v);
}

__attribute__((constructor)) static void sim_start(void)
{
	const char *v;
	char *e;

	/* on a 64-bit computer it starts just past 4 GB; a 32-bit build takes it as it comes */
	arena = mmap(0, ARENA + (sizeof(void *) > 4 ? 1ull << 32 : 0), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (arena == MAP_FAILED)
		abort();
	/* inside one 4 GB window */
	if (sizeof(void *) > 4)
		arena = (uint8_t *)(((uintptr_t)arena + 0xffffffffu) & ~(uintptr_t)0xffffffffu) + 4096;
	snprintf(root, sizeof(root), "%s", getenv("SBSIM_ROOT") ? getenv("SBSIM_ROOT") : ".");
	snprintf(outdir, sizeof(outdir), "%s", getenv("SBSIM_OUT") ? getenv("SBSIM_OUT") : ".");
	set_place(pol.dir, "APP_DIR");
	set_place(pol.data, "APP_DATA");
	set_place(pol.config, "APP_CONFIG");
	set_place(pol.cache, "APP_CACHE");
	if (getenv("APP_ID"))
		snprintf(pol.tmp, POLICY_PATH, "/tmp/app-%s", getenv("APP_ID"));
	if (getenv("PERMISSIONS"))
		pol.perms = policy_perms(getenv("PERMISSIONS"));
	if (pol.dir[0])
		snprintf(cwd, sizeof(cwd), "%s", pol.dir);
	if ((v = getenv("SBSIM_END")))
		end_ms = atol(v);
	if ((v = getenv("SBSIM_FRAME_MS")))
		frame_us = strtol(v, 0, 10) * 1000;
	if ((v = getenv("SBSIM_TERM")))
		term_ms = atol(v);
	if ((v = getenv("SBSIM_SCREEN")))
		screen_ms = atol(v) * 1000;
	realtime = getenv("SBSIM_REALTIME") != 0;
	real_start_us = real_us();
	for (v = getenv("SBSIM_KEYS"); v && *v && n_keys + 2 < MAX_KEYS; v = *e ? e + 1 : e) {
		long at = strtol(v, &e, 10);
		int code = (int)strtol(e + 1, &e, 10);

		add_key(at, code, 1);
		add_key(at + 100, code, 0);
	}
	for (v = getenv("SBSIM_TAPS"); v && *v && n_keys + 2 < MAX_KEYS; v = *e ? e + 1 : e) {
		long at = strtol(v, &e, 10);
		int code = (int)strtol(e + 1, &e, 10);

		add_key(at, code, 1);
		add_key(at, code, 0);
	}
	for (v = getenv("SBSIM_SHOTS"); v && *v && n_shots < 256; v = *e ? e + 1 : e)
		shots[n_shots++] = strtol(v, &e, 10);
	n_standby = spans(getenv("SBSIM_STANDBY"), standby_spans);
	n_ppa_off = spans(getenv("SBSIM_PPA_OFF"), ppa_off);
	if ((v = getenv("SBSIM_FIFO"))) {
		const char *sp = strchr(v, ' ');

		snprintf(fifo_path, sizeof(fifo_path), "%.*s", sp ? (int)(sp - v) : (int)strlen(v), v);
		for (v = sp ? sp + 1 : ""; *v && n_fifo < 64; v = *e ? e + 1 : e) {
			fifo_writes[n_fifo].ms = strtol(v, &e, 10);
			v = e + 1;
			e = strchr(v, ' ');
			if (!e)
				e = (char *)v + strlen(v);
			snprintf(fifo_writes[n_fifo++].text, 32, "%.*s\n", (int)(e - v), v);
		}
	}
	atexit(report);
}
