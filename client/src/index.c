/* SPDX-License-Identifier: MIT */
/*
 * store: the index of each source (docs/repository.md §5.1): fetched as
 * index.json.gz, checked (format, serial never lower than the cached one),
 * cached on the card (or in /tmp without one), and read into a compact
 * table: strings only, no descriptions. Details per app on demand,
 * checked against details_sha256.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/sha256.h>
#include <zlib.h>

#include "store.h"

#define INDEX_MAX	(8 * 1024 * 1024)
#define DETAILS_MAX	(512 * 1024)

struct index IX[MAX_INDEXES];
int n_indexes;

static char *src_cache(char *out, size_t n, const struct source *s, const char *file)
{
	snprintf(out, n, "%s/%s/%s", P.cache_dir, s->id, file);
	return out;
}

static int gunzip(const char *in, size_t inlen, char **out, size_t *outlen, size_t max)
{
	z_stream z;
	size_t cap = inlen * 4 + 1024, len = 0;
	char *buf;
	int r;

	memset(&z, 0, sizeof(z));
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
		return -1;
	buf = xmalloc(cap + 1);
	z.next_in = (unsigned char *)in;
	z.avail_in = (unsigned)inlen;
	do {
		if (len == cap) {
			if (cap >= max) {
				inflateEnd(&z);
				free(buf);
				return -1;
			}
			cap = cap * 2 > max ? max : cap * 2;
			buf = xrealloc(buf, cap + 1);
		}
		z.next_out = (unsigned char *)buf + len;
		z.avail_out = (unsigned)(cap - len);
		r = inflate(&z, Z_NO_FLUSH);
		len = cap - z.avail_out;
	} while (r == Z_OK);
	inflateEnd(&z);
	if (r != Z_STREAM_END) {
		free(buf);
		return -1;
	}
	buf[len] = 0;
	*out = buf;
	*outlen = len;
	return 0;
}

static char *dup_tok(const struct json *j, int obj, const char *key)
{
	return json_strdup(j, json_get(j, obj, key));
}

/* an array of strings as one space-separated string ("" if none) */
static char *join_arr(const struct json *j, int arr)
{
	size_t len = 0;
	char *out;

	for (int i = 0; i < json_len(j, arr) && json_is_arr(j, arr); i++) {
		char *s = json_strdup(j, json_item(j, arr, i));

		if (s)
			len += strlen(s) + 1;
		free(s);
	}
	out = xmalloc(len + 1);
	out[0] = 0;
	for (int i = 0; i < json_len(j, arr) && json_is_arr(j, arr); i++) {
		char *s = json_strdup(j, json_item(j, arr, i));

		if (s) {
			if (out[0])
				strcat(out, " ");
			strcat(out, s);
		}
		free(s);
	}
	return out;
}

void index_free(struct index *ix)
{
	for (int i = 0; i < ix->n; i++) {
		struct app *a = &ix->apps[i];
		char **f[] = { &a->id, &a->name, &a->summary, &a->type, &a->category, &a->author,
			       &a->updated, &a->icon, &a->menu_icon, &a->details_sha256, &a->withdrawn,
			       &a->install_location, &a->tags, &a->commands, &a->features,
			       &a->requires_apps, &a->version, &a->url, &a->sha256 };

		for (size_t k = 0; k < sizeof(f) / sizeof(f[0]); k++)
			free(*f[k]);
	}
	free(ix->apps);
	free(ix->generated);
	free(ix->base_url);
	free(ix->featured);
	free(ix->categories_sha256);
	memset(ix, 0, sizeof(*ix));
}

