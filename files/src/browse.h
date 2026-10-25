/* SPDX-License-Identifier: MIT */
/*
 * Files: the folder on screen and what can be done to its entries -- the
 * model behind the screens, without drawing (browse.c). Moved out of the
 * menu (why2025-linux home.c, "Files: a file browser") on 2026-10-25.
 *
 * dir "" is Drives (disks.h); every other dir is absolute. Folders come
 * first, then files, each by name with case ignored; names starting with a
 * dot only when hidden is set. At most BR_MAX entries are kept, the rest
 * counted in more.
 */
#ifndef FILES_BROWSE_H
#define FILES_BROWSE_H

#include <stddef.h>

#define BR_MAX		256
#define BR_SHOWN	20		/* entries in the list at a time */

enum br_kind { BK_FILE, BK_FOLDER, BK_IMAGE, BK_WEB, BK_DOC, BK_GAMEBOY, BK_TEXT };

struct br_ent {
	char name[64];
	unsigned long long size;	/* bytes */
	int dir;
	int reg;			/* a regular file: only those open */
	enum br_kind kind;
};

struct browse {
	char dir[256];			/* absolute; "" is Drives (the cross) */
	struct br_ent e[BR_MAX];
	int n, more;			/* entries; left out for want of room */
	int sel, off;			/* focused entry; first one in the list */
	int stale;			/* read dir again before showing it */
	char want[64];			/* after reading, focus this name */
	char notice[160];
	int notice_bad;
	char clip[256];			/* Copy/Cut: waiting for Paste */
	int clip_cut;
	int hidden;			/* show .names (Settings: files.hidden) */
	const char *log;		/* the log of copy, move and delete */
	/* runs argv, its output to log; the exit status. what: "Copying..." */
	int (*run)(struct browse *b, const char *what, char *const argv[]);
};

/* the extension after the last dot ("" for none, or a name like ".x") */
const char *br_ext(const char *name);
/* is ext ("JPG") one of the words in list ("jpg jpeg"); "*" matches all */
int ext_in(const char *list, const char *ext);
enum br_kind br_kind_of(const char *name, int dir);
/* dir/name into out (size n); 0 when it does not fit */
int br_join(char *out, size_t n, const char *dir, const char *name);

int br_drives(const struct browse *b);
/* go into dir (absolute; "" Drives), focusing want if given */
void br_go(struct browse *b, const char *dir, const char *want);
/* Esc: one level up, on the folder we came from; 0 at "/" or Drives */
int br_up(struct browse *b);
/* read the folder again when stale; focus want; keep sel in range */
void br_read(struct browse *b);
void br_say(struct browse *b, const char *a, const char *c, int bad);
/* the focused entry's path into out; 0 when there is none */
int br_sel_path(const struct browse *b, char *out, size_t n);
/* does the file start like text (no NUL, mostly printable)? */
int br_textlike(const char *path);

/* Copy or Cut the focused entry: it waits on the clipboard */
void br_copy(struct browse *b, int cut);
/* Paste here; 0 done, -1 with the reason in notice */
int br_paste(struct browse *b);
/* delete the focused entry */
int br_delete(struct browse *b);
/* New folder (rename 0) or Rename the focused entry to name: 0 done, or
 * 1 with *err for the text dialog to show */
int br_name(struct browse *b, const char *name, int rename, const char **err);
/* runs argv with its output added to b->log, which starts over past 16 KB */
int br_run(struct browse *b, const char *what, char *const argv[]);

#endif
