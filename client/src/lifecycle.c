/* SPDX-License-Identifier: MIT */
/*
 * store: install, update, remove, verify, repair, adopt and maintain
 * (docs/device.md §5), and what an installed app directory holds besides
 * the package's files: app.conf, .package/ and the command wrappers (§3,
 * package.md §2.3).
 *
 * A package is unpacked into apps/.<id>.new next to its place, on the same
 * filesystem (card or /data), so moving it in is two renames; the journal
 * lets store maintain finish or undo an install a power cut interrupted.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "store.h"

#define S_(x) ((x) ? (x) : "")

const char *state_name(enum state s)
{
	static const char *const n[] = { "not installed", "installed", "update available",
		"not on this card", "needs repair", "found on this card", "other app",
		"withdrawn", "on another card" };

	return n[s];
}

/* ---- the installed manifest ---- */

static int read_installed(const char *dir, char **text, struct json *j)
{
	char p[512];
	size_t len;

	snprintf(p, sizeof(p), "%s/.package/manifest.json", dir);
	if (read_file(p, text, &len, MAX_MANIFEST))
		return -1;
	if (json_parse(j, *text, len) || !json_is_obj(j, 0)) {
		free(*text);
		return -1;
	}
	return 0;
}

/* the env an app's hook gets (device.md §6.1); strings in buf */
static void app_env(const char *id, const char *dir, char buf[6][300], char *env[7])
{
	char d[256];

	snprintf(buf[0], 300, "APP_ID=%s", id);
	snprintf(buf[1], 300, "APP_DIR=%s", dir);
	snprintf(buf[2], 300, "APP_DATA=%s", appdata_dir(d, sizeof(d), id));
	snprintf(buf[3], 300, "APP_CONFIG=%s", config_dir(d, sizeof(d), id));
	snprintf(buf[4], 300, "APP_CACHE=%s", cache_dir_of(d, sizeof(d), id));
	snprintf(buf[5], 300, "WHYOS_API=%d", P.api);
	for (int i = 0; i < 6; i++)
		env[i] = buf[i];
	env[6] = 0;
}

static int hook(const char *id, const char *dir, const struct json *m, const char *name,
		char *const args[])
{
	char prog[128], buf[6][300];
	char *env[7];
	int r;

	if (json_str(m, json_path(m, 0, name), prog, sizeof(prog)) || !prog[0])
		return 0;
	app_env(id, dir, buf, env);
	r = run_hook(dir, prog, env, args);
	if (r)
		warn("%s: %s ended with status %d; the app may need attention", id, name, r);
	return r;
}

/* ---- app.conf, .package/ and wrappers ---- */

static void conf_put(FILE *f, const char *key, const char *v)
{
	if (!v || !*v)
		return;
	fprintf(f, "%s=\"", key);
	for (; *v; v++)
		fputc(*v == '"' || *v == '\n' ? '\'' : *v, f);
	fputs("\"\n", f);
}

static void conf_list(FILE *f, const struct json *m, const char *key, const char *path)
{
	int arr = json_path(m, 0, path);
	char line[512] = "", item[64];

	for (int i = 0; i < json_len(m, arr) && json_is_arr(m, arr); i++) {
		if (json_str(m, json_item(m, arr, i), item, sizeof(item)))
			continue;
		if (line[0])
			strncat(line, " ", sizeof(line) - strlen(line) - 1);
		strncat(line, item, sizeof(line) - strlen(line) - 1);
	}
	conf_put(f, key, line);
}

/*
 * app.conf (package.md §2.3) and .package/ (device.md §3) into dir, from
 * the manifest's text.
 */
