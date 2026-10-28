/* SPDX-License-Identifier: MIT */
/* app.c: the screens and keys, on a folder and apps of their own. */
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "t.h"

static char root[200], approot[200], conf[220];

static void put(const char *path, const char *text, int mode)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(text, f);
		fclose(f);
	}
	chmod(path, mode);
}

static const char *at(const char *rel)
{
	static char p[2][300];
	static int k;

	k ^= 1;
	snprintf(p[k], sizeof(p[k]), "%s%s%s", root, rel[0] ? "/" : "", rel);
	return p[k];
}

/* docs/ (empty), notes.md, page.html, x.bin; Write opens md, NetSurf html
 * and Write html too (OPENS_ALSO) */
static struct files *setup(void)
{
	static struct files f;
	const char *t = getenv("TMPDIR"), *r[1];
	char cmd[300], p[300];

	snprintf(root, sizeof(root), "%s/files-app-%d", t && t[0] ? t : "/tmp", (int)getpid());
	snprintf(approot, sizeof(approot), "%s-apps", root);
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", root, approot);
	if (system(cmd))
		return 0;
	mkdir(root, 0755);
	mkdir(approot, 0755);
	mkdir(at("docs"), 0755);
	put(at("notes.md"), "# Notes\n", 0644);
	put(at("page.html"), "<p>hi</p>\n", 0644);
	put(at("x.bin"), "\x01\x02\x00", 0644);
	snprintf(p, sizeof(p), "%s/write", approot);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/write/app.conf", approot);
	put(p, "NAME=\"Write\"\nEXEC=\"run\"\nOPENS=\"md txt\"\nOPENS_ALSO=\"html\"\n", 0644);
	snprintf(p, sizeof(p), "%s/write/run", approot);
	put(p, "#!/bin/sh\n", 0755);
	snprintf(p, sizeof(p), "%s/netsurf", approot);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/netsurf/app.conf", approot);
	put(p, "NAME=\"NetSurf\"\nEXEC=\"run\"\nOPENS=\"html\"\n", 0644);
	snprintf(p, sizeof(p), "%s/netsurf/run", approot);
	put(p, "#!/bin/sh\n", 0755);
	snprintf(conf, sizeof(conf), "%s-settings", root);
	unlink(conf);
	memset(&f, 0, sizeof(f));
	f.settings = conf;
	files_init(&f, at("notes.md"));
	r[0] = approot;
	apps_read(&f.apps, r, 1);
	snprintf(p, sizeof(p), "%s.log", root);
	unlink(p);
	{
		static char log[300];

		memcpy(log, p, sizeof(log));
		f.b.log = log;
	}
	files_build(&f);
	return &f;
}

static void keys(struct files *f, const int *k, int n)
{
	for (int i = 0; i < n; i++) {
		files_key(f, k[i], 1);
		files_build(f);
	}
}

#define KEYS(f, ...) do { static const int k_[] = { __VA_ARGS__ }; keys(f, k_, (int)(sizeof(k_) / sizeof(k_[0]))); } while (0)

static const char *focused(const struct files *f)
{
	return f->l.focus >= 0 ? f->l.rows[f->l.focus].label : "";
}

TEST(starts_on_the_file_it_was_given)
{
	struct files *f = setup();

	EQ(f->sc, SC_LIST);
	STREQ(f->b.dir, root);
	STREQ(focused(f), "notes.md");
	EQ(f->l.n, 4);
	EQ(f->l.rows[0].kind, R_FOLDER);
	EQ(f->l.rows[1].kind, R_FILE);
	STREQ(f->l.rows[1].value, "8 B");
	OK(strstr(root, f->l.title) != 0);
	STREQ(f->l.path, root);
}

TEST(enter_opens_folders_and_files_with_their_app)
{
	struct files *f = setup();

	KEYS(f, K_UP, K_ENTER);			/* docs/ */
	STREQ(f->b.dir, at("docs"));
	EQ(f->l.n, 1);
	EQ(f->l.rows[0].kind, R_INFO);
	STREQ(f->l.rows[0].label, "Empty folder");
	KEYS(f, K_ESC);				/* up, on docs */
	STREQ(f->b.dir, root);
	STREQ(focused(f), "docs");
	KEYS(f, K_DOWN, K_ENTER);		/* notes.md: only Write opens it */
	OK(f->quit);
	OK(strstr(f->open_dir, "/write") != 0);
	STREQ(f->open_file, at("notes.md"));
}

