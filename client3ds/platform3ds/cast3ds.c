#include "cast3ds.h"

#include "http3ds.h"
#include "json3ds.h"
#include "proto3ds.h"
#include "tls3ds.h"

#include "mbedtls/md5.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAST_APP     "CC32E753"   /* Spotify's Cast receiver */
#define NS_CONN      "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_RECEIVER  "urn:x-cast:com.google.cast.receiver"
#define NS_SPOTIFY   "urn:x-cast:com.spotify.chromecast.secure.v1"
#define SENDER       "sender-0"
#define RECEIVER     "receiver-0"
#define CAST_FRAME_MAX (64 * 1024)

void cast_device_id(const char *name, char out[CAST_DEV_LEN])
{
	unsigned char d[16];
	mbedtls_md5((const unsigned char *)name, strlen(name), d);
	for (int i = 0; i < 16; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* CastMessage: 1 protocol_version (0), 2 source_id, 3 destination_id,
 * 4 namespace, 5 payload_type (0 = string), 6 payload_utf8; each frame
 * goes out behind a 4-byte big-endian length. */
static int cast_send(tls_conn *c, const char *dst, const char *ns,
                     const char *json)
{
	const size_t cap = strlen(json) + strlen(dst) + strlen(ns) + 64;
	uint8_t *buf = malloc(cap + 4);
	if (!buf)
		return -1;
	pb m;
	pb_init(&m, buf + 4, cap);
	pb_varint(&m, 1, 0);
	pb_str(&m, 2, SENDER);
	pb_str(&m, 3, dst);
	pb_str(&m, 4, ns);
	pb_varint(&m, 5, 0);
	pb_str(&m, 6, json);
	int rc = -1;
	if (pb_ok(&m)) {
		buf[0] = (uint8_t)(m.len >> 24);
		buf[1] = (uint8_t)(m.len >> 16);
		buf[2] = (uint8_t)(m.len >> 8);
		buf[3] = (uint8_t)m.len;
		rc = tls_write(c, buf, m.len + 4) < 0 ? -1 : 0;
	}
	free(buf);
	return rc;
}

static int read_full(tls_conn *c, uint8_t *p, size_t n)
{
	while (n) {
		int got = tls_read(c, p, n);
		if (got <= 0)
			return -1;
		p += got;
		n -= (size_t)got;
	}
	return 0;
}

static void copy_field(const uint8_t *buf, size_t len, uint32_t field,
                       char *out, size_t cap)
{
	pbv v;
	out[0] = '\0';
	if (cap && pb_get_bytes(buf, len, field, &v)) {
		const size_t n = v.len < cap - 1 ? v.len : cap - 1;
		memcpy(out, v.p, n);
		out[n] = '\0';
	}
}

/* Reads frames until one on `ns` carries a "type" listed in `types`
 * (space separated), answering heartbeat pings on the way. The payload is
 * left in `out`. */
static int cast_wait(tls_conn *c, const char *ns, const char *types,
                     char *out, size_t cap, int timeout_ms, char *err,
                     size_t errlen)
{
	const u64 end = osGetTime() + (u64)timeout_ms;
	uint8_t *body = malloc(CAST_FRAME_MAX);
	if (!body) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	int rc = -1;
	while (osGetTime() < end) {
		uint8_t hdr[4];
		if (read_full(c, hdr, 4) != 0) {
			snprintf(err, errlen, "the speaker closed the connection");
			break;
		}
		const size_t n = (size_t)hdr[0] << 24 | (size_t)hdr[1] << 16 |
		                 (size_t)hdr[2] << 8 | hdr[3];
		if (n > CAST_FRAME_MAX || read_full(c, body, n) != 0) {
			snprintf(err, errlen, "bad frame from the speaker");
			break;
		}
		char fns[64], from[64], type[40];
		copy_field(body, n, 4, fns, sizeof(fns));
		copy_field(body, n, 2, from, sizeof(from));
		copy_field(body, n, 6, out, cap);
		json_str(out, "type", type, sizeof(type));
		if (strcmp(fns, NS_HEARTBEAT) == 0 && strcmp(type, "PING") == 0) {
			cast_send(c, from, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
			continue;
		}
		if (strcmp(fns, ns) != 0 || !type[0])
			continue;
		/* Whole-word match against the wanted list. */
		const char *hit = strstr(types, type);
		const size_t tl = strlen(type);
		if (hit && (hit == types || hit[-1] == ' ') &&
		    (hit[tl] == '\0' || hit[tl] == ' ')) {
			rc = 0;
			break;
		}
	}
	if (rc != 0 && !err[0])
		snprintf(err, errlen, "the speaker did not answer");
	free(body);
	return rc;
}

/* The first float after "key": in a JSON text; -1 when there is none. */
static float json_float(const char *json, const char *key)
{
	char pat[40];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(json, pat);
	return p ? strtof(p + strlen(pat), NULL) : -1.0f;
}

/* device-auth: our token in, a token for the speaker's client out. */
static int speaker_token(const char *sp_host, const char *bearer,
                         const char *client_id, const char *dev,
                         char *out, size_t cap, char *err, size_t errlen)
{
	char body[160];
	snprintf(body, sizeof(body), "{\"clientId\":\"%s\",\"deviceId\":\"%s\"}",
	         client_id, dev);
	http_reply r;
	memset(&r, 0, sizeof(r));
	if (http_request(sp_host, "/device-auth/v1/refresh", "POST", bearer,
	                 "text/plain;charset=UTF-8", "application/json",
	                 body, strlen(body), 0, &r) != 0) {
		snprintf(err, errlen, "device-auth: %s", r.err);
		return -1;
	}
	int rc = -1;
	if (r.status == 200 && json_str(r.body, "accessToken", out, cap))
		rc = 0;
	else
		snprintf(err, errlen, "device-auth: HTTP %d", r.status);
	http_reply_free(&r);
	return rc;
}

int cast_wake(const char *ip, const char *name, const char *sp_host,
              const char *bearer, const char *fallback,
              char dev[CAST_DEV_LEN], float *volume, char *err, size_t errlen)
{
	err[0] = '\0';
	*volume = -1.0f;
	cast_device_id(name, dev);
	tls_conn *c = tls_connect_lan(ip, 8009, 8000, err, errlen);
	if (!c)
		return -1;

	enum { CAP = 16384 };   /* a receiver status lists icons and namespaces */
	char *msg = malloc(CAP);
	char *tok = malloc(1024);
	int rc = -1;
	if (!msg || !tok) {
		snprintf(err, errlen, "out of memory");
		goto out;
	}

	if (cast_send(c, RECEIVER, NS_CONN, "{\"type\":\"CONNECT\"}") != 0 ||
	    cast_send(c, RECEIVER, NS_RECEIVER,
	              "{\"type\":\"LAUNCH\",\"appId\":\"" CAST_APP
	              "\",\"requestId\":1}") != 0) {
		snprintf(err, errlen, "could not talk to the speaker");
		goto out;
	}
	/* Status updates arrive until one lists Spotify with its transport. */
	char transport[64] = "";
	const u64 until = osGetTime() + 20000;
	while (!transport[0] && osGetTime() < until) {
		if (cast_wait(c, NS_RECEIVER,
		              "RECEIVER_STATUS LAUNCH_ERROR INVALID_REQUEST",
		              msg, CAP, 20000, err, errlen) != 0)
			goto out;
		char type[32];
		json_str(msg, "type", type, sizeof(type));
		if (strcmp(type, "RECEIVER_STATUS") != 0) {
			snprintf(err, errlen, "the speaker would not start Spotify");
			goto out;
		}
		const float lv = json_float(msg, "level");
		if (lv >= 0.0f)
			*volume = lv;
		const char *app = strstr(msg, "\"" CAST_APP "\"");
		if (app)
			json_str(app, "transportId", transport, sizeof(transport));
	}
	if (!transport[0]) {
		snprintf(err, errlen, "Spotify did not start on the speaker");
		goto out;
	}

	char line[256];
	snprintf(line, sizeof(line),
	         "{\"type\":\"getInfo\",\"payload\":{\"remoteName\":\"%s\","
	         "\"deviceID\":\"%s\",\"deviceAPI_isGroup\":false}}", name, dev);
	if (cast_send(c, transport, NS_CONN, "{\"type\":\"CONNECT\"}") != 0 ||
	    cast_send(c, transport, NS_SPOTIFY, line) != 0 ||
	    cast_wait(c, NS_SPOTIFY, "getInfoResponse getInfoError", msg, CAP,
	              10000, err, errlen) != 0)
		goto out;
	char client_id[48] = "";
	if (!json_str(msg, "clientID", client_id, sizeof(client_id))) {
		snprintf(err, errlen, "the speaker gave no client id");
		goto out;
	}
	/* The speaker reports the id it will register under; trust it. */
	char their_dev[CAST_DEV_LEN];
	if (json_str(msg, "deviceID", their_dev, sizeof(their_dev)) &&
	    strlen(their_dev) == CAST_DEV_LEN - 1)
		memcpy(dev, their_dev, CAST_DEV_LEN);

	char aerr[96] = "";
	if (speaker_token(sp_host, bearer, client_id, dev, tok, 1024, aerr,
	                  sizeof(aerr)) != 0 &&
	    (!fallback || !fallback[0] ||
	     speaker_token(sp_host, fallback, client_id, dev, tok, 1024, aerr,
	                   sizeof(aerr)) != 0)) {
		snprintf(err, errlen, "%s", aerr);
		goto out;
	}

	const size_t ulen = strlen(tok) + 96;
	char *add = malloc(ulen);
	if (!add) {
		snprintf(err, errlen, "out of memory");
		goto out;
	}
	snprintf(add, ulen, "{\"type\":\"addUser\",\"payload\":{\"blob\":\"%s\","
	         "\"tokenType\":\"accesstoken\"}}", tok);
	const int sent = cast_send(c, transport, NS_SPOTIFY, add);
	free(add);
	if (sent != 0 ||
	    cast_wait(c, NS_SPOTIFY, "addUserResponse addUserError", msg, CAP,
	              15000, err, errlen) != 0)
		goto out;
	long status = 0;
	if (strstr(msg, "addUserError") || !json_int(msg, "status", &status) ||
	    status != 101) {
		snprintf(err, errlen, "the speaker refused the sign-in (%ld)",
		         status);
		goto out;
	}
	rc = 0;
	/* The receiver keeps playing without a sender; leaving cleanly keeps
	 * a speaker from piling up dead connections until it refuses more. */
	cast_send(c, transport, NS_CONN, "{\"type\":\"CLOSE\"}");
	cast_send(c, RECEIVER, NS_CONN, "{\"type\":\"CLOSE\"}");
out:
	if (tok)
		memset(tok, 0, 1024);
	free(tok);
	free(msg);
	tls_close(c);
	return rc;
}

/* Commands go over one kept-open link when the caller set one: a fresh
 * TLS handshake per pause costs this console a second or more. */
static http_session *cmd_sess;

void cast_use_session(http_session *s)
{
	cmd_sess = s;
}

static int connect_state(const char *sp_host, const char *method,
                         const char *path, const char *bearer,
                         const char *body, char *err, size_t errlen)
{
	http_reply r;
	memset(&r, 0, sizeof(r));
	const int rc = cmd_sess
	    ? http_session_request(cmd_sess, path, method, bearer,
	                           "application/json", "application/json",
	                           body, strlen(body), 0, &r)
	    : http_request(sp_host, path, method, bearer, "application/json",
	                   "application/json", body, strlen(body), 0, &r);
	if (rc != 0) {
		snprintf(err, errlen, "%s", r.err);
		return -1;
	}
	const int st = r.status;
	if (st != 200)
		snprintf(err, errlen, "connect-state: HTTP %d", st);
	http_reply_free(&r);
	return st;
}

int cast_command(const char *sp_host, const char *bearer, const char *dev,
                 const char *command_json, char *err, size_t errlen)
{
	char path[128];
	snprintf(path, sizeof(path),
	         "/connect-state/v1/player/command/from/%s/to/%s", dev, dev);
	const size_t n = strlen(command_json) + 16;
	char *body = malloc(n);
	if (!body) {
		snprintf(err, errlen, "out of memory");
		return -1;
	}
	snprintf(body, n, "{\"command\":%s}", command_json);
	const int st = connect_state(sp_host, "POST", path, bearer, body, err,
	                             errlen);
	free(body);
	return st;
}

int cast_play(const char *sp_host, const char *bearer, const char *dev,
              const char *context, const char *track, long pos_ms,
              char *err, size_t errlen)
{
	char skip[160] = "{}";
	if (track && track[0])
		snprintf(skip, sizeof(skip), "{\"track_uri\":\"%s\"}", track);
	char cmd[640];
	snprintf(cmd, sizeof(cmd),
	         "{\"endpoint\":\"play\","
	         "\"context\":{\"uri\":\"%s\",\"url\":\"context://%s\","
	         "\"metadata\":{}},"
	         "\"play_origin\":{\"feature_identifier\":\"spotibacan\"},"
	         "\"options\":{\"license\":\"on-demand\",\"skip_to\":%s,"
	         "\"seek_to\":%ld,\"player_options_override\":{}}}",
	         context, context, skip, pos_ms > 0 ? pos_ms : 0L);
	return cast_command(sp_host, bearer, dev, cmd, err, errlen);
}

int cast_set_volume(const char *sp_host, const char *bearer, const char *dev,
                    float level, char *err, size_t errlen)
{
	if (level < 0.0f)
		level = 0.0f;
	if (level > 1.0f)
		level = 1.0f;
	char path[128], body[40];
	snprintf(path, sizeof(path),
	         "/connect-state/v1/connect/volume/from/%s/to/%s", dev, dev);
	snprintf(body, sizeof(body), "{\"volume\":%d}", (int)(level * 65535.0f));
	return connect_state(sp_host, "PUT", path, bearer, body, err, errlen);
}
