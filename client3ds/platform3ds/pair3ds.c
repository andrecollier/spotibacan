#include "pair3ds.h"
#include "http3ds.h"
#include "json3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ACCOUNTS_HOST "accounts.spotify.com"
#define DEVICE_PATH   "/oauth2/device/authorize"
#define TOKEN_PATH    "/api/token"

/* Both endpoints answer with JSON even when they fail, and the failure is an
 * HTTP 400 carrying {"error": "..."} -- so a 400 is a reply to be read, not a
 * transport problem. */
static void describe(const http_reply *r, char *err, size_t errlen)
{
	char code[64] = {0}, desc[160] = {0};
	if (r->body && json_str(r->body, "error", code, sizeof(code))) {
		json_str(r->body, "error_description", desc, sizeof(desc));
		if (desc[0])
			snprintf(err, errlen, "%s: %s", code, desc);
		else
			snprintf(err, errlen, "%s", code);
	} else {
		snprintf(err, errlen, "HTTP %d with no error field", r->status);
	}
}

int pair_begin(pair_request *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	char scope[512];
	url_encode(SPOTIFY_SCOPES, scope, sizeof(scope));
	char body[768];
	snprintf(body, sizeof(body), "client_id=%s&scope=%s",
	         SPOTIFY_CLIENT_ID, scope);

	http_reply r;
	if (http_post_form(ACCOUNTS_HOST, DEVICE_PATH, body, &r) != 0) {
		snprintf(err, errlen, "%s", r.err);
		return -1;
	}
	if (r.status != 200) {
		describe(&r, err, errlen);
		http_reply_free(&r);
		return -1;
	}

	bool ok = json_str(r.body, "user_code", out->user_code, sizeof(out->user_code)) &&
	          json_str(r.body, "device_code", out->device_code, sizeof(out->device_code)) &&
	          json_str(r.body, "verification_uri", out->verification_uri,
	                   sizeof(out->verification_uri));
	/* Optional, and only a convenience for the user. */
	json_str(r.body, "verification_uri_complete", out->verification_uri_complete,
	         sizeof(out->verification_uri_complete));

	long n;
	out->interval_s = json_int(r.body, "interval", &n) ? (int)n : 5;
	out->expires_in_s = json_int(r.body, "expires_in", &n) ? (int)n : 3599;
	http_reply_free(&r);

	if (!ok) {
		snprintf(err, errlen, "device response missing a required field");
		return -1;
	}
	return 0;
}

/* Shared by the device-code exchange and the refresh: same endpoint, same
 * response shape, same handling of a missing refresh_token. */
static int read_tokens(const http_reply *r, spotify_tokens *out,
                       char *err, size_t errlen)
{
	if (!json_str(r->body, "access_token", out->access_token,
	              sizeof(out->access_token))) {
		snprintf(err, errlen, "no access_token in a %d response", r->status);
		return -1;
	}
	/* A refresh that returns no new refresh token leaves the old one valid, so
	 * the caller must not overwrite what it stored on an empty value. */
	if (!json_str(r->body, "refresh_token", out->refresh_token,
	              sizeof(out->refresh_token)))
		out->refresh_token[0] = '\0';
	json_str(r->body, "scope", out->scope, sizeof(out->scope));

	long secs = 3600;
	json_int(r->body, "expires_in", &secs);
	/* Renew early: a token that expires mid-request is worse than one renewed
	 * a minute too soon. */
	out->expires_at = time(NULL) + (secs > 120 ? secs - 60 : secs);
	return 0;
}

pair_status pair_poll(const pair_request *req, spotify_tokens *out,
                      char *err, size_t errlen)
{
	char dev[640];
	url_encode(req->device_code, dev, sizeof(dev));
	char body[1024];
	snprintf(body, sizeof(body),
	         "client_id=%s"
	         "&grant_type=urn:ietf:params:oauth:grant-type:device_code"
	         "&device_code=%s",
	         SPOTIFY_CLIENT_ID, dev);

	http_reply r;
	if (http_post_form(ACCOUNTS_HOST, TOKEN_PATH, body, &r) != 0) {
		/* A dropped connection mid-wait is not a failed pairing: the code is
		 * still good, so the caller should keep polling. */
		snprintf(err, errlen, "%s", r.err);
		return PAIR_PENDING;
	}

	pair_status st;
	if (r.status == 200) {
		memset(out, 0, sizeof(*out));
		st = read_tokens(&r, out, err, errlen) == 0 ? PAIR_OK : PAIR_ERROR;
	} else {
		char code[64] = {0};
		json_str(r.body, "error", code, sizeof(code));
		if (strcmp(code, "authorization_pending") == 0)
			st = PAIR_PENDING;
		else if (strcmp(code, "slow_down") == 0)
			st = PAIR_SLOW_DOWN;
		else if (strcmp(code, "access_denied") == 0)
			st = PAIR_DENIED;
		else if (strcmp(code, "expired_token") == 0)
			st = PAIR_EXPIRED;
		else
			st = PAIR_ERROR;
		if (st == PAIR_ERROR || st == PAIR_DENIED || st == PAIR_EXPIRED)
			describe(&r, err, errlen);
	}
	http_reply_free(&r);
	return st;
}

int token_refresh(const char *refresh_token, spotify_tokens *out,
                  char *err, size_t errlen)
{
	char tok[1024];
	url_encode(refresh_token, tok, sizeof(tok));
	char body[1280];
	snprintf(body, sizeof(body),
	         "client_id=%s&grant_type=refresh_token&refresh_token=%s",
	         SPOTIFY_CLIENT_ID, tok);

	http_reply r;
	if (http_post_form(ACCOUNTS_HOST, TOKEN_PATH, body, &r) != 0) {
		snprintf(err, errlen, "%s", r.err);
		return -1;
	}
	if (r.status != 200) {
		/* invalid_grant here means the stored token is dead and the user has
		 * to pair again; nothing else recovers it. */
		describe(&r, err, errlen);
		http_reply_free(&r);
		return -1;
	}
	memset(out, 0, sizeof(*out));
	int rc = read_tokens(&r, out, err, errlen);
	http_reply_free(&r);
	return rc;
}
