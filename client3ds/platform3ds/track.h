/* One playable track: from a URI to decrypted Ogg Vorbis in memory.
 *
 * Ties together the three services that each hold one piece: spclient for the
 * metadata and the CDN location, the access point for the decryption key, and
 * the CDN for the bytes. None of them is useful without the other two.
 */
#ifndef TRACK_H
#define TRACK_H

#include "ap3ds.h"
#include "spclient.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Spotify puts a decoy Ogg page of this length at the start of every audio
 * file; the real Vorbis stream begins right after it.
 *
 * Do not try to find this by searching for the "OggS" magic: the decoy *is* an
 * OggS page, so the first match is the wrong one and the decoder then fails
 * with nothing more useful than "end of stream". */
#define SPOTIFY_OGG_HEADER 167

typedef struct {
	uint8_t *data;           /* decrypted file, including the decoy header */
	size_t   len;
	sp_track meta;
	int      format;         /* which AudioFile.Format was fetched */
	double   resolve_ms;
	double   key_ms;
	double   download_ms;
	double   decrypt_ms;
} track_audio;

/* Fetches and decrypts one track. `ap` must be a live session: the audio key
 * comes from there and nowhere else.
 *
 * `limit` caps the download; a track that would exceed it fails rather than
 * being truncated into something that decodes to silence halfway through.
 *
 * Returns 0, AP_KEY_REFUSED when Spotify withholds the key for this account,
 * or -1. */
int  track_fetch(const char *sp_host, const char *token, ap_session *ap,
                 const char *track_uri, size_t limit, track_audio *out,
                 char *err, size_t errlen);

void track_free(track_audio *t);

/* Where the Vorbis stream starts, and how long it is. */
static inline const uint8_t *track_vorbis(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->data + SPOTIFY_OGG_HEADER : NULL;
}
static inline size_t track_vorbis_len(const track_audio *t)
{
	return t->len > SPOTIFY_OGG_HEADER ? t->len - SPOTIFY_OGG_HEADER : 0;
}

#endif
