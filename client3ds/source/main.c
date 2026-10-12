// Native Spotify client for the 3DS.
//
// This grew out of a player that streamed raw PCM from a bridge on the LAN.
// The bridge is gone: the console now signs in to Spotify itself, browses the
// account and decodes Ogg Vorbis on the ARM11. What is left of that ancestry,
// and worth keeping, is the shape -- a ring buffer that NDSP drains, filled by
// whoever is producing audio, so the player and the audio path never have to
// know about each other.

#include <3ds.h>
#include <3ds/services/mcuhwc.h>
#include <3ds/services/ndm.h>
#include <citro2d.h>

#include "cfg3ds.h"
#include "lyrics3ds.h"
#include "native.h"
#include "ui/artwork.h"
#include "ui/screen_list.h"
#include "ui/screen_lyrics.h"
#include "ui/screen_player.h"
#include "ui/thumbs.h"
#include "ui/touch.h"
#include "ui/ui.h"
#include "ui/volume_overlay.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOC_BUFFER      0x100000
#define RING_BYTES      (512 * 1024)   // ~5.9 s at 22050 Hz stereo
#define WAVE_BUFFERS    4
#define FRAMES_PER_BUF  2048

typedef struct {
	u8 *data;
	volatile u32 head, tail;      // byte offsets; head == tail means empty
	LightLock lock;
} Ring;

static Ring ring;
static volatile bool running = true;
static volatile u32 underruns = 0;
// Spotify serves 44.1 kHz Vorbis and nothing else reaches the ring.
#define SAMPLE_RATE 44100
static char now_title[128] = "", now_artist[128] = "", now_album[128] = "";
static volatile bool art_pending = false, art_present = false;
static volatile bool server_playing = true;
static volatile float now_pos = 0.0f, now_dur = 0.0f;

// The list scrolls by pixels over the cached rows: sel is the absolute d-pad
// selection, scroll/velocity the touch physics.
typedef struct {
	bool open, loading;
	int total, sel;
	float scroll, velocity;
	char query[64];
} Library;
static Library lib;
static volatile bool lib_reload = false;

static native_view nat_view = NATIVE_PLAYLISTS;
static char nat_arg[64] = "";
static char nat_title[48] = "Listas";
static native_row nat_rows[NATIVE_ROWS];
static int  nat_count = 0;
static char nat_status[64] = "";
// What the cached rows correspond to, so turning a page does not refetch.
static native_view nat_loaded_view = (native_view)-1;
static char nat_loaded_arg[64] = "\x01";

// Touch is rebuilt every frame: each screen registers its hit rects as it
// draws, and the state machine consumes the previous frame's set.
static touch_state g_touch;
static touch_builder g_tb;

// File scope because both the input loop and the drawers consult them.
static bool muted = false;
static bool paused = false;

// Lid-closed playback. Closing the lid normally sleeps the console, and even
// awake it flips the wifi from infrastructure to StreetPass; either one kills
// the stream. While a track is playing or paused, sleep is refused and the
// ndm:u daemon is locked into infrastructure mode (the same recipe
// Universal-Updater uses for its downloads). Released the moment playback
// ends, so a closed lid sleeps normally again when nothing is playing.
static bool lid_mode = false;

static void lid_mode_set(bool on)
{
	if (on == lid_mode)
		return;
	if (on) {
		if (R_FAILED(ndmuInit()))
			return;
		Result res = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
		if (R_SUCCEEDED(res))
			res = NDMU_LockState();
		if (R_FAILED(res)) {
			ndmuExit();
			return;
		}
		aptSetSleepAllowed(false);
		lid_mode = true;
	} else {
		aptSetSleepAllowed(true);
		NDMU_LeaveExclusiveState();
		NDMU_LockState();
		ndmuExit();
		lid_mode = false;
	}
}

// The row the playing track came from, and the view that row belongs to.
// Prev/next are only meaningful while the list still shows that view.
static int now_idx = -1;
static native_view now_view = NATIVE_PLAYLISTS;
static char now_arg[64] = "";

// The hardware slider owns the volume; the overlay only follows it. Mute
// remains a local mix toggle, orthogonal to the slider.
static int volume = 100;        // display value, mirrored from the slider
static u8 vol_slider = 0xFF;    // last raw slider level seen (0..63)
static int vol_hold = 0;
static u8 vol_overlay = 0;

// Lyrics, filled by the art worker: the request carries what LRCLIB wants,
// the doc only becomes readable when lyr_state says so.
static bool lyr_open = false;
static lyrics_doc lyr_doc;
static volatile int lyr_state = LYR_ST_NONE;
static char lyr_error[160] = "";
static char lyr_track_uri[64] = "";
static volatile bool lyr_want = false;
static char lyr_w_track[128], lyr_w_artist[128], lyr_w_album[128];
static long lyr_w_dur = 0;
static float lyr_scroll = 0.0f;

// Audio runs on its own thread: the system keyboard suspends the main loop,
// and four wave buffers hold only a quarter second.
static volatile bool buffering = true;
static volatile u32 prebuffer_bytes = 0;
static ndspWaveBuf waves[WAVE_BUFFERS];
static u8 *art_staging = NULL;

#define ART_PIXELS 128
#define ART_BYTES  (ART_PIXELS * ART_PIXELS * 2)

// One dark palette, in code. The theme loader, the Rockbox backdrops and the
// .wps interpreter belonged to the local-music player this grew out of; a
// streaming client has no use for sixteen looks, and they cost 12 MB on the
// card to carry.
typedef struct {
	u32 top_bg, bottom_bg, panel, accent, on_accent, text, dim, meter, warm;
	bool dark;
} Skin;

#define RGB(r, g, b) C2D_Color32((r), (g), (b), 0xFF)

static Skin SKIN;

