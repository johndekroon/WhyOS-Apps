/* SPDX-License-Identifier: MIT */
/*
 * store: where everything lives (docs/device.md §1). From the environment
 * and /etc/whyos-release (§6.3); STORE_* variables move it all elsewhere
 * for the tests:
 *   HOME, STORE_RELEASE (/etc/whyos-release), STORE_SD (overrides SD=),
 *   STORE_FLASH_APPS (/data/apps), STORE_TMP (/tmp),
 *   STORE_SYS_SOURCES (/etc/whyos/sources.d)
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "store.h"

struct paths P;

static const char *env_or(const char *name, const char *def)
{
	const char *v = getenv(name);

	return v && *v ? v : def;
}

/* KEY=value or KEY="value" */
static int conf_line(const char *line, const char *key, char *out, size_t size)
{
	size_t n = strlen(key), len;
	const char *v;

	if (strncmp(line, key, n) || line[n] != '=')
		return 0;
	v = line + n + 1;
	len = strcspn(v, "\n");
	if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
		v++;
		len -= 2;
	}
	snprintf(out, size, "%.*s", (int)len, v);
	return 1;
}

void paths_init(void)
{
	char *rel = 0, sd[200] = "/mnt/sd", api[16] = "1";

	memset(&P, 0, sizeof(P));
	snprintf(P.home, sizeof(P.home), "%s", env_or("HOME", "/root"));
	if (!read_file(env_or("STORE_RELEASE", "/etc/whyos-release"), &rel, 0, 16384)) {
		for (char *l = rel; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
			conf_line(l, "SD", sd, sizeof(sd));
			conf_line(l, "API", api, sizeof(api));
			conf_line(l, "FEATURES", P.features, sizeof(P.features));
			conf_line(l, "VERSION", P.os_version, sizeof(P.os_version));
		}
		free(rel);
	}
	P.api = atoi(api);
	snprintf(P.sd, sizeof(P.sd), "%s", env_or("STORE_SD", sd));
	snprintf(P.flash_apps, sizeof(P.flash_apps), "%s", env_or("STORE_FLASH_APPS", "/data/apps"));
	snprintf(P.tmp, sizeof(P.tmp), "%s", env_or("STORE_TMP", "/tmp"));
	snprintf(P.sys_sources, sizeof(P.sys_sources), "%s",
		 env_or("STORE_SYS_SOURCES", "/etc/whyos/sources.d"));
	/* a card counts when its mount point can be written: without a card
	 * /mnt/sd is a directory of the read-only rootfs, and a card mounted
	 * read-only cannot take an install either */
	P.sd_present = is_dir(P.sd) && access(P.sd, W_OK) == 0;

	snprintf(P.sd_apps, sizeof(P.sd_apps), "%s/badge/apps", P.sd);
	snprintf(P.sd_appdata, sizeof(P.sd_appdata), "%s/badge/appdata", P.sd);
	snprintf(P.store_dir, sizeof(P.store_dir), "%s/badge/.store", P.sd);
	if (P.sd_present)
		snprintf(P.cache_dir, sizeof(P.cache_dir), "%s/cache", P.store_dir);
	else	/* no card: the index is cached in RAM until there is one */
		snprintf(P.cache_dir, sizeof(P.cache_dir), "%s/store-cache", P.tmp);
	snprintf(P.dl_dir, sizeof(P.dl_dir), "%s/download", P.store_dir);
	snprintf(P.whyos, sizeof(P.whyos), "%s/.whyos", P.home);
	snprintf(P.config_dir, sizeof(P.config_dir), "%s/config", P.whyos);
	snprintf(P.bin_dir, sizeof(P.bin_dir), "%s/bin", P.whyos);
	snprintf(P.registry_dir, sizeof(P.registry_dir), "%s/store/installed", P.whyos);
	snprintf(P.user_sources, sizeof(P.user_sources), "%s/store/sources.d", P.whyos);
}

int has_feature(const char *f)
{
	size_t n = strlen(f);

	for (const char *p = P.features; (p = strstr(p, f)); p += n)
		if ((p == P.features || p[-1] == ' ') && (p[n] == ' ' || !p[n]))
			return 1;
	return 0;
}

char *app_dir(char *out, size_t n, const char *id, const char *location)
{
	return path_join(out, n, location && !strcmp(location, "flash") ? P.flash_apps : P.sd_apps, id);
}

char *appdata_dir(char *out, size_t n, const char *id)
{
	return path_join(out, n, P.sd_appdata, id);
}

char *config_dir(char *out, size_t n, const char *id)
{
	return path_join(out, n, P.config_dir, id);
}

char *cache_dir_of(char *out, size_t n, const char *id)
{
	if (P.sd_present)
		snprintf(out, n, "%s/%s/.cache", P.sd_appdata, id);
	else
		snprintf(out, n, "%s/cache/%s", P.tmp, id);
	return out;
}
