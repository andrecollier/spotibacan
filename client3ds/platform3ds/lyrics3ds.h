/* Lyrics from LRCLIB: one unauthenticated GET, then either the synced (LRC)
 * or the plain document the answer carries.
 *
 * The doc is flat storage owned by the caller: at 400 lines of 208 bytes it
 * is ~83 KB, so it lives wherever the caller puts it (a global), never on a
 * thread stack. */
#ifndef LYRICS3DS_H
#define LYRICS3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LYRICS_MAX_LINES 400
#define LYRICS_TEXT_MAX  200
#define LYRICS_BODY_MAX  (256 * 1024)

typedef struct {
	uint32_t time_ms;   /* UINT32_MAX marks an unsynced (plain) line */
	char     text[LYRICS_TEXT_MAX + 1];
} lyric_line;

typedef struct {
	lyric_line lines[LYRICS_MAX_LINES];
	int        count;
	bool       synced;
} lyrics_doc;

typedef enum {
	LYR_OK = 0,
	LYR_NONE,   /* no lyrics known for this track (or instrumental) */
	LYR_ERR,
} lyrics_result;

/* Blocking lookup: a TLS round trip plus the parse, so it belongs on a
 * worker thread. duration_ms may be 0 when unknown; album may be NULL.
 * `out` is only touched on LYR_OK. */
lyrics_result lyrics_fetch(const char *track, const char *artist,
                           const char *album, long duration_ms,
                           lyrics_doc *out, char *err, size_t errlen);

/* The last synced line at or before position_ms, or -1. */
int lyrics_index_at(const lyrics_doc *doc, uint32_t position_ms);

#endif