TEST(more_than_one_app_asks_which)
{
	struct files *f = setup();

	KEYS(f, K_DOWN, K_ENTER);		/* page.html: NetSurf; Write only under Open with */
	OK(f->quit);
	OK(strstr(f->open_dir, "/netsurf") != 0);
	f = setup();
	KEYS(f, K_DOWN, K_F1);			/* the options: Open with... first */
	EQ(f->sc, SC_PICK);
	STREQ(f->p.label[0], "Open with\xe2\x80\xa6");
	KEYS(f, K_ENTER);
	EQ(f->sc, SC_PICK);
	OK(f->p.openwith);
	EQ(f->p.n, 2);				/* NetSurf, Write */
	KEYS(f, K_DOWN, K_ENTER);
	OK(f->quit);
	OK(strstr(f->open_dir, "/write") != 0);
	STREQ(f->open_file, at("page.html"));
}

TEST(no_app_for_it)
{
	struct files *f = setup();

	KEYS(f, K_DOWN, K_DOWN, K_ENTER);	/* x.bin */
	OK(!f->quit);
	STREQ(f->l.notice, "No app opens this kind of file: x.bin");
	OK(f->l.notice_bad);
	KEYS(f, K_UP);				/* a key clears it */
	OK(!f->l.notice);
}

TEST(esc_goes_up_and_closes_at_the_top)
{
	struct files *f = setup();

	br_go(&f->b, "/", 0);
	files_build(f);
	KEYS(f, K_ESC);
	OK(f->quit);
	OK(!f->open_file[0]);
}

TEST(triangle_home_cross_drives)
{
	struct files *f = setup();

	KEYS(f, K_F2);
	STREQ(f->b.dir, "/root");
	KEYS(f, K_F3);
	OK(br_drives(&f->b));
	STREQ(f->l.title, "Drives");
	EQ(f->l.rows[0].kind, R_GROUP);
	STREQ(f->l.rows[0].label, "REMOVABLE");
	EQ(f->l.rows[f->l.focus].kind, R_DRIVE);
	STREQ(focused(f), "System");		/* the drive /root is on */
	KEYS(f, K_UP, K_UP, K_UP, K_UP, K_UP);	/* up to the card's FAT, past the group */
	STREQ(focused(f), "SD card, Partition 1");
	KEYS(f, K_ENTER);			/* mounted: open it */
	STREQ(f->b.dir, "/media/sd");
	KEYS(f, K_F3, K_DOWN, K_DOWN, K_ENTER);	/* the USB drive: not mounted (the host mounts nothing) */
	OK(br_drives(&f->b));
	OK(f->l.notice_bad);
	KEYS(f, K_ESC);				/* Esc on Drives closes */
	OK(f->quit);
}

TEST(copy_paste_rename_delete)
{
	struct files *f = setup();

	KEYS(f, K_F1);				/* notes.md's options */
	EQ(f->sc, SC_PICK);
	EQ(f->p.n, 6);				/* Open with, Rename, Copy, Cut, New folder, Delete */
	STREQ(f->p.label[2], "Copy");
	KEYS(f, K_DOWN, K_DOWN, K_ENTER);
	EQ(f->sc, SC_LIST);
	STREQ(f->l.tag, "Copied: notes.md");
	KEYS(f, K_UP, K_ENTER, K_F1);		/* in docs/: Paste here */
	STREQ(f->p.label[0], "Paste here");
	STREQ(f->p.sub[0], "notes.md");
	KEYS(f, K_ENTER);
	OK(access(at("docs/notes.md"), F_OK) == 0);
	STREQ(f->l.notice, "Copied notes.md");
	STREQ(focused(f), "notes.md");

	/* Rename: the cursor before the extension, the name replaced */
	KEYS(f, K_F1, K_DOWN, K_ENTER);
	EQ(f->sc, SC_INPUT);
	STREQ(f->in.buf, "notes.md");
	EQ(f->in.cur, 5);
	KEYS(f, K_BACKSPACE, K_BACKSPACE, K_BACKSPACE, K_BACKSPACE, K_BACKSPACE, 25, 23);	/* "pi" */
	STREQ(f->in.buf, "pi.md");
	KEYS(f, K_ENTER);
	EQ(f->sc, SC_LIST);
	OK(access(at("docs/pi.md"), F_OK) == 0);
	STREQ(focused(f), "pi.md");

	/* Delete asks, Cancel first */
	KEYS(f, K_F1, K_UP, K_ENTER);		/* up: round to the last, Delete */
	EQ(f->sc, SC_CONFIRM);
	EQ(f->cfocus, 0);
	KEYS(f, K_ENTER);
	OK(access(at("docs/pi.md"), F_OK) == 0);
	KEYS(f, K_F1, K_UP, K_ENTER, K_DOWN, K_ENTER);
	OK(access(at("docs/pi.md"), F_OK) != 0);
	STREQ(f->l.notice, "Deleted pi.md");
}

