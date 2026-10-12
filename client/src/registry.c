/* SPDX-License-Identifier: MIT */
/*
 * store: what is installed (docs/device.md §2.2), the card's identity
 * (§4), the journal of the operation in progress and pending cleanups on
 * absent cards (§5.4, §5.7). Every file is written atomically.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "store.h"

static char *reg_path(char *out, size_t n, const char *id)
{
	snprintf(out, n, "%s/%s.json", P.registry_dir, id);
	return out;
}

int reg_read(const char *id, struct reg *r)
{
	char p[512], *data;
	size_t len;
	struct json j;
	int cards;

	memset(r, 0, sizeof(*r));
	if (read_file(reg_path(p, sizeof(p), id), &data, &len, 65536))
		return -1;
	if (json_parse(&j, data, len) || !json_is_obj(&j, 0)) {
		free(data);
		return -1;
	}
	json_str(&j, json_get(&j, 0, "id"), r->id, sizeof(r->id));
	json_str(&j, json_get(&j, 0, "source"), r->source, sizeof(r->source));
	json_str(&j, json_get(&j, 0, "name"), r->name, sizeof(r->name));
	json_str(&j, json_get(&j, 0, "version"), r->version, sizeof(r->version));
	json_str(&j, json_get(&j, 0, "location"), r->location, sizeof(r->location));
	json_str(&j, json_get(&j, 0, "installed"), r->installed, sizeof(r->installed));
	json_str(&j, json_get(&j, 0, "updated"), r->updated, sizeof(r->updated));
	r->build = (int)json_int(&j, json_get(&j, 0, "build"), 0);
	r->pinned = json_bool(&j, json_get(&j, 0, "pinned"), 0);
	r->auto_installed = json_bool(&j, json_get(&j, 0, "auto_installed"), 0);
	cards = json_get(&j, 0, "cards");
	for (int i = 0; i < json_len(&j, cards) && r->n_cards < MAX_CARDS; i++)
		if (!json_str(&j, json_item(&j, cards, i), r->cards[r->n_cards], sizeof(r->cards[0])))
			r->n_cards++;
	json_free(&j);
	free(data);
	if (strcmp(r->id, id))
		return -1;
	if (!r->location[0])
		snprintf(r->location, sizeof(r->location), "sd");
	return 0;
}

int reg_write(const struct reg *r)
{
	struct jw w;
	char p[512];
	int ret;

	jw_init(&w);
	jw_obj(&w, 0);
	jw_int(&w, "format", 1);
	jw_str(&w, "id", r->id);
	jw_str(&w, "source", r->source);
	jw_str(&w, "name", r->name);
	jw_str(&w, "version", r->version);
	jw_int(&w, "build", r->build);
	jw_str(&w, "location", r->location);
	jw_arr(&w, "cards");
	for (int i = 0; i < r->n_cards; i++)
		jw_str(&w, 0, r->cards[i]);
	jw_end(&w);
	jw_str(&w, "installed", r->installed);
	jw_str(&w, "updated", r->updated);
	jw_bool(&w, "pinned", r->pinned);
	jw_bool(&w, "auto_installed", r->auto_installed);
	jw_end(&w);
	ret = mkdir_p(P.registry_dir) || write_atomic(reg_path(p, sizeof(p), r->id), w.buf, w.len) ? -1 : 0;
	jw_free(&w);
	return ret;
}

int reg_delete(const char *id)
{
	char p[512];

	return unlink(reg_path(p, sizeof(p), id)) && errno != ENOENT ? -1 : 0;
}

static int cmp_id(const void *a, const void *b)
{
	return strcmp(a, b);
}

int reg_list(char ids[][33], int max)
{
	DIR *d = opendir(P.registry_dir);
	struct dirent *e;
	int n = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);

		if (l > 5 && l - 5 < 33 && !strcmp(e->d_name + l - 5, ".json") && e->d_name[0] != '.') {
			memcpy(ids[n], e->d_name, l - 5);
			ids[n++][l - 5] = 0;
		}
	}
	closedir(d);
	qsort(ids, (size_t)n, sizeof(ids[0]), cmp_id);
	return n;
}

int reg_has_card(const struct reg *r, const char *card)
{
	for (int i = 0; i < r->n_cards; i++)
		if (!strcmp(r->cards[i], card))
			return 1;
	return 0;
}

void reg_add_card(struct reg *r, const char *card)
{
	if (!card[0] || reg_has_card(r, card))
		return;
	if (r->n_cards == MAX_CARDS) {	/* forget the oldest */
		memmove(r->cards[0], r->cards[1], sizeof(r->cards[0]) * (MAX_CARDS - 1));
		r->n_cards--;
	}
	snprintf(r->cards[r->n_cards++], sizeof(r->cards[0]), "%s", card);
}

