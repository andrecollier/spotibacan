// Audio client for the bridge: pulls raw PCM over HTTP and feeds it to NDSP.
//
// The stream is s16le stereo at a fixed rate, so nothing is decoded here. A
// network thread fills a ring buffer; the main loop hands slices of it to the
// DSP. The bridge always sends something, even when idle, so an empty ring
// means the network fell behind rather than the music stopping.

#include <3ds.h>
#include <citro2d.h>

#include "wps.h"
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

#define LIB_ROWS 6
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

// Audio runs on its own thread: the system keyboard suspends the main loop,
// and four wave buffers hold only a quarter second.
static volatile bool buffering = true;
static volatile u32 prebuffer_bytes = 0;
static u8 *audio_base = NULL;
static ndspWaveBuf waves[WAVE_BUFFERS];
static u8 *art_staging = NULL;

#define ART_PIXELS 128
#define ART_BYTES  (ART_PIXELS * ART_PIXELS * 2)

// Two looks over one engine: only colours and arrangement differ.
typedef struct {
	char name[24];
	char author[32];
	char backdrop[48];
	char wps[48];
	u32 top_bg, bottom_bg, panel, accent, on_accent, text, dim, meter;
	bool dark;   // layout: art-forward (true) or framed list (false)
} Skin;

#define MAX_THEMES 16
#define THEME_DIR  "sdmc:/3ds/audio3ds/themes"

#define MAX_THEMES 24
#define THEME_DIR  "sdmc:/3ds/audio3ds/themes"

static Skin themes[MAX_THEMES];
static int theme_count = 0;
static bool theme_open = false;
static int theme_sel = 0;

// citro2d packs colours as 0xAABBGGRR; theme files write plain RRGGBB.
static u32 theme_colour(const char *hex, u32 fallback)
{
	unsigned v = 0;
	if (sscanf(hex, "%6x", &v) != 1) return fallback;
	return 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
}

// Themes are data, not code. That is what lets a player carry hundreds of them
// and lets you edit one with a text editor instead of a compiler.
static void theme_parse(const char *path, Skin *sk)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[192];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '#') continue;
		char *colon = strchr(line, ':');
		if (!colon) continue;
		*colon = 0;
		char *key = line, *val = colon + 1;
		while (*val == ' ' || *val == '\t') val++;
		val[strcspn(val, "\r\n")] = 0;
		if      (!strcmp(key, "name"))      snprintf(sk->name, sizeof(sk->name), "%s", val);
		else if (!strcmp(key, "author"))    snprintf(sk->author, sizeof(sk->author), "%s", val);
		else if (!strcmp(key, "top bg"))    sk->top_bg    = theme_colour(val, sk->top_bg);
		else if (!strcmp(key, "bottom bg")) sk->bottom_bg = theme_colour(val, sk->bottom_bg);
		else if (!strcmp(key, "panel"))     sk->panel     = theme_colour(val, sk->panel);
		else if (!strcmp(key, "accent"))    sk->accent    = theme_colour(val, sk->accent);
		else if (!strcmp(key, "on accent")) sk->on_accent = theme_colour(val, sk->on_accent);
		else if (!strcmp(key, "text"))      sk->text      = theme_colour(val, sk->text);
		else if (!strcmp(key, "dim"))       sk->dim       = theme_colour(val, sk->dim);
		else if (!strcmp(key, "meter"))     sk->meter     = theme_colour(val, sk->meter);
		else if (!strcmp(key, "layout"))    sk->dark      = (strstr(val, "art") != NULL);
		else if (!strcmp(key, "backdrop"))  snprintf(sk->backdrop, sizeof(sk->backdrop), "%s", val);
		else if (!strcmp(key, "wps"))       snprintf(sk->wps, sizeof(sk->wps), "%s", val);
	}
	fclose(f);
}

#define BACKDROP_DIR "sdmc:/3ds/audio3ds/backdrops"
#define BD_TEX 512
#define BD_TEX_H 256

