/* spclient: Spotify's internal API, the one that answers for this client.
 *
 * Needs a login5 Bearer token -- the OAuth token is refused here, and
 * api.spotify.com answers 429 for the desktop client id, so this is the only
 * way in. Everything speaks protobuf except a few endpoints that answer JSON.
 */
#ifndef SPCLIENT_H
#define SPCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP_MAX_FILES 8

/* Audio formats, from metadata.proto's AudioFile.Format. Only the Vorbis ones
 * matter here: the 3DS decodes Vorbis in software and has no AAC decoder worth
 * using. 160 kbps is the one to prefer -- 320 triples the bandwidth over
 * 802.11g for a difference the console's speakers cannot show. */
#define SP_FMT_OGG_96   0
#define SP_FMT_OGG_160  1
#define SP_FMT_OGG_320  2

typedef struct {
	uint8_t id[20];
	int     format;
} sp_file;

typedef struct {
	char    name[128];
	char    artist[128];
	char    album[128];
	int     duration_ms;
	sp_file files[SP_MAX_FILES];
	int     file_count;
} sp_track;

/* Which spclient host to use. Falls back to the well-known name when apresolve
 * has nothing to say, which is what librespot does too. */
int  sp_resolve(char *host, size_t hostlen, char *err, size_t errlen);

/* One track's metadata, via the extended-metadata batch service. */
int  sp_track_metadata(const char *host, const char *token, const char *track_uri,
                       sp_track *out, char *err, size_t errlen);

/* The user's playlist folder. `count` is how many entries the rootlist holds;
 * up to `max` of their URIs are copied into `uris`. An empty rootlist is a
 * valid answer, not an error. */
int  sp_rootlist(const char *host, const char *token, const char *username,
                 char uris[][96], int max, int *count,
                 char *err, size_t errlen);

/* Picks the best file this client can actually play, or NULL. */
const sp_file *sp_pick_file(const sp_track *t);
const char *sp_format_name(int format);

#endif
