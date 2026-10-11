/* Minimal HTTP/1.1 over tls3ds: what the Spotify endpoints actually need.
 *
 * GET and form POST, Content-Length and chunked bodies, one redirect hop. No
 * keep-alive: Spotify's token endpoint closes anyway, and a fresh connection
 * reuses the session ticket tls3ds already caches.
 */
#ifndef HTTP3DS_H
#define HTTP3DS_H

#include <stddef.h>

typedef struct {
	int    status;        /* HTTP status, or 0 if the request never completed */
	char  *body;          /* NUL-terminated; caller frees */
	size_t len;
	char   err[160];
} http_reply;

/* `body` is sent as application/x-www-form-urlencoded. Caller frees reply.body
 * on success. Returns 0 when a response was read, -1 otherwise -- note that an
 * HTTP 400 is a successful read: these endpoints report errors as JSON at 400. */
int http_post_form(const char *host, const char *path, const char *body,
                   http_reply *out);
int http_get(const char *host, const char *path, const char *bearer,
             http_reply *out);

/* Spotify's internal services speak protobuf over HTTPS, with the body and the
 * reply both binary; `out->body` is still NUL-terminated for convenience but
 * `out->len` is what counts. */
int http_post_proto(const char *host, const char *path,
                    const void *body, size_t len, http_reply *out);
int http_get_proto(const char *host, const char *path, const char *bearer,
                   http_reply *out);

/* The general form the three above are built from. `bearer`, `ctype` and
 * `accept` may be NULL. */
int http_request(const char *host, const char *path, const char *method,
                 const char *bearer, const char *ctype, const char *accept,
                 const void *body, size_t len, http_reply *out);
void http_reply_free(http_reply *r);

/* Percent-encodes everything outside the unreserved set, for form bodies. */
size_t url_encode(const char *in, char *out, size_t outlen);

#endif
