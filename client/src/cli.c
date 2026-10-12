/* SPDX-License-Identifier: MIT */
/*
 * store: the command line (docs/client.md §1). The UI (ui.c) calls the same
 * functions; `store` without arguments starts it on the badge.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "store.h"

#define S_(x) ((x) ? (x) : "")

static const char usage[] =
	"usage: store [--yes] [--json] [--keep] <command>\n"
	"  update                     fetch the index of every source\n"
	"  search <words>             search the cached index\n"
	"  info <id>                  details, versions and state\n"
	"  install <id>[@<version>]   install or update\n"
	"  remove <id>                remove everything the app owns\n"
	"  list [--missing|--updates|--other]\n"
	"  verify [<id>]              check installed files\n"
	"  repair <id>                reinstall the installed version\n"
	"  adopt <id>                 register an app found on this card\n"
	"  delete <id>                delete an app the store did not install\n"
	"  pin <id> | unpin <id>      stay on this version, or not\n"
	"  maintain                   finish interrupted work, pending cleanups\n"
	"  sources                    the sources and their indexes\n"
	"  version\n";

static int progress(long long done, long long total, void *arg)
{
	(void)arg;
	if (total > 0)
		fprintf(stderr, "\r  %lld of %lld KB (%d%%)   ", done / 1024, total / 1024,
			(int)(done * 100 / total));
	else
		fprintf(stderr, "\r  %lld KB   ", done / 1024);
	return 0;
}

static int ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; hay && *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

/* one app as a JSON object on a line, comma first unless first */
static void json_app(const char *id, const struct app *a, enum state s, const struct reg *r, int first)
{
	struct jw w;

	jw_init(&w);
	w.indent = 0;
	jw_obj(&w, 0);
	jw_str(&w, "id", id);
	jw_str(&w, "name", a ? a->name : r ? r->name : id);
	jw_str(&w, "state", state_name(s));
	if (a) {
		jw_str(&w, "type", S_(a->type));
		jw_str(&w, "category", S_(a->category));
		jw_str(&w, "summary", S_(a->summary));
		jw_str(&w, "version", S_(a->version));
		jw_bool(&w, "terminal", a->terminal);
		jw_str(&w, "source", a->src->id);
	}
	if (r && r->id[0]) {
		jw_str(&w, "installed_version", r->version);
		jw_str(&w, "location", r->location);
		jw_bool(&w, "pinned", r->pinned);
	}
	jw_end(&w);
	printf("%s %s\n", first ? "" : ",", w.buf);
	jw_free(&w);
}

static int cmd_search(int argc, char **argv, int json)
{
	int total = 0, first = 1;

	index_load_all();
	if (json)
		printf("[\n");
	for (int i = 0; i < n_indexes; i++)
		for (int k = 0; k < IX[i].n; k++) {
			struct app *a = &IX[i].apps[k];
			int ok = !a->withdrawn && a->type && strcmp(a->type, "system");

			total++;
			for (int w = 0; ok && w < argc; w++)
				ok = ci_has(a->id, argv[w]) || ci_has(a->name, argv[w]) ||
				     ci_has(a->summary, argv[w]) || ci_has(a->tags, argv[w]) ||
				     ci_has(a->author, argv[w]) || ci_has(a->commands, argv[w]);
			if (!ok)
				continue;
			if (json)
				json_app(a->id, a, app_state(a->id, 0, 0), 0, first);
			else
				printf("%-12s %-5s %-24s %s%s\n", a->id, a->type, a->name, S_(a->summary),
				       a->terminal ? "  [command-line]" : "");
			first = 0;
		}
	if (json)
		printf("]\n");
	else if (!total)
		warn("no index yet: run store update");
	return 0;
}

