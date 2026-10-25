/* SPDX-License-Identifier: MIT */
/* Files: the app as a state machine (app.h). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "app.h"

#define LOG_FILE	"/tmp/files.log"

/* US layout on the badge's keys, as the menu's key_char; 0 for keys that
 * type nothing */
static char key_char(int code, int shift)
{
	static const char lo[] =
		"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`"
		"\0" "\\zxcvbnm,./";
	static const char hi[] =
		"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~"
		"\0" "|ZXCVBNM<>?";

	if (code == K_SPACE)
		return ' ';
	if (code < 0 || code >= (int)sizeof(lo) - 1)
		return 0;
	return shift ? hi[code] : lo[code];
}

/* ---- settings: hidden files ------------------------------------------------ */

/*
 * $APP_CONFIG/settings: "hidden=1". Before Files was an app the menu kept
 * it as files.hidden in /etc/badge/settings (why2025-linux settings.h):
 * taken over while Files has no file of its own.
 */
static void settings_read(struct files *f)
{
	char buf[256];
	int fd = open(f->settings, O_RDONLY | O_CLOEXEC), n;

	if (fd < 0) {
		fd = open("/etc/badge/settings", O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return;
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		buf[n > 0 ? n : 0] = 0;
		for (char *l = buf; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0)
			if (!strncmp(l, "files.hidden\t", 13)) {
				char *v = strrchr(l, '\t'), *e = strchr(l, '\n');

				f->b.hidden = v && (!e || v < e) && !strncmp(v + 1, "on", 2);
			}
		return;
	}
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	for (char *l = buf; l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0)
		if (!strncmp(l, "hidden=", 7))
			f->b.hidden = l[7] == '1';
}

static void settings_write(const struct files *f)
{
	char tmp[300], line[16];
	int fd, n;

	if (!f->settings)
		return;
	snprintf(tmp, sizeof(tmp), "%s.new", f->settings);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	n = snprintf(line, sizeof(line), "hidden=%d\n", f->b.hidden);
	if (write(fd, line, (size_t)n) == n) {
		close(fd);
		rename(tmp, f->settings);
		return;
	}
	close(fd);
	unlink(tmp);
}

/* ---- the list on screen ----------------------------------------------------- */

static struct row *add(struct list *l, enum row_kind kind, const char *label, enum row_icon ic)
{
	struct row *r = &l->rows[l->n < LIST_MAX ? l->n++ : LIST_MAX - 1];

	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->ic = ic;
	if (label)
		snprintf(r->label, sizeof(r->label), "%s", label);
	return r;
}

static enum row_icon icon_of(enum br_kind k)
{
	static const enum row_icon ic[] = {
		[BK_FILE] = I_FILE, [BK_FOLDER] = I_FOLDER, [BK_IMAGE] = I_IMAGE, [BK_WEB] = I_GLOBE,
		[BK_DOC] = I_DOC, [BK_GAMEBOY] = I_GAMEBOY, [BK_TEXT] = I_TEXT,
	};

	return ic[k];
}

/* Drives (Docs/06 6.10): every disk's file systems, then the badge's own */
static void build_drives(struct files *f)
{
	struct list *l = &f->l;
	struct row *r;

	l->title = "Drives";
	add(l, R_GROUP, "REMOVABLE", I_NONE);
	if (!f->d.n_disk || f->d.disk[0].usb)
		add(l, R_INFO, "No SD card", I_SDCARD);
	for (int k = 0; k < f->d.n_disk; k++) {
		const struct diskinfo *dk = &f->d.disk[k];
		int any = 0;

		for (int i = 0; i < f->d.n; i++) {
			if (f->d.fs[i].disk != k)
				continue;
			r = add(l, R_DRIVE, 0, dk->usb ? I_USBSTICK : I_SDCARD);
			fs_name(&f->d, &f->d.fs[i], r->label, sizeof(r->label));
			r->idx = i;
			any = 1;
		}
		if (!any) {
			r = add(l, R_INFO, dk->label, dk->usb ? I_USBSTICK : I_SDCARD);
			snprintf(r->value, sizeof(r->value), "%s", dk->size ? "No partitions" : "No medium");
		}
	}
	add(l, R_GROUP, "BADGE", I_NONE);
	for (int i = 0; i < f->d.n; i++)
		if (f->d.fs[i].disk < 0) {
			r = add(l, R_DRIVE, f->d.fs[i].label, !strcmp(f->d.fs[i].mnt, "/tmp") ? I_CHIP : I_DRIVE);
			r->idx = i;
		}
	if (f->drive < 0 || f->drive >= f->d.n)
		f->drive = f->d.n_disk && f->d.fs[0].disk == 0 ? 0 : f->d.n - 3;
	l->focus = -1;
	for (int i = 0; i < l->n; i++)
		if (l->rows[i].kind == R_DRIVE && l->rows[i].idx == f->drive)
			l->focus = i;
}

static void build_folder(struct files *f)
{
	struct browse *b = &f->b;
	struct list *l = &f->l;
	const struct fsinfo *fs = disks_fs_of(&f->d, b->dir);
	const char *base = strrchr(b->dir, '/');
	size_t wn = strlen(b->dir);
	int last;

	if (b->clip[0]) {	/* the clipboard, in pink next to the path */
		const char *cb = strrchr(b->clip, '/');

		snprintf(l->tag, sizeof(l->tag), "%s%s", b->clip_cut ? "Cut: " : "Copied: ", cb ? cb + 1 : b->clip);
	}
	/* a drive's top by its name ("SD card"), else the folder's */
	if (fs && !strcmp(fs->mnt, b->dir))
		fs_name(&f->d, fs, l->title_buf, sizeof(l->title_buf));
	else
		snprintf(l->title_buf, sizeof(l->title_buf), "%s", base && base[1] ? base + 1 : b->dir);
	l->title = l->title_buf;
	/* long paths keep their end */
	if (wn < sizeof(l->path))
		memcpy(l->path, b->dir, wn + 1);
	else
		snprintf(l->path, sizeof(l->path), "\xe2\x80\xa6%s", b->dir + wn - (sizeof(l->path) - 4));
	if (fs && fs->size >= fs->used && strcmp(fs->type, "squashfs")) {
		strcat(human(l->title_r, fs->size - fs->used), " free");
	} else if (fs) {
		struct statvfs st;

		/* /root and /etc: overlays on /data over the read-only system */
		if (statvfs(b->dir, &st) == 0 && !(st.f_flag & ST_RDONLY))
			strcat(human(l->title_r, (unsigned long long)st.f_bavail * st.f_frsize), " free");
		else
			snprintf(l->title_r, sizeof(l->title_r), "read-only");
	}
	if (b->more && !l->notice) {
		static char more[64];

		snprintf(more, sizeof(more), "%d more not shown: the list holds %d", b->more, BR_MAX);
		l->notice = more;
		l->notice_bad = 1;
	}
	if (b->n == 0) {
		add(l, R_INFO, "Empty folder", I_INFO);
		l->focus = -1;
		return;
	}
	if (b->sel < b->off || b->sel >= b->off + BR_SHOWN)
		b->off = b->sel - BR_SHOWN / 2;
	if (b->off > b->n - BR_SHOWN)
		b->off = b->n - BR_SHOWN;
	if (b->off < 0)
		b->off = 0;
	last = b->off + BR_SHOWN < b->n ? b->off + BR_SHOWN : b->n;
	/* the scroll bar stands for all entries, not the 20 in the list */
	l->sb_total = b->n * (56 + 10) - 10;
	l->sb_first = b->off * (56 + 10);
	for (int i = b->off; i < last; i++) {
		const struct br_ent *e = &b->e[i];
		struct row *r = add(l, e->dir ? R_FOLDER : R_FILE, e->name, icon_of(e->kind));

		r->idx = i;
		if (i == b->sel)
			l->focus = l->n - 1;
		if (e->dir) {		/* a mount point: its free space */
			char path[320];
			const struct fsinfo *m = br_join(path, sizeof(path), b->dir, e->name) ?
						 disks_mount_at(&f->d, path) : 0;

			if (m && m->size > m->used)
				strcat(human(r->value, m->size - m->used), " free");
		} else if (e->reg) {
			human(r->value, e->size);
		}
	}
}

void files_build(struct files *f)
{
	struct list *l = &f->l;

	memset(l, 0, sizeof(*l));
	l->focus = -1;
	if (f->sc == SC_SETTINGS || (f->under == SC_SETTINGS && (f->sc == SC_HELP || f->sc == SC_LOG))) {
		struct row *r;

		l->title = "Files settings";
		r = add(l, R_TOGGLE, "Show hidden files", I_FILE);
		r->on = f->b.hidden;
		add(l, R_INFO, "Names that start with a dot", I_INFO);
		l->focus = 0;
		return;
	}
	br_read(&f->b);
	if (f->b.notice[0]) {
		l->notice = f->b.notice;
		l->notice_bad = f->b.notice_bad;
	}
	if (br_drives(&f->b))
		build_drives(f);
	else
		build_folder(f);
}

void files_refresh(struct files *f)
{
	disks_read(&f->d);
}

/* ---- opening ---------------------------------------------------------------- */

static void open_with(struct files *f, int app, const char *path)
{
	snprintf(f->open_dir, sizeof(f->open_dir), "%s", f->apps.a[app].dir);
	snprintf(f->open_file, sizeof(f->open_file), "%s", path);
	f->quit = 1;
}

static void pick_reset(struct files *f, const char *title)
{
	memset(&f->p, 0, sizeof(f->p));
	snprintf(f->p.title_buf, sizeof(f->p.title_buf), "%s", title);
	f->p.title = f->p.title_buf;
}

/* the apps for the focused file, under Open with; 0 when none */
static int pick_apps(struct files *f, int all)
{
	int n;

	pick_reset(f, "Open with");
	n = apps_for(&f->apps, br_ext(f->b.e[f->b.sel].name), all, f->p.cand, PICK_MAX);
	for (int i = 0; i < n; i++)
		f->p.label[i] = f->apps.a[f->p.cand[i]].name;
	f->p.n = n;
	f->p.openwith = 1;
	return n;
}

/* Enter on an entry: into a folder, or open a file with its app */
static void enter(struct files *f)
{
	struct browse *b = &f->b;
	const struct br_ent *e;
	char path[320];

	if (!b->n || !br_sel_path(b, path, sizeof(path)))
		return;
	e = &b->e[b->sel];
	if (e->dir) {
		br_go(b, path, 0);
		return;
	}
	if (!e->reg) {		/* a device, pipe or socket: opening it can do things */
		br_say(b, "Not a regular file: ", e->name, 1);
		return;
	}
	if (!pick_apps(f, 0)) {
		int t;

		/* no app for its extension: text goes to what opens .txt */
		if (br_textlike(path) && (t = apps_for_text(&f->apps)) >= 0) {
			open_with(f, t, path);
			return;
		}
		if (!pick_apps(f, 1)) {
			br_say(b, "No app opens this kind of file: ", e->name, 1);
			return;
		}
	}
	if (f->p.n == 1) {
		open_with(f, f->p.cand[0], path);
		return;
	}
	f->sc = SC_PICK;
	f->under = SC_LIST;
}

/* the options pop-up (the square key): first the item's options, under a
 * line the screen's own (Paste here, New folder), Delete last, in red */
static void options(struct files *f)
{
	static const char *const names[] = {
		"Open with\xe2\x80\xa6", "Rename", "Copy", "Cut", "Paste here", "New folder", "Delete",
	};
	struct browse *b = &f->b;
	struct pick *p = &f->p;
	int have = b->n > 0, n = 0;

	pick_reset(f, have ? b->e[b->sel].name : f->l.title ? f->l.title : "");
	if (have && b->e[b->sel].reg)
		p->ops[n++] = OP_OPENWITH;
	if (have) {
		p->ops[n++] = OP_RENAME;
		p->ops[n++] = OP_COPY;
		p->ops[n++] = OP_CUT;
	}
	if (b->clip[0])
		p->ops[n++] = OP_PASTE;
	p->ops[n++] = OP_MKDIR;
	if (have)
		p->ops[n++] = OP_DELETE;
	for (int i = 0; i < n; i++) {
		p->label[i] = names[p->ops[i]];
		p->sep[i] = i && (p->ops[i] == OP_DELETE || ((p->ops[i] == OP_PASTE || p->ops[i] == OP_MKDIR) &&
							 p->ops[i - 1] != OP_PASTE));
		p->danger[i] = p->ops[i] == OP_DELETE;
		if (p->ops[i] == OP_PASTE) {	/* what would be pasted */
			const char *base = strrchr(b->clip, '/');

			snprintf(p->paste, sizeof(p->paste), "%s", base ? base + 1 : b->clip);
			p->sub[i] = p->paste;
		}
	}
	p->n = n;
	f->sc = SC_PICK;
	f->under = SC_LIST;
}

static void input_start(struct files *f, int rename_it, const char *title, const char *value)
{
	struct input *in = &f->in;

	memset(in, 0, sizeof(*in));
	in->rename = rename_it;
	in->title = title;
	snprintf(in->buf, sizeof(in->buf), "%s", value);
	in->len = in->cur = (int)strlen(in->buf);
	if (rename_it) {		/* before the extension (6.10) */
		const char *dot = strrchr(in->buf, '.');

		if (dot && dot != in->buf)
			in->cur = (int)(dot - in->buf);
	} else {
		in->fresh = 1;
	}
	f->sc = SC_INPUT;
}

static void pick_done(struct files *f)
{
	struct pick *p = &f->p;
	char path[320];

	f->sc = SC_LIST;
	if (p->focus < 0 || p->focus >= p->n)
		return;
	if (p->openwith) {
		if (br_sel_path(&f->b, path, sizeof(path)))
			open_with(f, p->cand[p->focus], path);
		return;
	}
	switch (p->ops[p->focus]) {
	case OP_OPENWITH:
		if (!pick_apps(f, 1)) {
			br_say(&f->b, "No app opens this kind of file: ", f->b.e[f->b.sel].name, 1);
			break;
		}
		f->sc = SC_PICK;
		break;
	case OP_RENAME:
		input_start(f, 1, "Rename", f->b.e[f->b.sel].name);
		break;
	case OP_COPY:
	case OP_CUT:
		br_copy(&f->b, p->ops[p->focus] == OP_CUT);
		break;
	case OP_PASTE:
		br_paste(&f->b);
		break;
	case OP_MKDIR:
		input_start(f, 0, "New folder", "New folder");
		break;
	case OP_DELETE:
		f->cfocus = 0;		/* Cancel first */
		f->sc = SC_CONFIRM;
		break;
	}
}

static void input_key(struct files *f, int key)
{
	struct input *in = &f->in;
	char ch = key_char(key, f->shift);

	in->err = 0;
	if (in->fresh && key != K_BACKSPACE && ch) {
		in->len = in->cur = 0;
		in->buf[0] = 0;
	}
	in->fresh = 0;
	if (key == K_BACKSPACE) {
		if (in->cur) {
			memmove(in->buf + in->cur - 1, in->buf + in->cur, (size_t)(in->len - in->cur + 1));
			in->cur--;
			in->len--;
		}
	} else if (key == K_DELETE) {
		if (in->cur < in->len) {
			memmove(in->buf + in->cur, in->buf + in->cur + 1, (size_t)(in->len - in->cur));
			in->len--;
		}
	} else if (key == K_LEFT || key == K_RIGHT) {
		in->cur += key == K_LEFT ? (in->cur > 0 ? -1 : 0) : (in->cur < in->len ? 1 : 0);
	} else if (ch && in->len < (int)sizeof(in->buf) - 1) {
		memmove(in->buf + in->cur + 1, in->buf + in->cur, (size_t)(in->len - in->cur + 1));
		in->buf[in->cur++] = ch;
		in->len++;
	}
}

/* ---- the log (the circle key) -------------------------------------------- */

/* the end of the log; no file or an empty one shows "No log yet." */
static void log_open(struct files *f)
{
	int fd = open(f->b.log, O_RDONLY | O_CLOEXEC), n = 0;
	off_t end = 0;
	char *p;

	f->log_n = f->log_top = 0;
	f->log_buf[0] = 0;
	if (fd >= 0) {
		end = lseek(fd, 0, SEEK_END);
		lseek(fd, end > (off_t)sizeof(f->log_buf) - 1 ? end - (off_t)sizeof(f->log_buf) + 1 : 0, SEEK_SET);
		n = (int)read(fd, f->log_buf, sizeof(f->log_buf) - 1);
		close(fd);
		f->log_buf[n > 0 ? n : 0] = 0;
	}
	p = f->log_buf;
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
			if (f->log_n == LOG_LINES) {	/* keep the last ones */
				memmove(f->log_line, f->log_line + 1, sizeof(f->log_line) - sizeof(f->log_line[0]));
				f->log_n--;
			}
			f->log_line[f->log_n++] = p;
		}
		if (!*e)
			break;
		*e = 0;
		p = e + 1;
	}
	f->log_top = f->log_n > LOG_ROWS ? f->log_n - LOG_ROWS : 0;	/* the newest in view */
}

