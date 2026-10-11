/* Fase 5a completa: de un URI de Spotify a sonido por los parlantes.
 *
 * Signs in, resolves one track, asks the access point for its audio key,
 * downloads it from the CDN, decrypts it and decodes the Vorbis through NDSP.
 * Every step is timed, because this is the first time the whole chain runs on
 * the console and the interesting question is what it costs.
 *
 * The access point session is held open for the key and only for that: it is
 * the one piece no other service will hand over.
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
#include "track.h"
#include "tls3ds.h"

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

#define SOC_BUFFER      0x100000
#define WAVE_BUFFERS    4
#define FRAMES_PER_BUF  4096
/* A four minute track at 160 kbps is about 5 MB; 12 leaves room for longer
 * ones without pretending the whole catalogue fits in RAM. */
#define TRACK_LIMIT     (12 * 1024 * 1024)

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
			strncat(p, "spotify-play.log", sizeof(p) - strlen(p) - 1);
			logf = fopen(p, "w");
		}
	}
	if (!logf)
		logf = fopen("sdmc:/spotify-play.log", "w");
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

	say("3DS Spotify -- reproduccion nativa (fase 5a)\n");
	say("built %s\n\n", BUILD_STAMP);

	u32 *soc = memalign(0x1000, SOC_BUFFER);
	if (!soc || R_FAILED(socInit(soc, SOC_BUFFER))) {
		say("socInit fallo; wifi encendido?\n");
		goto hold;
	}
	if (R_FAILED(psInit())) {
		say("psInit fallo.\n");
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
		say("Sin parear. Corre probe_pair primero.\n");
		goto hold;
	}

	spotify_tokens tok;
	say("1. token OAuth...\n");
	if (token_refresh(stored, &tok, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		goto hold;
	}
	if (tok.refresh_token[0] && strcmp(tok.refresh_token, stored) != 0)
		cfg_save_refresh_token(tok.refresh_token, tok.scope);

	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	say("2. access point...\n");
	ap_candidate cands[AP_MAX_CANDIDATES];
	int ncands = ap_resolve_list(cands, AP_MAX_CANDIDATES, err, sizeof(err));
	if (ncands < 1) {
		say("   %s\n", err);
		goto hold;
	}
	ap_info info;
	int tried = 0;
	ap_session *ap = ap_login_any(cands, ncands, tok.access_token, device_id,
	                              &info, &tried, err, sizeof(err));
	if (!ap) {
		say("   %s\n", err);
		goto hold;
	}
	say("   %s como %s\n", info.ap, info.username);

	say("3. token de biblioteca...\n");
	login5_token l5;
	if (login5_from_credentials(info.username, info.reusable, info.reusable_len,
	                            device_id, &l5, err, sizeof(err)) != 0) {
		say("   %s\n", err);
		ap_close(ap);
		goto hold;
	}
	char sp_host[128], ignored[128];
	sp_resolve(sp_host, sizeof(sp_host), ignored, sizeof(ignored));

	say("4. pista, clave y descarga...\n");
	track_audio tr;
	rc = track_fetch(sp_host, l5.access_token, ap, SAMPLE_TRACK, TRACK_LIMIT,
	                 &tr, err, sizeof(err));
	/* The key was the only thing the session was for. */
	ap_close(ap);
	if (rc != 0) {
		say("   %s\n", err);
		if (rc == AP_KEY_REFUSED)
			say("   (cuenta nueva: probar con una mas antigua)\n");
		goto hold;
	}

	say("   %s - %s\n", tr.meta.artist, tr.meta.name);
	say("   %s, %u bytes\n", sp_format_name(tr.format), (unsigned)tr.len);
	say("   resolve %.0f  key %.0f  download %.0f  decrypt %.0f ms\n",
	    tr.resolve_ms, tr.key_ms, tr.download_ms, tr.decrypt_ms);

	say("5. decodificar y sonar...\n");
	int verr = 0;
	stb_vorbis *v = stb_vorbis_open_memory(track_vorbis(&tr),
	                                       (int)track_vorbis_len(&tr),
	                                       &verr, NULL);
	if (!v) {
		say("   stb_vorbis no pudo abrirlo (error %d)\n", verr);
		track_free(&tr);
		goto hold;
	}
	stb_vorbis_info vi = stb_vorbis_get_info(v);
	say("   %lu Hz, %d canales\n", (unsigned long)vi.sample_rate, vi.channels);

	if (R_FAILED(ndspInit())) {
		say("   ndspInit fallo (emulador sin firmware del DSP)\n");
		stb_vorbis_close(v);
		track_free(&tr);
		goto hold;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)vi.sample_rate);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	const u32 buf_bytes = FRAMES_PER_BUF * 4;
	u8 *audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
	static ndspWaveBuf waves[WAVE_BUFFERS];
	if (!audio) {
		say("   sin memoria lineal\n");
		goto hold;
	}
	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].nsamples = FRAMES_PER_BUF;
		waves[i].status = NDSP_WBUF_DONE;
	}

	say("   sonando. START corta.\n\n");
	unsigned frames = 0;
	int underruns = 0, stalled = 0;
	double decode_ms = 0;
	bool eof = false;
	TickCounter t;

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		bool filled = false;
		for (int i = 0; i < WAVE_BUFFERS && !eof; i++) {
			if (waves[i].status != NDSP_WBUF_DONE &&
			    waves[i].status != NDSP_WBUF_FREE)
				continue;
			s16 *dst = (s16 *)waves[i].data_vaddr;
			osTickCounterStart(&t);
			int got = stb_vorbis_get_samples_short_interleaved(
			              v, vi.channels, dst, FRAMES_PER_BUF * 2);
			osTickCounterUpdate(&t);
			decode_ms += osTickCounterRead(&t);
			if (got <= 0) {
				eof = true;
				break;
			}
			if (got < FRAMES_PER_BUF)
				memset(dst + got * 2, 0, (FRAMES_PER_BUF - got) * 4);
			frames += (unsigned)got;
			waves[i].nsamples = got;
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
			filled = true;
		}

		if (!eof && !filled) {
			int idle = 0;
			for (int i = 0; i < WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_DONE ||
				    waves[i].status == NDSP_WBUF_FREE)
					idle++;
			if (idle == WAVE_BUFFERS)
				underruns++;
			if (++stalled > 1800)
				break;
		} else {
			stalled = 0;
		}

		if (eof) {
			bool playing = false;
			for (int i = 0; i < WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_QUEUED ||
				    waves[i].status == NDSP_WBUF_PLAYING)
					playing = true;
			if (!playing)
				break;
		}
		frame();
	}

	double audio_s = (double)frames / vi.sample_rate;
	say("\n   %.1f s reproducidos, %d underruns\n", audio_s, underruns);
	if (decode_ms > 0)
		say("   decodificador a %.2fx tiempo real\n",
		    audio_s * 1000.0 / decode_ms);

	stb_vorbis_close(v);
	track_free(&tr);
	ndspExit();

hold:
	say("\nSTART sale.\n");
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
