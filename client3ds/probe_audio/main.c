/* Phase 5, the half that does not depend on Spotify: decode Ogg Vorbis on the
 * ARM11 and get it out of NDSP, and measure whether it keeps up.
 *
 * This is the question the PSP port answered by handing Vorbis to the Media
 * Engine, its second processor. The 3DS has no equivalent worth using, so the
 * decode shares the ARM11 with the network and the UI -- and if it does not
 * run comfortably faster than real time here, with nothing else going on, it
 * never will with a TLS stream underneath.
 *
 * Reads <app dir>/test.ogg, falling back to sdmc:/test.ogg.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.h"

/* Four buffers of 4096 frames is about 370 ms of ring at 44.1 kHz: enough to
 * ride out a slow decode without making the UI feel detached. */
#define WAVE_BUFFERS     4
#define FRAMES_PER_BUF   4096

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

static void open_log(const char *argv0, char *dir, size_t dirlen)
{
	dir[0] = '\0';
	if (argv0 && argv0[0]) {
		snprintf(dir, dirlen, "%s", argv0);
		char *slash = strrchr(dir, '/');
		if (slash)
			slash[1] = '\0';
		else
			dir[0] = '\0';
	}
	char p[384];
	snprintf(p, sizeof(p), "%sspotify-audio.log", dir);
	logf = fopen(p, "w");
	if (!logf)
		logf = fopen("sdmc:/spotify-audio.log", "w");
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static u8 *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fclose(f);
		return NULL;
	}
	u8 *buf = malloc((size_t)n);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf)
		*len = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	char dir[320];
	open_log((argv && argv[0]) ? argv[0] : NULL, dir, sizeof(dir));

	say("3DS Spotify -- Vorbis decode (phase 5b)\n");
	say("built %s\n\n", BUILD_STAMP);

	bool is_new = false;
	APT_CheckNew3DS(&is_new);
	say("console: %s 3DS\n", is_new ? "New" : "Old");

	char path[384];
	snprintf(path, sizeof(path), "%stest.ogg", dir);
	size_t len = 0;
	u8 *ogg = read_file(path, &len);
	if (!ogg) {
		ogg = read_file("sdmc:/test.ogg", &len);
		snprintf(path, sizeof(path), "sdmc:/test.ogg");
	}
	if (!ogg) {
		say("No test.ogg beside the .3dsx nor at the card root.\n");
		goto hold;
	}
	say("file: %s  (%u bytes)\n", path, (unsigned)len);

	int err = 0;
	TickCounter t;
	osTickCounterStart(&t);
	stb_vorbis *v = stb_vorbis_open_memory(ogg, (int)len, &err, NULL);
	osTickCounterUpdate(&t);
	if (!v) {
		say("stb_vorbis could not open it (error %d)\n", err);
		goto hold;
	}
	stb_vorbis_info info = stb_vorbis_get_info(v);
	unsigned total = stb_vorbis_stream_length_in_samples(v);
	say("open took %.0f ms\n", osTickCounterRead(&t));
	say("%ld Hz, %d ch, %u frames (%u:%02u)\n",
	    (long)info.sample_rate, info.channels, total,
	    total / info.sample_rate / 60, (total / info.sample_rate) % 60);
	/* What the decoder needs resident, which is the number that decides
	 * whether a second stream could ever run alongside this one. */
	say("decoder memory: %u setup + %u temp bytes\n",
	    info.setup_memory_required, info.temp_memory_required);

	/* Pass 1: decode alone, with no audio output in the way.
	 *
	 * This is the measurement that matters and the only one that is portable:
	 * NDSP needs the console's DSP firmware, which an emulator does not have,
	 * so pass 2 below simply does not run there. Keeping them apart means a
	 * missing DSP costs the playback test, not the benchmark. */
	say("\n-- pass 1: decode only --\n");
	{
		s16 *scratch = malloc(FRAMES_PER_BUF * 4);
		if (!scratch) {
			say("   out of memory\n");
			goto hold;
		}
		double ms = 0, worst = 0;
		unsigned frames = 0;
		int chunks = 0;
		for (;;) {
			osTickCounterStart(&t);
			int got = stb_vorbis_get_samples_short_interleaved(
			              v, info.channels, scratch, FRAMES_PER_BUF * 2);
			osTickCounterUpdate(&t);
			double chunk = osTickCounterRead(&t);
			ms += chunk;
			if (chunk > worst)
				worst = chunk;
			chunks++;
			if (got <= 0)
				break;
			frames += (unsigned)got;
			/* Keep the console responsive on a long file. */
			if ((chunks & 7) == 0) {
				hidScanInput();
				if (hidKeysDown() & KEY_START)
					break;
				frame();
			}
		}
		free(scratch);
		double audio_s = (double)frames / info.sample_rate;
		say("   %u frames = %.1f s of audio\n", frames, audio_s);
		say("   decode cpu %.0f ms over %d chunks (%.1f ms each, worst %.1f)\n",
		    ms, chunks, chunks ? ms / chunks : 0, worst);
		if (ms > 0) {
			say("\n   REALTIME FACTOR: %.2fx\n", audio_s * 1000.0 / ms);
			say("   cpu needed just to decode: %.0f%%\n", ms / (audio_s * 10.0));
		}
		say("   (on an emulator these numbers are the host's, not the ARM11's)\n");
	}

	/* Pass 2: actually play it. */
	say("\n-- pass 2: playback through NDSP --\n");
	if (R_FAILED(ndspInit())) {
		say("   ndspInit failed: no DSP firmware.\n");
		say("   Expected under an emulator; on hardware this should work.\n");
		goto hold;
	}
	stb_vorbis_seek_start(v);
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
	ndspChnSetRate(0, (float)info.sample_rate);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	float mix[12] = {0};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(0, mix);

	const u32 buf_bytes = FRAMES_PER_BUF * 4;          /* stereo s16 */
	u8 *audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
	if (!audio) {
		say("linearAlloc failed\n");
		goto hold;
	}
	static ndspWaveBuf waves[WAVE_BUFFERS];
	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].nsamples = FRAMES_PER_BUF;
		waves[i].status = NDSP_WBUF_DONE;
	}

	say("   playing; START stops.\n\n");

	double decode_ms = 0;
	unsigned frames_out = 0;
	int underruns = 0, refills = 0, stalled = 0;
	bool eof = false;
	double worst_chunk_ms = 0;
	TickCounter wall;
	osTickCounterStart(&wall);

	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;

		bool filled_any = false;
		for (int i = 0; i < WAVE_BUFFERS && !eof; i++) {
			if (waves[i].status != NDSP_WBUF_DONE &&
			    waves[i].status != NDSP_WBUF_FREE)
				continue;

			s16 *dst = (s16 *)waves[i].data_vaddr;
			osTickCounterStart(&t);
			int got = stb_vorbis_get_samples_short_interleaved(
			              v, info.channels, dst, FRAMES_PER_BUF * 2);
			osTickCounterUpdate(&t);
			double ms = osTickCounterRead(&t);
			decode_ms += ms;
			if (ms > worst_chunk_ms)
				worst_chunk_ms = ms;
			refills++;

			if (got <= 0) {
				eof = true;
				break;
			}
			/* A short read at the end would otherwise play stale samples. */
			if (got < FRAMES_PER_BUF)
				memset(dst + got * 2, 0, (FRAMES_PER_BUF - got) * 4);

			frames_out += (unsigned)got;
			waves[i].nsamples = got;
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
			filled_any = true;
		}

		/* Every buffer idle at once means the decoder fell behind and the
		 * channel ran dry -- the thing this pass exists to detect. */
		if (!eof && !filled_any) {
			int idle = 0;
			for (int i = 0; i < WAVE_BUFFERS; i++)
				if (waves[i].status == NDSP_WBUF_DONE ||
				    waves[i].status == NDSP_WBUF_FREE)
					idle++;
			if (idle == WAVE_BUFFERS)
				underruns++;
			stalled++;
		} else {
			stalled = 0;
		}
		/* A DSP that never retires a buffer would otherwise spin here for
		 * ever, which is exactly what a missing firmware looks like. */
		if (stalled > 600) {
			say("   the DSP never returned a buffer; giving up on playback.\n");
			break;
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

	osTickCounterUpdate(&wall);
	double wall_ms = osTickCounterRead(&wall);
	double audio_ms = total ? (double)frames_out * 1000.0 / info.sample_rate : 0;

	say("decoded %u frames = %.1f s of audio\n", frames_out, audio_ms / 1000.0);
	say("decode cpu: %.0f ms over %d refills (%.1f ms each)\n",
	    decode_ms, refills, refills ? decode_ms / refills : 0);
	say("worst single refill: %.1f ms\n", worst_chunk_ms);
	say("wall clock: %.1f s\n", wall_ms / 1000.0);
	if (decode_ms > 0) {
		say("\n  REALTIME FACTOR: %.2fx\n", audio_ms / decode_ms);
		say("  (1.0 = exactly keeps up; the network and UI need the rest)\n");
	}
	say("  cpu spent decoding: %.0f%%\n",
	    wall_ms > 0 ? decode_ms * 100.0 / wall_ms : 0);
	say("  underruns: %d\n", underruns);

	stb_vorbis_close(v);
	free(ogg);
	ndspExit();

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
	gfxExit();
	return 0;
}
