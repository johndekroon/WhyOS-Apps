/* SPDX-License-Identifier: MIT */
/*
 * Write: the app as a state machine (design/write/README.md §4-§10). Keys go
 * in, the document, the overlays and requests (quit, run a program) come
 * out; ui.c draws what is here and main.c does the input and output, so
 * everything a key does can be tested without a screen (tests/unit/test_app.c).
 */
#ifndef WRITE_APP_H
#define WRITE_APP_H

#include <time.h>

#include "edit.h"
#include "files.h"
#include "find.h"
#include "layout.h"
#include "syntax.h"

/* Linux key codes (linux/input-event-codes.h) the app uses */
#define K_ESC		1
#define K_1		2
#define K_7		8
#define K_8		9
#define K_0		11
#define K_BACKSPACE	14
#define K_TAB		15
#define K_ENTER		28
#define K_SPACE		57
#define K_F1		59	/* the symbol keys: square */
#define K_F2		60	/* triangle */
#define K_F3		61	/* cross */
#define K_F4		62	/* circle */
#define K_F5		63	/* cloud */
#define K_F6		64	/* diamond */
#define K_UP		103
#define K_LEFT		105
#define K_RIGHT		106
#define K_DOWN		108
#define K_FN		0x1d0

struct key {
	int code;
	int shift, ctrl, alt, fn;
	char text[8];		/* what it types, UTF-8; "" for none */
};

enum overlay {
	OV_NONE, OV_OPTIONS, OV_STYLE, OV_HELP, OV_DISCARD, OV_SAVEAS, OV_REPLACE_ASK, OV_LANGUAGE,
	OV_LOG, OV_SETTINGS, OV_FIND_MENU, OV_ERROR
};

enum screen { SC_EDIT, SC_OPEN };

enum after { AF_NONE, AF_CLOSE, AF_NEW, AF_OPEN, AF_RUN };

/* menu rows (Options, the find bar's menu, Settings) */
enum act {
	A_NONE, A_SAVE, A_SAVE_AS, A_NEW, A_OPEN, A_FIND, A_FULL, A_LANGUAGE, A_MATCH_CASE,
	A_WHOLE_WORD, A_REPLACE_ALL, A_TEXT_SIZE, A_LINE_NUMBERS
};

struct menu_row { enum act act; const char *label; char value[48]; int sep, toggle, on, chev; };

#define MENU_ROWS	12
#define LOG_LINES	40

struct open_row { char name[NAME_MAX_W + 1]; int dir, items, isnew, here; time_t mtime; };

struct app {
	struct ed e;
	struct find f;
	struct lay lay;
	char home[256];		/* /root: where new documents go */
	char path[512];		/* "" until the document has a name */
	char dir[512];
	char name[128];
	enum lang lang;
	int is_md;		/* markdown: rich text, ✕ switches to the source */
	time_t saved_at;
	long now;		/* ms, from main */

	int full;		/* full screen */
	long hint_until;	/* full screen's first seconds */
	int scroll, want_x;
	int fn_held;

	int find_open, find_focus;	/* 0: Find, 1: Replace */
	int replaced;		/* "Replaced N" after replace all */
	char clip_label[48];	/* "Copied 2 words", while the clipboard has it */
	char notice[96];	/* a line in the document bar for a moment */

	enum screen screen;
	enum overlay ov;
	int focus;		/* the overlay's focused row */
	struct menu_row rows[MENU_ROWS];
	int n_rows;
	enum after after;	/* what follows a save or a discard */
	char after_path[512];

	char sa_name[NAME_MAX_W + 1];	/* Save as */
	char sa_ext[16];
	char sa_dir[512];
	char sa_err[128];
	int sa_focus, sa_caret, sa_exists;

	char op_dir[512];	/* Open */
	struct open_row *op;
	int op_n, op_focus, op_folders, op_drives;
	char op_filter[48];
	char op_err[128];

	char err_title[64], err_text[256];

	char log[LOG_LINES][120];
	int n_log;

	int small, lineno;	/* settings */
	int quit;		/* 1: done */
	int run;		/* 1 run, 2 preview: main starts the program on path */
	int redraw;		/* everything changed (a new document, a mode) */
};

struct docbar {
	const char *name;
	int edited;
	char info[64];		/* ln/col, "not saved yet", "2 selected", "Copied 2 words" */
	int info_kind;		/* 0 grey, 1 cyan (selected), 2 pink (clipboard label) */
	const char *para;	/* "Body", "Heading 2"; 0 in code */
	int style;		/* ST_B | ST_I | ST_U at the caret */
	int md_view;		/* 0 rich, 1 markdown source, -1 none */
	const char *lang;	/* code: the language and the indent */
	char indent[16];
	char count[32];		/* "58 words", "14 lines" */
};

struct hint { const char *keys, *label; int right, pink; };

void app_init(struct app *a, const char *path);
void app_free(struct app *a);
void app_key(struct app *a, const struct key *k);
void app_fn(struct app *a, int down);
int app_tick(struct app *a, long now);	/* 1 when the screen must change */
void app_term(struct app *a);		/* SIGTERM (the logo key): save, then quit */

/* for drawing */
void app_docbar(struct app *a, struct docbar *d);
int app_hints(const struct app *a, struct hint *h, int max);
void app_geometry(const struct app *a, int *x, int *y, int *w, int *h);
void app_follow(struct app *a);
const char *app_para_name(enum blk kind);
void app_log(struct app *a, const char *fmt, ...);

/* paths that tests move: WRITE_HOME, WRITE_CLIP, WRITE_LOG, APP_CONFIG */
const char *app_clip_path(void);

#endif
