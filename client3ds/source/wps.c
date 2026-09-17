#include "wps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BITMAPS 64
#define MAX_VIEWPORTS 32
#define MAX_LINES 256
#define LINE_LEN 320

typedef struct {
	char id;              // the letter %xl gives it
	C3D_Tex tex;
	int w, h;             // one frame
	int frames;
	int tex_w, tex_h;
	bool ok;
} Bitmap;

typedef struct {
	char label;           // 0 for the unnamed default viewport
	int x, y, w, h;
	u32 fg;
	bool has_fg;
} Viewport;

static Bitmap bitmaps[MAX_BITMAPS];
static int bitmap_count;
static Viewport viewports[MAX_VIEWPORTS];
static int viewport_count;
static char lines[MAX_LINES][LINE_LEN];
static int line_count;
static bool loaded;
static char status[96] = "no theme";
static char base_dir[192];

// --------------------------------------------------------------- bitmaps

static inline int morton8(int x, int y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1)
	     | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static int pow2(int v)
{
	int n = 8;
	while (n < v && n < 1024) n <<= 1;
	return n;
}

// Rockbox ships theme images as plain BMPs; frame strips are stacked vertically.
static bool bmp_load(const char *path, Bitmap *bm, int frames)
{
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
	bool bottom_up = h > 0;
	if (h < 0) h = -h;
	if (w <= 0 || h <= 0 || w > 1024 || h > 1024 ||
	    (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) {
		fclose(f); return false;
	}

	u8 palette[1024] = {0};
	if (bpp <= 8) {
		fseek(f, 14 + info_len, SEEK_SET);
		fread(palette, 1, sizeof(palette), f);
	}

	int tw = pow2(w), th = pow2(h);
	int stride = ((w * bpp / 8) + 3) & ~3;
	if (bpp < 8) stride = ((w * bpp + 7) / 8 + 3) & ~3;
	u8 *row = malloc(stride);
	u16 *pixels = linearAlloc(tw * th * 2);
	if (!row || !pixels) { free(row); if (pixels) linearFree(pixels); fclose(f); return false; }
	memset(pixels, 0, tw * th * 2);

	for (int line = 0; line < h; line++) {
		int y = bottom_up ? (h - 1 - line) : line;
		fseek(f, data_off + (long)line * stride, SEEK_SET);
		if (fread(row, 1, stride, f) != (size_t)stride) break;
		for (int x = 0; x < w; x++) {
			u8 r, g, b;
			if (bpp == 1) {
				const u8 *p = palette + (((row[x >> 3] >> (7 - (x & 7))) & 1) * 4);
				b = p[0]; g = p[1]; r = p[2];
			} else if (bpp == 4) {
				int idx = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4);
				const u8 *p = palette + idx * 4;
				b = p[0]; g = p[1]; r = p[2];
			} else if (bpp == 8) {
				const u8 *p = palette + row[x] * 4;
				b = p[0]; g = p[1]; r = p[2];
			} else {
				const u8 *p = row + x * (bpp / 8);
				b = p[0]; g = p[1]; r = p[2];
			}
			int tile = (y >> 3) * (tw >> 3) + (x >> 3);
			pixels[tile * 64 + morton8(x & 7, y & 7)] =
				(u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
	free(row);
	fclose(f);

	bm->ok = C3D_TexInit(&bm->tex, tw, th, GPU_RGB565);
	if (bm->ok) {
		C3D_TexSetFilter(&bm->tex, GPU_NEAREST, GPU_NEAREST);  // pixel art
		C3D_TexLoadImage(&bm->tex, pixels, GPU_TEXFACE_2D, 0);
		C3D_TexFlush(&bm->tex);
		bm->tex_w = tw; bm->tex_h = th;
		bm->frames = frames > 0 ? frames : 1;
		bm->w = w;
		bm->h = h / bm->frames;
	}
	linearFree(pixels);
	return bm->ok;
}

static Bitmap *bitmap_find(char id)
{
	for (int i = 0; i < bitmap_count; i++)
		if (bitmaps[i].id == id) return &bitmaps[i];
	return NULL;
}

static Viewport *viewport_find(char label)
{
	for (int i = 0; i < viewport_count; i++)
		if (viewports[i].label == label) return &viewports[i];
	return NULL;
}

// ---------------------------------------------------------------- loading

static void strip(char *s)
{
	s[strcspn(s, "\r\n")] = 0;
}

bool wps_load(const char *wps_path, const char *dir)
{
	wps_unload();
	snprintf(base_dir, sizeof(base_dir), "%s", dir);
	FILE *f = fopen(wps_path, "r");
	if (!f) {
		snprintf(status, sizeof(status), "wps not found");
		return false;
	}
	char line[LINE_LEN];
	int skipped = 0;
	while (fgets(line, sizeof(line), f) && line_count < MAX_LINES) {
		strip(line);
		if (line[0] == '#' || line[0] == 0) continue;

		// %xl(id,file,x,y[,frames]) preloads an image, possibly a frame strip.
		if (!strncmp(line, "%xl(", 4) && bitmap_count < MAX_BITMAPS) {
			char id, file[128];
			int x, y, frames = 1;
			int got = sscanf(line + 4, "%c,%127[^,],%d,%d,%d", &id, file, &x, &y, &frames);
			if (got >= 4) {
				char *close = strchr(file, ')');
				if (close) *close = 0;
				char path[320];
				snprintf(path, sizeof(path), "%s/%s", base_dir, file);
				Bitmap *bm = &bitmaps[bitmap_count];
				memset(bm, 0, sizeof(*bm));
				bm->id = id;
				if (bmp_load(path, bm, frames)) bitmap_count++;
				else skipped++;
			}
			continue;
		}
		snprintf(lines[line_count++], LINE_LEN, "%s", line);
	}
	fclose(f);

	// Viewport declarations are picked up on the first render pass instead of
	// here, because a theme may redefine one partway down the file.
	loaded = line_count > 0;
	snprintf(status, sizeof(status), "%d bmp, %d lines%s",
	         bitmap_count, line_count, skipped ? ", some images missing" : "");
	return loaded;
}

void wps_unload(void)
{
	for (int i = 0; i < bitmap_count; i++)
		if (bitmaps[i].ok) C3D_TexDelete(&bitmaps[i].tex);
	bitmap_count = viewport_count = line_count = 0;
	loaded = false;
}

bool wps_ready(void) { return loaded; }
const char *wps_status(void) { return status; }

// --------------------------------------------------------------- rendering

typedef struct {
	const WpsState *st;
	float ox, oy;             // where the 320x240 theme sits on screen
	Viewport vp;              // current viewport
	int align;                // 0 left, 1 centre, 2 right
	C2D_TextBuf text_buf;
	int art_x, art_y, art_w, art_h;   // last %Cl rectangle
} Ctx;

static void draw_text_line(Ctx *c, const char *s)
{
	if (!*s) return;
	C2D_Text t;
	C2D_TextParse(&t, c->text_buf, s);
	C2D_TextOptimize(&t);
	float x = c->ox + c->vp.x;
	u32 flags = C2D_WithColor;
	if (c->align == 1) { x += c->vp.w / 2.0f; flags |= C2D_AlignCenter; }
	else if (c->align == 2) { x += c->vp.w; flags |= C2D_AlignRight; }
	C2D_DrawText(&t, flags, x, c->oy + c->vp.y, 0.4f, 0.42f, 0.42f,
	             c->vp.has_fg ? c->vp.fg : 0xFFFFFFFF);
}

static void draw_bitmap(Ctx *c, char id, int frame)
{
	Bitmap *bm = bitmap_find(id);
	if (!bm || !bm->ok) return;
	if (frame < 0 || frame >= bm->frames) frame = 0;
	Tex3DS_SubTexture sub = {
		(u16)bm->w, (u16)bm->h,
		0.0f,
		1.0f - (float)(frame * bm->h) / bm->tex_h,
		(float)bm->w / bm->tex_w,
		1.0f - (float)((frame + 1) * bm->h) / bm->tex_h,
	};
	C2D_Image img = {&bm->tex, &sub};
	C2D_DrawImageAt(img, c->ox + c->vp.x, c->oy + c->vp.y, 0.3f, NULL, 1.0f, 1.0f);
}

// Which branch of %?tag<...> applies. Unknown tags take the first branch: a
// theme that draws something is more useful than one that draws nothing.
static int condition_index(const Ctx *c, const char *tag)
{
	const WpsState *s = c->st;
	if (!strncmp(tag, "it", 2) || !strncmp(tag, "fn", 2)) return s->title[0] ? 0 : 1;
	if (!strncmp(tag, "ia", 2) || !strncmp(tag, "iA", 2)) return s->artist[0] ? 0 : 1;
	if (!strncmp(tag, "id", 2)) return s->album[0] ? 0 : 1;
	if (tag[0] == 'C') return s->has_art ? 0 : 1;
	if (!strncmp(tag, "mp", 2)) return s->playing ? 1 : 2;   // stop|play|pause|...
	if (!strncmp(tag, "bl", 2)) return 5;                    // mid battery
	if (!strncmp(tag, "pv", 2)) return 5;                    // mid volume
	if (!strncmp(tag, "mh", 2)) return 1;                    // hold off
	if (!strncmp(tag, "ps", 2) || !strncmp(tag, "mm", 2)) return 1;
	return 0;
}

static const char *tag_text(const Ctx *c, const char *tag, char *tmp, size_t n)
{
	const WpsState *s = c->st;
	if (!strncmp(tag, "it", 2) || !strncmp(tag, "fn", 2)) return s->title;
	if (!strncmp(tag, "ia", 2) || !strncmp(tag, "iA", 2)) return s->artist;
	if (!strncmp(tag, "id", 2)) return s->album;
	if (!strncmp(tag, "pc", 2)) {
		int t = (int)s->position;
		snprintf(tmp, n, "%d:%02d", t / 60, t % 60);
		return tmp;
	}
	if (!strncmp(tag, "pt", 2) || !strncmp(tag, "pe", 2)) {
		int t = (int)s->duration;
		snprintf(tmp, n, "%d:%02d", t / 60, t % 60);
		return tmp;
	}
	if (!strncmp(tag, "pr", 2)) {
		int t = (int)(s->duration - s->position);
		if (t < 0) t = 0;
		snprintf(tmp, n, "-%d:%02d", t / 60, t % 60);
		return tmp;
	}
	return "";
}

static void render_span(Ctx *c, const char *s, const char *end);

// Walk to the matching '>' of a conditional, respecting nesting.
static const char *match_close(const char *s, const char *end)
{
	int depth = 0;
	for (; s < end; s++) {
		if (*s == '<') depth++;
		else if (*s == '>') { if (--depth == 0) return s; }
	}
	return end;
}

static void render_span(Ctx *c, const char *s, const char *end)
{
	char text[LINE_LEN] = "";
	size_t used = 0;
	char tmp[32];

	while (s < end) {
		if (*s != '%') { if (used < sizeof(text) - 1) text[used++] = *s; s++; continue; }
		s++;
		if (s >= end) break;

		if (*s == 's' || *s == 'w') { s++; continue; }            // scroll / status bar
		if (*s == 'a' && s + 1 < end &&
		    (s[1] == 'l' || s[1] == 'c' || s[1] == 'r' || s[1] == 'x')) {
			if (s[1] != 'x') c->align = (s[1] == 'l') ? 0 : (s[1] == 'c' ? 1 : 2);
			s += 2;
			continue;
		}
		if (*s == '?') {                                          // conditional
			s++;
			const char *tag = s;
			while (s < end && *s != '<') s++;
			char name[8] = {0};
			size_t len = (size_t)(s - tag) < sizeof(name) - 1 ? (size_t)(s - tag) : sizeof(name) - 1;
			memcpy(name, tag, len);
			const char *close = match_close(s, end);
			int want = condition_index(c, name);
			// Pick the branch: options are separated by '|' at depth one.
			const char *p = s + 1;
			int index = 0, depth = 0;
			const char *start = p;
			while (p < close) {
				if (*p == '<') depth++;
				else if (*p == '>') depth--;
				else if (*p == '|' && depth == 0) {
					if (index == want) { render_span(c, start, p); break; }
					index++;
					start = p + 1;
				}
				p++;
			}
			if (p >= close && index == want) render_span(c, start, close);
			s = close < end ? close + 1 : end;
			continue;
		}
		if (*s == 'V') {                                          // viewport
			s++;
			char label = 0;
			if (s < end && (*s == 'l' || *s == 'd')) { label = *s; s++; }
			if (s < end && *s == '(') {
				char body[128] = {0};
				const char *b = ++s;
				while (s < end && *s != ')') s++;
				size_t n = (size_t)(s - b) < sizeof(body) - 1 ? (size_t)(s - b) : sizeof(body) - 1;
				memcpy(body, b, n);
				if (label == 'd') {                              // display a stored one
					Viewport *v = viewport_find(body[0]);
					if (v) c->vp = *v;
				} else if (label == 'l') {                        // declare and use
					char id; int x, y, w, h;
					if (sscanf(body, "%c,%d,%d,%d,%d", &id, &x, &y, &w, &h) == 5) {
						Viewport v = {id, x, y, w < 0 ? WPS_W - x : w,
						              h < 0 ? WPS_H - y : h, 0, false};
						if (viewport_count < MAX_VIEWPORTS && !viewport_find(id))
							viewports[viewport_count++] = v;
						else if (viewport_find(id)) *viewport_find(id) = v;
						c->vp = v;
					}
				} else {
					int x, y, w, h;
					if (sscanf(body, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
						c->vp = (Viewport){0, x, y, w < 0 ? WPS_W - x : w,
						                   h < 0 ? WPS_H - y : h, 0, false};
				}
				if (s < end) s++;
			}
			c->align = 0;
			continue;
		}
		if (*s == 'x' && s + 1 < end && s[1] == 'd') {            // draw bitmap
			s += 2;
			if (s < end && *s == '(') s++;
			char id = (s < end) ? *s++ : 0;
			int frame = 0;
			if (s < end && isalpha((unsigned char)*s)) frame = tolower((unsigned char)*s++) - 'a';
			if (s < end && *s == ')') s++;
			draw_bitmap(c, id, frame);
			continue;
		}
		if (*s == 'C') {                                          // album art
			s++;
			if (s < end && *s == 'l' && s + 1 < end && s[1] == '(') {
				s += 2;
				int x, y, w, h;
				if (sscanf(s, "%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
					c->art_x = x; c->art_y = y; c->art_w = w; c->art_h = h;
				}
				while (s < end && *s != ')') s++;
				if (s < end) s++;
			} else if (s < end && *s == 'd') {
				s++;
				if (c->st->has_art && c->art_w > 0) {
					C2D_DrawImageAt(c->st->art, c->ox + c->vp.x + c->art_x,
					                c->oy + c->vp.y + c->art_y, 0.35f, NULL,
					                c->art_w / 128.0f, c->art_h / 128.0f);
				}
			}
			continue;
		}
		if (*s == 'p' && s + 1 < end && s[1] == 'b') {            // progress bar
			s += 2;
			int x = 0, y = 0, w = c->vp.w, h = 6;
			if (s < end && *s == '(') sscanf(s + 1, "%d,%d,%d,%d", &x, &y, &w, &h);
			while (s < end && *s != ')' && *s != '%') s++;
			if (s < end && *s == ')') s++;
			float done = c->st->duration > 0.5f ? c->st->position / c->st->duration : 0.0f;
			if (done > 1.0f) done = 1.0f;
			u32 ink = c->vp.has_fg ? c->vp.fg : 0xFFFFFFFF;
			C2D_DrawRectSolid(c->ox + c->vp.x + x, c->oy + c->vp.y + y, 0.3f, w, h, 0x60000000);
			C2D_DrawRectSolid(c->ox + c->vp.x + x, c->oy + c->vp.y + y, 0.31f, w * done, h, ink);
			continue;
		}
		// Text-producing tags, and anything else we do not model yet.
		{
			char name[4] = {0};
			name[0] = *s;
			if (s + 1 < end && isalpha((unsigned char)s[1])) name[1] = s[1];
			const char *val = tag_text(c, name, tmp, sizeof(tmp));
			while (*val && used < sizeof(text) - 1) text[used++] = *val++;
			s += name[1] ? 2 : 1;
			if (s < end && *s == '(') { while (s < end && *s != ')') s++; if (s < end) s++; }
		}
	}
	text[used] = 0;
	draw_text_line(c, text);
}

void wps_render(const WpsState *state, float origin_x, float origin_y)
{
	if (!loaded) return;
	static C2D_TextBuf buf;
	if (!buf) buf = C2D_TextBufNew(2048);
	C2D_TextBufClear(buf);

	Ctx c = {0};
	c.st = state;
	c.ox = origin_x;
	c.oy = origin_y;
	c.vp = (Viewport){0, 0, 0, WPS_W, WPS_H, 0, false};
	c.text_buf = buf;
	c.art_w = c.art_h = 0;

	for (int i = 0; i < line_count; i++) {
		const char *s = lines[i];
		render_span(&c, s, s + strlen(s));
	}
}
