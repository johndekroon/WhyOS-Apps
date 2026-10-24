/* SPDX-License-Identifier: MIT */
/* Files (src/files.c): loading, saving safely, names, the Open list, dates. */
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "t.h"
#include "files.h"

static char dir[256];

static void fresh(void)
{
	snprintf(dir, sizeof(dir), "%s/write-test-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OK(mkdtemp(dir) != 0);
}

static void put(const char *name, const char *body, time_t mtime)
{
	char p[512];
	FILE *f;
	struct utimbuf u = { mtime, mtime };

	snprintf(p, sizeof(p), "%s/%s", dir, name);
	f = fopen(p, "w");
	fputs(body, f);
	fclose(f);
	if (mtime)
		utime(p, &u);
}

TEST(save_and_load)
{
	char p[512], err[128];
	char *data;
	size_t len;

	fresh();
	snprintf(p, sizeof(p), "%s/a.md", dir);
	EQ(file_save(p, "hello\n", 6, err, sizeof(err)), 0);
	EQ(file_load(p, &data, &len, err, sizeof(err)), 0);
	EQ(len, 6);
	OK(!memcmp(data, "hello\n", 6));
	free(data);
	/* nothing of the temporary file is left */
	snprintf(p, sizeof(p), "%s/.a.md.write", dir);
	EQ(access(p, F_OK), -1);
}

TEST(save_keeps_the_mode)
{
	char p[512], err[128];
	struct stat st;

	fresh();
	put("run.sh", "#!/bin/sh\n", 0);
	snprintf(p, sizeof(p), "%s/run.sh", dir);
	chmod(p, 0755);
	EQ(file_save(p, "#!/bin/sh\necho\n", 15, err, sizeof(err)), 0);
	stat(p, &st);
	EQ(st.st_mode & 0777, 0755);
}

TEST(save_reports_errors)
{
	char err[128];

	EQ(file_save("/nonexistent-dir/x.md", "x", 1, err, sizeof(err)), -1);
	OK(strlen(err) > 0);
}

TEST(load_refuses_big_and_binary_files)
{
	char p[512], err[128];
	char *data, *big;
	size_t len;
	FILE *f;

	fresh();
	snprintf(p, sizeof(p), "%s/big.txt", dir);
	big = malloc(FILE_MAX + 10);
	memset(big, 'x', FILE_MAX + 10);
	f = fopen(p, "w");
	fwrite(big, 1, FILE_MAX + 10, f);
	fclose(f);
	free(big);
	EQ(file_load(p, &data, &len, err, sizeof(err)), -1);
	STREQ(err, "Too large for Write (over 256 KB)");
	put("bin.dat", "a\0b", 0);
	snprintf(p, sizeof(p), "%s/bin.dat", dir);
	f = fopen(p, "w");
	fwrite("ab\0cd", 1, 5, f);
	fclose(f);
	EQ(file_load(p, &data, &len, err, sizeof(err)), -1);
	STREQ(err, "Not a text file");
	snprintf(p, sizeof(p), "%s/missing.md", dir);
	EQ(file_load(p, &data, &len, err, sizeof(err)), -1);
	STREQ(err, "No such file");
}

TEST(names)
{
	char why[128];

	EQ(name_ok("camp-notes v2_final.md", why, sizeof(why)), 1);
	EQ(name_ok("", why, sizeof(why)), 0);
	STREQ(why, "Type a name");
	EQ(name_ok("a/b", why, sizeof(why)), 0);
	STREQ(why, "A name cannot have a /");
	EQ(name_ok("what?", why, sizeof(why)), 0);
	STREQ(why, "Use letters, digits, spaces, - _ and .");
	EQ(name_ok(".", why, sizeof(why)), 0);
	EQ(name_ok("..", why, sizeof(why)), 0);
	EQ(name_ok("caf\xc3\xa9", why, sizeof(why)), 1);	/* letters beyond ASCII */
}

TEST(name_suggestions)
{
	char out[64];

	fresh();
	name_suggest(dir, 0, "Hacker camp notes", ".md", out, sizeof(out));
	STREQ(out, "hacker-camp-notes");
	name_suggest(dir, 0, "  What's up?! ", ".md", out, sizeof(out));
	STREQ(out, "whats-up");
	name_suggest(dir, 0, "", ".md", out, sizeof(out));
	STREQ(out, "untitled");
	name_suggest(dir, "camp-notes.md", "x", ".md", out, sizeof(out));
	STREQ(out, "camp-notes-v2");
	put("camp-notes-v2.md", "", 0);
	name_suggest(dir, "camp-notes.md", "x", ".md", out, sizeof(out));
	STREQ(out, "camp-notes-v3");
	name_suggest(dir, "camp-notes-v3.md", "x", ".md", out, sizeof(out));
	STREQ(out, "camp-notes-v4");
}

TEST(rescue_path_for_unnamed)
{
	char out[512], want[512];

	fresh();
	rescue_path(dir, out, sizeof(out));
	snprintf(want, sizeof(want), "%s/untitled.md", dir);
	STREQ(out, want);
	put("untitled.md", "", 0);
	rescue_path(dir, out, sizeof(out));
	snprintf(want, sizeof(want), "%s/untitled-2.md", dir);
	STREQ(out, want);
}

TEST(open_list)
{
	struct entry *e;
	int n;
	char err[128];

	fresh();
	put("old.md", "", 1000000);
	put("new.txt", "", 3000000);
	put("blink.py", "", 2000000);
	put("photo.jpg", "", 4000000);
	put(".hidden.md", "", 5000000);
	mkdir(strcat(strcpy(err, dir), "/talks"), 0755);
	mkdir(strcat(strcpy(err, dir), "/notes"), 0755);
	put("notes/a.md", "", 0);
	put("notes/b.md", "", 0);
	n = list_dir(dir, 0, &e, err, sizeof(err));
	EQ(n, 5);
	STREQ(e[0].name, "notes");		/* folders first, by name */
	EQ(e[0].dir, 1);
	EQ(e[0].items, 2);
	STREQ(e[1].name, "talks");
	STREQ(e[2].name, "new.txt");		/* then files, newest first */
	STREQ(e[3].name, "blink.py");
	STREQ(e[4].name, "old.md");
	free(e);
	n = list_dir(dir, 1, &e, err, sizeof(err));	/* folders only */
	EQ(n, 2);
	free(e);
}

TEST(dates_as_people_say_them)
{
	char out[32];
	struct tm tm = { 0 };
	time_t now, t;

	tm.tm_year = 126;
	tm.tm_mon = 9;
	tm.tm_mday = 24;
	tm.tm_hour = 15;
	tm.tm_min = 0;
	tm.tm_isdst = -1;
	now = mktime(&tm);
	tm.tm_hour = 14;
	tm.tm_min = 30;
	t = mktime(&tm);
	when_text(t, now, out, sizeof(out));
	STREQ(out, "Today 14:30");
	when_text(t - 86400, now, out, sizeof(out));
	STREQ(out, "Yesterday");
	when_text(t - 5 * 86400, now, out, sizeof(out));
	STREQ(out, "19 Oct");
	when_text(t - 400 * 86400, now, out, sizeof(out));
	STREQ(out, "19 Sep 2025");
}

TEST(path_parts)
{
	char d[256], b[256];

	path_split("/root/notes/camp.md", d, sizeof(d), b, sizeof(b));
	STREQ(d, "/root/notes");
	STREQ(b, "camp.md");
	path_split("/x.md", d, sizeof(d), b, sizeof(b));
	STREQ(d, "/");
	STREQ(b, "x.md");
	STREQ(path_ext("a/b.tar.gz"), ".gz");
	STREQ(path_ext("README"), "");
	STREQ(path_ext(".bashrc"), "");
}
