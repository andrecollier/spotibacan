#include "login5.h"
#include "http3ds.h"
#include "pair3ds.h"
#include "proto3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/md.h"

#define LOGIN5_HOST "login5.spotify.com"
#define LOGIN5_PATH "/v3/login"

/* LoginRequest fields: client_info 1, login_context 2, challenge_solutions 3,
 * stored_credential 100. LoginResponse: ok 1, error 2, challenges 3,
 * login_context 5. */
#define F_CLIENT_INFO   1
#define F_CONTEXT       2
#define F_SOLUTIONS     3
#define F_STORED_CRED   100
#define F_OK            1
#define F_ERROR         2
#define F_CHALLENGES    3

static const char *login5_error_text(uint64_t code)
{
	switch (code) {
	case 1:  return "invalid credentials";
	case 2:  return "bad request";
	case 3:  return "unsupported login protocol";
	case 4:  return "timeout";
	case 5:  return "unknown identifier";
	case 6:  return "too many attempts";
	case 7:  return "invalid phone number";
	case 8:  return "try again later";
	default: return "unknown error";
	}
}

static bool sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	return md && mbedtls_md(md, data, len, out) == 0;
}

static int64_t be64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return (int64_t)v;
}

static void wr_be64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)(v & 0xFF);
		v >>= 8;
	}
}

/* The proof of work login5 sometimes demands: find a counter whose SHA-1,
 * taken over the challenge prefix plus a 16-byte suffix, ends in `length`
 * zero bits. Transcribed from librespot's util::solve_hash_cash.
 *
 * On the ARM11 this is the one place in the whole client that is pure brute
 * force, so it is also the one that can simply be too slow; the caller is told
 * how long it took rather than left to guess. */
static bool solve_hashcash(const uint8_t *ctx, size_t ctxlen,
                           const uint8_t *prefix, size_t prefixlen,
                           int length, uint8_t suffix[16],
                           int *tries, double *ms)
{
	uint8_t md[20];
	if (!sha1(ctx, ctxlen, md))
		return false;
	int64_t target = be64(md + 12);

	TickCounter t;
	osTickCounterStart(&t);

	uint8_t buf[256];
	if (prefixlen + 16 > sizeof(buf))
		return false;
	memcpy(buf, prefix, prefixlen);

	for (int64_t counter = 0; counter < 20000000; counter++) {
		wr_be64(buf + prefixlen, (uint64_t)(target + counter));
		wr_be64(buf + prefixlen + 8, (uint64_t)counter);
		if (!sha1(buf, prefixlen + 16, md))
			return false;
		int64_t v = be64(md + 12);
		/* Count trailing zero bits exactly as the reference does. */
		int tz;
		if (v == 0) {
			tz = 64;
		} else {
			uint64_t low = (uint64_t)v & (uint64_t)(-v);
			tz = 0;
			while (!(low & 1)) {
				low >>= 1;
				tz++;
			}
		}
		if (tz >= length) {
			memcpy(suffix, buf + prefixlen, 16);
			osTickCounterUpdate(&t);
			if (tries)
				*tries = (int)counter;
			if (ms)
				*ms = osTickCounterRead(&t);
			return true;
		}
		/* The challenge expires server-side, so give up rather than grind. */
		if ((counter & 0xFFFF) == 0xFFFF) {
			osTickCounterUpdate(&t);
			if (osTickCounterRead(&t) > 20000.0)
				return false;
			osTickCounterStart(&t);
		}
	}
	return false;
}

