/* Phase 2: pair the console with a Spotify account.
 *
 * Shows a six-character code, waits for it to be entered at spotify.com/pair on
 * another device, stores the refresh token on the SD card, and then spends that
 * stored token straight away to prove it round-trips -- the point of the phase
 * is not getting a token once, it is booting later and still having one.
 *
 * No password is typed on the console and none passes through this code. Tokens
 * are never printed or logged; only their lengths and the granted scopes are.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg3ds.h"
#include "pair3ds.h"
#include "tls3ds.h"

#define SOC_BUFFER 0x100000

static FILE *logf;

static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	/* stderr is svcOutputDebugString, which an emulator records. */
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
			strncat(p, "spotify-pair.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-pair.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

/* Reports a token set without ever revealing one. */
static void describe_tokens(const spotify_tokens *t)
{
	say("  access token:  %u chars\n", (unsigned)strlen(t->access_token));
	say("  refresh token: %u chars\n", (unsigned)strlen(t->refresh_token));
	say("  valid for:     %ld s\n", (long)(t->expires_at - time(NULL)));
	if (t->scope[0])
		say("  scopes: %s\n", t->scope);
}

/* Spends a stored refresh token. This is the check that matters: it is what
 * every later boot will do instead of pairing. */
static bool try_stored_token(void)
{
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored)))
		return false;

	say("Found a stored refresh token (%u chars) in\n  %s\n",
	    (unsigned)strlen(stored), cfg_path());
	say("Spending it for a fresh access token...\n");

	spotify_tokens t;
	char err[192];
	if (token_refresh(stored, &t, err, sizeof(err)) != 0) {
		say("  refresh FAILED: %s\n", err);
		say("  (invalid_grant here means it is dead; pair again.)\n");
		return false;
	}
	say("  refresh ok\n");
	describe_tokens(&t);

	/* Spotify may or may not rotate the refresh token; when it does not, the
	 * stored one is still the live one and must be left alone. */
	if (t.refresh_token[0] && strcmp(t.refresh_token, stored) != 0) {
		say("  rotated; storing the new one\n");
		cfg_save_refresh_token(t.refresh_token, t.scope);
	} else {
		say("  not rotated; keeping the stored one\n");
	}
	return true;
}

static bool do_pairing(void)
{
	pair_request req;
	char err[192];

	say("Asking Spotify for a code...\n");
	if (pair_begin(&req, err, sizeof(err)) != 0) {
		say("pair_begin failed: %s\n", err);
		return false;
	}

	say("\n");
	say("   ================================\n");
	say("        CODE:   %s\n", req.user_code);
	say("   ================================\n");
	say("\n   Enter it at %s\n", req.verification_uri);
	if (req.verification_uri_complete[0])
		say("   or open %s\n", req.verification_uri_complete);
	say("\n   Expires in %d min. B cancels.\n\n", req.expires_in_s / 60);

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	int polls = 0;

	while (aptMainLoop()) {
		/* Wait out the interval a frame at a time, so the console stays
		 * responsive and B still cancels. */
		for (int i = 0; i < interval * 60; i++) {
			hidScanInput();
			if (hidKeysDown() & KEY_B) {
				say("\nCancelled.\n");
				return false;
			}
			frame();
			if (!aptMainLoop())
				return false;
		}
		if (time(NULL) > deadline) {
			say("\nThe code expired. Run it again for a new one.\n");
			return false;
		}

		spotify_tokens t;
		err[0] = '\0';
		pair_status st = pair_poll(&req, &t, err, sizeof(err));
		polls++;

		switch (st) {
		case PAIR_PENDING:
			say("  waiting... (%d)%s%s\n", polls,
			    err[0] ? " " : "", err[0] ? err : "");
			break;
		case PAIR_SLOW_DOWN:
			interval += 5;
			say("  slow_down; backing off to %d s\n", interval);
			break;
		case PAIR_OK:
			say("\nPaired.\n");
			describe_tokens(&t);
			if (!t.refresh_token[0]) {
				say("  but no refresh token came back; cannot persist.\n");
				return false;
			}
			if (!cfg_save_refresh_token(t.refresh_token, t.scope)) {
				say("  FAILED to write %s\n", cfg_path());
				return false;
			}
			say("  saved to %s\n", cfg_path());
			return true;
		case PAIR_DENIED:
			say("\nRefused in the browser: %s\n", err);
			return false;
		case PAIR_EXPIRED:
			say("\nThe code expired: %s\n", err);
			return false;
		case PAIR_ERROR:
			say("\nPairing failed: %s\n", err);
			return false;
		}
	}
	return false;
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("3DS Spotify -- pairing (phase 2)\n");
	say("built %s\n\n", BUILD_STAMP);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed; is Wi-Fi on?\n");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("psInit failed; no entropy source.\n");
		goto hold;
	}
	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}

	/* The console's clock signs nothing here, but a wrong year makes every
	 * certificate look expired, which is worth saying out loud rather than
	 * leaving as a mysterious handshake failure. */
	time_t now = time(NULL);
	say("console clock: %s", ctime(&now));
	say("config: %s\n\n", cfg_path());

	if (!try_stored_token()) {
		say("\nNo usable stored token; pairing.\n\n");
		if (do_pairing()) {
			say("\nNow re-reading it from disk, as a later boot would:\n");
			if (!try_stored_token())
				say("  the stored token did not work. That is the bug to chase.\n");
		}
	} else {
		say("\nAlready paired. X deletes the stored token and exits.\n");
	}

	say("\nSTART exits.\n");

hold:
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START)
			break;
		if (down & KEY_X) {
			say(cfg_clear() ? "Stored token deleted.\n"
			                : "Nothing to delete.\n");
		}
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
