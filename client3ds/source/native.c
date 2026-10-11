#include "native.h"

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The UI draws over the text console, so without a log this mode is opaque:
 * a failed fetch would show as an empty list and nothing else. Same approach
 * the probes use, and the same reason. */
static FILE *logf;

static void nlog(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stderr);
	if (!logf)
		logf = fopen("sdmc:/native.log", "a");
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static const char *view_name(native_view v)
{
	switch (v) {
	case NATIVE_PLAYLISTS: return "playlists";
	case NATIVE_LIKED:     return "liked";
	case NATIVE_SEARCH:    return "search";
	case NATIVE_RADIO:     return "radio";
	case NATIVE_TRACKS:    return "tracks";
	}
	return "?";
}

static struct {
	bool         ready;
	login5_token l5;
	char         sp_host[128];
	char         username[128];
} g;

bool native_ready(void) { return g.ready; }
const char *native_account(void) { return g.username; }

static void step(void (*progress)(const char *), const char *what)
{
	nlog("INIT: %s\n", what);
	if (progress)
		progress(what);
}

int native_init(void (*progress)(const char *step), char *err, size_t errlen)
{
	if (g.ready)
		return 0;

	step(progress, "Starting TLS...");
	if (R_FAILED(psInit())) {
		snprintf(err, errlen, "no entropy source (ps:ps)");
		return -1;
	}
	int rc = tls_global_init();
	if (rc) {
		tls_strerror(rc, err, errlen);
		return -1;
	}

	step(progress, "Reading the stored token...");
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		snprintf(err, errlen, "not paired yet; run probe_pair once");
		return -1;
	}

	step(progress, "Refreshing the token...");
	spotify_tokens tok;
	if (token_refresh(stored, &tok, err, errlen) != 0)
		return -1;
	/* Spotify rotates it on every exchange, so the stored one is already dead
	 * by the time this returns. */
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	step(progress, "Finding an access point...");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int n = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, errlen);
	if (n < 1)
		return -1;

	step(progress, "Signing in to the access point...");
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, n, tok.access_token, device_id, &info,
	                              &tried, err, errlen);
	if (!ap)
		return -1;
	snprintf(g.username, sizeof(g.username), "%s", info.username);
	/* Only the credentials it handed back are needed from here; the audio key
	 * would need this session kept open, and that is the part Spotify
	 * refuses, so it is closed rather than held for nothing. */
	ap_close(ap);

	step(progress, "Getting the library token...");
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &g.l5, err, errlen) != 0)
		return -1;

	char ignored[128];
	sp_resolve(g.sp_host, sizeof(g.sp_host), ignored, sizeof(ignored));

	g.ready = true;
	nlog("INIT: ok, %s via %s\n", g.username, g.sp_host);
	return 0;
}

void native_exit(void)
{
	if (!g.ready)
		return;
	memset(&g, 0, sizeof(g));
	tls_global_exit();
	psExit();
}

/* Turns a list of track URIs into rows, naming them in one batch: on this
 * console the cost is the round trips, so a screenful must cost one. */
static int name_tracks(char uris[][64], int n, native_row *rows, int max,
                       char *err, size_t errlen)
{
	nlog("  naming %d uris in one batch\n", n);
	if (n > max)
		n = max;
	if (n <= 0)
		return 0;

	static const char *ptrs[NATIVE_ROWS];
	static sp_track meta[NATIVE_ROWS];
	for (int i = 0; i < n; i++)
		ptrs[i] = uris[i];

	int named = sp_track_metadata_batch(g.sp_host, g.l5.access_token, ptrs, n,
	                                    meta, err, errlen);
	nlog("  -> %d named%s%s\n", named, named < 0 ? ": " : "",
	     named < 0 ? err : "");
	if (named < 0)
		return -1;

	for (int i = 0; i < named; i++) {
		snprintf(rows[i].label, sizeof(rows[i].label), "%s",
		         meta[i].name[0] ? meta[i].name : "(sin nombre)");
		snprintf(rows[i].sub, sizeof(rows[i].sub), "%s", meta[i].artist);
		snprintf(rows[i].uri, sizeof(rows[i].uri), "%s",
		         meta[i].uri[0] ? meta[i].uri : uris[i]);
		rows[i].is_playlist = false;
	}
	return named;
}

int native_fetch(native_view view, const char *arg, native_row *rows, int max,
                 char *err, size_t errlen)
{
	if (!g.ready) {
		snprintf(err, errlen, "not signed in");
		return -1;
	}
	if (max > NATIVE_ROWS)
		max = NATIVE_ROWS;
	memset(rows, 0, sizeof(*rows) * (size_t)max);

	static char uris[NATIVE_ROWS][64];
	const char *token = g.l5.access_token;

	nlog("FETCH %s arg=%s max=%d\n", view_name(view), arg ? arg : "-", max);

	switch (view) {
	case NATIVE_PLAYLISTS: {
		int count = 0;
		int n = sp_rootlist(g.sp_host, token, g.username, uris, max, &count,
		                    err, errlen);
		if (n < 0)
			return -1;
		/* The rootlist gives URIs and nothing else; the names come from each
		 * playlist, which would be one request per row. Rather than pay that
		 * here, the row shows the id and the name arrives when it is opened. */
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", uris[i]);
			snprintf(rows[i].label, sizeof(rows[i].label), "Lista %d", i + 1);
			rows[i].sub[0] = '\0';
			rows[i].is_playlist = true;
		}
		nlog("  -> %d playlists (total %d)\n", n, count);
		return n;
	}
	case NATIVE_LIKED: {
		int n = sp_liked(g.sp_host, token, g.username, uris, max, err, errlen);
		if (n < 0)
			return -1;
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_SEARCH: {
		if (!arg || !arg[0]) {
			snprintf(err, errlen, "empty query");
			return -1;
		}
		int n = sp_search_tracks(g.sp_host, token, arg, uris, max, err, errlen);
		if (n < 0)
			return -1;
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_TRACKS: {
		if (!arg || strncmp(arg, "spotify:playlist:", 17) != 0) {
			snprintf(err, errlen, "not a playlist uri");
			return -1;
		}
		int count = 0;
		char name[128];
		int n = sp_playlist(g.sp_host, token, arg + 17, name, sizeof(name),
		                    uris, max, &count, err, errlen);
		if (n < 0)
			return -1;
		return name_tracks(uris, n, rows, max, err, errlen);
	}
	case NATIVE_RADIO: {
		if (!arg || !arg[0]) {
			snprintf(err, errlen, "no seed track");
			return -1;
		}
		static sp_track radio[NATIVE_ROWS];
		int n = sp_radio(g.sp_host, token, arg, max, radio, max, err, errlen);
		if (n < 0)
			return -1;
		/* radio-apollo already carries the titles, so no metadata trip. */
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].label, sizeof(rows[i].label), "%s", radio[i].name);
			snprintf(rows[i].sub, sizeof(rows[i].sub), "%s", radio[i].artist);
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", radio[i].uri);
			rows[i].is_playlist = false;
		}
		return n;
	}
	}
	snprintf(err, errlen, "unknown view");
	return -1;
}

/* The playlist's real name, fetched when it is opened rather than for every
 * row of the rootlist. */
int native_playlist_name(const char *uri, char *name, size_t namelen)
{
	if (!g.ready || !uri || strncmp(uri, "spotify:playlist:", 17) != 0)
		return -1;
	char err[160];
	char throwaway[1][64];
	int count = 0;
	return sp_playlist(g.sp_host, g.l5.access_token, uri + 17, name, namelen,
	                   throwaway, 1, &count, err, sizeof(err)) < 0 ? -1 : 0;
}
