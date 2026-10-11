#include "http3ds.h"
#include "tls3ds.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECV_CHUNK  4096
/* Enough for any API reply. Audio passes its own, far larger limit. */
#define BODY_MAX    (512 * 1024)

/* newlib hides strcasestr behind _GNU_SOURCE; header names are few and short,
 * so a plain scan is cheaper than reasoning about feature macros. */
static bool header_present(const char *headers, const char *needle)
{
	size_t n = strlen(needle);
	for (; *headers; headers++) {
		size_t i = 0;
		while (i < n && headers[i] &&
		       tolower((unsigned char)headers[i]) == needle[i])
			i++;
		if (i == n)
			return true;
	}
	return false;
}

size_t url_encode(const char *in, char *out, size_t outlen)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t n = 0;
	for (; *in; in++) {
		unsigned char c = (unsigned char)*in;
		bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		            c == '.' || c == '~';
		if (safe) {
			if (n + 2 > outlen)
				break;
			out[n++] = (char)c;
		} else {
			if (n + 4 > outlen)
				break;
			out[n++] = '%';
			out[n++] = HEX[c >> 4];
			out[n++] = HEX[c & 15];
		}
	}
	out[n < outlen ? n : outlen - 1] = '\0';
	return n;
}

void http_reply_free(http_reply *r)
{
	if (!r)
		return;
	free(r->body);
	r->body = NULL;
	r->len = 0;
}

/* Reads the whole response into one buffer. These responses are small; the
 * audio path later streams instead of doing this.
 *
 * Reads land in RECV_CHUNK blocks rather than byte by byte: on this console the
 * cost is the number of syscalls, not the number of bytes. */
static char *slurp(tls_conn *c, size_t *len, size_t limit, char *err, size_t errlen)
{
	size_t cap = 8192, have = 0;
	char *buf = malloc(cap);
	if (!buf) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	for (;;) {
		if (have + RECV_CHUNK + 1 > cap) {
			if (cap >= limit) {
				snprintf(err, errlen, "response over %u bytes",
				         (unsigned)limit);
				free(buf);
				return NULL;
			}
			size_t want = cap * 2;
			char *bigger = realloc(buf, want);
			if (!bigger) {
				snprintf(err, errlen, "out of memory at %u bytes", (unsigned)want);
				free(buf);
				return NULL;
			}
			buf = bigger;
			cap = want;
		}
		int n = tls_read(c, buf + have, RECV_CHUNK);
		if (n == 0)
			break;                 /* the server closed, which is the signal */
		if (n < 0) {
			char reason[128];
			tls_strerror(n, reason, sizeof(reason));
			snprintf(err, errlen, "read: %s", reason);
			free(buf);
			return NULL;
		}
		have += n;
	}
	buf[have] = '\0';
	*len = have;
	return buf;
}

/* Rewrites a chunked body in place; returns the decoded length. */
static size_t dechunk(char *p, size_t len)
{
	char *out = p, *in = p;
	const char *end = p + len;
	while (in < end) {
		char *nl = memchr(in, '\n', end - in);
		if (!nl)
			break;
		unsigned long size = strtoul(in, NULL, 16);
		in = nl + 1;
		if (size == 0 || in + size > end)
			break;
		memmove(out, in, size);
		out += size;
		in += size;
		/* Step over the CRLF that closes the chunk. */
		while (in < end && (*in == '\r' || *in == '\n'))
			in++;
	}
	*out = '\0';
	return (size_t)(out - p);
}

static int finish(tls_conn *c, size_t limit, http_reply *out)
{
	size_t total = 0;
	char *raw = slurp(c, &total, limit, out->err, sizeof(out->err));
	tls_close(c);
	if (!raw)
		return -1;

	if (sscanf(raw, "HTTP/1.%*d %d", &out->status) != 1) {
		snprintf(out->err, sizeof(out->err), "no status line");
		free(raw);
		return -1;
	}

	char *sep = strstr(raw, "\r\n\r\n");
	if (!sep) {
		snprintf(out->err, sizeof(out->err), "headers never ended");
		free(raw);
		return -1;
	}
	*sep = '\0';
	char *body = sep + 4;
	size_t blen = total - (size_t)(body - raw);

	/* Compared case-insensitively: edges title-case header names differently. */
	bool chunked = header_present(raw, "transfer-encoding: chunked");
	if (chunked)
		blen = dechunk(body, blen);

	/* Hand back just the body, so callers never have to think about headers. */
	char *only = malloc(blen + 1);
	if (!only) {
		snprintf(out->err, sizeof(out->err), "out of memory");
		free(raw);
		return -1;
	}
	memcpy(only, body, blen);
	only[blen] = '\0';
	free(raw);

	out->body = only;
	out->len = blen;
	return 0;
}

