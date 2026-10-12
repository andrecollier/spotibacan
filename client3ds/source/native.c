#include "native.h"

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"
#include "track.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

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
	if (!logf) {
		/* Beside the .3dsx, like the token and the device id: everything the
		 * app owns lives in its own folder, so the card stays clean and the
		 * whole app is one directory to copy or delete. */
		char path[320];
		snprintf(path, sizeof(path), "%s", cfg_path());
		char *slash = strrchr(path, '/');
		if (slash)
			slash[1] = '\0';
		else
			path[0] = '\0';
		strncat(path, "native.log", sizeof(path) - strlen(path) - 1);
		logf = fopen(path, "a");
	}
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

/* The whole file is reserved up front and filled in order by a download
 * thread while the decoder walks behind it (track_stream in track.h). How
 * far behind is a safety margin: stb_vorbis parses whole pages while
 * decoding, and a page can run to ~64 KB. */
#define STREAM_START_BYTES (32 * 1024)   /* enough for the Vorbis headers */
#define STREAM_MARGIN      (128 * 1024)
#define NATIVE_RATE   44100

static struct {
	bool          ready;
	login5_token  l5;
	char          sp_host[128];
	char          username[128];
	ap_session   *ap;
	/* Everything a reconnect needs: the OAuth token the AP logs in with, the
	 * device id, and the candidate list apresolve gave at init. */
	char          access_token[1280];
	char          device_id[64];
	ap_candidate  cands[AP_MAX_CANDIDATES];
	int           cand_count;
	native_sink   sink;

	Thread        player;
	LightLock      lock;
	volatile bool  quit;
	volatile bool  dl_stop;   /* tells the download thread the UI moved on */
	/* Set by the UI thread, consumed by the player. */
	char           want[64];
	volatile bool  want_new;
	volatile bool  want_stop;
	/* Seek request in samples, -1 = none. Set by the UI, consumed by the
	 * decoder, which may reject it when the target is not downloaded yet. */
	volatile long  want_seek_frame;

	volatile native_state state;
	char          title[128];
	char          artist[128];
	char          album[128];
	char          art[80];
	char          profile_art[80];   /* the user's own avatar, for the idle hero */
	char          error[192];
	volatile unsigned frames_out;
	volatile unsigned total_frames;
} g;

bool native_ready(void) { return g.ready; }
const char *native_account(void) { return g.username; }

static void step(void (*progress)(const char *), const char *what)
{
	nlog("INIT: %s\n", what);
	if (progress)
		progress(what);
}

bool native_has_token(void)
{
	char stored[512];
	return cfg_load_refresh_token(stored, sizeof(stored));
}

int native_pair(void (*show_code)(const char *code, const char *url),
                bool (*poll)(int attempt), char *err, size_t errlen)
{
	/* Pairing needs TLS but not a session, and it runs before native_init,
	 * so it brings up what it needs itself. Both calls are no-ops the second
	 * time around. */
	if (R_FAILED(psInit())) {
		snprintf(err, errlen, "sin fuente de entropia (ps:ps)");
		return -1;
	}
	int rc = tls_global_init();
	if (rc) {
		tls_strerror(rc, err, errlen);
		return -1;
	}

	pair_request req;
	if (pair_begin(&req, err, errlen) != 0)
		return -1;
	nlog("PAIR: code %s\n", req.user_code);
	/* Always the bare verification_uri: the _complete variant embeds the code
	 * in the URL, and on screen it read as if the user had to type the whole
	 * long address instead of just the short code below it. */
	if (show_code)
		show_code(req.user_code, req.verification_uri);

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	for (int attempt = 1; time(NULL) < deadline; attempt++) {
		/* The caller owns the frame loop, so it does the waiting and can
		 * keep the code on screen and the buttons alive. */
		for (int i = 0; i < interval * 60; i++)
			if (poll && !poll(attempt)) {
				snprintf(err, errlen, "cancelado");
				return -1;
			}

		spotify_tokens tok;
		char perr[192] = "";
		pair_status st = pair_poll(&req, &tok, perr, sizeof(perr));
		if (st == PAIR_OK) {
			if (!tok.refresh_token[0] ||
			    !cfg_save_refresh_token(tok.refresh_token, tok.scope)) {
				snprintf(err, errlen, "no se pudo guardar el token");
				return -1;
			}
			nlog("PAIR: ok\n");
			return 0;
		}
		if (st == PAIR_SLOW_DOWN) {
			interval += 5;
			continue;
		}
		if (st != PAIR_PENDING) {
			snprintf(err, errlen, "%s", perr[0] ? perr : "el pareo fallo");
			return -1;
		}
	}
	snprintf(err, errlen, "el codigo caduco");
	return -1;
}

