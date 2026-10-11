// Audio client for the bridge: pulls raw PCM over HTTP and feeds it to NDSP.
//
// The stream is s16le stereo at a fixed rate, so nothing is decoded here. A
// network thread fills a ring buffer; the main loop hands slices of it to the
// DSP. The bridge always sends something, even when idle, so an empty ring
// means the network fell behind rather than the music stopping.

#include <3ds.h>
#include <citro2d.h>

#include "native.h"
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
#define CONFIG_PATH     "sdmc:/3ds/audio3ds/server.txt"
#define DEFAULT_HOST    "192.168.100.177"
#define DEFAULT_PORT    8080

typedef struct {
	u8 *data;
	volatile u32 head, tail;      // byte offsets; head == tail means empty
	LightLock lock;
} Ring;

static Ring ring;
static volatile bool running = true;
static volatile bool net_connected = false;
static volatile u32 bytes_received = 0, underruns = 0;
static char host[64] = DEFAULT_HOST;
static int port = DEFAULT_PORT;
static int sample_rate = 22050;
static char now_title[128] = "", now_artist[128] = "", now_album[128] = "";
static volatile bool art_pending = false, art_present = false;
static volatile bool server_playing = true, server_has_art = false;
static volatile float now_pos = 0.0f, now_dur = 0.0f;
static volatile int meta_soon = 0;
static const char *volatile pending_cmd = NULL;
static volatile bool cmd_busy = false;

#define LIB_ROWS 5
typedef struct {
	bool open, loading;
	int total, offset, sel, count;
	int index[LIB_ROWS];
	char name[LIB_ROWS][64];
	char dir[LIB_ROWS][40];
	char query[64];
} Library;
static Library lib;
static volatile bool lib_reload = false;

// ------------------------------------------------------------- native mode
// The same list, filled from Spotify instead of from the bridge. Browsing
// only: the audio key is refused by Spotify for this account, so nothing here
// can start a track. Selected with a first line of "native" in server.txt.
static bool native_mode = false;
static native_view nat_view = NATIVE_PLAYLISTS;
static char nat_arg[64] = "";
static char nat_title[48] = "Listas";
static native_row nat_rows[NATIVE_ROWS];
static int  nat_count = 0;
static char nat_status[64] = "";
// What the cached rows correspond to, so turning a page does not refetch.
static native_view nat_loaded_view = (native_view)-1;
static char nat_loaded_arg[64] = "\x01";

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
	u32 top_bg, bottom_bg, panel, accent, on_accent, text, dim, meter;
	bool dark;
} Skin;

static const Skin SKIN = {
	.top_bg    = 0xFF121212,   // ABGR, as citro2d wants it
	.bottom_bg = 0xFF1A1A1A,
	.panel     = 0xFF242424,
	.accent    = 0xFF4ADE80,
	.on_accent = 0xFF121212,
	.text      = 0xFFF5F5F5,
	.dim       = 0xFF8A8A8A,
	.meter     = 0xFF3A3A3A,
	.dark      = true,
};

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

// Reading headers one byte at a time cost ~180 syscalls per request, each one
// an IPC round trip competing with the audio stream. Read in blocks instead
// and hand back whatever body bytes arrived with them.
static int http_open(const char *path, u8 *spill, int *spill_len, int cap)
{
	if (spill_len) *spill_len = 0;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(host);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	char request[256];
	int n = snprintf(request, sizeof(request),
	                 "GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
	                 path, host, port);
	if (send(fd, request, n, 0) < 0) { close(fd); return -1; }

	char head[1024];
	int have = 0, body = -1;
	while (have < (int)sizeof(head) - 1) {
		int got = recv(fd, head + have, sizeof(head) - 1 - have, 0);
		if (got <= 0) break;
		have += got;
		head[have] = 0;
		char *blank = strstr(head, "\r\n\r\n");
		if (blank) { body = (int)(blank - head) + 4; break; }
	}
	if (body < 0) { close(fd); return -1; }

	int extra = have - body;
	if (extra > 0 && spill && spill_len) {
		if (extra > cap) extra = cap;
		memcpy(spill, head + body, extra);
		*spill_len = extra;
	}
	return fd;
}

// One-shot request: returns the body length, or -1.
static int http_get(const char *path, u8 *buf, int cap)
{
	int have = 0;
	int fd = http_open(path, buf, &have, cap);
	if (fd < 0) return -1;
	int n;
	while (have < cap && (n = recv(fd, buf + have, cap - have, 0)) > 0)
		have += n;
	close(fd);
	return have;
}

static void stream_thread(void *arg)
{
	(void)arg;
	u8 *chunk = malloc(8192);
	if (!chunk) return;
	while (running) {
		int spill = 0;
		int fd = http_open("/stream", chunk, &spill, 8192);
		if (fd < 0) {
			net_connected = false;
			svcSleepThread(1000000000LL);   // 1 s before retrying
			continue;
		}
		net_connected = true;
		if (spill > 0) { bytes_received += spill; ring_write(chunk, spill); }
		while (running) {
			int n = recv(fd, chunk, 8192, 0);
			if (n <= 0) break;
			bytes_received += n;
			ring_write(chunk, n);
		}
		close(fd);
		net_connected = false;
	}
	free(chunk);
}