/* Set for the next request only; the CDN is the only caller that needs it. */
static size_t pending_range;

int http_request(const char *host, const char *path, const char *method,
                 const char *bearer, const char *ctype, const char *accept,
                 const void *body, size_t len, size_t limit, http_reply *out)
{
	memset(out, 0, sizeof(*out));
	if (limit == 0)
		limit = BODY_MAX;
	tls_conn *c = tls_connect(host, 443, 20000, NULL, out->err, sizeof(out->err));
	if (!c)
		return -1;

	/* A bearer token is long -- login5's runs past 400 characters -- so the
	 * header buffer is sized for it rather than for a typical request. */
	size_t headcap = 1024 + (bearer ? strlen(bearer) : 0) + strlen(path);
	char *head = malloc(headcap);
	if (!head) {
		snprintf(out->err, sizeof(out->err), "out of memory");
		tls_close(c);
		return -1;
	}

	/* The range goes into its own buffer rather than being spliced in with
	 * ternaries: a %u conversion cannot be skipped the way a %s can, so with
	 * no range set it still printed a stray "0" into the headers and
	 * malformed every single request. */
	char range[48] = "";
	if (pending_range)
		snprintf(range, sizeof(range), "Range: bytes=0-%u\r\n",
		         (unsigned)(pending_range - 1));
	pending_range = 0;

	int n = snprintf(head, headcap,
	                 "%s %s HTTP/1.1\r\nHost: %s\r\n"
	                 "User-Agent: 3ds-spotify/0.1\r\n"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "%s%s%s"
	                 "%s"
	                 "Content-Length: %u\r\nConnection: close\r\n\r\n",
	                 method, path, host,
	                 bearer ? "Authorization: Bearer " : "", bearer ? bearer : "",
	                 bearer ? "\r\n" : "",
	                 ctype ? "Content-Type: " : "", ctype ? ctype : "",
	                 ctype ? "\r\n" : "",
	                 accept ? "Accept: " : "", accept ? accept : "",
	                 accept ? "\r\n" : "",
	                 range,
	                 (unsigned)len);
	if (n < 0 || (size_t)n >= headcap) {
		snprintf(out->err, sizeof(out->err), "request headers too long");
		free(head);
		tls_close(c);
		return -1;
	}

	bool sent = tls_write(c, head, n) == n &&
	            (len == 0 || tls_write(c, body, len) == (int)len);
	free(head);
	if (!sent) {
		snprintf(out->err, sizeof(out->err), "write failed");
		tls_close(c);
		return -1;
	}
	return finish(c, limit, out);
}

int http_post_form(const char *host, const char *path, const char *body,
                   http_reply *out)
{
	return http_request(host, path, "POST", NULL,
	                    "application/x-www-form-urlencoded", NULL,
	                    body, strlen(body), 0, out);
}

int http_get(const char *host, const char *path, const char *bearer,
             http_reply *out)
{
	return http_request(host, path, "GET", bearer, NULL, NULL, NULL, 0, 0, out);
}

int http_post_proto(const char *host, const char *path,
                    const void *body, size_t len, http_reply *out)
{
	return http_request(host, path, "POST", NULL, "application/x-protobuf",
	                    "application/x-protobuf", body, len, 0, out);
}

int http_get_proto(const char *host, const char *path, const char *bearer,
                   http_reply *out)
{
	return http_request(host, path, "GET", bearer, NULL,
	                    "application/x-protobuf", NULL, 0, 0, out);
}

int http_get_url(const char *url, size_t limit, http_reply *out)
{
	memset(out, 0, sizeof(*out));
	if (strncmp(url, "https://", 8) != 0) {
		snprintf(out->err, sizeof(out->err), "not an https url");
		return -1;
	}

	const char *p = url + 8;
	const char *slash = strchr(p, '/');
	if (!slash) {
		snprintf(out->err, sizeof(out->err), "url has no path");
		return -1;
	}
	char host[160];
	size_t hlen = (size_t)(slash - p);
	if (hlen >= sizeof(host)) {
		snprintf(out->err, sizeof(out->err), "host too long");
		return -1;
	}
	memcpy(host, p, hlen);
	host[hlen] = '\0';

	/* The path keeps its query exactly as given: the CDN signature covers it. */
	return http_request(host, slash, "GET", NULL, NULL, NULL, NULL, 0,
	                    limit, out);
}

int http_get_url_range(const char *url, size_t bytes, size_t limit,
                       http_reply *out)
{
	pending_range = bytes;
	int rc = http_get_url(url, limit, out);
	pending_range = 0;
	return rc;
}