static int write_package_files(const char *dir, const char *mtext, const char *source,
			       const char *url)
{
	struct json m;
	char p[512], v[256];
	FILE *f;
	int t, ok;

	if (json_parse(&m, mtext, strlen(mtext)))
		return -1;
	snprintf(p, sizeof(p), "%s/app.conf", dir);
	f = fopen(p, "w");
	if (!f) {
		json_free(&m);
		return -1;
	}
	fprintf(f, "# written by store from the manifest; do not edit (it is rewritten on update)\n");
	{
		static const char *const map[][2] = {
			{ "NAME", "name" }, { "INFO", "launch.info" }, { "CATEGORY", "launch.menu" },
			{ "ICON", "launch.icon" }, { "EXEC", "launch.exec" }, { "ARGS", "launch.args" },
			{ "FILES", "launch.files" }, { "ESC", "launch.esc" }, { "ASK", "launch.ask" },
			{ "ASK_DEFAULT", "launch.ask_default" }, { "ROTATION", "launch.rotation" },
			{ "ID", "id" }, { "TYPE", "type" }, { "SUBCATEGORY", "category" },
		};

		for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
			if (!json_str(&m, json_path(&m, 0, map[i][1]), v, sizeof(v)))
				conf_put(f, map[i][0], v);
	}
	conf_list(f, &m, "TYPES", "launch.types");
	conf_list(f, &m, "OPENS", "launch.opens");
	conf_list(f, &m, "OPENS_ALSO", "launch.opens_also");
	t = json_path(&m, 0, "launch.terminal");
	if (json_bool(&m, t, 0))
		conf_put(f, "TERMINAL", "1");
	else if (!json_str(&m, t, v, sizeof(v)))
		conf_put(f, "TERMINAL", v);
	ok = !ferror(f);
	if (fclose(f) || !ok) {
		json_free(&m);
		return -1;
	}

	snprintf(p, sizeof(p), "%s/.package", dir);
	if (mkdir_p(p)) {
		json_free(&m);
		return -1;
	}
	snprintf(p, sizeof(p), "%s/.package/manifest.json", dir);
	ok = !write_atomic(p, mtext, strlen(mtext));
	{
		struct manifest_files mf;

		snprintf(p, sizeof(p), "%s/.package/files.sha256", dir);
		if (ok && !manifest_files(&m, &mf)) {
			f = fopen(p, "w");
			for (int i = 0; f && i < mf.n; i++)
				fprintf(f, "%s  %s\n", mf.sha[i], mf.path[i]);
			ok = f && !ferror(f) && !fclose(f);
			manifest_files_free(&mf);
		}
	}
	snprintf(p, sizeof(p), "%s/.package/source", dir);
	f = ok ? fopen(p, "w") : 0;
	ok = f && fprintf(f, "ID=%s\nURL=%s\n", source, url) > 0 && !fclose(f);
	json_free(&m);
	return ok ? 0 : -1;
}

/* %d %D %C %T %% expanded (package.md §2.3) */
static void expand(const char *in, const char *id, const char *dir, char *out, size_t n)
{
	size_t o = 0;

	for (; *in && o + 1 < n; in++) {
		char sub[256] = "", *s = 0;

		if (*in == '%' && in[1]) {
			switch (in[1]) {
			case 'd': s = (char *)dir; break;
			case 'D': s = appdata_dir(sub, sizeof(sub), id); break;
			case 'C': s = config_dir(sub, sizeof(sub), id); break;
			case 'T': s = cache_dir_of(sub, sizeof(sub), id); break;
			case '%': s = "%"; break;
			}
			if (s) {
				in++;
				while (*s && o + 1 < n)
					out[o++] = *s++;
				continue;
			}
		}
		out[o++] = *in;
	}
	out[o] = 0;
}

static int command_name_ok(const char *n)
{
	static const char *const sys[] = { "/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/" };

	if (!*n || strlen(n) > 32)
		return 0;
	for (const char *p = n; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
		      *p == '-' || *p == '_' || *p == '.'))
			return 0;
	for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++) {
		char p[128];

		snprintf(p, sizeof(p), "%s%s", sys[i], n);
		if (!access(p, F_OK))
			return 0;	/* the OS has it (busybox applet or rootfs) */
	}
	return 1;
}

/* who wrote the wrapper bin_dir/name: its app id in owner, or "" */
static void wrapper_owner(const char *name, char *owner, size_t n)
{
	char p[512], *data, *q;

	owner[0] = 0;
	path_join(p, sizeof(p), P.bin_dir, name);
	if (read_file(p, &data, 0, 8192))
		return;
	q = strstr(data, "# written by store for ");
	if (q)
		snprintf(owner, n, "%.*s", (int)strcspn(q + 23, ";\n"), q + 23);
	free(data);
}

/* the [[commands]] wrappers in $HOME/.whyos/bin (package.md §2.3.1) */
int write_wrappers(const struct json *m, const char *dir)
{
	int cmds = json_get(m, 0, "commands");
	char id[40];

	json_str(m, json_get(m, 0, "id"), id, sizeof(id));
	for (int i = 0; i < json_len(m, cmds) && json_is_arr(m, cmds); i++) {
		int c = json_item(m, cmds, i);
		char name[64], exec[128], args[256] = "", ex[1024], owner[40], p[512], d[256];
		char *words[32];
		FILE *f;
		int nw;

		if (json_str(m, json_get(m, c, "name"), name, sizeof(name)) ||
		    json_str(m, json_get(m, c, "exec"), exec, sizeof(exec)))
			continue;
		json_str(m, json_get(m, c, "args"), args, sizeof(args));
		wrapper_owner(name, owner, sizeof(owner));
		if (owner[0] && strcmp(owner, id)) {
			warn("command %s is already %s's: not added", name, owner);
			continue;
		}
		if (!owner[0] && !command_name_ok(name)) {
			warn("command %s: the system has it already, or the name is not allowed: not added", name);
			continue;
		}
		expand(args, id, dir, ex, sizeof(ex));
		nw = split_words(ex, words, 32);
		if (mkdir_p(P.bin_dir))
			return -1;
		path_join(p, sizeof(p), P.bin_dir, name);
		f = fopen(p, "w");
		if (!f)
			return -1;
		fprintf(f, "#!/bin/sh\n# written by store for %s; do not edit\n", id);
		fprintf(f, "APP_ID=%s\nAPP_DIR=%s\n", id, dir);
		fprintf(f, "APP_DATA=%s\n", appdata_dir(d, sizeof(d), id));
		fprintf(f, "APP_CONFIG=%s\n", config_dir(d, sizeof(d), id));
		fprintf(f, "APP_CACHE=%s\n", cache_dir_of(d, sizeof(d), id));
		fprintf(f, "WHYOS_API=%d\n", P.api);
		fprintf(f, "export APP_ID APP_DIR APP_DATA APP_CONFIG APP_CACHE WHYOS_API\n");
		fprintf(f, "if [ ! -x \"$APP_DIR/%s\" ]; then\n"
			"\techo \"%s: not on this SD card; reinstall it in the store\" >&2\n"
			"\texit 127\nfi\nexec \"$APP_DIR/%s\"", exec, name, exec);
		for (int k = 0; k < nw; k++)
			fprintf(f, " \"%s\"", words[k]);
		fprintf(f, " \"$@\"\n");
		if (fclose(f) || chmod(p, 0755))
			return -1;
	}
	return 0;
}