/* ---- keys ------------------------------------------------------------------- */

void files_init(struct files *f, const char *path)
{
	const char *config = getenv("APP_CONFIG");
	static char conf[256];
	struct stat st;

	f->sc = f->under = SC_LIST;
	f->drive = -1;
	if (!f->settings && config) {
		mkdir(config, 0755);
		snprintf(conf, sizeof(conf), "%s/settings", config);
		f->settings = conf;
	}
	if (f->settings)
		settings_read(f);
	if (!f->b.log)
		f->b.log = LOG_FILE;
	if (!f->b.run)
		f->b.run = br_run;
	apps_read(&f->apps, apps_roots, 3);
	disks_read(&f->d);
	if (!path) {
		br_go(&f->b, "/", 0);
	} else if (!path[0]) {
		br_go(&f->b, "", 0);
	} else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		br_go(&f->b, path, 0);
	} else {			/* back in the folder, on the file */
		char dir[256];
		const char *slash = strrchr(path, '/');

		snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path) : 0, path);
		br_go(&f->b, dir[0] ? dir : "/", slash ? slash + 1 : path);
	}
}

/* the cross: to Drives, on the drive we are in */
static void to_drives(struct files *f)
{
	const struct fsinfo *fs;

	disks_read(&f->d);
	fs = disks_fs_of(&f->d, f->b.dir);
	br_go(&f->b, "", 0);
	f->drive = fs ? (int)(fs - f->d.fs) : -1;
}

