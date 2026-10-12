/* SPDX-License-Identifier: MIT */
/*
 * store: reading a package (docs/package.md §1): a gzip'd ustar archive,
 * streamed through zlib's inflate and checked entry by entry before any
 * byte is written:
 *   - manifest.json first, for the id and build the index promised;
 *   - only regular files and directories (no links, devices, GNU or pax
 *     entries); paths under files/ by the FAT rules (§1.2), no two that
 *     differ only in case, at most 4096 entries and 256 MB;
 *   - every file's SHA-256 as in the manifest, every listed file present.
 * Anything outside manifest.json and files/ is reserved and skipped.
 * With dest NULL nothing is written: the check CI and the tests use.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>
#include <zlib.h>

#include "store.h"

int name_ok(const char *rel, char *err, size_t errn)
{
	const char *p = rel;

	if (!*rel || *rel == '/') {
		snprintf(err, errn, "%s: absolute or empty path", rel);
		return 0;
	}
	if (strlen(rel) + 6 > MAX_PATH_LEN) {
		snprintf(err, errn, "%s: path longer than %d bytes", rel, MAX_PATH_LEN);
		return 0;
	}
	while (*p) {
		size_t n = strcspn(p, "/");

		if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) {
			snprintf(err, errn, "%s: empty, '.' or '..' component", rel);
			return 0;
		}
		if (n > MAX_COMPONENT) {
			snprintf(err, errn, "%s: component longer than %d bytes", rel, MAX_COMPONENT);
			return 0;
		}
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)p[i];

			if (c < 0x20 || c == 0x7f || strchr("\"*:<>?\\|", c)) {
				snprintf(err, errn, "%s: a character FAT cannot store", rel);
				return 0;
			}
		}
		if (p[n - 1] == '.' || p[n - 1] == ' ') {
			snprintf(err, errn, "%s: trailing dot or space", rel);
			return 0;
		}
		p += n + (p[n] == '/');
	}
	if (strcspn(rel, "/") == 8 && (!strncmp(rel, "app.conf", 8) || !strncmp(rel, ".package", 8))) {
		snprintf(err, errn, "%s: written by the store, not allowed in a package", rel);
		return 0;
	}
	return 1;
}

int manifest_files(const struct json *j, struct manifest_files *mf)
{
	int map = json_path(j, 0, "files.sha256");

	memset(mf, 0, sizeof(*mf));
	if (!json_is_obj(j, map))
		return -1;
	mf->n = json_len(j, map);
	if (mf->n > MAX_ENTRIES)
		return -1;
	mf->path = xmalloc((size_t)mf->n * sizeof(char *));
	mf->sha = xmalloc((size_t)mf->n * sizeof(*mf->sha));
	mf->seen = xmalloc((size_t)mf->n);
	memset(mf->seen, 0, (size_t)mf->n);
	for (int i = 0, t = map + 1; i < mf->n; i++) {
		mf->path[i] = json_strdup(j, t);
		if (!mf->path[i] || json_str(j, t + 1, mf->sha[i], sizeof(mf->sha[i])) ||
		    strlen(mf->sha[i]) != 64) {
			mf->n = i + (mf->path[i] != 0);
			return -1;
		}
		t = json_skip(j, t + 1);
	}
	return 0;
}

void manifest_files_free(struct manifest_files *mf)
{
	for (int i = 0; i < mf->n; i++)
		free(mf->path[i]);
	free(mf->path);
	free(mf->sha);
	free(mf->seen);
	memset(mf, 0, sizeof(*mf));
}

static int mf_find(const struct manifest_files *mf, const char *rel)
{
	for (int i = 0; i < mf->n; i++)
		if (!strcmp(mf->path[i], rel))
			return i;
	return -1;
}

struct tarst {
	unsigned char hdr[512];
	size_t hlen;
	long long left, pad;		/* bytes of the current entry's data, and padding */
	int kind;			/* 0 header, 1 manifest, 2 file, 3 skip */
	int entries, zero_blocks, done;
	long long total;
	char rel[256];
	char *man;
	size_t man_len;
	int out;
	mbedtls_sha256_context sha;
	int mi;				/* index of the file in the manifest */
	struct json mj;
	int mj_ok;
	struct manifest_files mf;
	char **lower;			/* lowercased names, for case collisions */
	int nlower;
	const char *dest, *want_id;
	int want_build;
	char *err;
	size_t errn;
};

static long long octal(const unsigned char *p, int n)
{
	long long v = 0;

	for (int i = 0; i < n && p[i]; i++) {
		if (p[i] == ' ')
			continue;
		if (p[i] < '0' || p[i] > '7')
			return -1;
		v = v * 8 + (p[i] - '0');
	}
	return v;
}

static int fail(struct tarst *t, const char *fmt, const char *arg)
{
	snprintf(t->err, t->errn, fmt, arg);
	return -1;
}

static int case_unique(struct tarst *t, const char *rel)
{
	char *low = xstrdup(rel);

	for (char *p = low; *p; p++)
		if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p + 32);
	for (int i = 0; i < t->nlower; i++)
		if (!strcmp(t->lower[i], low)) {
			free(low);
			return 0;
		}
	t->lower = xrealloc(t->lower, (size_t)(t->nlower + 1) * sizeof(char *));
	t->lower[t->nlower++] = low;
	return 1;
}