int remove_wrappers(const char *id)
{
	DIR *d = opendir(P.bin_dir);
	struct dirent *e;

	if (!d)
		return 0;
	while ((e = readdir(d))) {
		char owner[40], p[512];

		if (e->d_name[0] == '.')
			continue;
		wrapper_owner(e->d_name, owner, sizeof(owner));
		if (!strcmp(owner, id))
			unlink(path_join(p, sizeof(p), P.bin_dir, e->d_name));
	}
	closedir(d);
	return 0;
}

/* ---- state (device.md §5.1) ---- */

static int files_there(const char *dir, const char *id)
{
	char *text;
	struct json j;
	char exec[128], p[512];
	int ok;

	if (read_installed(dir, &text, &j))
		return 0;
	ok = json_eq(&j, json_get(&j, 0, "id"), id);
	if (ok && !json_str(&j, json_path(&j, 0, "launch.exec"), exec, sizeof(exec)))
		ok = is_file(path_join(p, sizeof(p), dir, exec));
	json_free(&j);
	free(text);
	return ok ? 1 : -1;	/* -1: there, but not whole */
}

enum state app_state(const char *id, struct reg *r, struct app **ap)
{
	struct reg rr;
	struct app *a;
	char dir[256], card[37];
	int there;

	if (!r)
		r = &rr;
	a = index_find(id, 0);
	if (!reg_read(id, r)) {
		a = index_find(id, r->source);
		if (ap)
			*ap = a;
		app_dir(dir, sizeof(dir), id, r->location);
		there = !strcmp(r->location, "flash") || P.sd_present ? files_there(dir, id) : 0;
		if (a && a->withdrawn)
			return ST_WITHDRAWN;
		if (there < 0)
			return ST_DAMAGED;
		if (!there) {
			card_id(card, 0);
			if (!strcmp(r->location, "sd") && card[0] && reg_has_card(r, card))
				return ST_DAMAGED;	/* this card had it: deleted by hand */
			return ST_MISSING;
		}
		if (a && a->version && a->build > r->build && !r->pinned)
			return ST_UPDATE;
		return ST_INSTALLED;
	}
	if (ap)
		*ap = a;
	for (int loc = 0; loc < 2; loc++) {
		char p[512];

		if (!loc && !P.sd_present)
			continue;
		app_dir(dir, sizeof(dir), id, loc ? "flash" : "sd");
		if (is_dir(path_join(p, sizeof(p), dir, ".package")))
			return ST_UNREGISTERED;
		if (is_file(path_join(p, sizeof(p), dir, "app.conf")))
			return ST_LEGACY;
	}
	return ST_NONE;
}

/* app directories in apps/ (card and /data): with .package/ or legacy */
int legacy_list(char ids[][33], int max, int with_package)
{
	int n = 0;

	for (int loc = 0; loc < 2; loc++) {
		DIR *d;
		struct dirent *e;

		if (!loc && !P.sd_present)
			continue;
		d = opendir(loc ? P.flash_apps : P.sd_apps);
		while (d && (e = readdir(d)) && n < max) {
			char p[512], q[512];
			struct reg r;
			int dup = 0;

			if (e->d_name[0] == '.' || strlen(e->d_name) > 32)
				continue;
			path_join(p, sizeof(p), loc ? P.flash_apps : P.sd_apps, e->d_name);
			if (!is_file(path_join(q, sizeof(q), p, "app.conf")))
				continue;
			if (is_dir(path_join(q, sizeof(q), p, ".package")) != with_package)
				continue;
			if (with_package && !reg_read(e->d_name, &r))
				continue;	/* registered: not "found" */
			for (int i = 0; i < n; i++)
				dup |= !strcmp(ids[i], e->d_name);
			if (!dup)
				snprintf(ids[n++], 33, "%s", e->d_name);
		}
		if (d)
			closedir(d);
	}
	return n;
}