int login5_from_credentials(const char *username,
                            const uint8_t *credentials, size_t credentials_len,
                            const char *device_id,
                            login5_token *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	uint8_t ci_store[256], cred_store[512], ctx_store[512], sol_store[512];
	pb ci, cred;
	pb_init(&ci, ci_store, sizeof(ci_store));
	pb_str(&ci, 1, SPOTIFY_CLIENT_ID);
	pb_str(&ci, 2, device_id);
	pb_init(&cred, cred_store, sizeof(cred_store));
	pb_str(&cred, 1, username);
	pb_bytes(&cred, 2, credentials, credentials_len);
	if (!pb_ok(&ci) || !pb_ok(&cred)) {
		snprintf(err, errlen, "login5 request did not fit");
		return -1;
	}

	size_t ctx_len = 0, sol_len = 0;

	/* Each challenge round costs a round trip, and the server has never been
	 * seen to issue more than one, so a handful of attempts is plenty. */
	for (int attempt = 1; attempt <= 4; attempt++) {
		uint8_t req_store[2048];
		pb req;
		pb_init(&req, req_store, sizeof(req_store));
		pb_sub(&req, F_CLIENT_INFO, &ci);
		if (ctx_len)
			pb_bytes(&req, F_CONTEXT, ctx_store, ctx_len);
		if (sol_len)
			pb_bytes(&req, F_SOLUTIONS, sol_store, sol_len);
		pb_sub(&req, F_STORED_CRED, &cred);
		if (!pb_ok(&req)) {
			snprintf(err, errlen, "login5 request did not fit");
			return -1;
		}

		http_reply r;
		if (http_post_proto(LOGIN5_HOST, LOGIN5_PATH, req.p, req.len, &r) != 0) {
			snprintf(err, errlen, "login5: %s", r.err);
			return -1;
		}
		if (r.status != 200) {
			snprintf(err, errlen, "login5 returned HTTP %d", r.status);
			http_reply_free(&r);
			return -1;
		}

		const uint8_t *body = (const uint8_t *)r.body;
		size_t blen = r.len;

		pbv ok;
		if (pb_get_bytes(body, blen, F_OK, &ok)) {
			pbv s;
			uint64_t expires = 3600;
			if (pb_get_bytes(ok.p, ok.len, 1, &s)) {
				size_t n = s.len < sizeof(out->username) - 1
				           ? s.len : sizeof(out->username) - 1;
				memcpy(out->username, s.p, n);
				out->username[n] = '\0';
			}
			if (!pb_get_bytes(ok.p, ok.len, 2, &s) ||
			    s.len >= sizeof(out->access_token)) {
				snprintf(err, errlen, "login5 token missing or too long");
				http_reply_free(&r);
				return -1;
			}
			memcpy(out->access_token, s.p, s.len);
			out->access_token[s.len] = '\0';
			if (pb_get_bytes(ok.p, ok.len, 3, &s) && s.len <= sizeof(out->stored)) {
				memcpy(out->stored, s.p, s.len);
				out->stored_len = s.len;
			}
			pb_get_varint(ok.p, ok.len, 4, &expires);
			out->expires_at = time(NULL) + (expires > 120 ? (time_t)expires - 60
			                                              : (time_t)expires);
			http_reply_free(&r);
			return 0;
		}

		uint64_t code;
		if (pb_get_varint(body, blen, F_ERROR, &code)) {
			snprintf(err, errlen, "login5 refused: %s (%lu)",
			         login5_error_text(code), (unsigned long)code);
			http_reply_free(&r);
			return -1;
		}

		pbv challenges;
		if (!pb_get_bytes(body, blen, F_CHALLENGES, &challenges)) {
			snprintf(err, errlen,
			         "login5 answered with neither ok, error nor challenges");
			http_reply_free(&r);
			return -1;
		}

		/* The context the solution is computed over comes from this response,
		 * and has to be echoed back with it. */
		pbv ctx;
		if (pb_get_bytes(body, blen, 5, &ctx) && ctx.len <= sizeof(ctx_store)) {
			memcpy(ctx_store, ctx.p, ctx.len);
			ctx_len = ctx.len;
		}

		pbv challenge;
		if (!pb_get_bytes(challenges.p, challenges.len, 1, &challenge)) {
			snprintf(err, errlen, "login5 sent an empty challenge list");
			http_reply_free(&r);
			return -1;
		}
		pbv hc;
		if (!pb_get_bytes(challenge.p, challenge.len, 1, &hc)) {
			snprintf(err, errlen,
			         "login5 wants a code challenge, which needs a human");
			http_reply_free(&r);
			return -1;
		}
		pbv prefix;
		uint64_t length = 0;
		if (!pb_get_bytes(hc.p, hc.len, 1, &prefix) ||
		    !pb_get_varint(hc.p, hc.len, 2, &length)) {
			snprintf(err, errlen, "hashcash challenge is malformed");
			http_reply_free(&r);
			return -1;
		}

		uint8_t suffix[16];
		int tries = 0;
		double ms = 0;
		bool solved = solve_hashcash(ctx_store, ctx_len, prefix.p, prefix.len,
		                             (int)length, suffix, &tries, &ms);
		http_reply_free(&r);
		if (!solved) {
			snprintf(err, errlen,
			         "could not solve the hashcash (length %lu) in time",
			         (unsigned long)length);
			return -1;
		}

		uint8_t dur_store[32], hcs_store[96], sol_one[128];
		pb dur, hcs, one;
		pb_init(&dur, dur_store, sizeof(dur_store));
		pb_varint(&dur, 1, (uint64_t)(ms / 1000.0));
		pb_varint(&dur, 2, (uint64_t)((ms - (long)(ms / 1000.0) * 1000.0) * 1e6));
		pb_init(&hcs, hcs_store, sizeof(hcs_store));
		pb_bytes(&hcs, 1, suffix, sizeof(suffix));
		pb_sub(&hcs, 2, &dur);
		pb_init(&one, sol_one, sizeof(sol_one));
		pb_sub(&one, 1, &hcs);          /* ChallengeSolution.hashcash */

		pb sols;
		pb_init(&sols, sol_store, sizeof(sol_store));
		pb_sub(&sols, 1, &one);         /* ChallengeSolutions.solutions */
		if (!pb_ok(&sols)) {
			snprintf(err, errlen, "hashcash solution did not fit");
			return -1;
		}
		sol_len = sols.len;
	}

	snprintf(err, errlen, "login5 kept issuing challenges");
	return -1;
}