static int cmd_info(const char *id, int json)
{
	struct reg r;
	struct app *a = 0;
	enum state s;
	char *data = 0, err[300];
	size_t len = 0;

	index_load_all();
	s = app_state(id, &r, &a);
	if (!a && s == ST_NONE) {
		warn("there is no app %s", id);
		return 1;
	}
	if (a && !a->withdrawn && details_get(a, &data, &len, err, sizeof(err)))
		warn("%s", err);
	if (json) {
		printf("{\"state\": \"%s\", \"details\": %s}\n", state_name(s), data ? data : "null");
		free(data);
		return 0;
	}
	if (a) {
		printf("%s (%s)%s\n%s\n\n", a->name, a->id, a->terminal ? "  [command-line]" : "",
		       S_(a->summary));
		printf("  version     %s (build %d)%s\n", S_(a->version), a->build,
		       a->withdrawn ? "  WITHDRAWN" : "");
		printf("  author      %s\n", S_(a->author));
		printf("  type        %s / %s\n", S_(a->type), S_(a->category));
		printf("  download    %lld KB, %lld KB installed\n", (a->size + 1023) / 1024,
		       (a->installed_size + 1023) / 1024);
		printf("  needs       WhyOS API %d, %d KB of memory%s%s\n", a->os_api, a->memory_kb,
		       a->features && *a->features ? ", " : "", S_(a->features));
		if (a->commands && *a->commands)
			printf("  commands    %s\n", a->commands);
		if (a->withdrawn)
			printf("  withdrawn   %s\n", a->withdrawn);
	} else {
		printf("%s (%s)\n", r.name[0] ? r.name : id, id);
	}
	printf("  state       %s", state_name(s));
	if (r.id[0])
		printf(" (%s, %s%s%s)", r.version, r.location, r.pinned ? ", pinned" : "",
		       r.auto_installed ? ", as a dependency" : "");
	printf("\n");
	if (data) {
		struct json j;

		if (!json_parse(&j, data, len)) {
			char *t = json_strdup(&j, json_get(&j, 0, "description"));
			char lic[200] = "";

			json_str(&j, json_get(&j, 0, "license"), lic, sizeof(lic));
			printf("  license     %s\n", lic);
			if (t)
				printf("\n%s\n", t[0] == '\n' ? t + 1 : t);
			free(t);
			json_free(&j);
		}
		free(data);
	}
	return 0;
}

static int cmd_list(const char *filter, int json)
{
	static char ids[256][33], other[64][33], found[64][33];
	int n = reg_list(ids, 256), no, nf, first = 1;
	int want_other = filter && !strcmp(filter, "--other");

	if (filter && strcmp(filter, "--missing") && strcmp(filter, "--updates") && !want_other) {
		fputs(usage, stderr);
		return 2;
	}
	index_load_all();
	no = legacy_list(other, 64, 0);
	nf = legacy_list(found, 64, 1);
	if (json)
		printf("[\n");
	for (int i = 0; i < n && !want_other; i++) {
		struct reg r;
		struct app *a = 0;
		enum state s = app_state(ids[i], &r, &a);

		if (filter && !strcmp(filter, "--missing") && s != ST_MISSING)
			continue;
		if (filter && !strcmp(filter, "--updates") && s != ST_UPDATE)
			continue;
		if (json)
			json_app(ids[i], a, s, &r, first);
		else if (s == ST_UPDATE && a)
			printf("%-12s %-18s %s -> %s\n", ids[i], state_name(s), r.version, a->version);
		else
			printf("%-12s %-18s %s%s\n", ids[i], state_name(s), r.version, r.pinned ? " (pinned)" : "");
		first = 0;
	}
	for (int pass = 0; pass < 2 && (!filter || want_other); pass++)
		for (int i = 0; i < (pass ? no : nf); i++) {
			const char *id = pass ? other[i] : found[i];
			enum state s = pass ? ST_LEGACY : ST_UNREGISTERED;

			if (json)
				json_app(id, index_find(id, 0), s, 0, first);
			else
				printf("%-12s %s\n", id, state_name(s));
			first = 0;
		}
	if (json)
		printf("]\n");
	else if (!filter)
		cleanup_list(stdout, 0);
	return 0;
}

static int cmd_sources(void)
{
	index_load_all();
	for (int i = 0; i < n_sources; i++) {
		const struct index *ix = 0;

		for (int k = 0; k < n_indexes; k++)
			if (IX[k].src == &S[i])
				ix = &IX[k];
		printf("%-10s %s %s\n", S[i].id, S[i].enabled ? "enabled " : "disabled", S[i].url);
		if (ix)
			printf("           index serial %d of %s, %d apps\n", ix->serial, S_(ix->generated), ix->n);
		else
			printf("           no index yet (store update)\n");
	}
	return 0;
}