static int check_manifest(struct tarst *t)
{
	char id[40] = "";
	int f, build;

	if (json_parse(&t->mj, t->man, t->man_len) || !json_is_obj(&t->mj, 0))
		return fail(t, "%s", "manifest.json is not valid JSON");
	t->mj_ok = 1;
	f = (int)json_int(&t->mj, json_get(&t->mj, 0, "format"), 0);
	if (f < 1 || f > STORE_FORMAT)
		return fail(t, "%s", "the package's format is newer than this store: update the store");
	json_str(&t->mj, json_get(&t->mj, 0, "id"), id, sizeof(id));
	build = (int)json_int(&t->mj, json_get(&t->mj, 0, "build"), -1);
	if (t->want_id && strcmp(id, t->want_id))
		return fail(t, "the package is %s, not what the index says", id);
	if (t->want_build > 0 && build != t->want_build)
		return fail(t, "%s", "the package's build is not what the index says");
	if (manifest_files(&t->mj, &t->mf))
		return fail(t, "%s", "manifest.json has no valid files.sha256");
	return 0;
}

static int header(struct tarst *t)
{
	const unsigned char *h = t->hdr;
	char name[256], *rel;
	unsigned sum = 0;
	long long size, want;
	char type = (char)h[156];

	for (int i = 0; i < 512; i++)
		sum += i >= 148 && i < 156 ? ' ' : h[i];
	if ((want = octal(h + 148, 8)) < 0 || (unsigned)want != sum)
		return fail(t, "%s", "a damaged archive header");
	size = octal(h + 124, 12);
	if (size < 0)
		return fail(t, "%s", "a damaged archive header");
	if (h[345])
		snprintf(name, sizeof(name), "%.155s/%.100s", (const char *)h + 345, (const char *)h);
	else
		snprintf(name, sizeof(name), "%.100s", (const char *)h);
	if (++t->entries > MAX_ENTRIES + 2)
		return fail(t, "more than %s entries", "4096");
	t->left = size;
	t->pad = (512 - size % 512) % 512;
	if (type != '0' && type != 0 && type != '5')
		return fail(t, "%s: not a regular file or directory", name);
	{
		size_t n = strlen(name);

		while (n && name[n - 1] == '/')
			name[--n] = 0;
	}
	if (t->entries == 1) {
		if (strcmp(name, "manifest.json") || type == '5' || size > MAX_MANIFEST)
			return fail(t, "%s", "manifest.json is not the first entry");
		t->kind = 1;
		t->man = xmalloc((size_t)size + 1);
		t->man_len = 0;
		if (size == 0)
			return fail(t, "%s", "manifest.json is empty");
		return 0;
	}
	if (!strcmp(name, "files") && type == '5') {
		t->kind = 3;
		return 0;
	}
	if (strncmp(name, "files/", 6)) {
		/* reserved for later formats: skipped, but never a way out */
		if (name[0] == '/' || strstr(name, "..") == name || strstr(name, "/../") ||
		    (strlen(name) >= 3 && !strcmp(name + strlen(name) - 3, "/..")))
			return fail(t, "%s: absolute path or '..'", name);
		t->kind = 3;
		return 0;
	}
	rel = name + 6;
	if (!name_ok(rel, t->err, t->errn))
		return -1;
	if (!case_unique(t, rel))
		return fail(t, "%s: differs from another name only in case", rel);
	snprintf(t->rel, sizeof(t->rel), "%s", rel);
	if (type == '5') {
		t->kind = 3;
		if (t->dest) {
			char p[512];

			if (mkdir_p(path_join(p, sizeof(p), t->dest, rel)))
				return fail(t, "cannot create %s", p);
		}
		return 0;
	}
	t->total += size;
	if (t->total > MAX_UNPACKED)
		return fail(t, "%s", "more than 256 MB unpacked");
	t->mi = mf_find(&t->mf, rel);
	if (t->mi < 0)
		return fail(t, "%s: not in the manifest's files.sha256", rel);
	t->kind = 2;
	mbedtls_sha256_init(&t->sha);
	mbedtls_sha256_starts(&t->sha, 0);
	t->out = -1;
	if (t->dest) {
		char p[512], *slash;

		path_join(p, sizeof(p), t->dest, rel);
		slash = strrchr(p, '/');
		*slash = 0;
		if (mkdir_p(p))
			return fail(t, "cannot create %s", p);
		*slash = '/';
		t->out = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (t->out < 0)
			return fail(t, "cannot write %s", p);
	}
	return 0;
}

static int end_file(struct tarst *t)
{
	unsigned char d[32];
	char hex[65];

	mbedtls_sha256_finish(&t->sha, d);
	mbedtls_sha256_free(&t->sha);
	sha256_hex(d, hex);
	if (t->out >= 0 && close(t->out))
		return fail(t, "cannot write %s (card full?)", t->rel);
	t->out = -1;
	if (strcmp(hex, t->mf.sha[t->mi]))
		return fail(t, "%s: SHA-256 differs from the manifest", t->rel);
	t->mf.seen[t->mi] = 1;
	return 0;
}

