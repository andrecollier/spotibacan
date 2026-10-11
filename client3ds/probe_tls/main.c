/* Phase 1 of the native Spotify client: prove TLS 1.2 works on the console.
 *
 * Handshakes against every host the real client will need, reports what was
 * negotiated and how long each stage took, then fetches apresolve over HTTPS so
 * the whole path -- DNS, TCP, handshake, chain verification, HTTP, decrypt --
 * is shown end to end rather than inferred. On a New 3DS the suite runs twice,
 * at 268 MHz and at 804 MHz, because on the PSP the clock was the difference
 * between a 1.3 s handshake and a 0.25 s one.
 *
 * Results also go to spotify-tls.log beside the .3dsx, since the console's
 * screen holds 30 lines and this prints more.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tls3ds.h"

#define SOC_BUFFER 0x100000

static const struct { const char *host; u16 port; const char *what; } HOSTS[] = {
	{ "apresolve.spotify.com",   443, "which access point to dial" },
	{ "login5.spotify.com",      443, "token exchange" },
	{ "accounts.spotify.com",    443, "device-code pairing" },
	{ "spclient.wg.spotify.com", 443, "metadata, playlists, storage-resolve" },
	{ "i.scdn.co",               443, "cover art" },
};

static FILE *logf;

/* Everything the probe says goes to both the screen and the log. */
static void say(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	/* stderr is libctru's svcOutputDebugString, which an emulator records in
	 * its own log -- the only way to read this run without a camera. */
	fputs(line, stderr);
	if (logf) {
		fputs(line, logf);
		fflush(logf);
	}
}

static void open_log(const char *argv0)
{
	/* Under the Homebrew Launcher argv[0] is sdmc:/3ds/<app>/<app>.3dsx, so the
	 * log lands beside the executable. An emulator loading the .3dsx from a
	 * host path gives an argv[0] the guest cannot open, hence the fallback to
	 * the card root. */
	if (argv0 && argv0[0]) {
		char path[256];
		snprintf(path, sizeof(path), "%s", argv0);
		char *slash = strrchr(path, '/');
		if (slash) {
			slash[1] = '\0';
			strncat(path, "spotify-tls.log", sizeof(path) - strlen(path) - 1);
			logf = fopen(path, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-tls.log", "w");
}

/* A GET over an established connection, just enough to prove the plaintext
 * comes back. Returns the body length, or -1. */
static int https_get(const char *host, const char *path, char *out, size_t outlen)
{
	char err[160];
	tls_stats st;
	tls_conn *c = tls_connect(host, 443, 15000, &st, err, sizeof(err));
	if (!c) {
		say("  GET %s: %s\n", path, err);
		return -1;
	}

	char req[384];
	int n = snprintf(req, sizeof(req),
	                 "GET %s HTTP/1.1\r\nHost: %s\r\n"
	                 "User-Agent: 3ds-spotify/0.1\r\nConnection: close\r\n\r\n",
	                 path, host);
	if (tls_write(c, req, n) != n) {
		say("  GET %s: write failed\n", path);
		tls_close(c);
		return -1;
	}

	size_t have = 0;
	for (;;) {
		int got = tls_read(c, out + have, outlen - 1 - have);
		if (got <= 0)
			break;
		have += got;
		if (have >= outlen - 1)
			break;
	}
	out[have] = '\0';
	tls_close(c);
	return (int)have;
}

static void run_host(int i)
{
	char err[160];
	tls_stats st;

	tls_conn *c = tls_connect(HOSTS[i].host, HOSTS[i].port, 15000, &st,
	                          err, sizeof(err));
	if (!c) {
		say("%-24s FAIL dns %.0f connect %.0f\n", HOSTS[i].host,
		    st.dns_ms, st.connect_ms);
		say("  %s\n", err);
		return;
	}

	say("%-24s %s %s\n", HOSTS[i].host, st.version, st.ciphersuite);
	say("  dns %.0f  tcp %.0f  handshake %.0f ms  verify %s\n",
	    st.dns_ms, st.connect_ms, st.handshake_ms,
	    st.verify_flags ? "FAILED" : "ok");
	tls_close(c);

	/* Straight back to the same host: the ticket from the connection just
	 * closed should skip the key exchange entirely. */
	tls_stats again;
	c = tls_connect(HOSTS[i].host, HOSTS[i].port, 15000, &again,
	                err, sizeof(err));
	if (c) {
		say("  again: handshake %.0f ms%s\n", again.handshake_ms,
		    again.resumed ? " (ticket reused)" : "");
		tls_close(c);
	}
}

static void run_suite(const char *label)
{
	say("\n==== %s ====\n", label);
	for (size_t i = 0; i < sizeof(HOSTS) / sizeof(HOSTS[0]); i++) {
		run_host(i);
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
		if (!aptMainLoop())
			return;
	}

	say("\n-- apresolve over HTTPS --\n");
	char *body = malloc(8192);
	if (!body)
		return;
	int n = https_get("apresolve.spotify.com", "/?type=accesspoint", body, 8192);
	if (n > 0) {
		/* Only the access-point list matters; the headers are noise here. */
		char *json = strstr(body, "\r\n\r\n");
		json = json ? json + 4 : body;
		char *ap = strstr(json, "ap-");
		say("  %d bytes, ap: %.60s\n", n, ap ? ap : json);
	}
	free(body);
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	open_log(argv ? argv[0] : NULL);

	say("3DS Spotify -- TLS 1.2 probe\n");
	say("mbedTLS 3.6.7, built %s\n\n", BUILD_STAMP);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit failed; is Wi-Fi on?\n");
		goto hold;
	}
	/* ps:ps is the console's CSPRNG and the only entropy mbedTLS will get. */
	if (R_FAILED(psInit())) {
		say("psInit failed; no entropy source, refusing to continue.\n");
		goto hold;
	}

	int rc = tls_global_init();
	if (rc) {
		char reason[128];
		tls_strerror(rc, reason, sizeof(reason));
		say("tls_global_init: %s\n", reason);
		goto hold;
	}
	say("trust anchors: %d\n", tls_anchor_count());

	bool is_new = false;
	APT_CheckNew3DS(&is_new);
	say("console: %s 3DS\n", is_new ? "New" : "Old");

	run_suite("268 MHz");
	if (is_new) {
		osSetSpeedupEnable(true);
		run_suite("804 MHz (New 3DS)");
		osSetSpeedupEnable(false);
	}

	say("\nDone. Log written beside the .3dsx.\n");

hold:
	say("\nPress START to exit.\n");
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	if (logf)
		fclose(logf);
	tls_global_exit();
	psExit();
	socExit();
	gfxExit();
	return 0;
}