// Pull "title" and "artist" out of the bridge's small JSON reply. A real parser
// would be overkill for two flat string fields.
static void json_field(const char *json, const char *key, char *out, size_t size)
{
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p) return;
	p = strchr(p + strlen(pattern), ':');
	if (!p) return;
	p = strchr(p, '"');
	if (!p) return;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i < size - 1) {
		if (*p == '\\' && p[1]) p++;    // keep escaped characters literal
		out[i++] = *p++;
	}
	out[i] = '\0';
}

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

static void url_escape(const char *in, char *out, size_t size)
{
	static const char *hex = "0123456789ABCDEF";
	size_t j = 0;
	for (; *in && j + 4 < size; in++) {
		unsigned char ch = (unsigned char)*in;
		if (ch == ' ') out[j++] = '+';
		else if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') out[j++] = ch;
		else { out[j++] = '%'; out[j++] = hex[ch >> 4]; out[j++] = hex[ch & 15]; }
	}
	out[j] = 0;
}

// Pulls one view from Spotify, then hands the UI the window it is showing.
// The fetch is cached by view and argument: paging with L/R must not cost a
// round trip, because on this console round trips are the expensive part.
static void native_library_fetch(void)
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
	int extra = (nat_view == NATIVE_PLAYLISTS) ? 1 : 0;
	lib.total = nat_count + extra;
	lib.count = 0;
	for (int i = lib.offset; i < lib.total && lib.count < LIB_ROWS; i++) {
		if (extra && i == 0) {
			snprintf(lib.name[lib.count], sizeof(lib.name[0]), "Me gusta");
			lib.dir[lib.count][0] = '\0';
			lib.index[lib.count] = -2;
		} else {
			const native_row *r = &nat_rows[i - extra];
			snprintf(lib.name[lib.count], sizeof(lib.name[0]), "%s", r->label);
			snprintf(lib.dir[lib.count], sizeof(lib.dir[0]), "%s", r->sub);
			lib.index[lib.count] = i - extra;
		}
		lib.count++;
	}
	if (lib.sel >= lib.count) lib.sel = lib.count ? lib.count - 1 : 0;
	lib.loading = false;
}

// Walk the flat {"i":..,"name":"..","dir":".."} records the bridge returns.
static void library_fetch(void)
{
	if (native_mode) { native_library_fetch(); return; }

	char escaped[160], path[256];
	url_escape(lib.query, escaped, sizeof(escaped));
	snprintf(path, sizeof(path), "/library?q=%s&offset=%d&limit=%d",
	         escaped, lib.offset, LIB_ROWS);
	lib.loading = true;
	static char body[8192];
	int total = http_get(path, (u8 *)body, sizeof(body) - 1);
	if (total < 0) { lib.loading = false; return; }
	body[total] = 0;

	char number[16] = "";
	json_field(body, "total", number, sizeof(number));
	const char *p = strstr(body, "\"total\"");
	lib.total = p ? atoi(strchr(p, ':') + 1) : 0;
	lib.count = 0;
	p = body;
	while (lib.count < LIB_ROWS && (p = strstr(p, "\"i\":"))) {
		lib.index[lib.count] = atoi(p + 4);
		const char *rec = p;
		json_field(rec, "name", lib.name[lib.count], sizeof(lib.name[0]));
		json_field(rec, "dir", lib.dir[lib.count], sizeof(lib.dir[0]));
		lib.count++;
		p += 4;
	}
	if (lib.sel >= lib.count) lib.sel = lib.count ? lib.count - 1 : 0;
	lib.loading = false;
}

static void fetch_art(void)
{
	int total = http_get("/art", art_staging, ART_BYTES);
	// The server sends it already tiled for the PICA200, so the console only
	// has to hand the bytes to the GPU.
	if (total == ART_BYTES) art_pending = true;
	else art_present = false;
}

static float json_num(const char *json, const char *key)
{
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p) return 0.0f;
	p = strchr(p + strlen(pattern), ':');
	return p ? (float)atof(p + 1) : 0.0f;
}

static bool json_bool(const char *json, const char *key)
{
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p) return false;
	p = strchr(p + strlen(pattern), ':');
	return p && strncmp(p + 1, " true", 5) == 0;
}

// In native mode the bridge's command thread is not running, and it is the one
// that services lib_reload. A fetch takes about a second against Spotify, so it
// gets its own thread for the same reason: never on the render loop.
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

// Transport requests run off the render loop: a stalled server must never
// freeze the interface or starve the audio thread.
static void command_thread(void *arg)
{
	(void)arg;
	while (running) {
		const char *cmd = pending_cmd;
		if (cmd) {
			pending_cmd = NULL;
			cmd_busy = true;
			u8 sink[256];
			http_get(cmd, sink, sizeof(sink));
			meta_soon = 1;   // refresh the track straight away
			cmd_busy = false;
		}
		if (lib_reload) {
			lib_reload = false;
			library_fetch();
		}
		svcSleepThread(40000000LL);   // 40 ms
	}
}