/* feed inflated bytes into the tar reader */
static int feed(struct tarst *t, const unsigned char *b, size_t n)
{
	while (n && !t->done) {
		if (t->kind == 0) {
			size_t c = 512 - t->hlen < n ? 512 - t->hlen : n;
			int zero = 1;

			memcpy(t->hdr + t->hlen, b, c);
			t->hlen += c;
			b += c;
			n -= c;
			if (t->hlen < 512)
				continue;
			t->hlen = 0;
			for (int i = 0; i < 512 && zero; i++)
				zero = !t->hdr[i];
			if (zero) {
				if (++t->zero_blocks == 2)
					t->done = 1;
				continue;
			}
			if (t->zero_blocks)
				return fail(t, "%s", "a damaged archive (data after its end)");
			if (header(t))
				return -1;
			if (t->left == 0 && t->kind != 1) {
				if (t->kind == 2 && end_file(t))
					return -1;
				t->kind = t->pad ? 4 : 0;
			}
			continue;
		}
		if (t->kind == 4) {		/* padding after the data */
			size_t c = (size_t)t->pad < n ? (size_t)t->pad : n;

			t->pad -= (long long)c;
			b += c;
			n -= c;
			if (!t->pad)
				t->kind = 0;
			continue;
		}
		{
			size_t c = (size_t)t->left < n ? (size_t)t->left : n;

			if (t->kind == 1) {
				memcpy(t->man + t->man_len, b, c);
				t->man_len += c;
			} else if (t->kind == 2) {
				mbedtls_sha256_update(&t->sha, b, c);
				if (t->out >= 0 && write(t->out, b, c) != (ssize_t)c)
					return fail(t, "cannot write %s (card full?)", t->rel);
			}
			t->left -= (long long)c;
			b += c;
			n -= c;
			if (t->left)
				continue;
			if (t->kind == 1) {
				t->man[t->man_len] = 0;
				if (check_manifest(t))
					return -1;
			} else if (t->kind == 2 && end_file(t)) {
				return -1;
			}
			t->kind = t->pad ? 4 : 0;
		}
	}
	return 0;
}

/*
 * Check and unpack pkg into dest (created; NULL: check only). want_id and
 * want_build (NULL, 0: any) are what the index promised. *manifest gets
 * manifest.json's text. 0, or -1 with err.
 */
int wpk_unpack(const char *pkg, const char *dest, const char *want_id, int want_build,
	       char **manifest, char *err, size_t errn)
{
	struct tarst t;
	z_stream z;
	unsigned char in[16384], out[32768];
	int fd, zr = Z_OK, r = 0;
	ssize_t got;

	memset(&t, 0, sizeof(t));
	t.out = -1;
	t.dest = dest;
	t.want_id = want_id;
	t.want_build = want_build;
	t.err = err;
	t.errn = errn;
	if (manifest)
		*manifest = 0;
	fd = open(pkg, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errn, "%s: %s", pkg, strerror(errno));
		return -1;
	}
	if (dest && mkdir_p(dest)) {
		snprintf(err, errn, "cannot create %s", dest);
		close(fd);
		return -1;
	}
	memset(&z, 0, sizeof(z));
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {
		close(fd);
		snprintf(err, errn, "inflate failed to start");
		return -1;
	}
	while (!r && zr != Z_STREAM_END && (got = read(fd, in, sizeof(in))) > 0) {
		z.next_in = in;
		z.avail_in = (unsigned)got;
		while (!r && z.avail_in && zr != Z_STREAM_END) {
			z.next_out = out;
			z.avail_out = sizeof(out);
			zr = inflate(&z, Z_NO_FLUSH);
			if (zr != Z_OK && zr != Z_STREAM_END) {
				snprintf(err, errn, "not a valid package (gzip: %s)", z.msg ? z.msg : "error");
				r = -1;
				break;
			}
			r = feed(&t, out, sizeof(out) - z.avail_out);
		}
	}
	inflateEnd(&z);
	close(fd);
	if (!r && zr != Z_STREAM_END) {
		snprintf(err, errn, "the package is cut short");
		r = -1;
	}
	if (!r && (!t.mj_ok || t.kind != 0)) {
		snprintf(err, errn, t.mj_ok ? "the package is cut short" : "manifest.json is not the first entry");
		r = -1;
	}
	for (int i = 0; !r && i < t.mf.n; i++)
		if (!t.mf.seen[i]) {
			snprintf(err, errn, "%s: in the manifest but not in the package", t.mf.path[i]);
			r = -1;
		}
	if (t.out >= 0)
		close(t.out);
	if (t.kind == 2)
		mbedtls_sha256_free(&t.sha);
	if (!r && manifest)
		*manifest = t.man;
	else
		free(t.man);
	if (t.mj_ok)
		json_free(&t.mj);
	manifest_files_free(&t.mf);
	for (int i = 0; i < t.nlower; i++)
		free(t.lower[i]);
	free(t.lower);
	return r;
}