TEST(new_folder_replaces_the_offered_name)
{
	struct files *f = setup();

	KEYS(f, K_F1, K_UP, K_UP, K_ENTER);	/* New folder, above Delete */
	EQ(f->sc, SC_INPUT);
	STREQ(f->in.buf, "New folder");
	KEYS(f, 50, 24);			/* "mo" replaces it */
	STREQ(f->in.buf, "mo");
	KEYS(f, K_ENTER);
	OK(access(at("mo"), F_OK) == 0);
	STREQ(focused(f), "mo");
	KEYS(f, K_F1, K_UP, K_UP, K_ENTER, 32, 24, 46, 31, K_ENTER);	/* "docs" */
	EQ(f->sc, SC_INPUT);
	STREQ(f->in.err, "That name is taken here");
	KEYS(f, K_ESC);
	EQ(f->sc, SC_LIST);
}

TEST(held_keys)
{
	struct files *f = setup();

	files_key(f, K_UP, 1);			/* docs */
	files_build(f);
	files_key(f, K_ENTER, 2);		/* a held Enter does not open */
	files_build(f);
	STREQ(f->b.dir, root);
	files_key(f, K_DOWN, 2);		/* a held arrow moves */
	files_key(f, K_DOWN, 2);
	files_build(f);
	STREQ(focused(f), "page.html");
	files_key(f, K_ESC, 2);			/* nor closes */
	OK(!f->quit);
}

TEST(settings_log_and_help)
{
	struct files *f = setup();
	char buf[64] = "";
	FILE *s;

	put(at(".dot"), "", 0644);
	KEYS(f, K_F5);
	EQ(f->sc, SC_SETTINGS);
	EQ(f->l.rows[0].kind, R_TOGGLE);
	STREQ(f->l.rows[0].label, "Show hidden files");
	OK(!f->l.rows[0].on);
	KEYS(f, K_ENTER);
	OK(f->l.rows[0].on);
	s = fopen(conf, "r");
	OK(s != 0);
	if (s) {
		if (!fgets(buf, sizeof(buf), s))
			buf[0] = 0;
		fclose(s);
	}
	STREQ(buf, "hidden=1\n");
	KEYS(f, K_ESC);
	EQ(f->sc, SC_LIST);
	STREQ(f->l.rows[1].label, ".dot");	/* after docs/ */

	KEYS(f, K_F6);
	EQ(f->sc, SC_HELP);
	EQ(f->under, SC_LIST);
	KEYS(f, K_F6);
	EQ(f->sc, SC_LIST);

	put(f->b.log, "/bin/cp -a a b\n/bin/rm -rf c\nfailed, status 1\n", 0644);
	KEYS(f, K_F4);
	EQ(f->sc, SC_LOG);
	EQ(f->log_n, 3);
	STREQ(f->log_line[2], "failed, status 1");
	KEYS(f, K_F6, K_ESC);			/* the help over the log, closed */
	EQ(f->sc, SC_LOG);
	KEYS(f, K_ESC);				/* back to the list, not the log again */
	EQ(f->sc, SC_LIST);
}

TEST(the_columns_page_keys_and_del)
{
	struct files *f = setup();

	STREQ(f->l.rows[0].label, "docs");
	STREQ(f->l.rows[0].value, "0 items");
	STREQ(f->l.rows[1].date, "today");
	KEYS(f, K_PGDN);			/* a page: to the end of these four */
	STREQ(focused(f), "x.bin");
	KEYS(f, K_PGUP);
	STREQ(focused(f), "docs");
	KEYS(f, K_RIGHT);			/* the arrows across too */
	STREQ(focused(f), "x.bin");
	KEYS(f, K_DELETE);			/* as the options' Delete: Cancel first */
	EQ(f->sc, SC_CONFIRM);
	EQ(f->cfocus, 0);
	KEYS(f, K_ESC);
	EQ(f->sc, SC_LIST);
	OK(access(at("x.bin"), F_OK) == 0);
	put(at("docs/a"), "", 0644);
	put(at("docs/.b"), "", 0644);
	f->b.stale = 1;
	files_build(f);
	STREQ(f->l.rows[0].value, "1 item");	/* .b is hidden */
}

TEST(not_a_regular_file)
{
	struct files *f = setup();

	mkfifo(at("pipe"), 0644);
	br_go(&f->b, root, "pipe");
	files_build(f);
	STREQ(focused(f), "pipe");
	KEYS(f, K_ENTER);
	STREQ(f->l.notice, "Not a regular file: pipe");
	OK(!f->quit);
}