/* this card's id (device.md §4); with create, made on first sight. "" if none */
int card_id(char out[37], int create)
{
	char p[512], *data;
	size_t len;
	struct json j;
	struct jw w;
	char now[24];

	out[0] = 0;
	if (!P.sd_present)
		return -1;
	snprintf(p, sizeof(p), "%s/card.json", P.store_dir);
	if (!read_file(p, &data, &len, 4096)) {
		if (!json_parse(&j, data, len)) {
			json_str(&j, json_get(&j, 0, "card"), out, 37);
			json_free(&j);
		}
		free(data);
		if (strlen(out) == 36)
			return 0;
	}
	if (!create)
		return -1;
	uuid4(out);
	now_iso(now);
	jw_init(&w);
	jw_obj(&w, 0);
	jw_int(&w, "format", 1);
	jw_str(&w, "card", out);
	jw_str(&w, "created", now);
	jw_end(&w);
	if (mkdir_p(P.store_dir) || write_atomic(p, w.buf, w.len)) {
		jw_free(&w);
		out[0] = 0;
		return -1;
	}
	jw_free(&w);
	return 0;
}

/* the journal: on the card, or in $HOME without one (flash installs) */
static char *journal_path(char *out, size_t n)
{
	if (P.sd_present)
		snprintf(out, n, "%s/journal", P.store_dir);
	else
		snprintf(out, n, "%s/store/journal", P.whyos);
	return out;
}

int journal_write(const char *op, const char *id, int build)
{
	char p[512], line[96], dir[512];

	journal_path(p, sizeof(p));
	snprintf(dir, sizeof(dir), "%s", p);
	*strrchr(dir, '/') = 0;
	snprintf(line, sizeof(line), "%s %s %d\n", op, id, build);
	return mkdir_p(dir) || write_atomic(p, line, strlen(line)) ? -1 : 0;
}

int journal_read(char *op, char *id, int *build)
{
	char p[512], *data;
	int r = -1;

	if (read_file(journal_path(p, sizeof(p)), &data, 0, 256))
		return -1;
	if (sscanf(data, "%15s %32s %d", op, id, build) == 3)
		r = 0;
	free(data);
	return r;
}

void journal_clear(void)
{
	char p[512];

	unlink(journal_path(p, sizeof(p)));
}

/* ---- pending cleanups on cards that were not there (device.md §5.4) ---- */

static char *cleanup_path(char *out, size_t n)
{
	snprintf(out, n, "%s/store/cleanup.json", P.whyos);
	return out;
}

struct pending {
	char card[37], id[33], source[33];
	char paths[4][160];
	int n;
};

static int cleanup_read(struct pending *pd, int max)
{
	char p[512], *data;
	size_t len;
	struct json j;
	int arr, n = 0;

	if (read_file(cleanup_path(p, sizeof(p)), &data, &len, 1 << 20))
		return 0;
	if (json_parse(&j, data, len)) {
		free(data);
		return 0;
	}
	arr = json_get(&j, 0, "pending");
	for (int i = 0; i < json_len(&j, arr) && n < max; i++) {
		int e = json_item(&j, arr, i), ps = json_get(&j, e, "paths");

		memset(&pd[n], 0, sizeof(pd[n]));
		json_str(&j, json_get(&j, e, "card"), pd[n].card, sizeof(pd[n].card));
		json_str(&j, json_get(&j, e, "id"), pd[n].id, sizeof(pd[n].id));
		json_str(&j, json_get(&j, e, "source"), pd[n].source, sizeof(pd[n].source));
		for (int k = 0; k < json_len(&j, ps) && pd[n].n < 4; k++)
			if (!json_str(&j, json_item(&j, ps, k), pd[n].paths[pd[n].n], sizeof(pd[n].paths[0])))
				pd[n].n++;
		n++;
	}
	json_free(&j);
	free(data);
	return n;
}

