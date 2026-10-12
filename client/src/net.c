/* SPDX-License-Identifier: MIT */
/*
 * store: HTTPS downloads (docs/client.md §3) with mbedTLS 3.6: SNI,
 * certificates checked against the roots built in (roots.pem) and a test
 * source's CAFILE, up to 5 redirects (HTTPS only), resume with Range.
 * file:// URLs are copied, for tests without a network.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "store.h"

static const char roots_pem[] =
#include "roots.inc"
;

#define MAX_REDIRECTS	5
#define READ_TIMEOUT_MS	30000
#define HEAD_MAX	8192

/* 2025-01-01: a clock before this was never set (no RTC; netd sets it) */
int clock_set(void)
{
	return time(0) > 1735689600;
}

struct sink {
	int fd;				/* or -1: memory */
	char *mem;
	size_t len, cap, max;
	long long done, total;
	progress_fn progress;
	void *arg;
};

static int sink_put(struct sink *k, const unsigned char *b, size_t n)
{
	if (k->fd >= 0) {
		while (n) {
			ssize_t w = write(k->fd, b, n);

			if (w <= 0)
				return -1;
			b += w;
			n -= (size_t)w;
			k->done += w;
		}
	} else {
		if (k->len + n > k->max)
			return -2;
		if (k->len + n + 1 > k->cap) {
			k->cap = (k->len + n + 1) * 2;
			if (k->cap > k->max + 1)
				k->cap = k->max + 1;
			k->mem = xrealloc(k->mem, k->cap);
		}
		memcpy(k->mem + k->len, b, n);
		k->len += n;
		k->mem[k->len] = 0;
		k->done += (long long)n;
	}
	if (k->progress && k->progress(k->done, k->total, k->arg))
		return -3;
	return 0;
}

struct url {
	char host[256], port[8], path[1024];
};

static int parse_url(const char *u, struct url *o)
{
	const char *h, *p;
	size_t hl;

	if (strncmp(u, "https://", 8))
		return -1;
	h = u + 8;
	p = strchr(h, '/');
	hl = p ? (size_t)(p - h) : strlen(h);
	if (!hl || hl >= sizeof(o->host))
		return -1;
	snprintf(o->host, sizeof(o->host), "%.*s", (int)hl, h);
	snprintf(o->path, sizeof(o->path), "%s", p ? p : "/");
	snprintf(o->port, sizeof(o->port), "443");
	{
		char *c = strrchr(o->host, ':');

		if (c && !strchr(c, ']')) {
			snprintf(o->port, sizeof(o->port), "%s", c + 1);
			*c = 0;
		}
	}
	return 0;
}

static void tls_error(int r, const char *what, char *err, size_t errn)
{
	char b[160];

	mbedtls_strerror(r, b, sizeof(b));
	snprintf(err, errn, "%s: %s", what, b);
}

static int file_get(const char *url, struct sink *k, char *err, size_t errn)
{
	int fd = open(url + 7, O_RDONLY | O_CLOEXEC);
	unsigned char buf[16384];
	ssize_t r;

	if (fd < 0) {
		snprintf(err, errn, "%s: %s", url, strerror(errno));
		return -1;
	}
	k->total = lseek(fd, 0, SEEK_END);
	lseek(fd, k->done, SEEK_SET);
	while ((r = read(fd, buf, sizeof(buf))) > 0)
		if (sink_put(k, buf, (size_t)r)) {
			close(fd);
			snprintf(err, errn, "%s", k->fd >= 0 ? "cannot write the download" : "too large");
			return -1;
		}
	close(fd);
	return r < 0 ? -1 : 0;
}

/*
 * One request. Returns the HTTP status (body written to k for 200/206),
 * or -1 with err. *location gets a redirect target.
 */