/* ---- install and update (device.md §5.2, §5.3) ---- */

struct target {
	char version[32], url[512], sha[65];
	int build;
	long long size, installed;
};

static int find_version(struct app *a, const char *version, struct target *t, char *err, size_t errn)
{
	char *data;
	size_t len;
	struct json j;
	int vs, found = 0;

	if (!version) {
		snprintf(t->version, sizeof(t->version), "%s", S_(a->version));
		snprintf(t->url, sizeof(t->url), "%s", S_(a->url));
		snprintf(t->sha, sizeof(t->sha), "%s", S_(a->sha256));
		t->build = a->build;
		t->size = a->size;
		t->installed = a->installed_size;
		return a->url && a->sha256 ? 0 : -1;
	}
	if (details_get(a, &data, &len, err, errn))
		return -1;
	if (json_parse(&j, data, len)) {
		free(data);
		snprintf(err, errn, "%s: details are not valid JSON", a->id);
		return -1;
	}
	vs = json_get(&j, 0, "versions");
	for (int i = 0; i < json_len(&j, vs) && !found; i++) {
		int v = json_item(&j, vs, i), pkg = json_get(&j, v, "package");

		if (!json_eq(&j, json_get(&j, v, "version"), version))
			continue;
		found = 1;
		snprintf(t->version, sizeof(t->version), "%s", version);
		json_str(&j, json_get(&j, pkg, "url"), t->url, sizeof(t->url));
		json_str(&j, json_get(&j, pkg, "sha256"), t->sha, sizeof(t->sha));
		t->build = (int)json_int(&j, json_get(&j, v, "build"), 0);
		t->size = json_int(&j, json_get(&j, pkg, "size"), 0);
		t->installed = json_int(&j, json_get(&j, pkg, "installed_size"), 0);
	}
	json_free(&j);
	free(data);
	if (!found)
		snprintf(err, errn, "%s has no published version %s", a->id, version);
	return found ? 0 : -1;
}

static int download(struct app *a, const struct target *t, const struct opts *o, char *pkg,
		    size_t pkgn, char *err, size_t errn)
{
	const char *base = strrchr(t->url, '/');
	char part[600], hex[65];
	const char *dir = P.sd_present ? P.dl_dir : P.tmp;

	snprintf(pkg, pkgn, "%s/%s", dir, base ? base + 1 : "package.wpk");
	if (!sha256_file(pkg, hex) && !strcmp(hex, t->sha))
		return 0;	/* downloaded before ("keep packages") */
	if (mkdir_p(dir)) {
		snprintf(err, errn, "cannot create %s", dir);
		return -1;
	}
	snprintf(part, sizeof(part), "%s.part", pkg);
	if (net_get(a->src, t->url, part, 1, o->progress, o->progress_arg, err, errn))
		return -1;
	if (file_size(part) != t->size || sha256_file(part, hex) || strcmp(hex, t->sha)) {
		unlink(part);
		snprintf(err, errn, "the download does not match the index (try again)");
		return -1;
	}
	if (rename(part, pkg)) {
		snprintf(err, errn, "%s: %s", pkg, strerror(errno));
		return -1;
	}
	return 0;
}

static void make_storage(const char *id, const struct json *m)
{
	char d[512], sub[512];
	int folders = json_path(m, 0, "storage.data.folders");

	if (P.sd_present) {
		mkdir_p(appdata_dir(d, sizeof(d), id));
		for (int i = 0; i < json_len(m, folders) && json_is_arr(m, folders); i++) {
			char f[128];

			if (!json_str(m, json_item(m, folders, i), f, sizeof(f)) && name_ok(f, sub, sizeof(sub)))
				mkdir_p(path_join(sub, sizeof(sub), d, f));
		}
		if (json_bool(m, json_path(m, 0, "storage.cache.enabled"), 0))
			mkdir_p(cache_dir_of(sub, sizeof(sub), id));
	}
	if (json_is_obj(m, json_path(m, 0, "storage.config")))
		mkdir_p(config_dir(d, sizeof(d), id));
}

static void fix_modes(const char *dir, const struct json *m)
{
	const char *keys[] = { "launch.exec", "hooks.post_install", "hooks.migrate", "hooks.pre_remove" };
	int cmds = json_get(m, 0, "commands");
	char f[128], p[512];

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!json_str(m, json_path(m, 0, keys[i]), f, sizeof(f)))
			chmod(path_join(p, sizeof(p), dir, f), 0755);
	for (int i = 0; i < json_len(m, cmds) && json_is_arr(m, cmds); i++)
		if (!json_str(m, json_get(m, json_item(m, cmds, i), "exec"), f, sizeof(f)))
			chmod(path_join(p, sizeof(p), dir, f), 0755);
}

static int confirm(const struct opts *o, const char *what)
{
	if (o->yes)
		return 1;
	if (o->ask)
		return o->ask(what, o->ask_arg);
	warn("%s: run again with --yes to go ahead", what);
	return 0;
}

