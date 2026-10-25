/* SPDX-License-Identifier: MIT */
/*
 * Files: the installed apps that open files (apps.c), from their app.conf
 * as the menu reads them (why2025-linux home.c app_parse): NAME, ID,
 * EXEC, OPENS (the extensions Enter opens with the app; "*" any) and
 * OPENS_ALSO (offered under Open with only). Files does not start apps
 * itself: it names the app and the file to the menu (main.c).
 */
#ifndef FILES_APPS_H
#define FILES_APPS_H

#define APPS_MAX	64

struct fapp {
	char dir[128];			/* /data/apps/write */
	char id[33];			/* ID; else the directory's name */
	char name[32];			/* NAME */
	char opens[96];			/* OPENS */
	char opens_also[96];		/* OPENS_ALSO */
};

struct apps {
	struct fapp a[APPS_MAX];
	int n;
};

/* the roots the menu reads, the card's first and the badge's own last */
extern const char *const apps_roots[3];

/* every app with a NAME and an EXEC that runs, by name, from roots */
void apps_read(struct apps *s, const char *const *roots, int n_roots);
/* the apps whose OPENS has ext, or with all also OPENS_ALSO, into out:
 * how many */
int apps_for(const struct apps *s, const char *ext, int all, int *out, int max);
/* the app for text without a known extension: the first whose OPENS has
 * txt and is not "*"; -1 none */
int apps_for_text(const struct apps *s);

#endif