static int request(const struct source *src, const char *u, struct sink *k, long long offset,
		   char *location, size_t locn, char *err, size_t errn)
{
	static int psa_ok;
	struct url url;
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	mbedtls_entropy_context ent;
	mbedtls_ctr_drbg_context drbg;
	char head[HEAD_MAX + 1], req[1500];
	unsigned char buf[16384];
	size_t hl = 0;
	long long clen = -1, got = 0;
	int status = -1, r;

	if (parse_url(u, &url)) {
		snprintf(err, errn, "not an https URL: %s", u);
		return -1;
	}
	if (!psa_ok && psa_crypto_init() != PSA_SUCCESS) {
		snprintf(err, errn, "crypto init failed");
		return -1;
	}
	psa_ok = 1;
	mbedtls_net_init(&net);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);
	mbedtls_entropy_init(&ent);
	mbedtls_ctr_drbg_init(&drbg);

	if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
				       (const unsigned char *)"whyos-store", 11))) {
		tls_error(r, "random", err, errn);
		goto out;
	}
	if ((r = mbedtls_x509_crt_parse(&ca, (const unsigned char *)roots_pem, sizeof(roots_pem))) < 0) {
		tls_error(r, "built-in roots", err, errn);
		goto out;
	}
	if (src && src->cafile[0] && (r = mbedtls_x509_crt_parse_file(&ca, src->cafile)) < 0) {
		tls_error(r, src->cafile, err, errn);
		goto out;
	}
	if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					     MBEDTLS_SSL_PRESET_DEFAULT))) {
		tls_error(r, "TLS setup", err, errn);
		goto out;
	}
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_read_timeout(&conf, READ_TIMEOUT_MS);
	if ((r = mbedtls_ssl_setup(&ssl, &conf)) || (r = mbedtls_ssl_set_hostname(&ssl, url.host))) {
		tls_error(r, "TLS setup", err, errn);
		goto out;
	}
	if ((r = mbedtls_net_connect(&net, url.host, url.port, MBEDTLS_NET_PROTO_TCP))) {
		snprintf(err, errn, "cannot reach %s (no network?)", url.host);
		goto out;
	}
	mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
	while ((r = mbedtls_ssl_handshake(&ssl))) {
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (mbedtls_ssl_get_verify_result(&ssl) & MBEDTLS_X509_BADCERT_FUTURE && !clock_set())
			snprintf(err, errn, "the clock is not set: connect to a network first");
		else if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			char v[200];

			mbedtls_x509_crt_verify_info(v, sizeof(v), "", mbedtls_ssl_get_verify_result(&ssl));
			v[strcspn(v, "\n")] = 0;
			snprintf(err, errn, "%s: certificate not trusted (%s)", url.host, v);
		} else
			tls_error(r, url.host, err, errn);
		goto out;
	}

	if (offset > 0)
		snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT
			 "\r\nAccept-Encoding: identity\r\nRange: bytes=%lld-\r\nConnection: close\r\n\r\n",
			 url.path, url.host, offset);
	else
		snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT
			 "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", url.path, url.host);
	for (size_t sent = 0, n = strlen(req); sent < n; ) {
		r = mbedtls_ssl_write(&ssl, (const unsigned char *)req + sent, n - sent);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0) {
			tls_error(r, "send", err, errn);
			goto out;
		}
		sent += (size_t)r;
	}

	/* the head: status line and headers, up to the empty line */
	for (;;) {
		char *end;

		r = mbedtls_ssl_read(&ssl, (unsigned char *)head + hl, HEAD_MAX - hl);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r <= 0) {
			snprintf(err, errn, "%s: connection closed before an answer", url.host);
			goto out;
		}
		hl += (size_t)r;
		head[hl] = 0;
		end = strstr(head, "\r\n\r\n");
		if (end) {
			size_t body = (size_t)(end + 4 - head);
			char *l;

			status = strncmp(head, "HTTP/1.", 7) ? -1 : atoi(head + 9);
			*end = 0;
			for (l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
				char *h = l + 2;

				if (!strncasecmp(h, "Content-Length:", 15))
					clen = atoll(h + 15);
				else if (!strncasecmp(h, "Location:", 9) && location) {
					h += 9;
					while (*h == ' ')
						h++;
					snprintf(location, locn, "%.*s", (int)strcspn(h, "\r"), h);
				} else if (!strncasecmp(h, "Content-Range:", 14)) {
					char *slash = strchr(h, '/');

					if (slash)
						k->total = atoll(slash + 1);
				}
			}
			if (status == 200 && clen >= 0)
				k->total = clen;
			if (status == 200 && offset > 0 && k->fd >= 0) {
				/* the whole file instead of the rest: start it over */
				if (ftruncate(k->fd, 0) || lseek(k->fd, 0, SEEK_SET) < 0) {
					snprintf(err, errn, "cannot restart the download");
					status = -1;
					goto out;
				}
				k->done = 0;
			}
			if (status == 200 || status == 206) {
				if (hl > body) {
					got = (long long)(hl - body);
					if ((r = sink_put(k, (unsigned char *)head + body, hl - body))) {
						snprintf(err, errn, r == -3 ? "cancelled" : r == -2 ? "too large" :
							 "cannot write the download");
						status = -1;
						goto out;
					}
				}
			}
			break;
		}
		if (hl == HEAD_MAX) {
			snprintf(err, errn, "%s: answer too long", url.host);
			goto out;
		}
	}
	if (status != 200 && status != 206)
		goto out;
	while (clen < 0 || got < clen) {
		r = mbedtls_ssl_read(&ssl, buf, sizeof(buf));
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			break;
		if (r < 0) {
			snprintf(err, errn, "%s: the connection dropped", url.host);
			status = -1;
			goto out;
		}
		got += r;
		if ((r = sink_put(k, buf, (size_t)r))) {
			snprintf(err, errn, r == -3 ? "cancelled" : r == -2 ? "too large" : "cannot write the download");
			status = -1;
			goto out;
		}
	}
	if (clen >= 0 && got < clen) {
		snprintf(err, errn, "%s: the connection dropped", url.host);
		status = -1;
	}