static void skin_init(void)
{
	SKIN.top_bg    = RGB(0x12, 0x14, 0x13);
	SKIN.bottom_bg = RGB(0x1B, 0x1E, 0x1C);
	SKIN.panel     = RGB(0x26, 0x2A, 0x27);
	SKIN.accent    = RGB(0x3D, 0xE8, 0x5B);   // the green of the action bar
	SKIN.on_accent = RGB(0x0E, 0x14, 0x10);
	SKIN.text      = RGB(0xF2, 0xF4, 0xF2);
	SKIN.dim       = RGB(0x8E, 0x96, 0x90);
	SKIN.meter     = RGB(0x33, 0x39, 0x35);
	SKIN.warm      = RGB(0xFF, 0x8A, 0x70);   // headings, against all the green
	SKIN.dark      = true;
}

// ---------------------------------------------------------------- ring buffer

static u32 ring_used(void)
{
	u32 h = ring.head, t = ring.tail;
	return (h >= t) ? (h - t) : (RING_BYTES - t + h);
}

static void ring_write(const u8 *src, u32 len)
{
	// Copy in spans. Byte-at-a-time under the lock burned CPU the ARM11
	// cannot spare while the DSP and the socket both want servicing.
	LightLock_Lock(&ring.lock);
	u32 free_bytes = RING_BYTES - 1 - ring_used();
	if (len > free_bytes) len = free_bytes;   // full: drop the excess
	u32 first = RING_BYTES - ring.head;
	if (first > len) first = len;
	memcpy(ring.data + ring.head, src, first);
	if (len > first) memcpy(ring.data, src + first, len - first);
	ring.head = (ring.head + len) % RING_BYTES;
	LightLock_Unlock(&ring.lock);
}

static void ring_clear(void)
{
	LightLock_Lock(&ring.lock);
	ring.tail = ring.head;
	LightLock_Unlock(&ring.lock);
}

static u32 ring_read(u8 *dst, u32 len)
{
	LightLock_Lock(&ring.lock);
	u32 have = ring_used();
	if (len > have) len = have;
	u32 first = RING_BYTES - ring.tail;
	if (first > len) first = len;
	memcpy(dst, ring.data + ring.tail, first);
	if (len > first) memcpy(dst + first, ring.data, len - first);
	ring.tail = (ring.tail + len) % RING_BYTES;
	LightLock_Unlock(&ring.lock);
	return len;
}

// ---------------------------------------------------------------------- http

static void audio_thread(void *arg)
{
	(void)arg;
	const u32 buf_bytes = FRAMES_PER_BUF * 4;
	while (running) {
		if (buffering && ring_used() >= prebuffer_bytes)
			buffering = false;
		if (!buffering) for (int i = 0; i < WAVE_BUFFERS; i++) {
			if (waves[i].status != NDSP_WBUF_DONE && waves[i].status != NDSP_WBUF_FREE)
				continue;
			if (ring_used() < buf_bytes) {
				underruns++;
				buffering = true;
				break;
			}
			u8 *dst = (u8 *)waves[i].data_vaddr;
			u32 got = ring_read(dst, buf_bytes);
			if (got < buf_bytes) memset(dst + got, 0, buf_bytes - got);
			DSP_FlushDataCache(dst, buf_bytes);
			ndspChnWaveBufAdd(0, &waves[i]);
		}
		svcSleepThread(5000000LL);   // 5 ms
	}
}

// Pulls one view from Spotify and caches the rows. The fetch is cached by
// view and argument: scrolling must not cost a round trip, because on this
// console round trips are the expensive part.
static void library_fetch(void)
{
	const char *arg = (nat_view == NATIVE_SEARCH) ? lib.query : nat_arg;
	bool same = nat_view == nat_loaded_view &&
	            strcmp(arg ? arg : "", nat_loaded_arg) == 0;

	if (!same) {
		lib.loading = true;
		char err[160] = "";
		int n = native_fetch(nat_view, arg, nat_rows, NATIVE_ROWS,
		                     err, sizeof(err));
		if (n < 0) {
			nat_count = 0;
			snprintf(nat_status, sizeof(nat_status), "%s", err);
		} else {
			nat_count = n;
			nat_status[0] = '\0';
			nat_loaded_view = nat_view;
			snprintf(nat_loaded_arg, sizeof(nat_loaded_arg), "%s",
			         arg ? arg : "");
		}
	}

	// The playlists view carries one synthetic row for Liked Songs, which is
	// not in the rootlist but is where most people look first.
	lib.total = nat_count + (nat_view == NATIVE_PLAYLISTS ? 1 : 0);
	if (lib.sel >= lib.total) lib.sel = lib.total ? lib.total - 1 : 0;
	lib.loading = false;
}

// A library fetch takes about a second against Spotify, so it never runs on
// the render loop.
static void native_library_thread(void *arg)
{
	(void)arg;
	while (running) {
		if (lib_reload) {
			lib_reload = false;
			library_fetch();
		}
		svcSleepThread(30000000LL);   // 30 ms
	}
}

// The art worker: fetches and decodes covers off the render loop. The hero
// goes first -- it is what the user is looking at; the row thumbnails queue
// behind it. Pixels land in staging buffers and the render thread uploads
// them, because textures belong to the GPU context's thread.
static volatile char hero_want[80];
static volatile bool hero_new = false;
static char hero_loaded[80] = "";

