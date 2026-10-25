/* SPDX-License-Identifier: MIT */
/* Files: the installed apps that open files (apps.h). */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "apps.h"
#include "browse.h"

const char *const apps_roots[3] = { "/mnt/sd/badge/apps", "/data/apps", "/usr/share/badge/apps" };

/* a value after KEY=, without its quotes */
static void conf_value(char *dst, int max, const char *v)
{
	int n = 0;

	if (*v == '"' || *v == '\'')
		v++;
	while (*v && *v != '"' && *v != '\'' && *v != '\n' && n < max - 1)
		dst[n++] = *v++;
	dst[n] = 0;
}

static int app_parse(struct fapp *ap, const char *dir)
{
	char path[160], buf[2048], exec[48] = "";
	int fd, n;

	memset(ap, 0, sizeof(*ap));
	if (strlen(dir) >= sizeof(ap->dir))
		return -1;
	memcpy(ap->dir, dir, strlen(dir) + 1);
	snprintf(path, sizeof(path), "%s/app.conf", dir);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	for (const char *l = buf; l && *l; ) {
		const char *nl = strchr(l, '\n');

		if (!strncmp(l, "NAME=", 5))
			conf_value(ap->name, sizeof(ap->name), l + 5);
		else if (!strncmp(l, "EXEC=", 5))
			conf_value(exec, sizeof(exec), l + 5);
		else if (!strncmp(l, "ID=", 3))
			conf_value(ap->id, sizeof(ap->id), l + 3);
		else if (!strncmp(l, "OPENS=", 6))
			conf_value(ap->opens, sizeof(ap->opens), l + 6);
		else if (!strncmp(l, "OPENS_ALSO=", 11))
			conf_value(ap->opens_also, sizeof(ap->opens_also), l + 11);
		l = nl ? nl + 1 : 0;
	}
	if (!ap->id[0]) {
		const char *b = strrchr(ap->dir, '/');

		strncpy(ap->id, b ? b + 1 : ap->dir, sizeof(ap->id) - 1);
	}
	if (!ap->name[0] || !exec[0])
		return -1;
	if (exec[0] == '/')
		snprintf(path, sizeof(path), "%s", exec);
	else
		snprintf(path, sizeof(path), "%s/%s", ap->dir, exec);
	return access(path, X_OK) == 0 ? 0 : -1;
}

void apps_read(struct apps *s, const char *const *roots, int n_roots)
{
	s->n = 0;
	for (int r = 0; r < n_roots; r++) {
		DIR *d = opendir(roots[r]);
		struct dirent *e;

		while (d && (e = readdir(d)) && s->n < APPS_MAX) {
			char dir[sizeof(s->a[0].dir)];
			int i;

			if (e->d_name[0] == '.' ||
			    snprintf(dir, sizeof(dir), "%s/%s", roots[r], e->d_name) >= (int)sizeof(dir))
				continue;
			if (app_parse(&s->a[s->n], dir) < 0)
				continue;
			for (i = s->n; i > 0 && strcasecmp(s->a[i - 1].name, s->a[s->n].name) > 0; i--)
				;
			if (i < s->n) {
				struct fapp t = s->a[s->n];

				memmove(&s->a[i + 1], &s->a[i], sizeof(s->a[0]) * (size_t)(s->n - i));
				s->a[i] = t;
			}
			s->n++;
		}
		if (d)
			closedir(d);
	}
}

int apps_for(const struct apps *s, const char *ext, int all, int *out, int max)
{
	int n = 0;

	for (int i = 0; i < s->n && n < max; i++)
		if (ext_in(s->a[i].opens, ext) || (all && ext_in(s->a[i].opens_also, ext)))
			out[n++] = i;
	return n;
}

int apps_for_text(const struct apps *s)
{
	for (int i = 0; i < s->n; i++)
		if (ext_in(s->a[i].opens, "txt") && !ext_in(s->a[i].opens, "*"))
			return i;
	return -1;
}