static void metadata_thread(void *arg)
{
	(void)arg;
	char *body = malloc(2048);
	if (!body) return;
	while (running) {
		int total = http_get("/now", (u8 *)body, 2047);
		{
			body[total > 0 ? total : 0] = '\0';
			if (total > 0) {
				char title[128] = "";
				json_field(body, "title", title, sizeof(title));
				bool changed = strcmp(title, now_title) != 0;
				strncpy(now_title, title, sizeof(now_title) - 1);
				json_field(body, "artist", now_artist, sizeof(now_artist));
				json_field(body, "album", now_album, sizeof(now_album));
				server_playing = strstr(body, "\"playing\": true") != NULL;
				server_has_art = json_bool(body, "art");
				now_pos = json_num(body, "position");
				now_dur = json_num(body, "duration");
				if (changed) art_present = false;   // never show the old cover
				// The server extracts the cover in the background, so on a skip
				// the title arrives before the image exists. Asking once and
				// giving up left the placeholder until the next track.
				if (server_has_art && !art_present && !art_pending)
					fetch_art();
				else if (!server_has_art)
					art_present = false;
			}
		}
		// Poll in short steps so a skip shows up quickly without hammering.
		for (int i = 0; i < 4 && running; i++) {
			if (meta_soon) { meta_soon = 0; break; }
			svcSleepThread(100000000LL);   // 100 ms
		}
	}
	free(body);
}

// -------------------------------------------------------------------- config

// Native is the default now: this is a Spotify client, and the bridge is the
// path it grew out of. No config file at all means native, so a fresh card
// needs nothing but the .3dsx.
static void load_config(void)
{
	native_mode = true;
	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f) return;
	char line[128];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (strcmp(line, "native") == 0 || line[0] == '\0') {
			fclose(f);
			return;
		}
		// A host:port line is the bridge asking for itself by name.
		native_mode = false;
		char *colon = strchr(line, ':');
		if (colon) {
			*colon = '\0';
			port = atoi(colon + 1);
		}
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0]) strncpy(host, line, sizeof(host) - 1);
	}
	if (fgets(line, sizeof(line), f)) {
		int r = atoi(line);
		if (r >= 8000 && r <= 48000) sample_rate = r;
	}
	fclose(f);
}

// ------------------------------------------------------------------ drawing

#define TOP_W 400.0f
#define BOT_W 320.0f
#define SCR_H 240.0f

static C2D_TextBuf text_buf;
static C3D_Tex art_tex;
static Tex3DS_SubTexture art_sub = {ART_PIXELS, ART_PIXELS, 0.0f, 1.0f, 1.0f, 0.0f};

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
	if (sk->dark) {
		// Art large on the left, text stacked beside it with the hierarchy
		// carried by size and brightness rather than by rules and bars.
		const float ART = 184.0f;
		draw_art(sk, 18 + par * 3.2f, 28, ART);

		const float tx = 18 + ART + 20;
		text_at(now_title[0] ? now_title : "---", tx + par * 2.0f, 44, 0.66f,
		        sk->text, 0);
		text_at(now_artist, tx + par * 1.4f, 82, 0.56f, sk->text, 0);
		text_at(now_album, tx + par * 1.0f, 110, 0.48f, sk->dim, 0);

		// A small lit dot carries the state; a word in capitals does not need
		// a bar over it to be read.
		float dy = 150;
		C2D_DrawCircleSolid(tx + 4 + par * 0.8f, dy + 5, 0.1f, 4,
		                    buffering ? sk->dim : sk->accent);
		text_at(buffering ? "CARGANDO" : (muted ? "SIN SONIDO" : "SONANDO"),
		        tx + 16 + par * 0.8f, dy, 0.46f, sk->dim, 0);
	} else {
		// Title bar and a framed thumbnail, closer to a hardware player.
		C2D_DrawRectSolid(0 + par * 0.2f, 0, 0.0f, TOP_W, 30, sk->accent);
		text_at("Now Playing", TOP_W / 2 + par * 0.6f, 6, 0.56f, sk->on_accent, C2D_AlignCenter);
		C2D_DrawRectSolid(26 + par * 2.6f, 56, 0.0f, 104, 104, sk->dim);
		draw_art(sk, 30 + par * 3.0f, 60, 96);
		text_at(now_title[0] ? now_title : "---", 152 + par * 1.8f, 64, 0.58f, sk->text, 0);
		text_at(now_artist, 152 + par * 1.3f, 94, 0.48f, sk->text, 0);
		text_at(now_album, 152 + par * 1.0f, 118, 0.44f, sk->dim, 0);
		C2D_DrawRectSolid(152 + par * 0.8f, 146, 0.0f, TOP_W - 172, 1, sk->meter);
		text_at(buffering ? "Buffering..." : (muted ? "Paused output" : "Playing"),
		        152 + par * 0.8f, 154, 0.44f, sk->dim, 0);
	}
}

typedef enum {
	IC_NONE, IC_PREV, IC_PLAY, IC_PAUSE, IC_NEXT,
	IC_SOUND, IC_MUTED, IC_LIST, IC_SEARCH, IC_CLOSE
} Icon;