int store_install(const char *spec, const struct opts *o, int auto_installed)
{
	char id[40], *at, err[300], card[37] = "", parent[256], target[300], stage[300], old[300];
	char pkg[600], *mtext = 0, now[24];
	const char *version = 0;
	struct app *a;
	struct target t;
	struct reg r;
	struct json m;
	int had, fresh, ret = 1, old_build;
	const char *loc;

	snprintf(id, sizeof(id), "%s", spec);
	if ((at = strchr(id, '@'))) {
		*at = 0;
		version = at + 1;
	}
	if (index_update(0, err, sizeof(err)) && !index_load_all()) {
		warn("%s", err);
		return 1;
	}
	index_load_all();
	had = !reg_read(id, &r);
	a = index_find(id, had ? r.source : 0);
	if (!a) {
		warn("there is no app %s in the store%s", id, had ? " anymore" : "");
		return 1;
	}
	if (a->withdrawn) {
		warn("%s was withdrawn from the store: %s", id, a->withdrawn);
		return 1;
	}
	if (find_version(a, version, &t, err, sizeof(err))) {
		warn("%s", err[0] ? err : "the index has no package for it");
		return 1;
	}
	if (had && t.build < r.build) {
		char q[200];

		if (!version) {
			warn("%s %s is installed, newer than the store's %s", id, r.version, t.version);
			return 1;
		}
		snprintf(q, sizeof(q), "%s %s is older than the installed %s", id, t.version, r.version);
		if (!confirm(o, q))
			return 2;
	}
	if (had && !version && t.build == r.build && app_state(id, 0, 0) == ST_INSTALLED) {
		say("%s %s is installed and up to date", id, r.version);
		return 0;
	}
	if (a->os_api > P.api) {
		warn("%s needs a newer WhyOS (API %d, this one has %d)", id, a->os_api, P.api);
		return 1;
	}
	if (P.features[0] && a->features) {
		char f[256], *w[16];
		int n;

		snprintf(f, sizeof(f), "%s", a->features);
		n = split_words(f, w, 16);
		for (int i = 0; i < n; i++)
			if (!has_feature(w[i]))
				warn("%s needs %s, which this badge does not list", id, w[i]);
	}
	if (a->requires_apps && a->requires_apps[0]) {
		char deps[256], *w[16];
		int n;

		snprintf(deps, sizeof(deps), "%s", a->requires_apps);
		n = split_words(deps, w, 16);
		for (int i = 0; i < n; i++) {
			struct reg dr;

			if (!strcmp(w[i], id) || !reg_read(w[i], &dr))
				continue;
			say("%s needs %s: installing that first", id, w[i]);
			if (store_install(w[i], o, 1))
				return 1;
		}
		index_load_all();
		a = index_find(id, had ? r.source : 0);
		if (!a)
			return 1;
	}

	loc = a->install_location && !strcmp(a->install_location, "flash") ? "flash" : "sd";
	if (!strcmp(loc, "sd") && !P.sd_present) {
		warn("no SD card (or it is read-only): %s installs on the card", id);
		return 1;
	}
	if (P.sd_present)
		card_id(card, 1);
	{
		const char *fs = !strcmp(loc, "sd") ? P.sd : P.flash_apps;
		long long need = t.installed + t.installed / 10 + (!strcmp(loc, "sd") ? t.size : 0);
		long long have = fs_free(fs);

		mkdir_p(!strcmp(loc, "sd") ? P.sd_apps : P.flash_apps);
		if (have >= 0 && have < need) {
			warn("%s full: %lld KB needed, %lld KB free", !strcmp(loc, "sd") ? "card" : "flash",
			     need / 1024, have / 1024);
			return 1;
		}
		/* only when it is new there: an update replaces what is in flash already */
		if (!strcmp(loc, "flash") && t.installed > 128 * 1024 && !had &&
		    !confirm(o, "it goes into flash, which is small"))
			return 2;
	}

	snprintf(parent, sizeof(parent), "%s", !strcmp(loc, "sd") ? P.sd_apps : P.flash_apps);
	snprintf(target, sizeof(target), "%s/%s", parent, id);
	if (!had && is_dir(target)) {
		char p[512], q[200];

		/* legacy or found on this card (device.md §5.8): replace only when asked */
		snprintf(q, sizeof(q), "%s replaces the %s already in %s", id,
			 is_dir(path_join(p, sizeof(p), target, ".package")) ? "copy" : "app installed by hand",
			 target);
		if (!confirm(o, q))
			return 2;
	}
	snprintf(stage, sizeof(stage), "%s/.%s.new", parent, id);
	snprintf(old, sizeof(old), "%s/.%s.old", parent, id);
	if (journal_write("install", id, t.build)) {
		warn("cannot write the journal");
		return 1;
	}
	say("%s %s: downloading %lld KB", id, t.version, (t.size + 1023) / 1024);
	if (download(a, &t, o, pkg, sizeof(pkg), err, sizeof(err))) {
		warn("%s", err);
		journal_clear();
		return 1;
	}
	rm_rf(stage);
	if (wpk_unpack(pkg, stage, id, t.build, &mtext, err, sizeof(err))) {
		warn("%s: %s", id, err);
		rm_rf(stage);
		unlink(pkg);
		journal_clear();
		return 1;
	}
	if (json_parse(&m, mtext, strlen(mtext))) {
		free(mtext);
		rm_rf(stage);
		journal_clear();
		return 1;
	}
	if (write_package_files(stage, mtext, a->src->id, t.url)) {
		warn("cannot write %s/app.conf", stage);
		goto fail;
	}
	fix_modes(stage, &m);	/* on FAT every file runs anyway; elsewhere they must */

	/* move into place: the old version aside first */
	rm_rf(old);
	if (is_dir(target) && rename(target, old)) {
		warn("cannot move %s aside: %s", target, strerror(errno));
		goto fail;
	}
	if (rename(stage, target)) {
		warn("cannot move %s into place: %s", stage, strerror(errno));
		rename(old, target);
		goto fail;
	}
	write_wrappers(&m, target);
	make_storage(id, &m);

	fresh = !had;
	old_build = had ? r.build : 0;
	now_iso(now);
	if (!had) {
		memset(&r, 0, sizeof(r));
		snprintf(r.id, sizeof(r.id), "%s", id);
		snprintf(r.installed, sizeof(r.installed), "%s", now);
		r.auto_installed = auto_installed;
	}
	snprintf(r.source, sizeof(r.source), "%s", a->src->id);
	json_str(&m, json_get(&m, 0, "name"), r.name, sizeof(r.name));
	snprintf(r.version, sizeof(r.version), "%s", t.version);
	r.build = t.build;
	snprintf(r.location, sizeof(r.location), "%s", loc);
	snprintf(r.updated, sizeof(r.updated), "%s", now);
	if (!strcmp(loc, "sd"))
		reg_add_card(&r, card);
	if (!auto_installed)
		r.auto_installed = 0;
	if (reg_write(&r)) {
		warn("cannot write the registry in %s", P.registry_dir);
		goto fail;
	}
	if (fresh) {
		hook(id, target, &m, "hooks.post_install", 0);
	} else {
		char ob[16], nb[16], *args[] = { ob, nb, 0 };

		snprintf(ob, sizeof(ob), "%d", old_build);
		snprintf(nb, sizeof(nb), "%d", t.build);
		hook(id, target, &m, "hooks.migrate", args);
	}
	rm_rf(old);
	if (!o->keep)
		unlink(pkg);
	journal_clear();
	say("%s %s installed%s", id, t.version, !strcmp(loc, "flash") ? " in flash" : "");
	ret = 0;
	goto out;
fail:
	rm_rf(stage);
	journal_clear();
out:
	json_free(&m);
	free(mtext);
	return ret;
}

