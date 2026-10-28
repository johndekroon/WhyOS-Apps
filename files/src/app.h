/* SPDX-License-Identifier: MIT */
/*
 * Files: the app as a state machine (app.c). Keys go in; out come the
 * screen to draw (a list of rows, pop-ups over it) and, when a file is to
 * be opened, the app and the file for the menu to start (open_dir,
 * open_file). The screens and keys are the menu's Files as it was
 * (why2025-linux docs/HOME.md "Files", WhyBadge Docs/06-design-system.md
 * 6.10): Drives and the folders, Enter opens, the square key's options,
 * the triangle /root, the cross Drives, Esc up a folder and closes at /.
 */
#ifndef FILES_APP_H
#define FILES_APP_H

#include "apps.h"
#include "browse.h"
#include "disks.h"

/* the keys (Linux codes) */
#define K_ESC		1
#define K_BACKSPACE	14
#define K_TAB		15
#define K_ENTER		28
#define K_LEFTSHIFT	42
#define K_RIGHTSHIFT	54
#define K_SPACE		57
#define K_F1		59	/* the square: options */
#define K_F2		60	/* the triangle: /root */
#define K_F3		61	/* the cross: Drives */
#define K_F4		62	/* the circle: the log */
#define K_F5		63	/* the cloud: settings */
#define K_F6		64	/* the diamond: help */
#define K_UP		103
#define K_PGUP		104	/* Fn + Up */
#define K_LEFT		105
#define K_RIGHT		106
#define K_DOWN		108
#define K_PGDN		109	/* Fn + Down */
#define K_DELETE	111
#define K_FN		0x1d0

enum screen { SC_LIST, SC_SETTINGS, SC_LOG, SC_PICK, SC_INPUT, SC_CONFIRM, SC_HELP };

/* a row of the list on screen */
enum row_kind { R_GROUP, R_FOLDER, R_FILE, R_INFO, R_DRIVE, R_TOGGLE };
enum row_icon { I_NONE, I_FOLDER, I_IMAGE, I_GLOBE, I_DOC, I_GAMEBOY, I_TEXT, I_FILE,
		I_SDCARD, I_USBSTICK, I_DRIVE, I_CHIP, I_INFO };

struct row {
	enum row_kind kind;
	enum row_icon ic;
	char label[64];
	char value[32];		/* a file's size, a folder's entries ("4 items") */
	char date[16];		/* when it changed: "today", "24 Oct", "Mar 2025" */
	int idx;		/* R_FOLDER, R_FILE: the entry; R_DRIVE: the drive */
	int on;			/* R_TOGGLE */
};

#define LIST_MAX	(BR_SHOWN + 16)

struct list {
	const char *title;
	char title_buf[64];
	char title_r[24];		/* right of the title: free space, "read-only" */
	char path[96];			/* under the title in mono */
	char tag[96];			/* the clipboard, in pink under the list */
	const char *notice;
	int notice_bad;
	struct row rows[LIST_MAX];
	int n, focus;			/* focus: a row, or -1 */
};

/* PgUp, PgDn and the arrows across move this many entries: the rows the
 * list shows less one (ui.c) */
#define FILES_PAGE	8

/* the square key's options and Open with */
enum op { OP_OPENWITH, OP_RENAME, OP_COPY, OP_CUT, OP_PASTE, OP_MKDIR, OP_DELETE };
#define PICK_MAX	APPS_MAX

struct pick {
	int openwith;			/* the apps for a file, else the options */
	const char *title;
	char title_buf[64];
	int n, focus;
	const char *label[PICK_MAX];
	const char *sub[PICK_MAX];	/* Paste here: what */
	int sep[PICK_MAX], danger[PICK_MAX];
	enum op ops[PICK_MAX];
	int cand[PICK_MAX];		/* Open with: index into apps */
	char paste[64];
};

struct input {
	int rename;			/* Rename, else New folder */
	const char *title;
	char buf[128];
	int len, cur;			/* the cursor is before buf[cur] */
	int fresh;			/* the first key typed replaces the text */
	const char *err;
};

#define LOG_LINES	200

struct files {
	struct browse b;
	struct disks d;
	struct apps apps;
	int drive;			/* Drives: the focused drive (d.fs index) */
	enum screen sc, under;		/* under: what the help or a pop-up is over */
	enum screen log_under;		/* what the log is over (the help may be over it) */
	struct list l;
	struct pick p;
	struct input in;
	int cfocus;			/* Delete?: 0 Cancel, 1 Delete */
	int shift;
	int held;			/* repeats of the key held (value 2) so far */
	/* the log screen */
	char log_buf[16384];
	const char *log_line[LOG_LINES];
	int log_n, log_top;
	const char *settings;		/* where hidden is kept ($APP_CONFIG/settings) */
	/* out */
	int quit;			/* Files closes */
	char open_dir[128];		/* the app to open open_file with (the menu starts it) */
	char open_file[320];
};

#define LOG_ROWS	20		/* lines on the log screen (ui.c checks) */

/* start in dir, on the file or folder at path (or /, or Drives for "") */
void files_init(struct files *f, const char *path);
/* a key: code; value 1 pressed, 2 held (repeated) */
void files_key(struct files *f, int code, int value);
/* the screen as it is now: the list built, the drives read again */
void files_build(struct files *f);
/* Drives: read the drives again (main: every 1.5 s on Drives) */
void files_refresh(struct files *f);

#endif