static int cmd_pin(const char *id, int pin)
{
	struct reg r;

	if (reg_read(id, &r)) {
		warn("%s is not installed", id);
		return 1;
	}
	r.pinned = pin;
	if (reg_write(&r))
		return 1;
	say("%s %s", id, pin ? "pinned: no update offers" : "unpinned");
	return 0;
}

/* CI and the tests: the package checks alone (package.md §1.2) */
static int cmd_check(const char *pkg, const char *sha)
{
	char err[300], hex[65], *m = 0;

	if (sha && (sha256_file(pkg, hex) || strcmp(hex, sha))) {
		printf("%s: REJECT: SHA-256 of the package does not match\n", pkg);
		return 1;
	}
	if (wpk_unpack(pkg, 0, 0, 0, &m, err, sizeof(err))) {
		printf("%s: REJECT: %s\n", pkg, err);
		return 1;
	}
	free(m);
	printf("%s: OK\n", pkg);
	return 0;
}

int cli_main(int argc, char **argv)
{
	struct opts o = { 0 };
	char *args[64], err[300];
	int n = 0;

	for (int i = 1; i < argc && n < 63; i++) {
		if (!strcmp(argv[i], "--yes") || !strcmp(argv[i], "-y"))
			o.yes = 1;
		else if (!strcmp(argv[i], "--json"))
			o.json = 1;
		else if (!strcmp(argv[i], "--keep"))
			o.keep = 1;
		else
			args[n++] = argv[i];
	}
	args[n] = 0;
	if (!n) {
		fputs(usage, stderr);
		return 2;
	}
	if (isatty(2) && !o.json)
		o.progress = progress;
	paths_init();
	sources_load();
#define IS(c) (!strcmp(args[0], c))
#define NEED(k) do { if (n != (k) + 1) { fputs(usage, stderr); return 2; } } while (0)
	if (IS("version")) {
		say("store %s (formats up to %d; WhyOS API %d)", STORE_VERSION, STORE_FORMAT, P.api);
		return 0;
	}
	if (IS("check-package")) {
		if (n < 2 || n > 3) {
			fputs("usage: store check-package <file.wpk> [<sha256>]\n", stderr);
			return 2;
		}
		return cmd_check(args[1], n == 3 ? args[2] : 0);
	}
	if (IS("update")) {
		NEED(0);
		if (index_update(1, err, sizeof(err))) {
			warn("%s", err);
			return 1;
		}
		index_load_all();
		for (int i = 0; i < n_indexes; i++)
			say("%s: %d apps (serial %d)", IX[i].src->id, IX[i].n, IX[i].serial);
		return 0;
	}
	if (IS("search"))
		return cmd_search(n - 1, args + 1, o.json);
	if (IS("info")) {
		NEED(1);
		return cmd_info(args[1], o.json);
	}
	if (IS("install")) {
		int r = 0;

		if (n < 2) {
			fputs(usage, stderr);
			return 2;
		}
		for (int i = 1; i < n; i++)
			r |= store_install(args[i], &o, 0);
		return r;
	}
	if (IS("remove")) {
		NEED(1);
		return store_remove(args[1], &o);
	}
	if (IS("list"))
		return cmd_list(n > 1 ? args[1] : 0, o.json);
	if (IS("verify")) {
		static char ids[256][33];
		int bad = 0, k;

		if (n == 2)
			return store_verify(args[1], 0) ? 1 : 0;
		k = reg_list(ids, 256);
		for (int i = 0; i < k; i++)
			bad |= store_verify(ids[i], 0) != 0;
		return bad;
	}
	if (IS("repair")) {
		NEED(1);
		return store_repair(args[1], &o);
	}
	if (IS("adopt")) {
		NEED(1);
		return store_adopt(args[1]);
	}
	if (IS("delete")) {
		NEED(1);
		return store_delete_legacy(args[1]);
	}
	if (IS("pin") || IS("unpin")) {
		NEED(1);
		return cmd_pin(args[1], IS("pin"));
	}
	if (IS("maintain")) {
		NEED(0);
		return store_maintain();
	}
	if (IS("sources")) {
		NEED(0);
		return cmd_sources();
	}
	fputs(usage, stderr);
	return 2;
}

int main(int argc, char **argv)
{
#ifndef STORE_HOST
	if (argc == 1 || (argc == 2 && !strncmp(argv[1], "--my-apps", 9)))
		return ui_main(argc, argv);
#endif
	return cli_main(argc, argv);
}
