/* The native Spotify source for the existing client.
 *
 * audio3ds was built around the bridge: it asks a server on the LAN for a list
 * and gets JSON back. This offers the same shape straight from Spotify, so the
 * UI -- skins, covers, the system keyboard, the touch list -- does not change.
 *
 * Tracks stream: the whole file is reserved up front, a download thread fills
 * it in the background, and the decoder walks behind the download watermark
 * without ever overtaking it. */
#ifndef NATIVE_H
#define NATIVE_H

#include <stdbool.h>
#include <stddef.h>

#define NATIVE_ROWS 64          /* one page the UI pages through */

typedef enum {
	NATIVE_PLAYLISTS,           /* the account's playlist folder */
	NATIVE_LIKED,
	NATIVE_SEARCH,              /* uses the query */
	NATIVE_RADIO,               /* seeded from the selected track */
	NATIVE_TRACKS,              /* inside one playlist */
} native_view;

typedef struct {
	char label[64];             /* what the row shows */
	char sub[40];               /* artist, or the track count */
	char uri[64];               /* spotify:track:... or spotify:playlist:... */
	char art[80];               /* 64px cover URL, empty when there is none */
	bool is_playlist;
} native_row;

/* Where decoded audio goes. The client already has a ring that NDSP drains,
 * so the native player is just another producer for it and the whole audio
 * path downstream stays exactly as it was. */
typedef struct {
	void (*write)(const unsigned char *pcm, unsigned len);
	unsigned (*free_space)(void);
} native_sink;

/* Pairs the console with an account when there is no stored token.
 *
 * The code has to stay on screen while the user walks to another device and
 * types it in, so this cannot be a step inside native_init: the caller draws
 * it, and this reports progress through the hook. `show_code` is called once
 * with the code and the url; `poll` is called between attempts and returns
 * false to give up. Returns 0 once a token is stored, or -1. */
int  native_pair(void (*show_code)(const char *code, const char *url),
                 bool (*poll)(int attempt), char *err, size_t errlen);

/* Whether a stored token exists, i.e. whether pairing can be skipped. */
bool native_has_token(void);

/* Signs in: stored refresh token -> OAuth -> access point -> login5 ->
 * spclient. Slow enough to need a screen of its own (a second or two), so the
 * caller passes a hook that reports each step. Returns 0, or -1 with err set.
 *
 * The access point session is kept open afterwards: audio keys come from
 * there and nowhere else, and reopening it per track would cost a second
 * each time. The player thread answers its pings while idle. */
int  native_init(const native_sink *sink, void (*progress)(const char *step),
                 char *err, size_t errlen);
void native_exit(void);
bool native_ready(void);

/* Fills up to `max` rows for a view. `arg` is the search query for
 * NATIVE_SEARCH and the playlist or seed URI for NATIVE_TRACKS / NATIVE_RADIO,
 * and is ignored otherwise. Returns the row count, or -1 with err set. */
int  native_fetch(native_view view, const char *arg, native_row *rows, int max,
                  char *err, size_t errlen);

/* ------------------------------------------------------------- playback */

/* Asks for a track. Returns immediately: resolving, fetching the key and
 * downloading take seconds, and none of that belongs on the render loop. */
void native_play(const char *track_uri);
void native_stop(void);

typedef enum {
	NATIVE_IDLE,
	NATIVE_LOADING,
	NATIVE_PLAYING,
	NATIVE_FAILED,
} native_state;

native_state native_playback_state(void);
const char  *native_now_title(void);
const char  *native_now_artist(void);
const char  *native_now_album(void);
/* 300px cover of the playing track, or "" -- the UI keeps its placeholder
 * until this has something to say. */
const char  *native_now_art(void);
/* The user's own avatar URL, for the idle hero art. Empty when the account
 * has no usable image. */
const char  *native_profile_art(void);
/* Retries the avatar fetch once; the startup call is rate-limited often
 * enough (HTTP 429) that the art worker polls this on a backoff instead. */
void         native_profile_art_refresh(void);
/* The URI last asked for; empty before the first play. */
const char  *native_now_uri(void);
const char  *native_last_error(void);
/* Seconds of audio handed to the ring, and the track's length. */
float        native_position(void);
float        native_duration(void);
/* Ask the decoder to jump to `seconds`. Honored only inside the downloaded
 * span; a jump past the watermark is ignored. */
void         native_seek(float seconds);

/* A playlist's title. Fetched when it is opened, not for every row of the
 * rootlist: that would be one request per row for names most of which the
 * user never looks at. */
int  native_playlist_name(const char *uri, char *name, size_t namelen);

/* The account name the access point reported, for the header. */
const char *native_account(void);

#endif