native_state native_playback_state(void) { return g.state; }
const char *native_now_title(void)  { return g.title; }
const char *native_now_artist(void) { return g.artist; }
const char *native_now_album(void)  { return g.album; }
/* The URI last asked for, so the lyrics view can tell its document belongs
 * to what is playing. Empty before the first play. */
const char *native_now_uri(void)    { return g.want; }
const char *native_now_art(void)    { return g.art; }
const char *native_profile_art(void) { return g.profile_art; }

/* One retry of the avatar fetch, from the art worker. No-op once the URL is
 * known, so the caller can poll on a timer without tracking state itself. */
void native_profile_art_refresh(void)
{
	if (!g.ready || g.profile_art[0] || !g.access_token[0])
		return;
	char url[80];
	char perr[128] = "";
	if (sp_user_image(g.access_token, url, sizeof(url), perr,
	                  sizeof(perr)) == 0) {
		snprintf(g.profile_art, sizeof(g.profile_art), "%s", url);
		/* Cache it: /v1/me is so rate-limited that a success may not repeat
		 * itself for hours, and the CDN URL outlives the session anyway. */
		cfg_save_profile_art(url);
		nlog("INIT: foto de perfil recuperada\n");
	} else {
		nlog("INIT: foto de perfil sigue sin llegar: %s\n", perr);
	}
}
const char *native_last_error(void) { return g.error; }

float native_position(void)
{
	return (float)g.frames_out / NATIVE_RATE;
}

void native_seek(float seconds)
{
	if (seconds < 0.0f)
		seconds = 0.0f;
	long frame = (long)(seconds * NATIVE_RATE);
	__sync_synchronize();
	g.want_seek_frame = frame;
}

float native_duration(void)
{
	return (float)g.total_frames / NATIVE_RATE;
}

void native_play(const char *track_uri)
{
	if (!g.ready || !track_uri)
		return;
	LightLock_Lock(&g.lock);
	snprintf(g.want, sizeof(g.want), "%s", track_uri);
	g.want_new = true;
	g.want_stop = false;
	LightLock_Unlock(&g.lock);
}

void native_stop(void)
{
	g.want_stop = true;
}

static void download_thread(void *arg)
{
	track_stream_download(arg, &g.dl_stop);
}

/* Decodes one streaming track into the ring until it ends, the download
 * fails, the UI asks for something else, or the session is shutting down. */