typedef struct { float x, y, w, h; Icon icon; bool accent; } Button;

// Transport glyphs drawn from primitives: no textures to ship, and they scale
// to whatever size a layout gives them.
static void draw_icon(Icon ic, float cx, float cy, float s, u32 col)
{
	const float h = s / 2;
	switch (ic) {
	case IC_PLAY:
		C2D_DrawTriangle(cx - h * 0.6f, cy - h, col, cx - h * 0.6f, cy + h, col,
		                 cx + h * 0.8f, cy, col, 0.1f);
		break;
	case IC_PAUSE:
		C2D_DrawRectSolid(cx - h * 0.6f, cy - h, 0.1f, s * 0.22f, s, col);
		C2D_DrawRectSolid(cx + h * 0.16f, cy - h, 0.1f, s * 0.22f, s, col);
		break;
	case IC_PREV:
		C2D_DrawRectSolid(cx - h, cy - h, 0.1f, s * 0.16f, s, col);
		C2D_DrawTriangle(cx + h, cy - h, col, cx + h, cy + h, col,
		                 cx - h * 0.5f, cy, col, 0.1f);
		break;
	case IC_NEXT:
		C2D_DrawRectSolid(cx + h * 0.84f, cy - h, 0.1f, s * 0.16f, s, col);
		C2D_DrawTriangle(cx - h, cy - h, col, cx - h, cy + h, col,
		                 cx + h * 0.5f, cy, col, 0.1f);
		break;
	case IC_SOUND:
	case IC_MUTED:
		C2D_DrawRectSolid(cx - h, cy - h * 0.35f, 0.1f, s * 0.28f, s * 0.35f, col);
		C2D_DrawTriangle(cx - h * 0.44f, cy - h * 0.35f, col,
		                 cx - h * 0.44f, cy + h * 0.35f, col,
		                 cx + h * 0.1f, cy, col, 0.1f);
		C2D_DrawRectSolid(cx - h * 0.44f, cy - h * 0.9f, 0.1f, s * 0.1f, s * 0.9f, col);
		if (ic == IC_MUTED) {
			C2D_DrawLine(cx + h * 0.25f, cy - h * 0.5f, col,
			             cx + h * 0.95f, cy + h * 0.5f, col, s * 0.11f, 0.2f);
		} else {
			C2D_DrawLine(cx + h * 0.35f, cy - h * 0.45f, col,
			             cx + h * 0.35f, cy + h * 0.45f, col, s * 0.1f, 0.2f);
			C2D_DrawLine(cx + h * 0.72f, cy - h * 0.75f, col,
			             cx + h * 0.72f, cy + h * 0.75f, col, s * 0.1f, 0.2f);
		}
		break;
	case IC_LIST:
		for (int i = 0; i < 3; i++) {
			float y = cy - h * 0.7f + i * h * 0.7f;
			C2D_DrawRectSolid(cx - h, y, 0.1f, s * 0.18f, s * 0.18f, col);
			C2D_DrawRectSolid(cx - h * 0.5f, y + s * 0.04f, 0.1f, s * 0.9f, s * 0.11f, col);
		}
		break;
	case IC_SEARCH:
		C2D_DrawCircleSolid(cx - h * 0.2f, cy - h * 0.2f, 0.1f, h * 0.62f, col);
		C2D_DrawCircleSolid(cx - h * 0.2f, cy - h * 0.2f, 0.2f, h * 0.38f, 0);
		C2D_DrawLine(cx + h * 0.22f, cy + h * 0.22f, col,
		             cx + h * 0.85f, cy + h * 0.85f, col, s * 0.14f, 0.3f);
		break;
	case IC_CLOSE:
		C2D_DrawLine(cx - h * 0.7f, cy - h * 0.7f, col, cx + h * 0.7f, cy + h * 0.7f,
		             col, s * 0.14f, 0.1f);
		C2D_DrawLine(cx + h * 0.7f, cy - h * 0.7f, col, cx - h * 0.7f, cy + h * 0.7f,
		             col, s * 0.14f, 0.1f);
		break;
	default: break;
	}
}

// No box around anything. Hierarchy comes from size and brightness, and only
// the primary action gets a filled shape -- boxing every control is what made
// the old layout look like a remote control.
static void draw_button(const Skin *sk, const Button *b, bool lit, float glyph)
{
	float cx = b->x + b->w / 2, cy = b->y + b->h / 2;
	u32 ink = lit ? sk->accent : sk->text;

	if (b->accent) {
		float r = b->h * 0.42f;
		C2D_DrawCircleSolid(cx, cy, 0.0f, r, lit ? sk->accent : sk->text);
		draw_icon(b->icon, cx, cy, glyph, sk->bottom_bg);
		return;
	}
	draw_icon(b->icon, cx, cy, glyph, ink);
}

#define LIST_X 10.0f
#define LIST_W 300.0f
#define ROW_Y  52.0f
#define ROW_H  34.0f