/* ---- remove (device.md §5.4) ---- */

static void tmp_files(const char *id, const struct json *m)
{
	char p[512], exec[128];

	snprintf(p, sizeof(p), "%s/app-%s.log", P.tmp, id);
	unlink(p);
	snprintf(p, sizeof(p), "%s/app-%s.ask", P.tmp, id);
	unlink(p);
	if (m && !json_str(m, json_path(m, 0, "launch.exec"), exec, sizeof(exec))) {
		const char *b = strrchr(exec, '/');

		snprintf(p, sizeof(p), "%s/app-bin/%s", P.tmp, b ? b + 1 : exec);
		unlink(p);
	}
	snprintf(p, sizeof(p), "%s/cache/%s", P.tmp, id);
	rm_rf(p);
}

int store_remove(const char *id, const struct opts *o)
{
	struct reg r;
	char dir[256], data[256], conf[256], card[37] = "", *text = 0;
	struct json m;
	int have_m = 0, there;

	if (reg_read(id, &r)) {
		enum state s = app_state(id, 0, 0);

		if (s == ST_LEGACY || s == ST_UNREGISTERED)
			warn("%s is not installed by the store: use store delete %s", id, id);
		else
			warn("%s is not installed", id);
		return 1;
	}
	app_dir(dir, sizeof(dir), id, r.location);
	there = (!strcmp(r.location, "flash") || P.sd_present) && files_there(dir, id) != 0;
	if (there && !read_installed(dir, &text, &m))
		have_m = 1;
	if (P.sd_present)
		card_id(card, 0);

	/* user folders: say what goes, ask once (device.md §5.4) */
	if (have_m && P.sd_present) {
		int uf = json_path(&m, 0, "storage.data.user_folders");
		char what[512] = "";

		for (int i = 0; i < json_len(&m, uf) && json_is_arr(&m, uf); i++) {
			char f[128], p[512], one[160];
			long files = 0;
			long long sz;

			if (json_str(&m, json_item(&m, uf, i), f, sizeof(f)))
				continue;
			appdata_dir(data, sizeof(data), id);
			sz = dir_size(path_join(p, sizeof(p), data, f), &files);
			if (!files)
				continue;
			snprintf(one, sizeof(one), "%s%ld file(s) in %s, %.1f MB", what[0] ? "; " : "",
				 files, f, (double)sz / (1024 * 1024));
			strncat(what, one, sizeof(what) - strlen(what) - 1);
		}
		if (what[0]) {
			char q[600];

			snprintf(q, sizeof(q), "removing %s deletes %s", id, what);
			if (!confirm(o, q)) {
				json_free(&m);
				free(text);
				return 2;
			}
		}
	}
	if (have_m)
		hook(id, dir, &m, "hooks.pre_remove", 0);
	journal_write("remove", id, r.build);
	if (there)
		rm_rf(dir);
	if (P.sd_present && (!strcmp(r.location, "flash") || there || reg_has_card(&r, card)))
		rm_rf(appdata_dir(data, sizeof(data), id));
	rm_rf(config_dir(conf, sizeof(conf), id));
	remove_wrappers(id);
	tmp_files(id, have_m ? &m : 0);
	/* copies on cards that are not here: deleted when they come back */
	if (!strcmp(r.location, "sd")) {
		char pa[2][80];
		const char *paths[2] = { pa[0], pa[1] };

		snprintf(pa[0], sizeof(pa[0]), "badge/apps/%s", id);
		snprintf(pa[1], sizeof(pa[1]), "badge/appdata/%s", id);
		for (int i = 0; i < r.n_cards; i++)
			if (strcmp(r.cards[i], card))	/* this card's copy is gone already */
				cleanup_add(r.cards[i], id, paths, 2);
	}
	reg_delete(id);
	journal_clear();
	if (have_m)
		json_free(&m);
	free(text);
	say("%s removed%s", id, there ? "" : " (its files on another card go when that card is back)");
	return 0;
}