static void art_thread(void *arg)
{
	(void)arg;
	u64 profile_try_at = 0;
	while (running) {
		/* The avatar's first fetch races the Web API rate limit (HTTP 429 on
		 * this shared client id), so retry it here every 30 s until it
		 * lands. The same backoff re-offers the hero when the URL is known
		 * but the decode failed: getting the URL and getting the pixels are
		 * two separate round trips, and either can lose. It only takes the
		 * hero while nothing has played yet: once track art owns the
		 * screen, the avatar no longer displaces it. */
		if (!hero_loaded[0] && !hero_new) {
			const u64 now = osGetTime();
			if (!profile_try_at || now - profile_try_at >= 30000) {
				profile_try_at = now;
				const char *part = native_profile_art();
				if (!part[0]) {
					native_profile_art_refresh();
					part = native_profile_art();
				}
				if (part[0]) {
					snprintf((char *)hero_want, 80, "%s", part);
					__sync_synchronize();
					hero_new = true;
				}
			}
		}
		if (hero_new) {
			char url[80];
			snprintf(url, sizeof(url), "%s", (const char *)hero_want);
			hero_new = false;
			char err[128] = "";
			if (url[0] &&
			    artwork_fetch_decode(url, ART_PIXELS, art_staging,
			                         err, sizeof(err))) {
				snprintf(hero_loaded, sizeof(hero_loaded), "%s", url);
				__sync_synchronize();
				art_pending = true;
			}
			continue;
		}
		if (lyr_want) {
			lyr_want = false;
			char err[160] = "";
			lyrics_result rc =
			    lyrics_fetch(lyr_w_track, lyr_w_artist, lyr_w_album,
			                 lyr_w_dur, &lyr_doc, err, sizeof(err));
			if (rc == LYR_ERR)
				snprintf(lyr_error, sizeof(lyr_error), "%s", err);
			__sync_synchronize();
			lyr_state = rc == LYR_OK ? LYR_ST_OK :
			            rc == LYR_NONE ? LYR_ST_NONE : LYR_ST_ERR;
			continue;
		}
		if (thumbs_work_pending()) {
			thumbs_work_one();
			continue;
		}
		svcSleepThread(20000000LL);   // 20 ms
	}
}

// -------------------------------------------------------------------- config

// ------------------------------------------------------------------ drawing

#define TOP_W 400.0f
#define BOT_W 320.0f
#define SCR_H 240.0f

static C2D_TextBuf text_buf;
static C3D_Tex art_tex;
static Tex3DS_SubTexture art_sub = {ART_PIXELS, ART_PIXELS, 0.0f, 1.0f, 1.0f, 0.0f};

// Cuts a string to fit a width, with an ellipsis. Without this a long artist
// name simply runs past the edge of the screen and is clipped mid-letter.
static void text_fit(const char *str, float x, float y, float scale, u32 colour,
                     float max_w)
{
	if (!str || !*str) return;
	char buf[192];
	snprintf(buf, sizeof(buf), "%s", str);

	C2D_Text t;
	float w = 0, h = 0;
	C2D_TextParse(&t, text_buf, buf);
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	if (w > max_w) {
		size_t n = strlen(buf);
		while (n > 1 && w > max_w) {
			n--;
			buf[n] = '\0';
			C2D_TextParse(&t, text_buf, buf);
			C2D_TextGetDimensions(&t, scale, scale, &w, &h);
		}
		if (n > 3) {
			buf[n - 1] = '.';
			buf[n] = '.';
			buf[n + 1] = '\0';
		}
		C2D_TextParse(&t, text_buf, buf);
	}
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, colour);
}

static void clock_text(char *out, size_t n, float secs);

static void text_at(const char *str, float x, float y, float scale, u32 colour, u32 align)
{
	if (!str || !*str) return;
	C2D_Text t;
	C2D_TextParse(&t, text_buf, str);
	C2D_TextOptimize(&t);
	C2D_DrawText(&t, C2D_WithColor | align, x, y, 0.5f, scale, scale, colour);
}

// A square stand-in for the hundreds of tracks that carry no embedded cover.
static void draw_no_art(const Skin *sk, float x, float y, float size)
{
	C2D_DrawRectSolid(x, y, 0.0f, size, size, sk->panel);
	float pad = size * 0.28f;
	C2D_DrawRectSolid(x + pad, y + pad, 0.1f, size - pad * 2, size * 0.10f, sk->accent);
	C2D_DrawRectSolid(x + pad, y + size * 0.5f, 0.1f, size - pad * 2, size * 0.10f, sk->meter);
	C2D_DrawRectSolid(x + pad, y + size * 0.68f, 0.1f, (size - pad * 2) * 0.6f, size * 0.10f, sk->meter);
}

static void draw_art(const Skin *sk, float x, float y, float size)
{
	if (art_present) {
		C2D_Image img = {&art_tex, &art_sub};
		C2D_DrawImageAt(img, x, y, 0.1f, NULL, size / ART_PIXELS, size / ART_PIXELS);
	} else {
		draw_no_art(sk, x, y, size);
	}
}

// Stereo depth: each element shifts horizontally by its own amount, so the
// cover sits clearly in front of the text and the text in front of the panel.
// par is signed per eye and already scaled by the 3D slider.
static void draw_top(const Skin *sk, bool buffering, bool muted, float par)
{
	// Art on the left with a real margin, text beside it, and nothing else.
	// The text column is measured, so a long title stops at the edge instead
	// of running off it.
	const float ART = 152.0f;
	const float AX = 20.0f, AY = 44.0f;
	const float TX = AX + ART + 22.0f;
	const float TW = TOP_W - TX - 20.0f;

	draw_art(sk, AX + par * 3.0f, AY, ART);

	text_fit(now_title[0] ? now_title : "Nada sonando", TX + par * 2.0f, AY + 2,
	         0.70f, sk->text, TW);
	text_fit(now_artist, TX + par * 1.4f, AY + 36, 0.54f, sk->dim, TW);
	text_fit(now_album, TX + par * 1.0f, AY + 60, 0.46f, sk->meter, TW);

	const char *state = buffering ? "CARGANDO" :
	                    paused    ? "PAUSA" :
	                    muted     ? "SIN SONIDO" : "SONANDO";
	C2D_DrawCircleSolid(TX + 5 + par * 0.8f, AY + 112, 0.1f, 4,
	                    buffering ? sk->warm : sk->accent);
	text_at(state, TX + 18 + par * 0.8f, AY + 103, 0.46f, sk->dim, 0);

	if (now_dur > 1.0f) {
		float done = now_pos / now_dur;
		if (done > 1.0f) done = 1.0f;
		float bx = TX + par * 0.6f, by = AY + 140, bw = TW;
		C2D_DrawRectSolid(bx, by, 0.0f, bw, 3, sk->meter);
		C2D_DrawRectSolid(bx, by, 0.1f, bw * done, 3, sk->accent);
		char a[12], b[12];
		clock_text(a, sizeof(a), now_pos);
		clock_text(b, sizeof(b), now_dur);
		text_at(a, bx, by + 8, 0.42f, sk->dim, 0);
		text_at(b, bx + bw, by + 8, 0.42f, sk->dim, C2D_AlignRight);
	}

	// Last, above whatever the top screen shows: it only reports, and the
	// bottom screen keeps its controls uncovered.
	volume_overlay_args va = { text_buf, volume, vol_overlay };
	volume_overlay_draw(&va);
}