static void list_key(struct files *f, int key, int steps)
{
	struct browse *b = &f->b;
	struct list *l = &f->l;

	if (br_drives(b)) {
		if (key == K_UP || key == K_DOWN) {
			for (int i = l->focus + (key == K_UP ? -1 : 1); l->focus >= 0 && i >= 0 && i < l->n;
			     i += key == K_UP ? -1 : 1)
				if (l->rows[i].kind == R_DRIVE) {
					f->drive = l->rows[i].idx;
					break;
				}
		} else if (key == K_ENTER && l->focus >= 0 && l->rows[l->focus].kind == R_DRIVE) {
			int i = disks_open(&f->d, l->rows[l->focus].idx, b->notice, sizeof(b->notice));

			if (i >= 0)
				br_go(b, f->d.fs[i].mnt, 0);
			else
				b->notice_bad = 1;
		} else if (key == K_F2) {
			br_go(b, "/root", 0);
		} else if (key == K_ESC) {
			f->quit = 1;
		}
		return;
	}
	if (key == K_UP || key == K_DOWN || key == K_LEFT || key == K_RIGHT) {
		/* the arrows across: a page */
		b->sel += key == K_UP ? -steps : key == K_DOWN ? steps : key == K_LEFT ? -7 : 7;
		b->sel = b->sel < 0 ? 0 : b->sel >= b->n ? b->n - 1 : b->sel;
		if (b->sel < 0)
			b->sel = 0;
	} else if (key == K_ENTER) {
		enter(f);
	} else if (key == K_F1) {
		options(f);
	} else if (key == K_F2) {
		br_go(b, "/root", 0);
	} else if (key == K_F3) {
		to_drives(f);
	} else if (key == K_ESC) {
		if (!br_up(b))
			f->quit = 1;
	}
}