/* a directory without .package/ (device.md §5.8), or an unregistered one */
int store_delete_legacy(const char *id)
{
	enum state s = app_state(id, 0, 0);
	char dir[256];

	if (s != ST_LEGACY && s != ST_UNREGISTERED) {
		warn("%s is not an app the store left alone: use store remove %s", id, id);
		return 1;
	}
	for (int loc = 0; loc < 2; loc++) {
		char p[512];

		app_dir(dir, sizeof(dir), id, loc ? "flash" : "sd");
		if ((loc || P.sd_present) && is_file(path_join(p, sizeof(p), dir, "app.conf"))) {
			rm_rf(dir);
			say("%s deleted (only its own directory: the store cannot know what else it wrote)", id);
			return 0;
		}
	}
	return 1;
}

/* ---- verify, repair, adopt ---- */

int store_verify(const char *id, int quiet)
{
	struct reg r;
	char dir[256], p[512], *data, hex[65];
	int bad = 0, n = 0;

	if (reg_read(id, &r) && app_state(id, 0, 0) != ST_UNREGISTERED) {
		if (!quiet)
			warn("%s is not installed", id);
		return -1;
	}
	if (!r.location[0])
		snprintf(r.location, sizeof(r.location), is_dir(app_dir(dir, sizeof(dir), id, "sd")) ? "sd" : "flash");
	app_dir(dir, sizeof(dir), id, r.location);
	snprintf(p, sizeof(p), "%s/.package/files.sha256", dir);
	if (read_file(p, &data, 0, 1 << 20)) {
		if (!quiet)
			warn("%s: its files are not here", id);
		return -1;
	}
	for (char *l = data; *l; ) {
		size_t len = strcspn(l, "\n");
		char f[300], want[65];

		if (len > 66 && len < 66 + sizeof(f)) {
			snprintf(want, sizeof(want), "%.64s", l);
			snprintf(f, sizeof(f), "%.*s", (int)(len - 66), l + 66);
			n++;
			if (sha256_file(path_join(p, sizeof(p), dir, f), hex) || strcmp(hex, want)) {
				bad++;
				if (!quiet)
					say("%s: %s %s", id, f, is_file(p) ? "changed" : "missing");
			}
		}
		l += len + (l[len] == '\n');
	}
	free(data);
	if (!quiet)
		say("%s: %d of %d files as installed", id, n - bad, n);
	return bad;
}

int store_repair(const char *id, const struct opts *o)
{
	struct reg r;
	char spec[80];

	if (reg_read(id, &r)) {
		warn("%s is not installed", id);
		return 1;
	}
	snprintf(spec, sizeof(spec), "%s@%s", id, r.version);
	return store_install(spec, o, r.auto_installed);
}