static void play_stream(track_stream *ts)
{
	/* Wait for enough of the file to hold the Vorbis headers. */
	for (;;) {
		if (g.quit || g.want_stop || g.want_new)
			return;
		if (ts->dl_failed) {
			snprintf(g.error, sizeof(g.error), "%s",
			         ts->dl_err[0] ? ts->dl_err : "la descarga fallo");
			g.state = NATIVE_FAILED;
			return;
		}
		__sync_synchronize();
		if (ts->data && ts->downloaded >= STREAM_START_BYTES)
			break;
		svcSleepThread(20000000LL);   // 20 ms
	}

	if (memcmp(ts->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		snprintf(g.error, sizeof(g.error),
		         "no Ogg stream after the %d byte header -- wrong key?",
		         SPOTIFY_OGG_HEADER);
		g.state = NATIVE_FAILED;
		return;
	}

	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(ts->data + SPOTIFY_OGG_HEADER,
	                                       (int)(ts->len - SPOTIFY_OGG_HEADER),
	                                       &verr, NULL);
	if (!v) {
		snprintf(g.error, sizeof(g.error),
		         "el decodificador rechazo el stream (%d)", verr);
		g.state = NATIVE_FAILED;
		return;
	}

	/* The length comes from the metadata: asking stb_vorbis would scan the
	 * tail of the file, which is still zeroed while the download runs. */
	g.total_frames =
	    (unsigned)((uint64_t)ts->meta.duration_ms * NATIVE_RATE / 1000);
	stb_vorbis_set_length(v, g.total_frames);
	g.frames_out = 0;
	g.state = NATIVE_PLAYING;
	nlog("PLAY %s - %s\n", g.artist, g.title);

	static s16 pcm[4096 * 2];
	long pending = -1;            /* far seek waiting on the download */
	unsigned pending_prev = 0;    /* where the stream was when it started */
	while (!g.quit && !g.want_stop && !g.want_new) {
		long want = g.want_seek_frame;
		if (want >= 0) {
			g.want_seek_frame = -1;
			__sync_synchronize();
			if ((unsigned)want >= g.total_frames)
				want = (long)g.total_frames - 1;
			unsigned prev = pending >= 0 ? pending_prev : g.frames_out;
			if (stb_vorbis_seek(v, (unsigned)want)) {
				/* Inside the downloaded span the jump is immediate. */
				g.frames_out = (unsigned)want;
				pending = -1;
				nlog("SEEK ok -> %ld\n", want);
			} else if (ts->len) {
				stb_vorbis_seek(v, prev);
				/* Past the watermark the buffer is zero fill and no page
				 * scan can land there: move the download to the target
				 * and wait the moment it takes the bytes to arrive. */
				size_t at = (size_t)((double)want /
				                     (double)g.total_frames *
				                     (double)ts->len) & ~(size_t)15;
				ts->dl_restart_at = (long)at;
				__sync_synchronize();
				pending = want;
				pending_prev = prev;
				g.frames_out = (unsigned)want;   /* the bar shows the goal */
				nlog("SEEK lejos -> %ld (byte %zu)\n", want, at);
			} else {
				stb_vorbis_seek(v, prev);
			}
		}
		if (pending >= 0) {
			if (ts->dl_failed) {
				pending = -1;
				g.frames_out = pending_prev;
			} else if (stb_vorbis_seek(v, (unsigned)pending)) {
				nlog("SEEK lejos aterrizo -> %ld\n", pending);
				pending = -1;
			} else {
				/* Nothing honest to play until the target page exists:
				 * keep the stream parked and let the ring run dry, which
				 * the UI already shows as buffering. */
				stb_vorbis_seek(v, pending_prev);
				svcSleepThread(10000000LL);   // 10 ms
				continue;
			}
		}
		/* Only decode when the ring has room: the ring is the pacing, and
		 * running ahead of it would just burn battery. */
		if (g.sink.free_space() < sizeof(pcm)) {
			svcSleepThread(10000000LL);   // 10 ms
			continue;
		}
		/* And never past the watermark: one decode call can read ahead a
		 * whole page, so the margin has to swallow the largest one. */
		__sync_synchronize();
		if (!ts->dl_done) {
			size_t off = (size_t)stb_vorbis_get_file_offset(v) +
			             SPOTIFY_OGG_HEADER;
			if (off + STREAM_MARGIN > ts->downloaded) {
				if (ts->dl_failed)
					break;
				svcSleepThread(20000000LL);   // 20 ms
				continue;
			}
		}
		int got = stb_vorbis_get_samples_short_interleaved(v, 2, pcm, 4096 * 2);
		if (got <= 0)
			break;
		g.sink.write((const unsigned char *)pcm, (unsigned)got * 4);
		g.frames_out += (unsigned)got;
	}

	stb_vorbis_close(v);
	if (ts->dl_failed && !ts->dl_done) {
		snprintf(g.error, sizeof(g.error), "%s",
		         ts->dl_err[0] ? ts->dl_err : "la descarga fallo a mitad");
		g.state = NATIVE_FAILED;
	}
	nlog("PLAY fin: %u frames\n", g.frames_out);
}

/* Rebuilds the access point session with the credentials kept from init.
 * The access token may have expired since, so one token refresh is tried
 * before giving up. Runs on the player thread, the only owner of g.ap. */
static bool ap_reconnect(void)
{
	char err[192] = "";
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(g.cands, g.cand_count, g.access_token,
	                              g.device_id, &info, &tried,
	                              err, sizeof(err));
	if (!ap) {
		char stored[512];
		spotify_tokens tok;
		char rerr[192] = "";
		if (!cfg_load_refresh_token(stored, sizeof(stored)) ||
		    token_refresh(stored, &tok, rerr, sizeof(rerr)) != 0) {
			nlog("AP: reconexion fallo: %s / %s\n", err, rerr);
			return false;
		}
		if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
			cfg_save_refresh_token(tok.refresh_token, tok.scope);
		snprintf(g.access_token, sizeof(g.access_token), "%s",
		         tok.access_token);
		ap = ap_login_any(g.cands, g.cand_count, g.access_token,
		                  g.device_id, &info, &tried, err, sizeof(err));
		if (!ap) {
			nlog("AP: reconexion fallo: %s\n", err);
			return false;
		}
	}
	g.ap = ap;
	nlog("AP: reconectado (%d candidatos)\n", tried);
	return true;
}

static void player_thread(void *arg)
{
	(void)arg;
	while (!g.quit) {
		if (g.want_new) {
			char uri[64];
			LightLock_Lock(&g.lock);
			snprintf(uri, sizeof(uri), "%s", g.want);
			g.want_new = false;
			LightLock_Unlock(&g.lock);

			g.state = NATIVE_LOADING;
			g.error[0] = '\0';
			g.frames_out = g.total_frames = 0;
			g.want_seek_frame = -1;   /* a seek belongs to its track */
			nlog("FETCH track %s\n", uri);

			/* The idle loop drops g.ap when the access point hangs up; the
			 * background reconnect below rebuilds it, so a track asked for
			 * right after a hiccup only has to wait for it. */
			if (!g.ap && !ap_reconnect()) {
				snprintf(g.error, sizeof(g.error),
				         "reconectando con Spotify, intenta de nuevo");
				g.state = NATIVE_FAILED;
				nlog("  fallo: sin sesion AP\n");
				continue;
			}

			track_stream ts;
			char err[192];
			int rc = track_stream_begin(g.sp_host, g.l5.access_token, g.ap,
			                            uri, NULL, &ts, err, sizeof(err));
			if (rc != 0) {
				snprintf(g.error, sizeof(g.error), "%s", err);
				g.state = NATIVE_FAILED;
				nlog("  fallo: %s\n", err);
				continue;
			}
			snprintf(g.title, sizeof(g.title), "%s", ts.meta.name);
			snprintf(g.artist, sizeof(g.artist), "%s", ts.meta.artist);
			snprintf(g.album, sizeof(g.album), "%s", ts.meta.album);
			snprintf(g.art, sizeof(g.art), "%s", ts.meta.art_large);

			/* The download runs beside the decoder: `ts` lives on this
			 * frame's stack and the thread is joined before it goes away. */
			g.dl_stop = false;
			Thread dl = threadCreate(download_thread, &ts, 48 * 1024,
			                         0x31, -2, false);
			if (!dl) {
				snprintf(g.error, sizeof(g.error),
				         "no se pudo crear el hilo de descarga");
				g.state = NATIVE_FAILED;
				track_stream_free(&ts);
				continue;
			}
			play_stream(&ts);
			g.dl_stop = true;
			threadJoin(dl, U64_MAX);
			threadFree(dl);
			track_stream_free(&ts);
			if (g.state == NATIVE_PLAYING)
				g.state = NATIVE_IDLE;
			continue;
		}

		if (g.want_stop) {
			g.want_stop = false;
			g.state = NATIVE_IDLE;
		}

		/* The access point session is rebuilt in the background with a 5 s
		 * backoff: a dropped session used to end listening until the app
		 * restarted, and the console's wifi hiccups often enough for that
		 * to matter. */
		if (!g.ap) {
			static unsigned long long next_try = 0;
			unsigned long long now = osGetTime();
			if (now >= next_try) {
				ap_reconnect();
				next_try = osGetTime() + 5000;
			}
			svcSleepThread(20000000LL);   // 20 ms
			continue;
		}

		/* Idle: keep the access point session alive. It only speaks to say
		 * ping, but a session that stops answering is dropped, and then the
		 * next audio key would fail for a reason that looks like anything
		 * but a missed ping. */
		if (ap_wait(g.ap, 200) == 1) {
			uint8_t buf[2048];
			uint8_t cmd;
			int n = ap_recv(g.ap, &cmd, buf, sizeof(buf));
			if (n == AP_PKT_TOO_BIG) {
				/* fine: nothing we need is that large */
			} else if (n < 0) {
				nlog("AP: sesion perdida mientras esperaba\n");
				ap_close(g.ap);
				g.ap = NULL;
			} else if (cmd == AP_PKT_PING) {
				ap_handle_ping(g.ap, buf, (size_t)n);
			}
		} else {
			svcSleepThread(20000000LL);   // 20 ms
		}
	}
}

int native_init(const native_sink *sink, void (*progress)(const char *step),
                char *err, size_t errlen)
{
	if (g.ready)
		return 0;
	if (!sink || !sink->write || !sink->free_space) {
		snprintf(err, errlen, "sin destino de audio");
		return -1;
	}
	g.sink = *sink;

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
		snprintf(err, errlen, "sin parear");
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
	/* Held, not closed: every audio key comes from this session. */
	g.ap = ap;
	/* Kept so a dropped session can be rebuilt without an app restart. */
	snprintf(g.access_token, sizeof(g.access_token), "%s", tok.access_token);
	snprintf(g.device_id, sizeof(g.device_id), "%s", device_id);
	memcpy(g.cands, cands, (size_t)n * sizeof(ap_candidate));
	g.cand_count = n;

	/* The avatar is no longer fetched here: the Web API rate-limits this
	 * shared client id often enough (HTTP 429) that a blocking call at init
	 * mostly slowed the sign-in down to still come back empty. The cached
	 * URL loads instead, and the art worker keeps retrying the fetch in the
	 * background via native_profile_art_refresh() when there is none. */
	cfg_load_profile_art(g.profile_art, sizeof(g.profile_art));

	step(progress, "Getting the library token...");
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &g.l5, err, errlen) != 0)
		return -1;

	char ignored[128];
	sp_resolve(g.sp_host, sizeof(g.sp_host), ignored, sizeof(ignored));

	LightLock_Init(&g.lock);
	g.want_seek_frame = -1;
	g.state = NATIVE_IDLE;
	g.ready = true;
	g.player = threadCreate(player_thread, NULL, 64 * 1024, 0x30, -2, false);
	if (!g.player) {
		snprintf(err, errlen, "no se pudo crear el hilo del reproductor");
		g.ready = false;
		return -1;
	}
	nlog("INIT: ok, %s via %s\n", g.username, g.sp_host);
	return 0;
}

void native_exit(void)
{
	if (!g.ready)
		return;
	g.quit = true;
	if (g.player) {
		threadJoin(g.player, 2000000000LL);
		threadFree(g.player);
	}
	if (g.ap)
		ap_close(g.ap);
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
		snprintf(rows[i].art, sizeof(rows[i].art), "%s", meta[i].art_small);
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
		static char names[NATIVE_ROWS][64];
		int count = 0;
		int n = sp_rootlist(g.sp_host, token, g.username, uris, names, max,
		                    &count, err, errlen);
		if (n < 0)
			return -1;
		for (int i = 0; i < n; i++) {
			snprintf(rows[i].uri, sizeof(rows[i].uri), "%s", uris[i]);
			snprintf(rows[i].label, sizeof(rows[i].label), "%s",
			         names[i][0] ? names[i] : "Lista sin nombre");
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