void files_key(struct files *f, int key, int value)
{
	int steps = 1;

	if (key == K_LEFTSHIFT || key == K_RIGHTSHIFT) {
		f->shift = value != 0;
		return;
	}
	if (value == 2) {
		/* held: the steps that repeat (why2025-linux keyrep.h), faster
		 * after 1.5 s (30 repeats) and 2.5 s, as the menu's lists */
		int step = key == K_UP || key == K_DOWN || key == K_LEFT || key == K_RIGHT ||
			   key == K_BACKSPACE || key == K_DELETE || key_char(key, 0);

		if (!step)
			return;
		f->held++;
		if (f->sc == SC_LIST && (key == K_UP || key == K_DOWN))
			steps = f->held >= 50 ? 4 : f->held >= 30 ? 2 : 1;
	} else if (value == 1) {
		f->held = 0;
	} else {
		return;
	}
	f->b.notice[0] = 0;
	/* the right three symbol keys do the same on every screen but pop-ups
	 * (Docs/06 6.6): the circle the log, the cloud the settings, the
	 * diamond this screen's help */
	if ((f->sc == SC_LIST || f->sc == SC_SETTINGS || f->sc == SC_LOG) &&
	    (key == K_F4 || key == K_F5 || key == K_F6)) {
		if (key == K_F6) {
			f->under = f->sc;
			f->sc = SC_HELP;
		} else if (key == K_F4 && f->sc != SC_LOG) {
			log_open(f);
			f->under = f->sc;
			f->sc = SC_LOG;
		} else if (key == K_F5 && f->sc == SC_LIST) {
			f->sc = SC_SETTINGS;
		}
		return;
	}
	switch (f->sc) {
	case SC_HELP:
		if (key == K_ESC || key == K_F6)
			f->sc = f->under;
		break;
	case SC_LOG:
		if (key == K_ESC)
			f->sc = f->under;
		else if (key == K_UP)
			f->log_top = f->log_top > steps ? f->log_top - steps : 0;
		else if (key == K_DOWN && f->log_top + LOG_ROWS < f->log_n)
			f->log_top++;
		break;
	case SC_SETTINGS:
		if (key == K_ESC) {
			f->sc = SC_LIST;
		} else if (key == K_ENTER) {
			f->b.hidden = !f->b.hidden;
			f->b.stale = 1;
			settings_write(f);
		}
		break;
	case SC_PICK:
		if ((key == K_UP || key == K_DOWN) && f->p.n)
			f->p.focus = (f->p.focus + (key == K_UP ? f->p.n - 1 : 1)) % f->p.n;
		else if (key == K_ESC)
			f->sc = SC_LIST;
		else if (key == K_ENTER)
			pick_done(f);
		break;
	case SC_INPUT:
		if (key == K_ESC) {
			f->sc = SC_LIST;
		} else if (key == K_ENTER) {
			if (!br_name(&f->b, f->in.buf, f->in.rename, &f->in.err))
				f->sc = SC_LIST;
		} else {
			input_key(f, key);
		}
		break;
	case SC_CONFIRM:
		if (key == K_UP || key == K_DOWN) {
			f->cfocus ^= 1;
		} else if (key == K_ESC) {
			f->sc = SC_LIST;
		} else if (key == K_ENTER) {
			f->sc = SC_LIST;
			if (f->cfocus == 1)
				br_delete(&f->b);
		}
		break;
	case SC_LIST:
		list_key(f, key, steps);
		break;
	}
}