int index_parse(struct index *ix, const char *data, size_t len, char *err, size_t errn)
{
	struct json j;
	int apps;

	memset(ix, 0, sizeof(*ix));
	if (json_parse(&j, data, len) || !json_is_obj(&j, 0)) {
		snprintf(err, errn, "the index is not valid JSON");
		return -1;
	}
	ix->format = (int)json_int(&j, json_get(&j, 0, "format"), 0);
	if (ix->format < 1 || ix->format > STORE_FORMAT) {
		snprintf(err, errn, "the index is format %d: update the store", ix->format);
		json_free(&j);
		return -1;
	}
	ix->serial = (int)json_int(&j, json_get(&j, 0, "serial"), 0);
	ix->generated = dup_tok(&j, 0, "generated");
	ix->base_url = dup_tok(&j, 0, "base_url");
	ix->featured = join_arr(&j, json_get(&j, 0, "featured"));
	ix->categories_sha256 = dup_tok(&j, 0, "categories_sha256");
	apps = json_get(&j, 0, "apps");
	if (!json_is_arr(&j, apps)) {
		snprintf(err, errn, "the index has no apps");
		json_free(&j);
		index_free(ix);
		return -1;
	}
	ix->apps = xmalloc((size_t)(json_len(&j, apps) + 1) * sizeof(struct app));
	memset(ix->apps, 0, (size_t)(json_len(&j, apps) + 1) * sizeof(struct app));
	for (int i = 0, t = apps + 1; i < json_len(&j, apps); i++, t = json_skip(&j, t)) {
		struct app *a = &ix->apps[ix->n];
		int req = json_get(&j, t, "requires"), lat = json_get(&j, t, "latest"), pkg;

		a->id = dup_tok(&j, t, "id");
		if (!a->id)
			continue;
		a->name = dup_tok(&j, t, "name");
		a->summary = dup_tok(&j, t, "summary");
		a->type = dup_tok(&j, t, "type");
		a->category = dup_tok(&j, t, "category");
		a->author = dup_tok(&j, t, "author");
		a->updated = dup_tok(&j, t, "updated");
		a->icon = dup_tok(&j, t, "icon");
		a->menu_icon = dup_tok(&j, t, "menu_icon");
		a->details_sha256 = dup_tok(&j, t, "details_sha256");
		a->withdrawn = dup_tok(&j, t, "withdrawn");
		a->install_location = dup_tok(&j, t, "install_location");
		a->tags = join_arr(&j, json_get(&j, t, "tags"));
		a->commands = join_arr(&j, json_get(&j, t, "commands"));
		a->terminal = json_bool(&j, json_get(&j, t, "terminal"), 0);
		a->features = join_arr(&j, json_get(&j, req, "features"));
		a->requires_apps = join_arr(&j, json_get(&j, req, "apps"));
		a->os_api = (int)json_int(&j, json_get(&j, req, "os_api"), 1);
		a->memory_kb = (int)json_int(&j, json_get(&j, req, "memory_kb"), 0);
		a->version = json_strdup(&j, json_get(&j, lat, "version"));
		a->build = (int)json_int(&j, json_get(&j, lat, "build"), 0);
		pkg = json_get(&j, lat, "package");
		a->url = json_strdup(&j, json_get(&j, pkg, "url"));
		a->sha256 = json_strdup(&j, json_get(&j, pkg, "sha256"));
		a->size = json_int(&j, json_get(&j, pkg, "size"), 0);
		a->installed_size = json_int(&j, json_get(&j, pkg, "installed_size"), 0);
		if (!a->name)
			a->name = xstrdup(a->id);
		ix->n++;
	}
	json_free(&j);
	return 0;
}

static int read_cache(const struct source *s, char **data, size_t *len)
{
	char p[512];

	return read_file(src_cache(p, sizeof(p), s, "index.json"), data, len, INDEX_MAX);
}

static int cached_serial(const struct source *s)
{
	char *data, err[200];
	size_t len;
	struct index ix;
	int serial;

	if (read_cache(s, &data, &len))
		return 0;
	if (index_parse(&ix, data, len, err, sizeof(err))) {
		free(data);
		return 0;
	}
	serial = ix.serial;
	index_free(&ix);
	free(data);
	return serial;
}

/*
 * Fetch the index of every enabled source whose cache is older than an hour
 * (force: all). A source that cannot be reached keeps its cache; err gets
 * the first problem. 0 when every source is fresh or was fetched.
 */
int index_update(int force, char *err, size_t errn)
{
	int bad = 0;

	err[0] = 0;
	for (int i = 0; i < n_sources; i++) {
		struct source *s = &S[i];
		char path[512], url[512], e[300], *gz, *data;
		size_t gzlen, len;
		struct stat st;
		struct index ix;
		int old;

		if (!s->enabled)
			continue;
		src_cache(path, sizeof(path), s, "index.json");
		if (!force && !stat(path, &st) && time(0) - st.st_mtime < INDEX_MAX_AGE &&
		    time(0) >= st.st_mtime)
			continue;
		snprintf(url, sizeof(url), "%sindex.json.gz", s->url);
		if (net_get_mem(s, url, &gz, &gzlen, INDEX_MAX, e, sizeof(e))) {
			if (!bad++)
				snprintf(err, errn, "%s: %s", s->id, e);
			continue;
		}
		if (gunzip(gz, gzlen, &data, &len, INDEX_MAX)) {
			free(gz);
			if (!bad++)
				snprintf(err, errn, "%s: the index is damaged", s->id);
			continue;
		}
		free(gz);
		if (index_parse(&ix, data, len, e, sizeof(e))) {
			free(data);
			if (!bad++)
				snprintf(err, errn, "%s: %s", s->id, e);
			continue;
		}
		old = cached_serial(s);
		if (ix.serial < old) {
			/* an older index than we had: a stale mirror or cache */
			if (!bad++)
				snprintf(err, errn, "%s: the index is older than the one we have (serial %d < %d)",
					 s->id, ix.serial, old);
			index_free(&ix);
			free(data);
			continue;
		}
		index_free(&ix);
		{
			char dir[512];

			snprintf(dir, sizeof(dir), "%s/%s", P.cache_dir, s->id);
			if (mkdir_p(dir) || write_atomic(path, data, len)) {
				if (!bad++)
					snprintf(err, errn, "cannot write %s", path);
			}
		}
		free(data);
	}
	return bad ? -1 : 0;
}