#define BAND_H 40.0f          // title band across the top
#define BAR_Y  198.0f         // the full-width action bar (setup screens)

// Defined with the playback helpers below; the list needs them for the
// now-playing badge.
static bool now_matches_view(void);

static const C2D_Image *list_thumb(const char *url, void *ctx)
{
	(void)ctx;
	return thumbs_get(url);
}

// The visible window over the cached rows, rebuilt every frame: the list
// scrolls by pixels, so the window slides with it rather than in pages.
static void draw_native_list(const Skin *sk)
{
	// While the fetch thread is between views, the cached rows still belong
	// to the previous one. Drawing them under the new title reads as a
	// broken list, so the window stays empty and the screen shows the
	// loading state until the cache matches what is on screen.
	const char *want_arg = (nat_view == NATIVE_SEARCH) ? lib.query : nat_arg;
	bool loading = lib.loading || nat_view != nat_loaded_view ||
	               strcmp(want_arg ? want_arg : "", nat_loaded_arg) != 0;

	int extra = (nat_view == NATIVE_PLAYLISTS) ? 1 : 0;
	int first = (!loading && lib.total) ? (int)(lib.scroll / SL_ROW_H) : 0;
	float frac = loading ? 0.0f : lib.scroll - first * SL_ROW_H;

	sl_row rows[8];
	int n = 0;
	if (!loading) {
		for (int i = first; i < lib.total && n < 7; i++) {
			sl_row *r = &rows[n++];
			if (extra && i == 0) {
				*r = (sl_row){"Me gusta", "", "", true, false};
			} else {
				const native_row *nr = &nat_rows[i - extra];
				*r = (sl_row){nr->label, nr->sub, nr->art, nr->is_playlist,
				              now_matches_view() && i - extra == now_idx &&
				              native_playback_state() == NATIVE_PLAYING};
			}
		}
	}

	char count[32] = "";
	if (!loading && lib.total > 0)
		snprintf(count, sizeof(count), "%d / %d", lib.sel + 1, lib.total);

	screen_list_args a = {0};
	a.buf = text_buf;
	a.tb = &g_tb;
	a.title = nat_title;
	a.can_back = nat_view != NATIVE_PLAYLISTS;
	a.count = count[0] ? count : NULL;
	a.empty = loading ? "Cargando" :
	          nat_status[0] ? nat_status : "Nada por aqui";
	a.loading = loading;
	a.rows = rows;
	a.n = n;
	a.first = first;
	a.frac = frac;
	a.total = loading ? 0 : lib.total;
	a.sel = lib.sel;
	a.pressed_id = g_touch.down ? g_touch.press_id : -1;
	a.animation_ms = (unsigned)osGetTime();
	a.thumb = list_thumb;
	a.accent = sk->accent;
	a.text = sk->text;
	a.dim = sk->dim;
	a.panel = sk->panel;
	a.meter = sk->meter;
	screen_list_draw(&a);
}


static void clock_text(char *out, size_t n, float secs)
{
	int t = (int)(secs < 0 ? 0 : secs);
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}


static void draw_bottom(const Skin *sk)
{
	if (lyr_open) {
		screen_lyrics_args a = {0};
		a.buf = text_buf;
		a.tb = &g_tb;
		a.track = now_title[0] ? now_title : "Nada sonando";
		a.state = (screen_lyrics_state)lyr_state;
		a.error = lyr_error;
		a.doc = &lyr_doc;
		a.current = lyr_state == LYR_ST_OK
		            ? lyrics_index_at(&lyr_doc,
		                              (uint32_t)(now_pos * 1000.0f))
		            : -1;
		a.scroll = lyr_scroll;
		a.position_ms = (long)(now_pos * 1000.0f);
		a.pressed_id = g_touch.down ? g_touch.press_id : -1;
		a.animation_ms = (unsigned)osGetTime();
		a.accent = sk->accent;
		a.text = sk->text;
		a.dim = sk->dim;
		a.panel = sk->panel;
		a.meter = sk->meter;
		screen_lyrics_draw(&a);
	} else if (lib.open) {
		draw_native_list(sk);
	} else {
		// The player owns the whole bottom screen. Its scrubber only
		// reports: the backend streams a bounded fetch and there is
		// nothing to seek inside yet.
		native_state st = native_playback_state();
		bool match = now_matches_view();

		screen_player_args a = {0};
		a.buf = text_buf;
		a.tb = &g_tb;
		a.track = now_title[0] ? now_title : "Nada sonando";
		a.artist = now_artist;
		a.album = now_album;
		a.playing = st == NATIVE_PLAYING && !paused;
		a.loading = st == NATIVE_LOADING;
		a.muted = muted;
		a.can_prev = match && now_idx > 0;
		a.can_next = match && now_idx + 1 < nat_count;
		a.progress_ms = (long)(now_pos * 1000.0f);
		a.duration_ms = (long)(now_dur * 1000.0f);
		a.pressed_id = g_touch.down ? g_touch.press_id : -1;
		a.animation_ms = (unsigned)osGetTime();
		a.accent = sk->accent;
		a.on_accent = sk->on_accent;
		a.panel = sk->panel;
		screen_player_draw(&a);
	}
	text_at(BUILD_STAMP, BOT_W - 6, SCR_H - 14, 0.40f, sk->meter, C2D_AlignRight);
}

