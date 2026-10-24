/* SPDX-License-Identifier: MIT */
/*
 * Write: files (design/write/README.md §4, §5). Saving writes a temporary
 * file in the same folder and renames it over the original, so a reset
 * while saving leaves the old file whole.
 */
#ifndef WRITE_FILES_H
#define WRITE_FILES_H

#include <stddef.h>
#include <time.h>

#define FILE_MAX	(256 * 1024)	/* README §11: larger files are refused */
#define NAME_MAX_W	96

struct entry {
	char name[NAME_MAX_W + 1];
	int dir;
	time_t mtime;
	int items;		/* a folder's entries (not hidden) */
};

int file_load(const char *path, char **data, size_t *len, char *err, int errn);
int file_save(const char *path, const char *data, size_t len, char *err, int errn);
int file_exists(const char *path);
int name_ok(const char *name, char *why, int whyn);
void name_suggest(const char *dir, const char *cur, const char *heading, const char *ext,
		  char *out, int n);
void rescue_path(const char *dir, char *out, int n);
int list_dir(const char *dir, int dirs_only, struct entry **out, char *err, int errn);
void when_text(time_t t, time_t now, char *out, int n);
void path_split(const char *path, char *dir, int dn, char *base, int bn);
const char *path_ext(const char *path);

#endif