int index_load_all(void)
{
	for (int i = 0; i < n_indexes; i++)
		index_free(&IX[i]);
	n_indexes = 0;
	for (int i = 0; i < n_sources; i++) {
		char *data, err[200];
		size_t len;

		if (!S[i].enabled || read_cache(&S[i], &data, &len))
			continue;
		if (!index_parse(&IX[n_indexes], data, len, err, sizeof(err))) {
			IX[n_indexes].src = &S[i];
			for (int k = 0; k < IX[n_indexes].n; k++)
				IX[n_indexes].apps[k].src = &S[i];
			n_indexes++;
		}
		free(data);
	}
	return n_indexes;
}

struct app *index_find(const char *id, const char *source)
{
	for (int i = 0; i < n_indexes; i++) {
		if (source && strcmp(IX[i].src->id, source))
			continue;
		for (int k = 0; k < IX[i].n; k++)
			if (!strcmp(IX[i].apps[k].id, id))
				return &IX[i].apps[k];
	}
	return 0;
}

static int sha_ok(const char *data, size_t len, const char *want)
{
	unsigned char d[32];
	char hex[65];

	if (!want)
		return 0;
	mbedtls_sha256((const unsigned char *)data, len, d, 0);
	sha256_hex(d, hex);
	return !strcmp(hex, want);
}

/* file (relative to the source) from the cache when it matches sha, else fetched and cached */
static int cached_get(const struct source *src, const char *file, const char *sha, size_t max,
		      char **data, size_t *len, char *err, size_t errn)
{
	char path[512], url[512], dir[512], *slash;

	src_cache(path, sizeof(path), src, file);
	if (!read_file(path, data, len, max)) {
		if (sha_ok(*data, *len, sha))
			return 0;
		free(*data);
	}
	snprintf(url, sizeof(url), "%s%s", src->url, file);
	if (net_get_mem(src, url, data, len, max, err, errn))
		return -1;
	if (!sha_ok(*data, *len, sha)) {
		free(*data);
		snprintf(err, errn, "%s does not match the index (refresh with store update)", file);
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s", path);
	if ((slash = strrchr(dir, '/')))
		*slash = 0;
	if (!mkdir_p(dir))
		write_atomic(path, *data, *len);
	return 0;
}

/* the details of an app (repository.md §5.2), from the cache or fetched */
int details_get(const struct app *a, char **data, size_t *len, char *err, size_t errn)
{
	char file[64];

	snprintf(file, sizeof(file), "apps/%s.json", a->id);
	if (!cached_get(a->src, file, a->details_sha256, DETAILS_MAX, data, len, err, errn))
		return 0;
	if (strstr(err, "does not match"))
		snprintf(err, errn, "%s: the details do not match the index (refresh with store update)", a->id);
	return -1;
}

/* categories.json of an index (repository.md §4), checked against categories_sha256 */
int categories_get(const struct index *ix, char **data, size_t *len, char *err, size_t errn)
{
	if (!ix->categories_sha256) {
		snprintf(err, errn, "the index names no categories");
		return -1;
	}
	return cached_get(ix->src, "categories.json", ix->categories_sha256, DETAILS_MAX, data, len,
			  err, errn);
}

/*
 * An icon or screenshot (rel: "icons/x.qoi", relative to the source) as a
 * cached file; its path in out. The index has no hash for these: they are
 * pictures, checked by the QOI decoder, and fetched again after "clear cache".
 */
int asset_get(const struct source *src, const char *rel, char *out, size_t n, char *err, size_t errn)
{
	char url[512], dir[512], *slash;

	if (strstr(rel, "..") || rel[0] == '/' || strchr(rel, ':')) {
		snprintf(err, errn, "a strange path in the index: %s", rel);
		return -1;
	}
	src_cache(out, n, src, rel);
	if (file_size(out) > 0)
		return 0;
	snprintf(dir, sizeof(dir), "%s", out);
	if ((slash = strrchr(dir, '/')))
		*slash = 0;
	mkdir_p(dir);
	snprintf(url, sizeof(url), "%s%s", src->url, rel);
	if (net_get(src, url, out, 0, 0, 0, err, errn)) {
		unlink(out);
		return -1;
	}
	return 0;
}