int store_adopt(const char *id)
{
	struct reg r;
	char dir[256], p[512], *text, *src = 0, now[24], card[37] = "";
	struct json m;
	int loc;

	if (!reg_read(id, &r)) {
		warn("%s is registered already", id);
		return 1;
	}
	for (loc = 0; loc < 2; loc++) {
		if (!loc && !P.sd_present)
			continue;
		app_dir(dir, sizeof(dir), id, loc ? "flash" : "sd");
		if (is_dir(path_join(p, sizeof(p), dir, ".package")))
			break;
	}
	if (loc == 2 || read_installed(dir, &text, &m)) {
		warn("there is no %s with a .package/ here to adopt", id);
		return 1;
	}
	if (store_verify(id, 1)) {
		warn("%s is damaged: install it instead", id);
		json_free(&m);
		free(text);
		return 1;
	}
	memset(&r, 0, sizeof(r));
	snprintf(r.id, sizeof(r.id), "%s", id);
	snprintf(p, sizeof(p), "%s/.package/source", dir);
	if (!read_file(p, &src, 0, 4096))
		sscanf(src, "ID=%32s", r.source);
	free(src);
	if (!r.source[0])
		snprintf(r.source, sizeof(r.source), "whyos");
	json_str(&m, json_get(&m, 0, "name"), r.name, sizeof(r.name));
	json_str(&m, json_get(&m, 0, "version"), r.version, sizeof(r.version));
	r.build = (int)json_int(&m, json_get(&m, 0, "build"), 0);
	snprintf(r.location, sizeof(r.location), "%s", loc ? "flash" : "sd");
	now_iso(now);
	snprintf(r.installed, sizeof(r.installed), "%s", now);
	snprintf(r.updated, sizeof(r.updated), "%s", now);
	if (!loc && !card_id(card, 1))
		reg_add_card(&r, card);
	write_wrappers(&m, dir);
	make_storage(id, &m);
	json_free(&m);
	free(text);
	if (reg_write(&r)) {
		warn("cannot write the registry");
		return 1;
	}
	say("%s %s adopted", id, r.version);
	return 0;
}

/* ---- maintain (device.md §5.7) ---- */

static void tidy_dir(const char *parent)
{
	DIR *d = opendir(parent);
	struct dirent *e;

	while (d && (e = readdir(d))) {
		size_t l = strlen(e->d_name);
		char p[512], target[512];

		if (e->d_name[0] != '.' || l < 6)
			continue;
		path_join(p, sizeof(p), parent, e->d_name);
		if (!strcmp(e->d_name + l - 4, ".new")) {
			rm_rf(p);	/* an unpack that never finished */
		} else if (!strcmp(e->d_name + l - 4, ".old")) {
			snprintf(target, sizeof(target), "%s/%.*s", parent, (int)(l - 5), e->d_name + 1);
			if (is_dir(target))
				rm_rf(p);
			else
				rename(p, target);	/* put the old version back */
		}
	}
	if (d)
		closedir(d);
}

int store_maintain(void)
{
	char op[16], id[40], card[37] = "";
	int build, done;

	if (P.sd_present)
		card_id(card, 1);
	/* the store an OS image put in /data (why2025-linux post-build.sh)
	 * comes with .package/ but no registry entry: it takes itself in */
	if (app_state("store", 0, 0) == ST_UNREGISTERED && !store_adopt("store"))
		say("the store registered itself");
	if (!journal_read(op, id, &build)) {
		struct opts o = { .yes = 1 };

		if (!strcmp(op, "remove")) {
			say("finishing the removal of %s", id);
			store_remove(id, &o);
		} else if (!strcmp(op, "install")) {
			struct reg r;
			char dir[256];
			int ok;

			/* an install stopped: complete if its files are whole and
			 * of that build, else back to what was there */
			ok = 0;
			for (int loc = 0; loc < 2 && !ok; loc++) {
				char *text;
				struct json m;

				if (!loc && !P.sd_present)
					continue;
				app_dir(dir, sizeof(dir), id, loc ? "flash" : "sd");
				if (read_installed(dir, &text, &m))
					continue;
				ok = json_int(&m, json_get(&m, 0, "build"), 0) == build;
				json_free(&m);
				free(text);
			}
			if (ok && reg_read(id, &r)) {
				ok = !store_verify(id, 1) && !store_adopt(id);
			} else if (ok) {
				ok = !store_verify(id, 1);
				if (ok && r.build != build) {
					/* moved in, registry not yet updated */
					r.build = build;
					reg_write(&r);
				}
			}
			say("an interrupted install of %s: %s", id, ok ? "finished" : "undone");
		}
		journal_clear();
	}
	if (P.sd_present)
		tidy_dir(P.sd_apps);
	tidy_dir(P.flash_apps);
	if (P.sd_present) {
		DIR *d = opendir(P.dl_dir);
		struct dirent *e;

		while (d && (e = readdir(d))) {
			char p[512];
			struct stat st;
			size_t l = strlen(e->d_name);

			path_join(p, sizeof(p), P.dl_dir, e->d_name);
			if (l > 5 && !strcmp(e->d_name + l - 5, ".part") && !stat(p, &st) &&
			    time(0) - st.st_mtime > 7 * 24 * 3600)
				unlink(p);
		}
		if (d)
			closedir(d);
		done = cleanup_run(card);
		if (done)
			say("deleted %d app(s) removed while this card was out", done);
	}
	return 0;
}
