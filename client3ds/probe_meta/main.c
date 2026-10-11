/* Phase 4: the library and the metadata.
 *
 * Chains the whole thing together for the first time: stored refresh token ->
 * OAuth access token -> access point login -> the reusable credentials
 * APWelcome returns -> a login5 Bearer token -> spclient. Then reads the
 * rootlist and a track's metadata, which is where the audio file ids live and
 * therefore what the next phase needs.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"

#define SOC_BUFFER 0x100000

/* "Mr. Brightside" -- a track that is certainly on Spotify and in the
 * catalogue everywhere, so a failure here is this client's fault. */
#define SAMPLE_TRACK "spotify:track:3n3Ppam7vgaVa1iaRUc9Lp"

static FILE *logf;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fputs(line, stderr);
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static void open_log(const char *argv0)
{
	if (argv0 && argv0[0]) {
		char p[320];
		snprintf(p, sizeof(p), "%s", argv0);
		char *slash = strrchr(p, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(p, "spotify-meta.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-meta.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("3DS Spotify -- library & metadata (phase 4)\n");
	say("built %s\n\n", BUILD_STAMP);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed; is Wi-Fi on?\n");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("psInit failed.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	char err[256];
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("Not paired. Run probe_pair first.\n");
		goto hold;
	}

	spotify_tokens tok;
	say("1. OAuth access token...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);
	say("   %u chars\n", (unsigned)strlen(tok.access_token));

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	say("2. Access point...\n");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("   apresolve failed: %s\n", err);
		goto hold;
	}
	say("   %d candidates, first %s:%u\n", ncands, cands[0].host, cands[0].port);
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	if (!ap) {
		say("   all %d failed, last: %s\n", tried, err);
		goto hold;
	}
	say("   %s (attempt %d), %s, %u bytes of credentials\n",
	    info.ap, tried, info.username, (unsigned)info.reusable_len);

	/* The AP session is not needed for the rest of this phase, but closing it
	 * is deliberate rather than accidental: the next phase keeps it open,
	 * because audio keys only come from here. */
	ap_close(ap);

	say("3. login5 token...\n");
	login5_token l5;
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &l5, err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
		goto hold;
	}
	say("   %u chars, valid %ld s\n", (unsigned)strlen(l5.access_token),
	    (long)(l5.expires_at - time(NULL)));

	say("4. spclient host...\n");
	char sp_host[128];
	err[0] = '\0';
	sp_resolve(sp_host, sizeof(sp_host), err, sizeof(err));
	say("   %s%s%s\n", sp_host, err[0] ? "  -- " : "", err[0] ? err : "");

	say("\n5. Track metadata\n");
	sp_track t;
	if (sp_track_metadata(sp_host, l5.access_token, SAMPLE_TRACK, &t,
	                      err, sizeof(err)) != 0) {
		say("   failed: %s\n", err);
	} else {
		say("   %s\n   by %s\n   from %s\n   %d:%02d\n", t.name, t.artist,
		    t.album, t.duration_ms / 60000, (t.duration_ms / 1000) % 60);
		say("   %d audio files:\n", t.file_count);
		for (int i = 0; i < t.file_count; i++) {
			char hex[41];
			for (int j = 0; j < 20; j++)
				snprintf(hex + j * 2, 3, "%02x", t.files[i].id[j]);
			say("     %s  %s\n", hex, sp_format_name(t.files[i].format));
		}
		const sp_file *pick = sp_pick_file(&t);
		say("   pick for this console: %s\n",
		    pick ? sp_format_name(pick->format) : "none playable!");
	}

	say("\n6. Search (no library needed)\n");
	static char found[8][64];
	int nfound = sp_search_tracks(sp_host, l5.access_token, "the killers",
	                              found, 8, err, sizeof(err));
	if (nfound < 0) {
		say("   failed: %s\n", err);
	} else {
		say("   %d track uris; naming them in one batch...\n", nfound);
		static sp_track batch[8];
		static const char *ptrs[8];
		for (int i = 0; i < nfound; i++)
			ptrs[i] = found[i];
		int named = sp_track_metadata_batch(sp_host, l5.access_token, ptrs,
		                                    nfound, batch, err, sizeof(err));
		if (named < 0) {
			say("   batch failed: %s\n", err);
		} else {
			for (int i = 0; i < named; i++) {
				const sp_file *f = sp_pick_file(&batch[i]);
				say("     %s - %s  %d:%02d  %s\n", batch[i].artist,
				    batch[i].name, batch[i].duration_ms / 60000,
				    (batch[i].duration_ms / 1000) % 60,
				    f ? sp_format_name(f->format) : "no playable file");
			}
		}
	}

	say("\n7. Radio from one track (titles come inline)\n");
	static sp_track radio[8];
	int nradio = sp_radio(sp_host, l5.access_token, SAMPLE_TRACK, 8, radio, 8,
	                      err, sizeof(err));
	if (nradio < 0) {
		say("   failed: %s\n", err);
	} else {
		for (int i = 0; i < nradio; i++)
			say("     %s - %s  (%s)\n", radio[i].artist, radio[i].name,
			    radio[i].album);
	}

	say("\n8. Rootlist\n");
	static char uris[16][64];
	int count = 0;
	int got = sp_rootlist(sp_host, l5.access_token, info.username, uris, 16,
	                      &count, err, sizeof(err));
	if (got < 0) {
		say("   failed: %s\n", err);
	} else {
		say("   length field %d, %d uris read\n", count, got);
		for (int i = 0; i < got; i++)
			say("     %s\n", uris[i]);
		if (count == 0 && got == 0)
			say("   (the account's playlist folder is empty)\n");
	}

	say("\n9. Contents of the first playlist\n");
	const char *first = NULL;
	for (int i = 0; i < got; i++)
		if (strncmp(uris[i], "spotify:playlist:", 17) == 0) {
			first = uris[i] + 17;
			break;
		}
	if (!first) {
		say("   no playlist in the rootlist\n");
	} else {
		static char tracks[24][64];
		char plname[128];
		int total = 0;
		int n = sp_playlist(sp_host, l5.access_token, first, plname,
		                    sizeof(plname), tracks, 24, &total,
		                    err, sizeof(err));
		if (n < 0) {
			say("   failed: %s\n", err);
		} else {
			say("   \"%s\": %d tracks, %d read\n", plname, total, n);
			static const char *ptrs2[24];
			int show = n < 8 ? n : 8;
			for (int i = 0; i < show; i++)
				ptrs2[i] = tracks[i];
			static sp_track batch2[8];
			int named = sp_track_metadata_batch(sp_host, l5.access_token,
			                                    ptrs2, show, batch2,
			                                    err, sizeof(err));
			for (int i = 0; i < named; i++)
				say("     %s - %s\n", batch2[i].artist, batch2[i].name);
			if (named > 0 && n > named)
				say("     ... y %d mas\n", n - named);
		}
	}

	say("\n10. Liked Songs\n");
	{
		static char liked[24][64];
		int n = sp_liked(sp_host, l5.access_token, info.username, liked, 24,
		                 err, sizeof(err));
		if (n < 0) {
			say("   failed: %s\n", err);
		} else if (n == 0) {
			say("   (nothing saved)\n");
		} else {
			static const char *ptrs3[8];
			int show = n < 8 ? n : 8;
			for (int i = 0; i < show; i++)
				ptrs3[i] = liked[i];
			static sp_track batch3[8];
			int named = sp_track_metadata_batch(sp_host, l5.access_token,
			                                    ptrs3, show, batch3,
			                                    err, sizeof(err));
			say("   %d liked, showing %d\n", n, named);
			for (int i = 0; i < named; i++)
				say("     %s - %s\n", batch3[i].artist, batch3[i].name);
		}
	}

hold:
	say("\nSTART exits.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}
	if (logf)
		fclose(logf);
	tls_global_exit();
	psExit();
	socExit();
	gfxExit();
	return 0;
}
