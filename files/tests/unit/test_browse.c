/* SPDX-License-Identifier: MIT */
/* browse.c: folders, entries, Esc, the clipboard and the file operations,
 * on a folder of its own under $TMPDIR. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "browse.h"
#include "t.h"

static char root[200];

static void put_file(const char *rel, const char *text)
{
	char p[300];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", root, rel);
	f = fopen(p, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

static int exists(const char *rel)
{
	char p[300];

	snprintf(p, sizeof(p), "%s/%s", root, rel);
	return access(p, F_OK) == 0;
}

static char *at(const char *rel)
{
	static char p[300];

	snprintf(p, sizeof(p), "%s%s%s", root, rel[0] ? "/" : "", rel);
	return p;
}

/* a fresh folder: a/, B/, b.txt, c.PNG, .hidden */
static void fresh(struct browse *b)
{
	char cmd[300];
	const char *t = getenv("TMPDIR");

	snprintf(root, sizeof(root), "%s/files-test-%d", t && t[0] ? t : "/tmp", (int)getpid());
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
	if (system(cmd))
		return;
	mkdir(root, 0755);
	mkdir(at("a"), 0755);
	mkdir(at("B"), 0755);
	put_file("b.txt", "hello\n");
	put_file("c.PNG", "\x89PNG\r\n\x1a\n");
	put_file(".hidden", "x");
	memset(b, 0, sizeof(*b));
	{
		static char log[300];

		snprintf(log, sizeof(log), "%s.log", root);
		b->log = log;
		unlink(log);
	}
	b->run = br_run;
	br_go(b, root, 0);
	br_read(b);
}

TEST(join_and_extensions)
{
	char out[16];

	OK(br_join(out, sizeof(out), "/", "bin"));
	STREQ(out, "/bin");
	OK(br_join(out, sizeof(out), "/root", "a.md"));
	STREQ(out, "/root/a.md");
	OK(!br_join(out, sizeof(out), "/root/longer", "name.txt"));
	STREQ(br_ext("photo.JPG"), "JPG");
	STREQ(br_ext(".profile"), "");
	STREQ(br_ext("README"), "");
	OK(ext_in("jpg jpeg png", "JPG"));
	OK(ext_in("md, txt", "txt"));
	OK(!ext_in("md txt", "t"));
	OK(ext_in("*", "anything"));
	OK(!ext_in("md", ""));
	EQ(br_kind_of("x.jpeg", 0), BK_IMAGE);
	EQ(br_kind_of("x.jpeg", 1), BK_FOLDER);
	EQ(br_kind_of("index.htm", 0), BK_WEB);
	EQ(br_kind_of("game.gbc", 0), BK_GAMEBOY);
	EQ(br_kind_of("notes.md", 0), BK_TEXT);
	EQ(br_kind_of("book.pdf", 0), BK_DOC);
	EQ(br_kind_of("firmware.bin", 0), BK_FILE);
}

TEST(lists_folders_first_by_name)
{
	static struct browse b;

	fresh(&b);
	EQ(b.n, 4);
	STREQ(b.e[0].name, "a");
	STREQ(b.e[1].name, "B");
	STREQ(b.e[2].name, "b.txt");
	STREQ(b.e[3].name, "c.PNG");
	OK(b.e[0].dir && !b.e[0].reg);
	OK(b.e[2].reg && !b.e[2].dir);
	EQ(b.e[2].size, 6);
	EQ(b.e[3].kind, BK_IMAGE);
	b.hidden = 1;
	b.stale = 1;
	br_read(&b);
	EQ(b.n, 5);
	STREQ(b.e[2].name, ".hidden");
}

TEST(go_up_focuses_where_we_came_from)
{
	static struct browse b;

	fresh(&b);
	br_go(&b, at("B"), 0);
	br_read(&b);
	EQ(b.n, 0);
	OK(br_up(&b));
	br_read(&b);
	STREQ(b.dir, root);
	STREQ(b.e[b.sel].name, "B");
	br_go(&b, "/", 0);
	OK(!br_up(&b));			/* Esc closes Files at / */
	br_go(&b, "", 0);
	OK(br_drives(&b));
	OK(!br_up(&b));			/* and on Drives */
	br_go(&b, "/bin", 0);
	OK(br_up(&b));
	STREQ(b.dir, "/");
	STREQ(b.want, "bin");
}

