/* SPDX-License-Identifier: MIT */
/* apps.c: app.conf files under two roots of their own, under $TMPDIR. */
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apps.h"
#include "t.h"

static char roots[2][200];

static void app(int r, const char *dir, const char *conf, int runs)
{
	char p[300];
	FILE *f;

	snprintf(p, sizeof(p), "%s/%s", roots[r], dir);
	mkdir(p, 0755);
	snprintf(p, sizeof(p), "%s/%s/app.conf", roots[r], dir);
	f = fopen(p, "w");
	fputs(conf, f);
	fclose(f);
	snprintf(p, sizeof(p), "%s/%s/run", roots[r], dir);
	f = fopen(p, "w");
	fputs("#!/bin/sh\n", f);
	fclose(f);
	chmod(p, runs ? 0755 : 0644);
}

static void setup(struct apps *s)
{
	const char *t = getenv("TMPDIR");
	const char *r[2];
	char cmd[300];

	for (int i = 0; i < 2; i++) {
		snprintf(roots[i], sizeof(roots[i]), "%s/files-apps-%d-%d", t && t[0] ? t : "/tmp", (int)getpid(), i);
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", roots[i]);
		if (system(cmd))
			return;
		mkdir(roots[i], 0755);
		r[i] = roots[i];
	}
	app(0, "write", "NAME=\"Write\"\nEXEC=\"run\"\nARGS=\"%f\"\nOPENS=\"md markdown txt\"\n"
		"OPENS_ALSO=\"conf sh py\"\nID=\"write\"\n", 1);
	app(0, "netsurf", "NAME=\"NetSurf\"\nEXEC=\"run\"\nOPENS=\"html htm\"\nOPENS_ALSO=\"txt\"\n", 1);
	app(0, "broken", "NAME=\"Broken\"\nEXEC=\"run\"\nOPENS=\"md\"\n", 0);	/* does not run */
	app(1, "hex-viewer", "NAME=\"Hex viewer\"\nEXEC=\"run\"\nCATEGORY=\"none\"\nOPENS_ALSO=\"*\"\n", 1);
	app(1, "text-viewer", "NAME=\"Text viewer\"\nEXEC=\"run\"\nCATEGORY=\"none\"\nOPENS=\"txt log\"\n", 1);
	app(1, "noname", "EXEC=\"run\"\nOPENS=\"md\"\n", 1);
	apps_read(s, r, 2);
}

TEST(reads_the_apps_that_run_by_name)
{
	static struct apps s;

	setup(&s);
	EQ(s.n, 4);
	STREQ(s.a[0].name, "Hex viewer");
	STREQ(s.a[1].name, "NetSurf");
	STREQ(s.a[2].name, "Text viewer");
	STREQ(s.a[3].name, "Write");
	STREQ(s.a[3].id, "write");
	STREQ(s.a[1].id, "netsurf");		/* no ID: the directory's name */
	STREQ(s.a[3].opens, "md markdown txt");
	OK(strstr(s.a[3].dir, "/write") != 0);
}

TEST(which_apps_open_a_file)
{
	static struct apps s;
	int c[APPS_MAX];

	setup(&s);
	EQ(apps_for(&s, "MD", 0, c, APPS_MAX), 1);
	STREQ(s.a[c[0]].name, "Write");
	EQ(apps_for(&s, "txt", 0, c, APPS_MAX), 2);	/* Text viewer, Write */
	EQ(apps_for(&s, "txt", 1, c, APPS_MAX), 4);	/* and NetSurf, Hex viewer */
	EQ(apps_for(&s, "bin", 0, c, APPS_MAX), 0);
	EQ(apps_for(&s, "bin", 1, c, APPS_MAX), 1);	/* Hex viewer: "*" */
	EQ(apps_for_text(&s), 2);			/* Text viewer, by name first */
}