out:
	mbedtls_ssl_close_notify(&ssl);
	mbedtls_net_free(&net);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_x509_crt_free(&ca);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&ent);
	return status;
}

/* follow redirects; returns 0 when the body is in k */
static int fetch(const struct source *src, const char *url, struct sink *k, long long offset,
		 char *err, size_t errn)
{
	char cur[1024], loc[1024];

	if (!strncmp(url, "file://", 7)) {
		k->done = offset;
		return file_get(url, k, err, errn);
	}
	snprintf(cur, sizeof(cur), "%s", url);
	for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
		int st;

		loc[0] = 0;
		st = request(src, cur, k, offset, loc, sizeof(loc), err, errn);
		if (st == 200 || st == 206)
			return 0;
		if (st == 416 && offset > 0)	/* asked past the end: it is complete */
			return 0;
		if (st >= 300 && st < 400 && loc[0]) {
			if (loc[0] == '/') {		/* relative: same host */
				struct url u;

				parse_url(cur, &u);
				snprintf(cur, sizeof(cur), "https://%s%s%s%s", u.host,
					 strcmp(u.port, "443") ? ":" : "", strcmp(u.port, "443") ? u.port : "", loc);
			} else if (!strncmp(loc, "https://", 8)) {
				snprintf(cur, sizeof(cur), "%s", loc);
			} else {
				snprintf(err, errn, "refused a redirect away from HTTPS");
				return -1;
			}
			continue;
		}
		if (st > 0)
			snprintf(err, errn, "%s: HTTP %d", cur, st);
		return -1;
	}
	snprintf(err, errn, "too many redirects");
	return -1;
}

/*
 * Download url into path. With resume, a partial file is continued with a
 * Range request; a server that answers with the whole file starts it over.
 */
int net_get(const struct source *src, const char *url, const char *path, int resume,
	    progress_fn progress, void *arg, char *err, size_t errn)
{
	struct sink k = { .fd = -1, .progress = progress, .arg = arg, .total = -1 };
	long long have = resume ? file_size(path) : 0;
	int r;

	if (have < 0)
		have = 0;
	k.fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (have ? O_APPEND : O_TRUNC), 0644);
	if (k.fd < 0) {
		snprintf(err, errn, "%s: %s", path, strerror(errno));
		return -1;
	}
	k.done = have;
	r = fetch(src, url, &k, have, err, errn);
	if (fsync(k.fd) || close(k.fd)) {
		if (!r)
			snprintf(err, errn, "%s: %s", path, strerror(errno));
		r = -1;
	}
	return r;
}

int net_get_mem(const struct source *src, const char *url, char **data, size_t *len,
		size_t max, char *err, size_t errn)
{
	struct sink k = { .fd = -1, .max = max, .total = -1 };

	if (fetch(src, url, &k, 0, err, errn)) {
		free(k.mem);
		return -1;
	}
	if (!k.mem)
		k.mem = xstrdup("");
	*data = k.mem;
	*len = k.len;
	return 0;
}