// Left half of the split view. Browsing never takes the transport away: the
// controls keep their own column on the right.
// Two lines per row -- title over artist -- with the selection marked by a
// bar and a colour instead of a filled block, which is what makes a long list
// readable at this size.
static void draw_native_list(const Skin *sk)
{
	char head[96];
	snprintf(head, sizeof(head), "%s", native_mode ? nat_title : "Biblioteca");
	text_at(head, LIST_X, 8, 0.56f, sk->text, 0);

	if (lib.total > 0) {
		char count[32];
		snprintf(count, sizeof(count), "%d-%d / %d", lib.offset + 1,
		         lib.offset + lib.count, lib.total);
		text_at(count, LIST_X + LIST_W, 12, 0.44f, sk->dim, C2D_AlignRight);
	}
	text_at(lib.query[0] ? lib.query : (native_mode ? "TEMAS" : "BIBLIOTECA"),
	        LIST_X, 32, 0.44f, sk->dim, 0);

	if (lib.loading && lib.count == 0) {
		text_at("Cargando...", LIST_X, ROW_Y + 8, 0.50f, sk->dim, 0);
		return;
	}
	if (lib.count == 0) {
		text_at("Nada por aqui", LIST_X, ROW_Y + 8, 0.50f, sk->dim, 0);
		return;
	}

	for (int i = 0; i < lib.count; i++) {
		float y = ROW_Y + i * ROW_H;
		bool sel = (i == lib.sel);
		if (sel)
			C2D_DrawRectSolid(LIST_X - 6, y, 0.05f, 3, ROW_H - 4, sk->accent);
		text_at(lib.name[i], LIST_X + 4, y, 0.50f,
		        sel ? sk->accent : sk->text, 0);
		if (lib.dir[i][0])
			text_at(lib.dir[i], LIST_X + 4, y + 17, 0.44f, sk->dim, 0);
	}

	// A hairline scrollbar: present when it means something, invisible when
	// everything already fits.
	if (lib.total > lib.count) {
		float track_h = ROW_H * LIB_ROWS;
		float frac = (float)lib.count / (float)lib.total;
		float pos = (float)lib.offset / (float)lib.total;
		C2D_DrawRectSolid(LIST_X + LIST_W + 2, ROW_Y, 0.0f, 2, track_h, sk->meter);
		C2D_DrawRectSolid(LIST_X + LIST_W + 2, ROW_Y + track_h * pos, 0.1f, 2,
		                  track_h * frac, sk->accent);
	}
}


static bool hit(const Button *b, touchPosition t)
{
	return t.px >= b->x && t.px < b->x + b->w && t.py >= b->y && t.py < b->y + b->h;
}

#define SEEK_X 16.0f
#define SEEK_Y 88.0f
#define SEEK_W (BOT_W - 32)
#define SEEK_H 16.0f

static void clock_text(char *out, size_t n, float secs)
{
	int t = (int)(secs < 0 ? 0 : secs);
	snprintf(out, n, "%d:%02d", t / 60, t % 60);
}


static void draw_bottom(const Skin *sk, const Button *buttons, int count,
                        u32 used, bool buffering, int flash)
{
	if (lib.open) {
		draw_native_list(sk);
	} else {
		// Seconds, not a share of the ring: the ring is deliberately oversized,
		// so its percentage says nothing. Two seconds is the readable range.
		float secs = (float)used / (float)(sample_rate * 4);
		float fill = secs / 2.0f;
		if (fill > 1.0f) fill = 1.0f;
		C2D_DrawRectSolid(16, 22, 0.0f, BOT_W - 32, 8, sk->meter);
		C2D_DrawRectSolid(16, 22, 0.1f, (BOT_W - 32) * fill, 8, sk->accent);
		char line[96];
		snprintf(line, sizeof(line), "%s  buffer %.1fs  underruns %u",
		         net_connected ? (buffering ? "filling" : "connected") : "reconnecting",
		         secs, (unsigned)underruns);
		text_at(line, 16, 34, 0.34f, sk->dim, 0);
		text_at(now_title[0] ? now_title : "-", 16, 54, 0.40f, sk->text, 0);

		// Seek bar: the only place the touch screen can scrub, since the top
		// screen cannot be touched.
		float done = (now_dur > 0.5f) ? now_pos / now_dur : 0.0f;
		if (done > 1.0f) done = 1.0f;
		C2D_DrawRectSolid(SEEK_X, SEEK_Y, 0.0f, SEEK_W, SEEK_H, sk->meter);
		C2D_DrawRectSolid(SEEK_X, SEEK_Y, 0.1f, SEEK_W * done, SEEK_H, sk->accent);
		C2D_DrawRectSolid(SEEK_X + SEEK_W * done - 2, SEEK_Y - 3, 0.2f, 5,
		                  SEEK_H + 6, sk->text);
		char a[12], b[12];
		clock_text(a, sizeof(a), now_pos);
		clock_text(b, sizeof(b), now_dur);
		text_at(a, SEEK_X, SEEK_Y + SEEK_H + 2, 0.32f, sk->dim, 0);
		text_at(b, SEEK_X + SEEK_W, SEEK_Y + SEEK_H + 2, 0.32f, sk->dim, C2D_AlignRight);
	}
	for (int i = 0; i < count; i++)
		if (buttons[i].w > 0.0f)
			draw_button(sk, &buttons[i], i == flash, buttons[i].h * 0.52f);
	text_at(BUILD_STAMP, BOT_W - 6, SCR_H - 14, 0.40f, sk->meter, C2D_AlignRight);
}

