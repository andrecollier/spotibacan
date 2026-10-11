/* login5: the Bearer token spclient actually accepts.
 *
 * spclient refuses the OAuth access token, and api.spotify.com answers 429 for
 * the desktop client id, so neither is a way in. The route that works is the
 * one librespot takes: the access point hands back reusable credentials with
 * APWelcome, and login5 trades those for a Bearer token.
 *
 * So the order is fixed and cannot be shortcut: pair -> OAuth token -> AP login
 * -> reusable credentials -> login5 -> spclient.
 */
#ifndef LOGIN5_H
#define LOGIN5_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
	char   access_token[1024];
	char   username[128];
	time_t expires_at;
	/* login5 may hand back fresh credentials; they replace what the AP gave. */
	uint8_t stored[320];
	size_t  stored_len;
} login5_token;

/* Trades the AP's reusable credentials for a Bearer token. Solves any hashcash
 * challenge the server issues. Returns 0, or -1 with `err` set. */
int login5_from_credentials(const char *username,
                            const uint8_t *credentials, size_t credentials_len,
                            const char *device_id,
                            login5_token *out, char *err, size_t errlen);

#endif
