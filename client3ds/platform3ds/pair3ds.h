/* Spotify's OAuth 2.0 device authorization grant (RFC 8628).
 *
 * The console shows a short code, the user types it in at spotify.com/pair on
 * any other device, and the console learns the outcome only by polling. No
 * password is ever typed on the 3DS and none passes through this code.
 *
 * Only the desktop client ID is enabled for this flow; the Android and iOS IDs
 * librespot ships are refused, and report it as invalid_scope rather than as
 * unauthorized_client, which misleads.
 */
#ifndef PAIR3DS_H
#define PAIR3DS_H

#include <stddef.h>
#include <time.h>

#define SPOTIFY_CLIENT_ID "65b708073fc0480ea92a077233ca87bd"

/* What this client asks for, and nothing more: playback, the library and the
 * playlists it lists, plus the account's country and name, which the access
 * point wants. Spotify validates the whole list at once -- one unrecognised
 * scope fails the request -- so this cannot be probed by asking for extras. */
#define SPOTIFY_SCOPES \
	"streaming playlist-read-private playlist-read-collaborative " \
	"user-library-read user-library-modify user-read-email user-read-private"

typedef struct {
	char user_code[16];                 /* shown to the user */
	char verification_uri[128];
	char verification_uri_complete[192];
	char device_code[320];              /* secret; never display or log it */
	int  interval_s;
	int  expires_in_s;
} pair_request;

typedef enum {
	PAIR_PENDING,      /* the user has not acted yet; wait interval_s */
	PAIR_SLOW_DOWN,    /* polled too fast; back off and keep waiting */
	PAIR_OK,
	PAIR_DENIED,       /* the user refused */
	PAIR_EXPIRED,      /* the code timed out; start over */
	PAIR_ERROR
} pair_status;

typedef struct {
	char   access_token[1280];
	char   refresh_token[512];
	char   scope[640];
	time_t expires_at;                  /* wall clock, from the console's RTC */
} spotify_tokens;

/* Asks Spotify for a code. Returns 0, or -1 with `err` set. */
int pair_begin(pair_request *out, char *err, size_t errlen);

/* One poll. PAIR_PENDING and PAIR_SLOW_DOWN are normal and expected. */
pair_status pair_poll(const pair_request *req, spotify_tokens *out,
                      char *err, size_t errlen);

/* Trades a stored refresh token for a fresh access token. Spotify may or may
 * not return a new refresh token; when it does not, the old one stays valid. */
int token_refresh(const char *refresh_token, spotify_tokens *out,
                  char *err, size_t errlen);

#endif