static C3D_Tex backdrop_tex;
static bool backdrop_ready = false;
static int backdrop_w = 0, backdrop_h = 0;
static char backdrop_name[48] = "";
static char backdrop_status[64] = "no backdrop";

// Offset of (x, y) inside an 8x8 tile: the PICA200 stores textures that way and
// citro3d has no linear-to-tiled helper.
static inline int morton8(int x, int y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
	     | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// Minimal BMP reader: 24/32-bit and 8-bit palettised, which covers what player
// themes ship. Rows are bottom-up and padded to four bytes.
static bool backdrop_load(const char *file)
{
	char path[160];
	snprintf(path, sizeof(path), BACKDROP_DIR "/%s", file);
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	u8 head[54];
	if (fread(head, 1, 54, f) != 54 || head[0] != 'B' || head[1] != 'M') {
		fclose(f); return false;
	}
	u32 data_off = head[10] | (head[11] << 8) | (head[12] << 16) | (head[13] << 24);
	u32 info_len = head[14] | (head[15] << 8) | (head[16] << 16) | (head[17] << 24);
	int w = (int)(head[18] | (head[19] << 8) | (head[20] << 16) | (head[21] << 24));
	int h = (int)(head[22] | (head[23] << 8) | (head[24] << 16) | (head[25] << 24));
	int bpp = head[28] | (head[29] << 8);
	bool flip = h > 0;
	if (h < 0) h = -h;
	if (w <= 0 || w > BD_TEX || h <= 0 || h > BD_TEX_H ||
	    (bpp != 24 && bpp != 32 && bpp != 8 && bpp != 1)) {
		fclose(f); return false;
	}

	u8 palette[1024] = {0};
	if (bpp == 8 || bpp == 1) {
		fseek(f, 14 + info_len, SEEK_SET);
		fread(palette, 1, sizeof(palette), f);
	}
	int stride = ((w * bpp / 8) + 3) & ~3;
	u8 *row = malloc(stride);
	u16 *tex = linearAlloc(BD_TEX * BD_TEX_H * 2);
	if (!row || !tex) { free(row); if (tex) linearFree(tex); fclose(f); return false; }
	memset(tex, 0, BD_TEX * BD_TEX_H * 2);

	for (int line = 0; line < h; line++) {
		int y = flip ? (h - 1 - line) : line;
		fseek(f, data_off + (long)line * stride, SEEK_SET);
		if (fread(row, 1, stride, f) != (size_t)stride) break;
		for (int x = 0; x < w; x++) {
			u8 r, g, b;
			if (bpp == 1) {
				// Monochrome: eight pixels per byte, most significant first.
				int bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
				const u8 *p = palette + bit * 4;
				b = p[0]; g = p[1]; r = p[2];
			} else if (bpp == 8) {
				const u8 *p = palette + row[x] * 4;
				b = p[0]; g = p[1]; r = p[2];
			} else {
				const u8 *p = row + x * (bpp / 8);
				b = p[0]; g = p[1]; r = p[2];
			}
			int tile = (y >> 3) * (BD_TEX >> 3) + (x >> 3);
			tex[tile * 64 + morton8(x & 7, y & 7)] =
				(u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
	free(row);
	fclose(f);

	if (backdrop_ready) C3D_TexDelete(&backdrop_tex);
	backdrop_ready = C3D_TexInit(&backdrop_tex, BD_TEX, BD_TEX_H, GPU_RGB565);
	if (backdrop_ready) {
		C3D_TexSetFilter(&backdrop_tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexLoadImage(&backdrop_tex, tex, GPU_TEXFACE_2D, 0);
		C3D_TexFlush(&backdrop_tex);
		backdrop_w = w; backdrop_h = h;
	}
	linearFree(tex);
	return backdrop_ready;
}

#define WPS_DIR "sdmc:/3ds/audio3ds/wps"

static char wps_loaded_for[24] = "";

// A theme may ship a Rockbox .wps: the layout its author drew. When one loads,
// it replaces our own now-playing screen; otherwise we fall back to ours.
static void wps_apply(const Skin *sk)
{
	if (!strcmp(wps_loaded_for, sk->name)) return;
	snprintf(wps_loaded_for, sizeof(wps_loaded_for), "%s", sk->name);
	wps_unload();
	if (!sk->wps[0]) return;
	char file[256], dir[224];
	snprintf(file, sizeof(file), WPS_DIR "/%s", sk->wps);
	char stem[64];
	snprintf(stem, sizeof(stem), "%s", sk->wps);
	char *dot = strrchr(stem, '.');
	if (dot) *dot = 0;
	snprintf(dir, sizeof(dir), WPS_DIR "/%s", stem);
	wps_load(file, dir);
}

static void backdrop_apply(const Skin *sk)
{
	if (!strcmp(backdrop_name, sk->backdrop)) return;   // already current
	snprintf(backdrop_name, sizeof(backdrop_name), "%s", sk->backdrop);
	if (!sk->backdrop[0]) {
		snprintf(backdrop_status, sizeof(backdrop_status), "theme declares none");
	} else if (backdrop_load(sk->backdrop)) {
		snprintf(backdrop_status, sizeof(backdrop_status), "%s %dx%d",
		         sk->backdrop, backdrop_w, backdrop_h);
		return;
	} else {
		snprintf(backdrop_status, sizeof(backdrop_status), "%s NOT FOUND", sk->backdrop);
	}
	if (backdrop_ready) C3D_TexDelete(&backdrop_tex);
	backdrop_ready = false;
}

// Cover, not stretch: scale until the screen is filled and centre the
// overflow, so a 320x240 theme backdrop keeps its proportions on a 400x240
// screen instead of being pulled wide.
static void draw_backdrop(float screen_w, float screen_h)
{
	if (!backdrop_ready || backdrop_w <= 0 || backdrop_h <= 0) return;
	float sx = screen_w / backdrop_w, sy = screen_h / backdrop_h;
	float sc = sx > sy ? sx : sy;
	Tex3DS_SubTexture sub = {(u16)backdrop_w, (u16)backdrop_h, 0.0f, 1.0f,
	                         (float)backdrop_w / BD_TEX,
	                         1.0f - (float)backdrop_h / BD_TEX_H};
	C2D_Image img = {&backdrop_tex, &sub};
	C2D_DrawImageAt(img, (screen_w - backdrop_w * sc) / 2,
	                (screen_h - backdrop_h * sc) / 2, 0.0f, NULL, sc, sc);
}

static void themes_load(void)
{
	// Built-ins, so the player still looks right on a card with no theme files.
	themes[0] = (Skin){"Verde", "", "", "", 0xFF121212, 0xFF0A0A0A, 0xFF242424, 0xFF54CE72,
	                   0xFF0A0A0A, 0xFFFFFFFF, 0xFF9A9A9A, 0xFF3A3A3A, true};
	themes[1] = (Skin){"Clasica", "", "", "", 0xFFF4F2EF, 0xFFE9E6E2, 0xFFFFFFFF, 0xFFD67B2B,
	                   0xFFFFFFFF, 0xFF1A1A1A, 0xFF7A7A7A, 0xFFCFCBC6, false};
	theme_count = 2;

	DIR *dir = opendir(THEME_DIR);
	if (!dir) return;
	struct dirent *entry;
	while (theme_count < MAX_THEMES && (entry = readdir(dir)) != NULL) {
		const char *dot = strrchr(entry->d_name, '.');
		if (!dot || strcmp(dot, ".cfg") != 0) continue;
		char path[320];
		snprintf(path, sizeof(path), THEME_DIR "/%s", entry->d_name);
		Skin sk = themes[0];                    // inherit, then override
		snprintf(sk.name, sizeof(sk.name), "%.*s", (int)(dot - entry->d_name), entry->d_name);
		sk.author[0] = sk.backdrop[0] = sk.wps[0] = 0;
		theme_parse(path, &sk);
		themes[theme_count++] = sk;
	}
	closedir(dir);
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

// Walk the flat {"i":..,"name":"..","dir":".."} records the bridge returns.
static void library_fetch(void)
{
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

static void load_config(void)
{
	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f) return;
	char line[128];
	if (fgets(line, sizeof(line), f)) {
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
		// Art on the left, text stacked beside it.
		draw_art(sk, 20 + par * 3.2f, 56, 128);
		text_at(now_title[0] ? now_title : "---", 168 + par * 2.0f, 70, 0.62f, sk->text, 0);
		text_at(now_artist, 168 + par * 1.4f, 100, 0.50f, sk->accent, 0);
		text_at(now_album, 168 + par * 1.0f, 124, 0.44f, sk->dim, 0);
		C2D_DrawRectSolid(168 + par * 1.0f, 152, 0.0f, 64, 3, sk->accent);
		text_at(buffering ? "BUFFERING" : (muted ? "MUTED" : "PLAYING"),
		        168 + par * 1.0f, 164, 0.44f, sk->dim, 0);
		text_at("3DS AUDIO", 20 + par * 0.4f, 18, 0.50f, sk->dim, 0);
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
	IC_SOUND, IC_MUTED, IC_LIST, IC_SKIN, IC_SEARCH, IC_CLOSE
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
	case IC_SKIN:
		C2D_DrawCircleSolid(cx, cy, 0.1f, h * 0.95f, col);
		C2D_DrawTriangle(cx, cy - h, col, cx, cy + h, col, cx + h, cy + h, col, 0.2f);
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

static void draw_button(const Skin *sk, const Button *b, bool lit, float glyph)
{
	bool on = (b->accent != lit);
	C2D_DrawRectSolid(b->x, b->y, 0.0f, b->w, b->h, on ? sk->accent : sk->panel);
	if (!on) C2D_DrawRectSolid(b->x, b->y + b->h - 3, 0.05f, b->w, 3, sk->accent);
	if (lit) C2D_DrawRectSolid(b->x, b->y, 0.05f, b->w, 3, sk->text);
	draw_icon(b->icon, b->x + b->w / 2, b->y + b->h / 2, glyph,
	          on ? sk->on_accent : sk->text);
}

#define LIST_X 6.0f
#define LIST_W 178.0f
#define ROW_Y  22.0f
#define ROW_H  28.0f

// Left half of the split view. Browsing never takes the transport away: the
// controls keep their own column on the right.
static void draw_list_panel(const Skin *sk)
{
	char header[80];
	if (lib.query[0]) snprintf(header, sizeof(header), "%s (%d)", lib.query, lib.total);
	else snprintf(header, sizeof(header), "%d tracks", lib.total);
	text_at(header, LIST_X + 2, 4, 0.34f, sk->dim, 0);
	if (lib.total > LIB_ROWS) {
		char page[16];
		snprintf(page, sizeof(page), "%d+", lib.offset + 1);
		text_at(page, LIST_X + LIST_W - 2, 4, 0.32f, sk->dim, C2D_AlignRight);
	}
	if (lib.count == 0) {
		text_at(lib.loading ? "..." : "nothing found", LIST_X + LIST_W / 2, 96,
		        0.38f, sk->dim, C2D_AlignCenter);
		return;
	}
	for (int i = 0; i < lib.count; i++) {
		float y = ROW_Y + i * ROW_H;
		bool on = (i == lib.sel);
		if (on) C2D_DrawRectSolid(LIST_X, y, 0.0f, LIST_W, ROW_H - 3, sk->accent);
		text_at(lib.name[i], LIST_X + 5, y - 1, 0.33f, on ? sk->on_accent : sk->text, 0);
		if (lib.dir[i][0])
			text_at(lib.dir[i], LIST_X + 5, y + 12, 0.26f, on ? sk->on_accent : sk->dim, 0);
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

#define TH_Y 26.0f
#define TH_H 31.0f
#define TH_PAGE 6

static void draw_theme_panel(const Skin *sk)
{
	char head[40];
	snprintf(head, sizeof(head), "THEMES  (%d)", theme_count);
	text_at(head, 12, 5, 0.38f, sk->dim, 0);
	int first = (theme_sel / TH_PAGE) * TH_PAGE;
	for (int i = first; i < theme_count && i < first + TH_PAGE; i++) {
		float y = TH_Y + (i - first) * TH_H;
		const Skin *t = &themes[i];
		bool on = (i == theme_sel);
		if (on) C2D_DrawRectSolid(8, y, 0.0f, BOT_W - 16, TH_H - 4, sk->accent);
		// Swatches: pick by eye without having to apply each one first.
		C2D_DrawRectSolid(14, y + 5, 0.1f, 15, TH_H - 15, t->top_bg);
		C2D_DrawRectSolid(31, y + 5, 0.1f, 15, TH_H - 15, t->accent);
		C2D_DrawRectSolid(48, y + 5, 0.1f, 15, TH_H - 15, t->panel);
		text_at(t->name, 72, y + 2, 0.38f, on ? sk->on_accent : sk->text, 0);
		if (t->author[0])
			text_at(t->author, 72, y + 16, 0.26f, on ? sk->on_accent : sk->dim, 0);
		text_at(t->dark ? "art" : "list", BOT_W - 14, y + 8, 0.28f,
		        on ? sk->on_accent : sk->dim, C2D_AlignRight);
	}
	C2D_DrawRectSolid(BOT_W - 44, 4, 0.0f, 38, 20, sk->panel);
	draw_icon(IC_CLOSE, BOT_W - 25, 14, 11, sk->text);
	text_at("Up/Down pick   A apply   B back", BOT_W / 2 - 20, SCR_H - 15, 0.32f,
	        sk->dim, C2D_AlignCenter);
}

static void draw_bottom(const Skin *sk, const Button *buttons, int count,
                        u32 used, bool buffering, int flash)
{
	if (theme_open) {
		draw_theme_panel(sk);
		return;                 // exclusive: the buttons must not sit on top
	}
	if (lib.open) {
		C2D_DrawRectSolid(LIST_X + LIST_W + 4, 8, 0.0f, 1, SCR_H - 16, sk->meter);
		draw_list_panel(sk);
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
			draw_button(sk, &buttons[i], i == flash, buttons[i].h * 0.44f);
	text_at("build " BUILD_STAMP, BOT_W - 4, SCR_H - 11, 0.28f, sk->dim, C2D_AlignRight);
	text_at(wps_ready() ? wps_status() : backdrop_status, 4, SCR_H - 11, 0.28f, sk->dim, 0);
}

enum { B_PREV, B_PLAY, B_NEXT, B_MUTE, B_LIST, B_SKIN, B_SEARCH, B_COUNT };

// Two arrangements over one array. Zero width hides a button in that mode.
static void layout_buttons(Button *b, bool split)
{
	if (split) {
		b[B_PREV]   = (Button){194,  20, 120, 42, IC_PREV,  false};
		b[B_PLAY]   = (Button){194,  66, 120, 42, IC_PLAY,  true };
		b[B_NEXT]   = (Button){194, 112, 120, 42, IC_NEXT,  false};
		b[B_MUTE]   = (Button){194, 158, 120, 34, IC_SOUND, false};
		b[B_LIST]   = (Button){194, 196, 120, 34, IC_CLOSE, false};
		b[B_SKIN]   = (Button){0, 0, 0, 0, IC_SKIN, false};
		b[B_SEARCH] = (Button){LIST_X, 196, LIST_W, 34, IC_SEARCH, false};
	} else {
		b[B_PREV]   = (Button){ 16, 118,  88, 56, IC_PREV,  false};
		b[B_PLAY]   = (Button){116, 118,  88, 56, IC_PLAY,  true };
		b[B_NEXT]   = (Button){216, 118,  88, 56, IC_NEXT,  false};
		b[B_MUTE]   = (Button){ 16, 184,  92, 40, IC_SOUND, false};
		b[B_LIST]   = (Button){114, 184,  92, 40, IC_LIST,  false};
		b[B_SKIN]   = (Button){212, 184,  92, 40, IC_SKIN,  false};
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

int main(void)
{
	gfxInitDefault();
	load_config();
	themes_load();

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
	if (fail) { fail_screen(fail); gfxExit(); return 1; }

	audio_base = audio;
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

	Thread net = threadCreate(stream_thread, NULL, 32 * 1024, 0x30, -2, false);
	Thread meta = threadCreate(metadata_thread, NULL, 32 * 1024, 0x3F, -2, false);
	Thread cmd = threadCreate(command_thread, NULL, 48 * 1024, 0x3F, -2, false);
	Thread snd = threadCreate(audio_thread, NULL, 16 * 1024, 0x18, -2, false);

	Button buttons[B_COUNT];
	layout_buttons(buttons, false);

	// One second: the link measured comfortable at 1024 kbps, and every extra
	// second of cushion is a second of lag after a skip.
	prebuffer_bytes = (u32)sample_rate * 4;
	bool muted = false;
	int skin = 0, flash = -1, flash_ttl = 0;


	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		if (down & KEY_START) break;
		touchPosition touch = {0, 0};
		bool touch_used = false, input_used = false;
		if (down & KEY_TOUCH) hidTouchRead(&touch);

		if (theme_open) {
			if (down & KEY_DUP)   theme_sel = (theme_sel + theme_count - 1) % theme_count;
			if (down & KEY_DDOWN) theme_sel = (theme_sel + 1) % theme_count;
			if (down & KEY_TOUCH) {
				if (touch.px > BOT_W - 44 && touch.py < 24) {
					theme_open = false;
				} else {
					int first = (theme_sel / TH_PAGE) * TH_PAGE;
					int row = (int)((touch.py - TH_Y) / TH_H);
					if (row >= 0 && row < TH_PAGE && first + row < theme_count) {
						theme_sel = first + row;
						skin = theme_sel;
					}
				}
			}
			if (down & KEY_A) skin = theme_sel;
			if (down & (KEY_B | KEY_X | KEY_SELECT)) theme_open = false;
			goto skip_input;
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
			if (((down & KEY_A) || (row >= 0 && row < lib.count)) && lib.count) {
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
		if (down & (KEY_SELECT | KEY_X)) pressed = B_SKIN;
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
			case B_SKIN: theme_open = true; theme_sel = skin; break;
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
				swkbdSetHintText(&kb, "Search your library");
				swkbdSetInitialText(&kb, lib.query);
				char typed[sizeof(lib.query)] = "";
				if (swkbdInputText(&kb, typed, sizeof(typed)) == SWKBD_BUTTON_RIGHT) {
					strncpy(lib.query, typed, sizeof(lib.query) - 1);
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

		const Skin *sk = &themes[skin];
		backdrop_apply(sk);
		wps_apply(sk);
		buttons[B_PLAY].icon = server_playing ? IC_PAUSE : IC_PLAY;
		buttons[B_MUTE].icon = muted ? IC_MUTED : IC_SOUND;

		// The slider is the user's control: at zero the right eye is never
		// drawn, so 2D costs nothing extra.
		float depth = osGet3DSliderState() * 4.0f;
		C2D_TextBufClear(text_buf);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, sk->top_bg);
		C2D_SceneBegin(top);
		draw_backdrop(TOP_W, SCR_H);
		if (wps_ready()) {
			// Themes are drawn for 320x240; centre them on the 400-wide screen
			// at native size rather than stretching the author's pixels.
			WpsState ws = {now_title, now_artist, now_album, now_pos, now_dur,
			               server_playing, art_present, {&art_tex, &art_sub}};
			wps_render(&ws, (TOP_W - WPS_W) / 2 - depth, 0);
		} else {
			draw_top(sk, buffering, muted, -depth);
		}
		if (depth > 0.0f) {
			C2D_TargetClear(top_right, sk->top_bg);
			C2D_SceneBegin(top_right);
			draw_backdrop(TOP_W, SCR_H);
			if (wps_ready()) {
				WpsState ws = {now_title, now_artist, now_album, now_pos, now_dur,
				               server_playing, art_present, {&art_tex, &art_sub}};
				wps_render(&ws, (TOP_W - WPS_W) / 2 + depth, 0);
			} else {
				draw_top(sk, buffering, muted, depth);
			}
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
