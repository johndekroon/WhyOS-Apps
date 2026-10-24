/* SPDX-License-Identifier: MIT */
/*
 * Write: editing a document (design/write/README.md §3, §6, §7, §10).
 *
 * Three modes over the same text buffer:
 *   M_RICH    markdown shown with its styles; the caret moves over visible
 *             text only and edits keep the markdown valid
 *   M_SOURCE  the same markdown as plain text
 *   M_CODE    any other text: plain bytes, with indentation help
 *
 * In M_RICH an edit is made in the source where the caret maps to; then the
 * line is read back and compared with what the user should see. When it
 * differs (a style lost its last letter, a line split inside **bold**), only
 * that line is written again from its visible text and styles. Lines nobody
 * touched are never rewritten, so markdown Write does not know survives.
 *
 * Positions (caret, anchor) are source positions; in M_RICH they always sit
 * on a visible boundary (md_src of a visible offset).
 */
#ifndef WRITE_EDIT_H
#define WRITE_EDIT_H

#include "md.h"
#include "text.h"

enum mode { M_RICH, M_SOURCE, M_CODE };

enum move {
	MV_LEFT, MV_RIGHT, MV_WORD_LEFT, MV_WORD_RIGHT, MV_HOME, MV_END,
	MV_DOC_START, MV_DOC_END
};

struct ed {
	struct text t;
	struct pos caret, anchor;
	int sel;		/* anchor..caret is selected */
	int tstyle;		/* the style of what is typed next; -1: the caret's */
	enum mode mode;
	int indent;		/* M_CODE: spaces per level, 0: a tab */
	struct mdline m, m2;	/* scratch */
};

void ed_init(struct ed *e);
void ed_free(struct ed *e);
int ed_load(struct ed *e, const char *data, size_t len, enum mode mode);
void ed_set_mode(struct ed *e, enum mode mode);

/* where things are */
int ed_in_code(const struct ed *e, int line);	/* inside a ``` block */
struct mdline *ed_parse(struct ed *e, int line);	/* e->m, or 0 when out of memory */
int ed_vis_of(struct ed *e, struct pos p);	/* M_RICH: the visible offset; else the column */
struct pos ed_pos_of(struct ed *e, int line, int k);
void ed_caret_vis(struct ed *e, int line, int k, int select);
void ed_set_caret(struct ed *e, struct pos p, int select);
void ed_sel_range(const struct ed *e, struct pos *a, struct pos *b);
int ed_style_at(struct ed *e);
enum blk ed_block_at(struct ed *e);
int ed_words(struct ed *e);
int ed_sel_words(struct ed *e);
int ed_guess_indent(const struct ed *e);
int ed_word_at(struct ed *e, char *out, int cap);	/* the selection or the word at the caret */

/* editing; -1 when out of memory (nothing changed) */
int ed_type(struct ed *e, const char *s, int n);
int ed_newline(struct ed *e);
int ed_backspace(struct ed *e);
int ed_delete(struct ed *e);
int ed_tab(struct ed *e);
int ed_delete_sel(struct ed *e);
int ed_paste(struct ed *e, const char *s, size_t n);
char *ed_copy(struct ed *e, size_t *n, int *words);
char *ed_cut(struct ed *e, size_t *n, int *words);
int ed_toggle(struct ed *e, int flag, int word);
int ed_block(struct ed *e, enum blk kind, int toggle);
int ed_replace_range(struct ed *e, int line, int k0, int k1, const char *s, int n);
int ed_undo(struct ed *e);
int ed_redo(struct ed *e);

void ed_move(struct ed *e, enum move how, int select);
void ed_select_all(struct ed *e);

#endif