// Starts a track from the cached rows and remembers where it sits, which is
// what gives prev/next their bounds. Unpausing matters: a skip lands while
// the channel may be paused, and NDSP would stay silent otherwise.
static void play_index(int idx)
{
	native_play(nat_rows[idx].uri);
	now_idx = idx;
	now_view = nat_view;
	snprintf(now_arg, sizeof(now_arg), "%s",
	         nat_view == NATIVE_SEARCH ? lib.query : nat_arg);
	buffering = true;
	paused = false;
	ndspChnSetPaused(0, false);
	ring_clear();
}

// Whether the list still shows the view the playing track came from.
static bool now_matches_view(void)
{
	const char *arg = nat_view == NATIVE_SEARCH ? lib.query : nat_arg;
	return now_idx >= 0 && now_view == nat_view &&
	       strcmp(now_arg, arg ? arg : "") == 0;
}

// What activating an absolute list row means: the synthetic Liked Songs row,
// drilling into a playlist, or playing a track.
static void activate_row(int abs)
{
	int extra = (nat_view == NATIVE_PLAYLISTS) ? 1 : 0;
	int idx = abs - extra;
	if (extra && abs == 0) {
		nat_view = NATIVE_LIKED;
		nat_arg[0] = '\0';
		snprintf(nat_title, sizeof(nat_title), "Me gusta");
	} else if (idx >= 0 && idx < nat_count && nat_rows[idx].is_playlist) {
		snprintf(nat_arg, sizeof(nat_arg), "%s", nat_rows[idx].uri);
		if (native_playlist_name(nat_arg, nat_title, sizeof(nat_title)) != 0)
			snprintf(nat_title, sizeof(nat_title), "Lista");
		nat_view = NATIVE_TRACKS;
	} else if (idx >= 0 && idx < nat_count) {
		play_index(idx);
		return;
	} else {
		return;
	}
	lib.sel = 0;
	lib.scroll = lib.velocity = 0.0f;
	lib_reload = true;
}

// Opens the lyrics view, queueing a fetch when the track changed since the
// last document. The art worker does the round trip; the view shows its
// state until the answer lands.
static void request_lyrics(void)
{
	lyr_open = true;
	const char *uri = native_now_uri();
	if (!uri[0] || strcmp(uri, lyr_track_uri) == 0)
		return;
	snprintf(lyr_track_uri, sizeof(lyr_track_uri), "%s", uri);
	snprintf(lyr_w_track, sizeof(lyr_w_track), "%s", native_now_title());
	snprintf(lyr_w_artist, sizeof(lyr_w_artist), "%s", native_now_artist());
	snprintf(lyr_w_album, sizeof(lyr_w_album), "%s", native_now_album());
	lyr_w_dur = (long)(native_duration() * 1000.0f);
	lyr_scroll = 0.0f;
	lyr_state = LYR_ST_LOADING;
	__sync_synchronize();
	lyr_want = true;
}

static void fail_screen(const char *why)
{
	consoleInit(GFX_TOP, NULL);
	printf("\x1b[2J\x1b[2;2H%s\n\n  Press START to exit.\n", why);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}
}

static unsigned ring_free_space(void)
{
	return RING_BYTES - 1 - ring_used();
}

static void ring_sink_write(const unsigned char *pcm, unsigned len)
{
	ring_write(pcm, len);
}

// Each step of the native sign-in takes a visible moment, so it says which.
static char setup_step[64] = "";
static char setup_code[16] = "";
static char setup_url[128] = "";
static char setup_error[192] = "";

static C3D_RenderTarget *g_top, *g_bottom;