enum { B_PREV, B_PLAY, B_NEXT, B_MUTE, B_LIST, B_SEARCH, B_COUNT };

// Two arrangements over one array. Zero width hides a button in that mode.
// Touch targets stay large even though nothing is drawn around them: a finger
// needs the area, the eye does not need the border.
static void layout_buttons(Button *b, bool split)
{
	if (split) {
		// The list owns the screen; the transport shrinks to a strip under it.
		b[B_PREV]   = (Button){ 96, 200, 44, 40, IC_PREV,  false};
		b[B_PLAY]   = (Button){144, 198, 44, 44, IC_PLAY,  true };
		b[B_NEXT]   = (Button){192, 200, 44, 40, IC_NEXT,  false};
		b[B_SEARCH] = (Button){248,   4, 36, 32, IC_SEARCH, false};
		b[B_LIST]   = (Button){284,   4, 32, 32, IC_CLOSE,  false};
		b[B_MUTE]   = (Button){ 40, 200, 44, 40, IC_SOUND, false};
	} else {
		// One centred row, the way a player looks rather than a keypad.
		b[B_MUTE]   = (Button){ 36, 136, 48, 48, IC_SOUND, false};
		b[B_PREV]   = (Button){ 92, 136, 48, 48, IC_PREV,  false};
		b[B_PLAY]   = (Button){144, 130, 60, 60, IC_PLAY,  true };
		b[B_NEXT]   = (Button){212, 136, 48, 48, IC_NEXT,  false};
		b[B_LIST]   = (Button){268, 136, 48, 48, IC_LIST,  false};
		b[B_SEARCH] = (Button){0, 0, 0, 0, IC_SEARCH, false};
	}
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
static void native_progress(const char *step)
{
	printf("  %s\n", step);
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

// The pairing code has to stay readable while the user walks to a phone, so
// it is printed once and the waiting happens under it.
static void pair_show_code(const char *code, const char *url)
{
	printf("\n\n   Entra a:\n   %s\n", url);
	printf("\n   y escribe este codigo:\n\n       %s\n\n", code);
	printf("   Esperando (B cancela)");
}

static bool pair_poll_frame(int attempt)
{
	(void)attempt;
	hidScanInput();
	if (hidKeysDown() & KEY_B)
		return false;
	// One dot per second of waiting, so a stuck pairing is visible.
	static int ticks = 0;
	if (++ticks % 60 == 0)
		printf(".");
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
	return aptMainLoop();
}

int main(void)
{
	gfxInitDefault();
	load_config();
	if (native_mode)
		sample_rate = 44100;     // Spotify's Vorbis; the config rate is the bridge's

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
		ndspChnSetRate(0, (float)sample_rate);
		ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
		mix[0] = mix[1] = 1.0f;
		ndspChnSetMix(0, mix);
		audio = linearAlloc(buf_bytes * WAVE_BUFFERS);
		if (!audio) fail = "Audio memory failed.";
	}
	if (!fail && native_mode) {
		consoleInit(GFX_TOP, NULL);
		printf("\n  Spotify nativo\n\n");
		static char nat_err[192];
		static const native_sink sink = { ring_sink_write, ring_free_space };
		// First run on a console: pair here rather than in a separate tool.
		if (!native_has_token() &&
		    native_pair(pair_show_code, pair_poll_frame, nat_err,
		                sizeof(nat_err)) != 0) {
			printf("\n\n  No se pudo parear:\n  %s\n", nat_err);
			printf("\n  START para salir.\n");
			while (aptMainLoop()) {
				hidScanInput();
				if (hidKeysDown() & KEY_START) break;
				gfxFlushBuffers();
				gfxSwapBuffers();
				gspWaitForVBlank();
			}
			gfxExit();
			return 1;
		}
		if (native_init(&sink, native_progress, nat_err, sizeof(nat_err)) != 0) {
			printf("\n  No se pudo entrar:\n  %s\n", nat_err);
			printf("\n  START para salir.\n");
			while (aptMainLoop()) {
				hidScanInput();
				if (hidKeysDown() & KEY_START) break;
				gfxFlushBuffers();
				gfxSwapBuffers();
				gspWaitForVBlank();
			}
			gfxExit();
			return 1;
		}
		printf("  %s\n", native_account());
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
	gfxSet3D(true);
	C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget *top_right = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	text_buf = C2D_TextBufNew(4096);
	C3D_TexInit(&art_tex, ART_PIXELS, ART_PIXELS, GPU_RGB565);
	C3D_TexSetFilter(&art_tex, GPU_LINEAR, GPU_LINEAR);

	// In native mode there is no bridge to stream from, poll or command.
	// The audio thread still runs: it only moves the ring into NDSP, and with
	// an empty ring it idles.
	Thread net = NULL, meta = NULL, cmd = NULL;
	if (!native_mode) {
		net = threadCreate(stream_thread, NULL, 32 * 1024, 0x30, -2, false);
		meta = threadCreate(metadata_thread, NULL, 32 * 1024, 0x3F, -2, false);
		cmd = threadCreate(command_thread, NULL, 48 * 1024, 0x3F, -2, false);
	} else {
		cmd = threadCreate(native_library_thread, NULL, 32 * 1024, 0x3F, -2, false);
		buffering = false;
		lib.open = true;
		lib_reload = true;
		snprintf(now_title, sizeof(now_title), "Spotify: sin audio todavia");
		snprintf(now_artist, sizeof(now_artist), "%s", native_account());
	}
	Thread snd = threadCreate(audio_thread, NULL, 16 * 1024, 0x18, -2, false);
	int nat_state_seen = -1;

	Button buttons[B_COUNT];
	layout_buttons(buttons, false);

	// One second: the link measured comfortable at 1024 kbps, and every extra
	// second of cushion is a second of lag after a skip.
	prebuffer_bytes = (u32)sample_rate * 4;
	bool muted = false;
	int flash = -1, flash_ttl = 0;


	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) break;
		touchPosition touch = {0, 0};
		bool touch_used = false, input_used = false;
		if (down & KEY_TOUCH) hidTouchRead(&touch);

		if (native_mode) {
			// The player runs on its own thread; this just mirrors it.
			native_state st = native_playback_state();
			if (st != nat_state_seen) {
				nat_state_seen = (int)st;
				switch (st) {
				case NATIVE_LOADING:
					snprintf(now_title, sizeof(now_title), "Cargando...");
					now_artist[0] = '\0';
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
				now_pos = native_position();
				now_dur = native_duration();
				server_playing = true;
			}
		}

		if (lib.open) {
			if (down & KEY_DUP)   lib.sel = (lib.sel + lib.count - 1) % (lib.count ?: 1);
			if (down & KEY_DDOWN) lib.sel = (lib.sel + 1) % (lib.count ?: 1);
			if ((down & KEY_L) && lib.offset >= LIB_ROWS) {
				lib.offset -= LIB_ROWS; lib_reload = true;
			}
			if ((down & KEY_R) && lib.offset + LIB_ROWS < lib.total) {
				lib.offset += LIB_ROWS; lib_reload = true;
			}
			int row = -1;
			if ((down & KEY_TOUCH) && touch.px < LIST_X + LIST_W)
				row = (int)((touch.py - ROW_Y) / ROW_H);
			// Closing the list re-lays out the buttons, so this same coordinate
			// would then land on one of them. Consume it here.
			if (row >= 0 && row < lib.count) { lib.sel = row; touch_used = true; }
			if (native_mode && (down & KEY_B) && nat_view != NATIVE_PLAYLISTS) {
				nat_view = NATIVE_PLAYLISTS;
				nat_arg[0] = '\0';
				snprintf(nat_title, sizeof(nat_title), "Listas");
				lib.offset = lib.sel = 0;
				lib_reload = true;
			} else if (native_mode &&
			           ((down & KEY_A) || (row >= 0 && row < lib.count)) &&
			           lib.count) {
				int idx = lib.index[lib.sel];
				if (idx == -2) {
					nat_view = NATIVE_LIKED;
					nat_arg[0] = '\0';
					snprintf(nat_title, sizeof(nat_title), "Me gusta");
					lib.offset = lib.sel = 0;
					lib_reload = true;
				} else if (idx >= 0 && idx < nat_count &&
				           nat_rows[idx].is_playlist) {
					snprintf(nat_arg, sizeof(nat_arg), "%s", nat_rows[idx].uri);
					if (native_playlist_name(nat_arg, nat_title,
					                         sizeof(nat_title)) != 0)
						snprintf(nat_title, sizeof(nat_title), "Lista");
					nat_view = NATIVE_TRACKS;
					lib.offset = lib.sel = 0;
					lib_reload = true;
				} else if (idx >= 0 && idx < nat_count) {
					// A plays the track; Y on the list builds a radio from it
					// instead, which is the other thing worth doing here.
					native_play(nat_rows[idx].uri);
					buffering = true;
					ring_clear();
				}
			} else if (!native_mode &&
			           ((down & KEY_A) || (row >= 0 && row < lib.count)) &&
			           lib.count) {
				static char jump[48];
				snprintf(jump, sizeof(jump), "/control/play/%d", lib.index[lib.sel]);
				pending_cmd = jump;
				buffering = true;
				ring_clear();
				lib.open = false;
				layout_buttons(buttons, false);
				input_used = true;   // closing the list must not re-read this press
			}
			if (down & KEY_B) { lib.open = false; layout_buttons(buttons, false); }
		} else if ((down & KEY_TOUCH) && touch.py >= SEEK_Y - 6 &&
		           touch.py <= SEEK_Y + SEEK_H + 6 && now_dur > 1.0f) {
			// Scrub: the bar is the only touchable timeline, the top screen is not.
			float frac = (touch.px - SEEK_X) / SEEK_W;
			if (frac < 0) frac = 0;
			if (frac > 1) frac = 1;
			static char jump[48];
			snprintf(jump, sizeof(jump), "/control/seek/%.1f", frac * now_dur);
			pending_cmd = jump;
			buffering = true;
			ring_clear();
			touch_used = true;
		}

		int pressed = -1;
		if (input_used) goto skip_input;
		if (down & KEY_L && !lib.open) pressed = B_PREV;
		if (down & KEY_R && !lib.open) pressed = B_NEXT;
		if (down & KEY_A && !lib.open) pressed = B_PLAY;
		if (down & KEY_Y) pressed = lib.open ? B_SEARCH : B_MUTE;
		if (native_mode && lib.open && (down & KEY_R) && lib.count) {
			// R on a track: radio seeded from it. L/R still page when the
			// selection is a playlist.
			int idx = lib.index[lib.sel];
			if (idx >= 0 && idx < nat_count && !nat_rows[idx].is_playlist) {
				snprintf(nat_arg, sizeof(nat_arg), "%s", nat_rows[idx].uri);
				nat_view = NATIVE_RADIO;
				snprintf(nat_title, sizeof(nat_title), "Radio");
				lib.offset = lib.sel = 0;
				lib_reload = true;
			}
		}
		if ((down & KEY_TOUCH) && !touch_used)
			for (int i = 0; i < B_COUNT; i++)
				if (buttons[i].w > 0.0f && hit(&buttons[i], touch)) pressed = i;

		bool toggle_mute = false;
		if (pressed >= 0 && buttons[pressed].w > 0.0f) {
			flash = pressed;
			flash_ttl = 10;
			switch (pressed) {
			case B_PREV: pending_cmd = "/control/prev"; break;
			case B_NEXT: pending_cmd = "/control/next"; break;
			case B_PLAY: pending_cmd = "/control/toggle"; break;
			case B_MUTE: toggle_mute = true; break;
			case B_LIST:
				lib.open = !lib.open;
				if (lib.open) { lib.offset = lib.sel = 0; lib_reload = true; }
				layout_buttons(buttons, lib.open);
				break;
			case B_SEARCH: {
				// The system keyboard suspends this loop; audio keeps playing
				// because it lives on its own thread.
				SwkbdState kb;
				swkbdInit(&kb, SWKBD_TYPE_QWERTY, 2, sizeof(lib.query) - 1);
				swkbdSetHintText(&kb, native_mode ? "Buscar en Spotify"
				                                  : "Search your library");
				swkbdSetInitialText(&kb, lib.query);
				char typed[sizeof(lib.query)] = "";
				if (swkbdInputText(&kb, typed, sizeof(typed)) == SWKBD_BUTTON_RIGHT) {
					strncpy(lib.query, typed, sizeof(lib.query) - 1);
					if (native_mode) {
						nat_view = lib.query[0] ? NATIVE_SEARCH
						                        : NATIVE_PLAYLISTS;
						snprintf(nat_title, sizeof(nat_title), "%s",
						         lib.query[0] ? "Busqueda" : "Listas");
					}
					lib.offset = lib.sel = 0;
					lib_reload = true;
				}
				break;
			}
			}
			if (pressed == B_PREV || pressed == B_NEXT ||
			    (pressed == B_PLAY && server_playing)) {
				buffering = true;
				ring_clear();
			}
		}
		if (toggle_mute) {
			muted = !muted;
			mix[0] = mix[1] = muted ? 0.0f : 1.0f;
			ndspChnSetMix(0, mix);
		}
skip_input:
		if (flash_ttl > 0 && --flash_ttl == 0) flash = -1;

		// GPU calls belong to this thread, so the fetcher only stages bytes.
		if (art_pending) {
			C3D_TexLoadImage(&art_tex, art_staging, GPU_TEXFACE_2D, 0);
			C3D_TexFlush(&art_tex);
			art_present = true;
			art_pending = false;
		}

		const Skin *sk = &SKIN;
		buttons[B_PLAY].icon = server_playing ? IC_PAUSE : IC_PLAY;
		buttons[B_MUTE].icon = muted ? IC_MUTED : IC_SOUND;

		// The slider is the user's control: at zero the right eye is never
		// drawn, so 2D costs nothing extra.
		float depth = osGet3DSliderState() * 4.0f;
		C2D_TextBufClear(text_buf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, sk->top_bg);
		C2D_SceneBegin(top);
		draw_top(sk, buffering, muted, -depth);
		if (depth > 0.0f) {
			C2D_TargetClear(top_right, sk->top_bg);
			C2D_SceneBegin(top_right);
			draw_top(sk, buffering, muted, depth);
		}
		C2D_TargetClear(bottom, sk->bottom_bg);
		C2D_SceneBegin(bottom);
		draw_bottom(sk, buttons, B_COUNT, ring_used(), buffering, flash);
		C3D_FrameEnd(0);
	}

	running = false;
	threadJoin(net, 2000000000LL);
	threadJoin(meta, 5000000000LL);
	threadJoin(cmd, 2000000000LL);
	threadJoin(snd, 2000000000LL);
	C3D_TexDelete(&art_tex);
	C2D_TextBufDelete(text_buf);
	C2D_Fini();
	C3D_Fini();
	ndspExit();
	socExit();
	gfxExit();
	return 0;
}