static int cleanup_write(const struct pending *pd, int n)
{
	struct jw w;
	char p[512], dir[512];
	int r;

	cleanup_path(p, sizeof(p));
	if (!n)
		return unlink(p) && errno != ENOENT ? -1 : 0;
	jw_init(&w);
	jw_obj(&w, 0);
	jw_int(&w, "format", 1);
	jw_arr(&w, "pending");
	for (int i = 0; i < n; i++) {
		jw_obj(&w, 0);
		jw_str(&w, "card", pd[i].card);
		jw_str(&w, "id", pd[i].id);
		jw_str(&w, "source", pd[i].source);
		jw_arr(&w, "paths");
		for (int k = 0; k < pd[i].n; k++)
			jw_str(&w, 0, pd[i].paths[k]);
		jw_end(&w);
		jw_end(&w);
	}
	jw_end(&w);
	jw_end(&w);
	snprintf(dir, sizeof(dir), "%s/store", P.whyos);
	r = mkdir_p(dir) || write_atomic(p, w.buf, w.len) ? -1 : 0;
	jw_free(&w);
	return r;
}

#define MAX_PENDING 64

/* paths relative to the card ("badge/apps/<id>"); the source comes from
 * the registry entry, which the caller deletes afterwards */
int cleanup_add(const char *card, const char *id, const char *const *paths, int n)
{
	struct pending pd[MAX_PENDING];
	int np = cleanup_read(pd, MAX_PENDING - 1);
	struct reg r;

	memset(&pd[np], 0, sizeof(pd[np]));
	snprintf(pd[np].card, sizeof(pd[np].card), "%s", card);
	snprintf(pd[np].id, sizeof(pd[np].id), "%s", id);
	if (!reg_read(id, &r))
		snprintf(pd[np].source, sizeof(pd[np].source), "%s", r.source);
	for (int i = 0; i < n && i < 4; i++)
		snprintf(pd[np].paths[pd[np].n++], sizeof(pd[np].paths[0]), "%s", paths[i]);
	return cleanup_write(pd, np + 1);
}

/* is apps/<id> on this card still the store's copy of that app? */
static int still_ours(const struct pending *p)
{
	char path[512], *data, src[64] = "";
	size_t len;
	struct json j;
	int ok = 0;

	snprintf(path, sizeof(path), "%s/%s/.package/manifest.json", P.sd_apps, p->id);
	if (read_file(path, &data, &len, MAX_MANIFEST))
		return 0;		/* not the store's copy: leave the card alone */
	if (!json_parse(&j, data, len)) {
		ok = json_eq(&j, json_get(&j, 0, "id"), p->id);
		json_free(&j);
	}
	free(data);
	snprintf(path, sizeof(path), "%s/%s/.package/source", P.sd_apps, p->id);
	if (ok && !read_file(path, &data, 0, 4096)) {
		sscanf(data, "ID=%63s", src);
		free(data);
		ok = !p->source[0] || !strcmp(src, p->source);
	}
	return ok;
}

/* run the cleanups pending for this card; returns how many were done */
int cleanup_run(const char *card)
{
	struct pending pd[MAX_PENDING];
	int n = cleanup_read(pd, MAX_PENDING), keep = 0, done = 0;

	for (int i = 0; i < n; i++) {
		if (strcmp(pd[i].card, card)) {
			pd[keep++] = pd[i];
			continue;
		}
		if (still_ours(&pd[i])) {
			for (int k = 0; k < pd[i].n; k++) {
				char full[512];

				path_join(full, sizeof(full), P.sd, pd[i].paths[k]);
				rm_rf(full);
			}
			done++;
		}
		/* not ours anymore (the card was reused): drop it, touch nothing */
	}
	if (done || keep != n)
		cleanup_write(pd, keep);
	return done;
}

int cleanup_list(FILE *out, int json)
{
	struct pending pd[MAX_PENDING];
	int n = cleanup_read(pd, MAX_PENDING);

	for (int i = 0; i < n; i++) {
		if (json)
			fprintf(out, "{\"card\": \"%s\", \"id\": \"%s\"}%s\n", pd[i].card, pd[i].id,
				i + 1 < n ? "," : "");
		else
			fprintf(out, "%-12s on card %.8s…: %d folder(s) to delete when it is inserted\n",
				pd[i].id, pd[i].card, pd[i].n);
	}
	return n;
}