// One painted screen for everything that happens before the library exists:
// signing in, pairing, failing. Same palette and spacing as the rest.
static void draw_setup(void)
{
	const Skin *sk = &SKIN;
	C2D_TextBufClear(text_buf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

	C2D_TargetClear(g_top, sk->top_bg);
	C2D_SceneBegin(g_top);
	if (setup_error[0]) {
		text_at("No se pudo entrar", 28, 40, 0.80f, sk->warm, 0);
		text_fit(setup_error, 28, 86, 0.50f, sk->text, TOP_W - 56);
	} else if (setup_code[0]) {
		text_at("Vincula tu cuenta", 28, 36, 0.80f, sk->warm, 0);
		text_at("En el navegador, entra a", 28, 84, 0.50f, sk->dim, 0);
		text_fit(setup_url, 28, 108, 0.52f, sk->accent, TOP_W - 56);
		text_at("y escribe el codigo que aparece abajo.", 28, 146, 0.50f,
		        sk->dim, 0);
	} else {
		text_at("Spotify", 28, 40, 0.90f, sk->warm, 0);
		text_fit(setup_step, 28, 96, 0.52f, sk->dim, TOP_W - 56);
	}

	C2D_TargetClear(g_bottom, sk->bottom_bg);
	C2D_SceneBegin(g_bottom);
	if (setup_code[0]) {
		C2D_DrawRectSolid(0, 0, 0.0f, BOT_W, BAND_H, sk->panel);
		text_at("CODIGO", 16, 12, 0.46f, sk->dim, 0);
		text_at(setup_code, BOT_W / 2, 74, 1.30f, sk->text, C2D_AlignCenter);
		C2D_DrawRectSolid(0, BAR_Y, 0.0f, BOT_W, SCR_H - BAR_Y, sk->accent);
		text_at("B  Cancelar", BOT_W / 2, BAR_Y + 10, 0.54f, sk->on_accent,
		        C2D_AlignCenter);
	} else if (setup_error[0]) {
		C2D_DrawRectSolid(0, BAR_Y, 0.0f, BOT_W, SCR_H - BAR_Y, sk->accent);
		text_at("START  Salir", BOT_W / 2, BAR_Y + 10, 0.54f, sk->on_accent,
		        C2D_AlignCenter);
	} else {
		text_fit(setup_step, 16, 100, 0.50f, sk->dim, BOT_W - 32);
	}
	C3D_FrameEnd(0);
}

static void native_progress(const char *step)
{
	snprintf(setup_step, sizeof(setup_step), "%s", step);
	draw_setup();
}

// The pairing code has to stay readable while the user walks to a phone, so
// it is printed once and the waiting happens under it.
static void pair_show_code(const char *code, const char *url)
{
	snprintf(setup_code, sizeof(setup_code), "%s", code);
	snprintf(setup_url, sizeof(setup_url), "%s", url);
	draw_setup();
}

static bool pair_poll_frame(int attempt)
{
	(void)attempt;
	hidScanInput();
	if (hidKeysDown() & KEY_B)
		return false;
	draw_setup();
	return aptMainLoop();
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	skin_init();
	// The token belongs beside the .3dsx, like every other app here; without
	// this it would land at the card root and not match the probes.
	cfg_set_path((argv && argv[0]) ? argv[0] : NULL);

	// Everything that can fail runs first, while a text console is still the
	// only thing initialised and can report why.
	const char *fail = NULL;
	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	if (!soc_buffer || socInit(soc_buffer, SOC_BUFFER) != 0)
		fail = "Network init failed.";

	LightLock_Init(&ring.lock);
	ring.data = malloc(RING_BYTES);
	art_staging = malloc(ART_BYTES);
	if (!fail && (!ring.data || !art_staging)) fail = "Out of memory.";

	const u32 buf_bytes = FRAMES_PER_BUF * 4;   // stereo, 16-bit
	u8 *audio = NULL;
	float mix[12] = {0};
	if (!fail) {
		ndspInit();
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
		ndspChnSetRate(0, (float)SAMPLE_RATE);
		ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		mix[0] = mix[1] = 1.0f;
		ndspChnSetMix(0, mix);
		mcuHwcInit();   // the overlay follows the hardware volume slider
		audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
		if (!audio) fail = "Audio memory failed.";
	}
	if (fail) { fail_screen(fail); gfxExit(); return 1; }

	memset(waves, 0, sizeof(waves));
	for (int i = 0; i < WAVE_BUFFERS; i++) {
		waves[i].data_vaddr = audio + i * buf_bytes;
		waves[i].nsamples = FRAMES_PER_BUF;
		waves[i].status = NDSP_WBUF_DONE;
	}

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	// Up before signing in, so pairing and progress are drawn screens rather
	// than a text console.
	// No stereo. The right-eye buffer was only cleared when the slider was
	// up, so with 3D off it kept whatever uninitialised memory it had -- and
	// the hardware showed it. Nothing here needs depth, and this halves the
	// drawing besides.
	gfxSet3D(false);
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_buf = C2D_TextBufNew(4096);
	ui_init();
	thumbs_init();
	g_top = top;
	g_bottom = bottom;

	{
		static char nat_err[192];
		static const native_sink sink = { ring_sink_write, ring_free_space };
		snprintf(setup_step, sizeof(setup_step), "Iniciando...");
		draw_setup();

		// First run on a console: pair here rather than in a separate tool.
		bool ok = true;
		if (!native_has_token())
			ok = native_pair(pair_show_code, pair_poll_frame, nat_err,
			                 sizeof(nat_err)) == 0;
		setup_code[0] = '\0';
		if (ok)
			ok = native_init(&sink, native_progress, nat_err,
			                 sizeof(nat_err)) == 0;
		if (!ok) {
			snprintf(setup_error, sizeof(setup_error), "%s", nat_err);
			while (aptMainLoop()) {
				hidScanInput();
				if (hidKeysDown() & KEY_START) break;
				draw_setup();
			}
			ui_exit();
			C2D_Fini();
			C3D_Fini();
			gfxExit();
			return 1;
		}
	}
	C3D_TexInit(&art_tex, ART_PIXELS, ART_PIXELS, GPU_RGB565);
	C3D_TexSetFilter(&art_tex, GPU_LINEAR, GPU_LINEAR);

	// Three threads beside the render loop: one services library requests,
	// one moves the ring into NDSP, one fetches and decodes cover art.
	Thread lister = threadCreate(native_library_thread, NULL, 32 * 1024, 0x3F, -2, false);
	Thread snd = threadCreate(audio_thread, NULL, 16 * 1024, 0x18, -2, false);
	Thread art = threadCreate(art_thread, NULL, 48 * 1024, 0x32, -2, false);
	buffering = false;
	lib.open = true;
	lib_reload = true;
	snprintf(now_artist, sizeof(now_artist), "%s", native_account());
	/* The idle hero is the user's own avatar; the art thread fetches it (and
	 * retries it past the Web API rate limit) and hands it over as soon as it
	 * lands, while nothing has played yet. */
	int nat_state_seen = -1;

	// One second: the link measured comfortable at 1024 kbps, and every extra
	// second of cushion is a second of lag after a skip.
	prebuffer_bytes = (u32)SAMPLE_RATE * 4;


	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) break;
		bool open_search = false;

		// The rects belong to the frame just drawn, so this runs before any
		// of them are rebuilt below.
		touch_update(&g_touch, g_tb.rects, g_tb.n);

		// A scrub is a drag, not a tap: the position follows the finger while
		// held (a preview only -- the decoder keeps playing), and the seek
		// fires on release.
		static bool scrubbing = false;
		if (!lib.open && !lyr_open && now_dur > 1.0f) {
			if (g_touch.pressed && g_touch.press_id == BTN_P_SCRUB)
				scrubbing = true;
		} else {
			scrubbing = false;
		}
		if (scrubbing) {
			if (g_touch.down) {
				float frac =
				    (g_touch.px - SCRUB_BAR_X) / SCRUB_BAR_W;
				if (frac < 0.0f) frac = 0.0f;
				if (frac > 1.0f) frac = 1.0f;
				now_pos = frac * now_dur;
			} else {
				native_seek(now_pos);
				scrubbing = false;
			}
		}

		// Player-mode touches fire on release.
		if (!lib.open && !lyr_open && g_touch.clicked >= 0) {
			// The same bounds the draw used to grey prev/next out: a
			// greyed control still has a rect, so the tap is checked here.
			bool match = now_matches_view();
			switch (g_touch.clicked) {
			case BTN_P_PLAY:
				if (native_playback_state() == NATIVE_PLAYING) {
					paused = !paused;
					ndspChnSetPaused(0, paused);
				}
				break;
			case BTN_P_PREV:
				if (match && now_idx > 0)
					play_index(now_idx - 1);
				break;
			case BTN_P_NEXT:
				if (match && now_idx + 1 < nat_count)
					play_index(now_idx + 1);
				break;
			case BTN_P_LIBRARY:
				lib.open = true;
				lib_reload = true;
				break;
			case BTN_P_LYRICS:
				request_lyrics();
				break;
			case BTN_P_MUTE:
				muted = !muted;
				mix[0] = mix[1] = muted ? 0.0f : 1.0f;   // slider owns the level
				ndspChnSetMix(0, mix);
				break;
			}
		}

		{
			// The player runs on its own thread; this just mirrors it.
			native_state st = native_playback_state();
			if (st != nat_state_seen) {
				nat_state_seen = (int)st;
				if (st == NATIVE_PLAYING)
					lid_mode_set(true);
				else if (st == NATIVE_IDLE || st == NATIVE_FAILED)
					lid_mode_set(false);
				switch (st) {
				case NATIVE_LOADING:
					snprintf(now_title, sizeof(now_title), "Cargando...");
					now_artist[0] = '\0';
					now_album[0] = '\0';
					break;
				case NATIVE_FAILED:
					snprintf(now_title, sizeof(now_title), "No se pudo reproducir");
					snprintf(now_artist, sizeof(now_artist), "%s",
					         native_last_error());
					break;
				default:
					break;
				}
			}
			if (st == NATIVE_PLAYING) {
				snprintf(now_title, sizeof(now_title), "%s", native_now_title());
				snprintf(now_artist, sizeof(now_artist), "%s", native_now_artist());
				snprintf(now_album, sizeof(now_album), "%s", native_now_album());
				now_pos = scrubbing ? now_pos : native_position();
				now_dur = native_duration();
				server_playing = true;
				// The cover follows the track; the art worker fetches it and
				// the placeholder shows until it lands.
				const char *art = native_now_art();
				if (!hero_new && strcmp(art, hero_loaded) != 0) {
					snprintf((char *)hero_want, 80, "%s", art);
					__sync_synchronize();
					hero_new = true;
					art_present = false;
				}
			} else if (paused) {
				// The track ended or failed under a paused channel; do not
				// carry the pause into whatever plays next.
				paused = false;
				ndspChnSetPaused(0, false);
			}
		}

		if (lib.open) {
			if (down & KEY_DUP && lib.sel > 0) lib.sel--;
			if (down & KEY_DDOWN && lib.sel + 1 < lib.total) lib.sel++;
			if (down & KEY_L) lib.sel = lib.sel >= 5 ? lib.sel - 5 : 0;
			if ((down & KEY_R) && lib.total)
				lib.sel = lib.sel + 5 < lib.total ? lib.sel + 5
				                                  : lib.total - 1;

			// Keep the d-pad selection inside the window, but only when it
			// just moved: applied unconditionally this clamp drags the
			// window back over any touch scrolling, anchoring the list to
			// whatever row the buttons last highlighted.
			static int prev_sel = -1;
			if (lib.total && lib.sel != prev_sel) {
				float top = lib.sel * SL_ROW_H, bottom = top + SL_ROW_H;
				if (top < lib.scroll) lib.scroll = top;
				else if (bottom > lib.scroll + SL_CONTENT_H)
					lib.scroll = bottom - SL_CONTENT_H;
			}
			prev_sel = lib.sel;

			// Drag 1:1 while held, then coast: retain a filtered portion of
			// the final motion and decay it after release. A fresh touch
			// always catches the list immediately.
			if (g_touch.pressed)
				lib.velocity = 0.0f;
			if (g_touch.down && g_touch.dragging) {
				float delta = -(float)g_touch.dy;
				lib.scroll += delta;
				lib.velocity = lib.velocity * 0.25f + delta * 0.75f;
				if (lib.velocity > SL_FLING_MAX) lib.velocity = SL_FLING_MAX;
				if (lib.velocity < -SL_FLING_MAX) lib.velocity = -SL_FLING_MAX;
			} else if (!g_touch.down) {
				lib.scroll += lib.velocity;
				lib.velocity *= SL_FLING_FRICTION;
				if (lib.velocity > -SL_FLING_STOP &&
				    lib.velocity < SL_FLING_STOP)
					lib.velocity = 0.0f;
			}
			float maxs = screen_list_max_scroll(lib.total);
			if (lib.scroll < 0.0f) { lib.scroll = 0.0f; lib.velocity = 0.0f; }
			if (lib.scroll > maxs) { lib.scroll = maxs; lib.velocity = 0.0f; }

			if (g_touch.clicked == SL_BTN_SEARCH)
				open_search = true;
			else if (g_touch.clicked == SL_BTN_CLOSE)
				lib.open = false;
			else if (g_touch.clicked == SL_BTN_BACK &&
			         nat_view != NATIVE_PLAYLISTS) {
				// Same trip the B button makes: up to the playlists.
				nat_view = NATIVE_PLAYLISTS;
				nat_arg[0] = '\0';
				snprintf(nat_title, sizeof(nat_title), "Listas");
				lib.sel = 0;
				lib.scroll = lib.velocity = 0.0f;
				lib_reload = true;
			}
			else if (g_touch.clicked >= SL_ROW0)
				activate_row(g_touch.clicked - SL_ROW0);

			if (down & KEY_A)
				activate_row(lib.sel);
			if (down & KEY_Y)
				open_search = true;
			if ((down & KEY_X) && lib.total) {
				// X on a track seeds a radio from it.
				int extra = (nat_view == NATIVE_PLAYLISTS) ? 1 : 0;
				int idx = lib.sel - extra;
				if (idx >= 0 && idx < nat_count &&
				    !nat_rows[idx].is_playlist) {
					snprintf(nat_arg, sizeof(nat_arg), "%s",
					         nat_rows[idx].uri);
					nat_view = NATIVE_RADIO;
					snprintf(nat_title, sizeof(nat_title), "Radio");
					lib.sel = 0;
					lib.scroll = lib.velocity = 0.0f;
					lib_reload = true;
				}
			}
			if (down & KEY_B) {
				if (nat_view != NATIVE_PLAYLISTS) {
					nat_view = NATIVE_PLAYLISTS;
					nat_arg[0] = '\0';
					snprintf(nat_title, sizeof(nat_title), "Listas");
					lib.sel = 0;
					lib.scroll = lib.velocity = 0.0f;
					lib_reload = true;
				} else {
					lib.open = false;
				}
			}
		}

		if (!lib.open && (down & KEY_A)) {
			// A mirrors the on-screen disc: pause, not stop. The decoder
			// sleeps when the ring is full, so resume costs nothing.
			if (native_playback_state() == NATIVE_PLAYING) {
				paused = !paused;
				ndspChnSetPaused(0, paused);
			}
		}
		if (!lib.open && (down & KEY_Y)) {
			muted = !muted;
			mix[0] = mix[1] = muted ? 0.0f : 1.0f;   // slider owns the level
			ndspChnSetMix(0, mix);
		}

		// D-pad left/right are the physical prev/next, under the same bounds
		// the on-screen glyphs grey out with.
		if (!lib.open && !lyr_open && (down & (KEY_DLEFT | KEY_DRIGHT))) {
			bool match = now_matches_view();
			if ((down & KEY_DRIGHT) && match && now_idx + 1 < nat_count)
				play_index(now_idx + 1);
			if ((down & KEY_DLEFT) && match && now_idx > 0)
				play_index(now_idx - 1);
		}

		// L/R are the physical prev/next: with the lid closed they are the
		// only transport within reach. Volume lives on the hardware slider,
		// so these keys are free for the job that actually needs them.
		if (!lib.open && !lyr_open && (down & (KEY_L | KEY_R))) {
			bool match = now_matches_view();
			if ((down & KEY_R) && match && now_idx + 1 < nat_count)
				play_index(now_idx + 1);
			if ((down & KEY_L) && match && now_idx > 0)
				play_index(now_idx - 1);
		}

		// The overlay mirrors the hardware slider; any move pops it up.
		{
			u8 lvl;
			if (R_SUCCEEDED(MCUHWC_GetSoundSliderLevel(&lvl)) &&
			    lvl != vol_slider) {
				vol_slider = lvl;
				volume = lvl * 100 / 63;
				vol_overlay = 255;
				vol_hold = 90;
			}
		}
		if (vol_hold > 0)
			vol_hold--;
		else if (vol_overlay > 0)
			vol_overlay = vol_overlay > 4 ? vol_overlay - 4 : 0;

		// Lyrics mode: back, or dragging a plain document by hand.
		if (lyr_open) {
			if (g_touch.clicked == LYR_BTN_BACK || (down & KEY_B))
				lyr_open = false;
			if (lyr_state == LYR_ST_OK && !lyr_doc.synced &&
			    g_touch.down && g_touch.dragging) {
				lyr_scroll -= g_touch.dy;
				float max = screen_lyrics_max_scroll(lyr_doc.count);
				if (lyr_scroll < 0.0f) lyr_scroll = 0.0f;
				if (lyr_scroll > max) lyr_scroll = max;
			}
		}

		if (open_search) {
			// The system keyboard suspends this loop; audio keeps playing
			// because it lives on its own thread.
			SwkbdState kb;
			swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, sizeof(lib.query) - 1);
			swkbdSetHintText(&kb, "Buscar en Spotify");
			swkbdSetInitialText(&kb, lib.query);
			char typed[sizeof(lib.query)] = "";
			if (swkbdInputText(&kb, typed, sizeof(typed)) == SWKBD_BUTTON_RIGHT) {
				strncpy(lib.query, typed, sizeof(lib.query) - 1);
				nat_view = lib.query[0] ? NATIVE_SEARCH : NATIVE_PLAYLISTS;
				snprintf(nat_title, sizeof(nat_title), "%s",
				         lib.query[0] ? "Busqueda" : "Listas");
				lib.sel = 0;
				lib.scroll = lib.velocity = 0.0f;
				lib_reload = true;
			}
		}

		// GPU calls belong to this thread, so the fetchers only stage bytes.
		if (art_pending) {
			static uint8_t art_tiled[ART_BYTES];
			artwork_tile_rgb565(art_staging, art_tiled, ART_PIXELS);
			C3D_TexLoadImage(&art_tex, art_tiled, GPU_TEXFACE_2D, 0);
			C3D_TexFlush(&art_tex);
			art_present = true;
			art_pending = false;
		}
		thumbs_pump();

		const Skin *sk = &SKIN;

		C2D_TextBufClear(text_buf);
		tb_reset(&g_tb);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, sk->top_bg);
		C2D_SceneBegin(top);
		draw_top(sk, buffering, muted, 0.0f);
		C2D_TargetClear(bottom, sk->bottom_bg);
		C2D_SceneBegin(bottom);
		draw_bottom(sk);
		C3D_FrameEnd(0);
	}

	running = false;
	threadJoin(lister, 2000000000LL);
	threadJoin(snd, 2000000000LL);
	threadJoin(art, 2000000000LL);
	lid_mode_set(false);
	native_exit();
	C3D_TexDelete(&art_tex);
	thumbs_free_all();
	ui_exit();
	C2D_TextBufDelete(text_buf);
	C2D_Fini();
	C3D_Fini();
	ndspExit();
	mcuHwcExit();
	socExit();
	gfxExit();
	return 0;
}
