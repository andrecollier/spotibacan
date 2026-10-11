/* The native Spotify source for the existing client.
 *
 * audio3ds was built around the bridge: it asks a server on the LAN for a list
 * and gets JSON back. This offers the same shape straight from Spotify, so the
 * UI -- skins, covers, the system keyboard, the touch list -- does not change.
 *
 * What it cannot do yet is play anything: the audio key is refused by Spotify
 * for this account (librespot#1649), and that is the one piece missing. So
 * this browses, and the player stays on the bridge.
 */
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
	bool is_playlist;
} native_row;

/* Signs in: stored refresh token -> OAuth -> access point -> login5 ->
 * spclient. Slow enough to need a screen of its own (a second or two), so the
 * caller passes a hook that reports each step. Returns 0, or -1 with err set. */
int  native_init(void (*progress)(const char *step), char *err, size_t errlen);
void native_exit(void);
bool native_ready(void);

/* Fills up to `max` rows for a view. `arg` is the search query for
 * NATIVE_SEARCH and the playlist or seed URI for NATIVE_TRACKS / NATIVE_RADIO,
 * and is ignored otherwise. Returns the row count, or -1 with err set. */
int  native_fetch(native_view view, const char *arg, native_row *rows, int max,
                  char *err, size_t errlen);

/* A playlist's title. Fetched when it is opened, not for every row of the
 * rootlist: that would be one request per row for names most of which the
 * user never looks at. */
int  native_playlist_name(const char *uri, char *name, size_t namelen);

/* The account name the access point reported, for the header. */
const char *native_account(void);

#endif