TEST(a_folder_that_cannot_be_read)
{
	static struct browse b;

	fresh(&b);
	br_go(&b, at("gone"), 0);
	br_read(&b);
	EQ(b.n, 0);
	OK(b.notice_bad);
	OK(strstr(b.notice, "Cannot open ") == b.notice);
}

TEST(keeps_256_and_counts_the_rest)
{
	static struct browse b;
	char name[16];

	fresh(&b);
	for (int i = 0; i < 260; i++) {
		snprintf(name, sizeof(name), "a/f%03d", i);
		put_file(name, "");
	}
	br_go(&b, at("a"), "f259");
	br_read(&b);
	EQ(b.n, BR_MAX);
	EQ(b.more, 4);
	b.sel = 999;
	b.stale = 1;
	br_read(&b);
	EQ(b.sel, BR_MAX - 1);
}

TEST(copy_paste_cut_and_delete)
{
	static struct browse b;
	char log[4096] = "";
	int fd;

	fresh(&b);
	b.sel = 2;				/* b.txt */
	br_copy(&b, 0);
	STREQ(b.clip, at("b.txt"));
	OK(!b.clip_cut);
	EQ(br_paste(&b), -1);			/* it is here already */
	OK(strstr(b.notice, "Already here: b.txt") != 0);
	br_go(&b, at("a"), 0);
	br_read(&b);
	EQ(br_paste(&b), 0);
	STREQ(b.notice, "Copied b.txt");
	OK(!b.notice_bad);
	OK(exists("a/b.txt") && exists("b.txt"));
	STREQ(b.clip, at("b.txt"));		/* a copy can be pasted again */
	br_read(&b);
	STREQ(b.e[b.sel].name, "b.txt");

	/* a folder into itself */
	br_go(&b, root, "a");
	br_read(&b);
	br_copy(&b, 1);
	br_go(&b, at("a"), 0);
	br_read(&b);
	EQ(br_paste(&b), -1);
	OK(strstr(b.notice, "Cannot put a folder inside itself: a") != 0);

	/* Cut, then Paste: moved, and the clipboard is empty */
	br_go(&b, root, "c.PNG");
	br_read(&b);
	br_copy(&b, 1);
	br_go(&b, at("B"), 0);
	br_read(&b);
	EQ(br_paste(&b), 0);
	STREQ(b.notice, "Moved c.PNG");
	OK(exists("B/c.PNG") && !exists("c.PNG"));
	STREQ(b.clip, "");

	/* Delete: gone, and so is a clipboard that held it */
	br_go(&b, root, "a");
	br_read(&b);
	br_copy(&b, 0);
	EQ(br_delete(&b), 0);
	STREQ(b.notice, "Deleted a");
	OK(!exists("a"));
	STREQ(b.clip, "");

	/* the log: the commands, and nothing else when they worked */
	fd = open(b.log, O_RDONLY);
	OK(fd >= 0);
	if (fd >= 0) {
		log[read(fd, log, sizeof(log) - 1)] = 0;
		close(fd);
	}
	OK(strstr(log, "/bin/cp -a ") != 0);
	OK(strstr(log, "/bin/mv ") != 0);
	OK(strstr(log, "/bin/rm -rf ") != 0);
	OK(strstr(log, "failed") == 0);
}

TEST(new_folder_and_rename)
{
	static struct browse b;
	const char *err = 0;

	fresh(&b);
	EQ(br_name(&b, "x/y", 0, &err), 1);
	STREQ(err, "A name without /");
	EQ(br_name(&b, "..", 0, &err), 1);
	EQ(br_name(&b, "", 0, &err), 1);
	EQ(br_name(&b, "B", 0, &err), 1);
	STREQ(err, "That name is taken here");
	EQ(br_name(&b, "New folder", 0, &err), 0);
	OK(exists("New folder"));
	br_read(&b);
	STREQ(b.e[b.sel].name, "New folder");
	b.sel = 3;				/* b.txt */
	STREQ(b.e[b.sel].name, "b.txt");
	EQ(br_name(&b, "notes.txt", 1, &err), 0);
	OK(exists("notes.txt") && !exists("b.txt"));
	br_read(&b);
	STREQ(b.e[b.sel].name, "notes.txt");
}

TEST(text_or_not)
{
	static struct browse b;

	fresh(&b);
	OK(br_textlike(at("b.txt")));
	OK(!br_textlike(at("c.PNG")));
	OK(!br_textlike(at("nothing")));
}
