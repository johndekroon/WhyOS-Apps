/* SPDX-License-Identifier: MIT */
/*
 * store — the WhyOS app store client (docs/client.md): shared declarations.
 *
 * One program for the CLI and the UI. Paths, sources and the cached index
 * come from the environment and /etc/whyos-release (docs/device.md), so the
 * host build runs the same code against a temporary root in the tests.
 */
#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define STORE_VERSION	"0.1.0"
#define STORE_FORMAT	1		/* the formats this client reads */
#define USER_AGENT	"WhyOS-store/" STORE_VERSION

#define MAX_ENTRIES	4096		/* package.md §1.2 */
#define MAX_UNPACKED	(256L * 1024 * 1024)
#define MAX_COMPONENT	100
#define MAX_PATH_LEN	240
#define MAX_MANIFEST	(256 * 1024)
#define INDEX_MAX_AGE	3600		/* device.md §5.2: refresh after an hour */
#define HOOK_TIMEOUT	30

/* ---- util.c ---- */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *path_join(char *out, size_t size, const char *a, const char *b);
int mkdir_p(const char *path);
int rm_rf(const char *path);
int is_dir(const char *path);
int is_file(const char *path);
long long file_size(const char *path);
long long dir_size(const char *path, long *files);
int read_file(const char *path, char **data, size_t *len, size_t max);
int write_atomic(const char *path, const void *data, size_t len);
int sha256_file(const char *path, char hex[65]);
void sha256_hex(const unsigned char d[32], char hex[65]);
void now_iso(char out[24]);
void uuid4(char out[37]);
long long fs_free(const char *path);
int run_hook(const char *dir, const char *prog, char *const env[], char *const extra_args[]);
void say(const char *fmt, ...);		/* stdout */
void warn(const char *fmt, ...);	/* stderr, "store: " */
extern void (*say_hook)(int is_warn, const char *text);	/* the UI: instead of stdout/stderr */
int split_words(char *s, char **words, int max);

/* ---- json.c ---- */
struct json {
	const char *s;
	struct jsmntok *t;
	int n;
};
int json_parse(struct json *j, const char *s, size_t len);
void json_free(struct json *j);
int json_skip(const struct json *j, int i);
int json_get(const struct json *j, int obj, const char *key);
int json_path(const struct json *j, int obj, const char *dotted);
int json_eq(const struct json *j, int tok, const char *s);
char *json_strdup(const struct json *j, int tok);	/* unescaped, NULL if not a string */
int json_str(const struct json *j, int tok, char *out, size_t size);
long long json_int(const struct json *j, int tok, long long def);
int json_bool(const struct json *j, int tok, int def);
int json_len(const struct json *j, int tok);		/* array or object size */
int json_item(const struct json *j, int arr, int i);	/* i-th element of an array */
int json_is_obj(const struct json *j, int tok);
int json_is_arr(const struct json *j, int tok);

struct jw {				/* a small JSON writer */
	char *buf;
	size_t len, cap;
	int depth, first[16], indent;
	char kind[16];		/* '}' or ']' per open level */
};
void jw_init(struct jw *w);
void jw_free(struct jw *w);
void jw_obj(struct jw *w, const char *key);	/* key NULL in arrays and at the top */
void jw_arr(struct jw *w, const char *key);
void jw_end(struct jw *w);
void jw_str(struct jw *w, const char *key, const char *v);
void jw_int(struct jw *w, const char *key, long long v);
void jw_bool(struct jw *w, const char *key, int v);
void jw_raw(struct jw *w, const char *key, const char *raw, size_t n);
void jw_quote(struct jw *w, const char *s);

/* ---- paths.c: where everything lives (device.md §1, §6.3) ---- */
struct paths {
	char home[200];		/* $HOME */
	char sd[200];		/* the card's mount point */
	int sd_present;		/* mounted and writable */
	char flash_apps[200];	/* /data/apps */
	char tmp[200];		/* /tmp */
	char sys_sources[200];	/* /etc/whyos/sources.d */
	int api;
	char features[256];
	char os_version[32];
	/* derived */
	char sd_apps[256], sd_appdata[256], store_dir[256], cache_dir[256], dl_dir[256];
	char whyos[256], config_dir[256], bin_dir[256], registry_dir[256], user_sources[256];
};
extern struct paths P;
void paths_init(void);
int has_feature(const char *f);
char *app_dir(char *out, size_t n, const char *id, const char *location);
char *appdata_dir(char *out, size_t n, const char *id);
char *config_dir(char *out, size_t n, const char *id);
char *cache_dir_of(char *out, size_t n, const char *id);

/* ---- sources.c ---- */
struct source {
	char id[33];
	char url[256];		/* ends in '/' */
	char cafile[256];	/* extra CA roots (test sources), or "" */
	int enabled;
};
#define MAX_SOURCES 8
extern struct source S[MAX_SOURCES];
extern int n_sources;
void sources_load(void);
struct source *source_find(const char *id);

