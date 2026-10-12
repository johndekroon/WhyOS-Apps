/* SPDX-License-Identifier: MIT */
/*
 * store: sources (docs/device.md §2.3). The default source is built in;
 * *.conf files in /etc/whyos/sources.d and then $HOME/.whyos/store/sources.d
 * add sources or override one (a mirror), as KEY=value lines:
 *   ID=whyos  URL=https://…/v1/  ENABLED=1  CAFILE=/path/ca.pem (test sources)
 */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "store.h"

#define DEFAULT_ID	"whyos"
#define DEFAULT_URL	"https://raw.githubusercontent.com/johndekroon/WhyOS-Apps/main/dist/v1/"

struct source S[MAX_SOURCES];
int n_sources;

struct source *source_find(const char *id)
{
	for (int i = 0; i < n_sources; i++)
		if (!strcmp(S[i].id, id))
			return &S[i];
	return 0;
}

static void value(const char *v, size_t len, char *out, size_t size)
{
	if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
		v++;
		len -= 2;
	}
	snprintf(out, size, "%.*s", (int)len, v);
}

static void read_conf(const char *path)
{
	char *data, id[33] = "", url[256] = "", ca[256] = "", en[8] = "";
	int have_url = 0, have_ca = 0, have_en = 0;
	struct source *s;

	if (read_file(path, &data, 0, 8192))
		return;
	for (char *l = data; *l; ) {
		size_t len = strcspn(l, "\n");
		char *eq = memchr(l, '=', len);

		if (eq && *l != '#') {
			size_t kl = (size_t)(eq - l), vl = len - kl - 1;

			if (kl == 2 && !strncmp(l, "ID", 2))
				value(eq + 1, vl, id, sizeof(id));
			else if (kl == 3 && !strncmp(l, "URL", 3))
				value(eq + 1, vl, url, sizeof(url)), have_url = 1;
			else if (kl == 6 && !strncmp(l, "CAFILE", 6))
				value(eq + 1, vl, ca, sizeof(ca)), have_ca = 1;
			else if (kl == 7 && !strncmp(l, "ENABLED", 7))
				value(eq + 1, vl, en, sizeof(en)), have_en = 1;
		}
		l += len + (l[len] == '\n');
	}
	free(data);
	if (!id[0])
		return;
	s = source_find(id);
	if (!s) {
		if (n_sources >= MAX_SOURCES || !have_url)
			return;
		s = &S[n_sources++];
		memset(s, 0, sizeof(*s));
		snprintf(s->id, sizeof(s->id), "%s", id);
		s->enabled = 1;
	}
	if (have_url) {
		size_t n = strlen(url);

		snprintf(s->url, sizeof(s->url), "%s%s", url, n && url[n - 1] == '/' ? "" : "/");
	}
	if (have_ca)
		snprintf(s->cafile, sizeof(s->cafile), "%s", ca);
	if (have_en)
		s->enabled = atoi(en) != 0;
}

static void read_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char names[32][64];
	int n = 0;

	if (!d)
		return;
	while ((e = readdir(d)) && n < 32) {
		size_t l = strlen(e->d_name);

		if (l > 5 && l < 64 && !strcmp(e->d_name + l - 5, ".conf"))
			snprintf(names[n++], 64, "%s", e->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof(names[0]), (int (*)(const void *, const void *))strcmp);
	for (int i = 0; i < n; i++) {
		char p[512];

		read_conf(path_join(p, sizeof(p), dir, names[i]));
	}
}

void sources_load(void)
{
	n_sources = 1;
	memset(&S[0], 0, sizeof(S[0]));
	snprintf(S[0].id, sizeof(S[0].id), DEFAULT_ID);
	snprintf(S[0].url, sizeof(S[0].url), DEFAULT_URL);
	S[0].enabled = 1;
	read_dir(P.sys_sources);
	read_dir(P.user_sources);
}
