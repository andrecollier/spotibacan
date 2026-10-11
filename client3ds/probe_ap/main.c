/* Phase 3: a real session with Spotify's access point.
 *
 * Raw TCP, a Diffie-Hellman handshake whose server key is verified by RSA
 * signature, then Shannon on every packet. Logs in with the stored OAuth token
 * and waits for APWelcome, then holds the session open answering pings, since
 * a handshake that works for one packet and dies at the first ping would not
 * be worth much.
 *
 * No token is printed or logged; only lengths, and the account name the AP
 * itself reports back.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
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
			strncat(p, "spotify-ap.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-ap.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static const char *packet_name(uint8_t cmd)
{
	switch (cmd) {
	case 0x04: return "Ping";
	case 0x4a: return "PongAck";
	case 0x1b: return "CountryCode";
	case 0x50: return "SecretBlock";
	case 0x75: return "ProductInfo";
	case 0x76: return "LegacyWelcome";
	case 0x0d: return "AesKey";
	case 0x0e: return "AesKeyError";
	case 0x09: return "StreamChunkRes";
	case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: return "MercuryReq";
	default:   return "?";
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	const char *argv0 = (argv && argv[0]) ? argv[0] : NULL;
	open_log(argv0);
	cfg_set_path(argv0);

	say("3DS Spotify -- access point (phase 3)\n");
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

	/* 1. The stored refresh token becomes an access token. */
	char stored[512];
	if (!cfg_load_refresh_token(stored, sizeof(stored))) {
		say("No stored token in %s.\nRun probe_pair first.\n", cfg_path());
		goto hold;
	}
	char err[256];
	spotify_tokens tok;
	say("Refreshing the access token...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("  failed: %s\n", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);
	say("  access token %u chars\n", (unsigned)strlen(tok.access_token));

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));
	say("  device id %s\n", device_id);

	/* 2. Which access point to dial. */
	char host[128];
	u16 port = 0;
	if (ap_resolve(host, sizeof(host), &port, err, sizeof(err)) != 0) {
		say("apresolve failed: %s\n", err);
		goto hold;
	}
	say("Access point: %s:%u\n", host, port);

	/* 3. Handshake and login. */
	ap_info info;
	ap_session *s = ap_login(host, port, tok.access_token, device_id, &info,
	                         err, sizeof(err));
	say("  tcp %.0f ms  handshake %.0f ms  login %.0f ms\n",
	    info.connect_ms, info.handshake_ms, info.login_ms);
	if (!s) {
		say("  FAILED: %s\n", err);
		if (info.login_error)
			say("  the AP's own reason: %s\n",
			    ap_login_error_text(info.login_error));
		goto hold;
	}

	say("\n  *** APWelcome ***\n");
	say("  account: %s\n", info.username);
	say("  account type: %d (0 = Spotify)\n", info.account_type);
	say("  reusable credentials: %u bytes (not stored)\n",
	    (unsigned)info.reusable_len);

	/* 4. Hold the session: the AP pings, and a session that cannot answer is
	 *    no use for the audio keys the next phase needs. */
	say("\nHolding the session. B disconnects.\n");
	u8 *buf = malloc(64 * 1024);
	if (!buf) {
		say("out of memory\n");
		ap_close(s);
		goto hold;
	}
	int pings = 0, packets = 0, idle_s = 0;
	/* Long enough to see a second ping: the AP goes quiet for around a minute
	 * between them, and surviving that silence is the point of this part. */
	time_t until = time(NULL) + 150;
	while (aptMainLoop() && time(NULL) < until) {
		hidScanInput();
		if (hidKeysDown() & KEY_B)
			break;

		/* Wait in short slices so the frame keeps drawing and B still works.
		 * Nothing to read is not the end of the session. */
		int ready = ap_wait(s, 100);
		frame();
		if (ready == 0) {
			if (++idle_s % 100 == 0)
				say("  (quiet for %ds)\n", idle_s / 10);
			continue;
		}
		if (ready < 0) {
			say("  connection broken after %d packets\n", packets);
			break;
		}

		u8 cmd;
		int n = ap_recv(s, &cmd, buf, 64 * 1024);
		if (n < 0) {
			say("  read failed after %d packets\n", packets);
			break;
		}
		idle_s = 0;
		packets++;
		if (cmd == AP_PKT_PING) {
			pings++;
			ap_handle_ping(s, buf, (size_t)n);
			say("  Ping -> Pong (%d)\n", pings);
		} else if (cmd == AP_PKT_COUNTRYCODE) {
			char cc[8] = {0};
			memcpy(cc, buf, n < 7 ? (size_t)n : 7);
			say("  CountryCode: %s\n", cc);
		} else {
			say("  packet 0x%02x %s, %d bytes\n", cmd, packet_name(cmd), n);
		}
	}
	say("\n%d packets, %d pings answered.\n", packets, pings);
	free(buf);
	ap_close(s);
	say("Session closed cleanly.\n");

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