/* ---- net.c ---- */
typedef int (*progress_fn)(long long done, long long total, void *arg);	/* nonzero: cancel */
int net_get(const struct source *src, const char *url, const char *path, int resume,
	    progress_fn progress, void *arg, char *err, size_t errn);
int net_get_mem(const struct source *src, const char *url, char **data, size_t *len,
		size_t max, char *err, size_t errn);
int clock_set(void);

/* ---- index.c ---- */
struct app {
	char *id, *name, *summary, *type, *category, *author, *updated;
	char *icon, *menu_icon, *details_sha256, *withdrawn, *install_location;
	char *tags, *commands, *features, *requires_apps;	/* space-separated */
	char *version, *url, *sha256;
	int build, os_api, memory_kb, terminal;
	long long size, installed_size;
	const struct source *src;
};
struct index {
	int format, serial, n;
	char *generated, *base_url, *featured;	/* featured: space-separated ids */
	char *categories_sha256;
	struct app *apps;
	const struct source *src;
};
#define MAX_INDEXES MAX_SOURCES
extern struct index IX[MAX_INDEXES];
extern int n_indexes;
int index_update(int force, char *err, size_t errn);	/* fetch where stale */
int index_load_all(void);				/* from the cache */
struct app *index_find(const char *id, const char *source);
int index_parse(struct index *ix, const char *data, size_t len, char *err, size_t errn);
void index_free(struct index *ix);
int details_get(const struct app *a, char **data, size_t *len, char *err, size_t errn);
int categories_get(const struct index *ix, char **data, size_t *len, char *err, size_t errn);
int asset_get(const struct source *src, const char *rel, char *out, size_t n, char *err, size_t errn);

/* ---- registry.c (device.md §2.2, §4, §5.4, §5.7) ---- */
#define MAX_CARDS 8
struct reg {
	char id[33], source[33], name[64], version[32], location[8];
	int build, pinned, auto_installed, n_cards;
	char cards[MAX_CARDS][37];
	char installed[24], updated[24];
};
int reg_read(const char *id, struct reg *r);
int reg_write(const struct reg *r);
int reg_delete(const char *id);
int reg_list(char ids[][33], int max);
int reg_has_card(const struct reg *r, const char *card);
void reg_add_card(struct reg *r, const char *card);
int card_id(char out[37], int create);
int journal_write(const char *op, const char *id, int build);
int journal_read(char *op, char *id, int *build);
void journal_clear(void);
int cleanup_add(const char *card, const char *id, const char *const *paths, int n);
int cleanup_run(const char *card);
int cleanup_list(FILE *out, int json);

/* ---- wpk.c ---- */
struct manifest_files {
	int n;
	char **path;
	char (*sha)[65];
	unsigned char *seen;
};
int wpk_unpack(const char *pkg, const char *dest, const char *want_id, int want_build,
	       char **manifest, char *err, size_t errn);	/* dest NULL: check only */
int manifest_files(const struct json *j, struct manifest_files *mf);
void manifest_files_free(struct manifest_files *mf);
int name_ok(const char *rel, char *err, size_t errn);

/* ---- lifecycle.c ---- */
enum state { ST_NONE, ST_INSTALLED, ST_UPDATE, ST_MISSING, ST_DAMAGED, ST_UNREGISTERED,
	     ST_LEGACY, ST_WITHDRAWN, ST_ELSEWHERE };
struct opts {
	int yes;		/* answer yes to questions */
	int keep;		/* keep downloaded packages */
	int json;
	progress_fn progress;
	void *progress_arg;
	int (*ask)(const char *question, void *arg);	/* the UI: 1 to go ahead */
	void *ask_arg;
};
const char *state_name(enum state s);
enum state app_state(const char *id, struct reg *r, struct app **a);
int store_install(const char *spec, const struct opts *o, int auto_installed);
int store_remove(const char *id, const struct opts *o);
int store_verify(const char *id, int quiet);
int store_repair(const char *id, const struct opts *o);
int store_adopt(const char *id);
int store_maintain(void);
int store_delete_legacy(const char *id);
int is_system_app(const char *id);
int write_wrappers(const struct json *m, const char *app_dir_path);
int remove_wrappers(const char *id);
int legacy_list(char ids[][33], int max, int with_package);

/* ---- qoi.c ---- */
struct picture {
	int w, h;
	unsigned short *rgb;	/* RGB565 */
	unsigned char *alpha;	/* NULL without an alpha channel */
};
int qoi_decode(const char *path, struct picture *pic);
void picture_free(struct picture *pic);

/* ---- cli.c / ui.c ---- */
int cli_main(int argc, char **argv);
int ui_main(int argc, char **argv);

#endif
