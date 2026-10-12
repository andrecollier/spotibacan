#include "screen_player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "lang.h"

/* Bottom screen is 320x240. The layout follows the reference client's player
 * stripped to three transport controls: prev / play-pause / next on one
 * baseline, a fixed header, and a display-only scrubber. */

#define HEADER_H   30.0f
#define ROW_Y      150.0f /* transport baseline */
#define CTRL_GAP   26.0f
#define CTRL_SMALL 34.0f
#define PLAY_R     20.0f /* 40px disc */

#define SCRUB_TIMES_Y 196.0f

#define CLR_WHITE   C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_DISC_FG C2D_Color32(0x0B, 0x0B, 0x0B, 0xFF)
#define CLR_IDLE    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)
#define CLR_LABEL   C2D_Color32(0x6A, 0x6A, 0x6A, 0xFF)
#define CLR_TRACK   C2D_Color32(0x4D, 0x4D, 0x4D, 0xFF)
#define CLR_TILE    C2D_Color32(0x19, 0x19, 0x19, 0xFF)
#define CLR_TIME    C2D_Color32(0xB3, 0xB3, 0xB3, 0xFF)

/* citro2d packs RGBA with alpha in the top byte, so the pressed halo and the
 * disc's fade ring are the accent with its alpha swapped out. */
static u32 with_alpha(u32 clr, u8 alpha)
{
	return (clr & 0x00FFFFFFu) | ((u32)alpha << 24);
}

/* With the boxes gone there is no fill to highlight, so the glyph itself has
 * to carry the press: a pressed control turns accent-green and gets a soft
 * halo behind it, which is what actually makes the tap feel like it landed. */
static u32 press_clr(u32 clr, bool pressed, u32 accent)
{
	return pressed ? accent : clr;
}

static void press_halo(float cx, float cy, bool pressed, u32 accent)
{
	if (pressed)
		ui_disc(cx, cy, 19.0f, with_alpha(accent, 0x33));
}

static void tri_right(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x, y, clr, x, y + h, clr, x + w, y + h / 2, clr, 0.0f);
}

static void tri_left(float x, float y, float w, float h, u32 clr)
{
	C2D_DrawTriangle(x + w, y, clr, x + w, y + h, clr, x, y + h / 2, clr,
	                 0.0f);
}

static void draw_prev(float cx, float cy, u32 clr)
{
	tri_left(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx - 9.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

static void draw_next(float cx, float cy, u32 clr)
{
	tri_right(cx - 6.0f, cy - 8.0f, 12.0f, 16.0f, clr);
	C2D_DrawRectSolid(cx + 6.0f, cy - 8.0f, 0.0f, 3.0f, 16.0f, clr);
}

static void draw_play_disc(float cx, float cy, bool pressed, u32 accent)
{
	const int segments = 32;
	const float inner_r = PLAY_R - 0.75f;
	const float outer_r = PLAY_R + 0.25f;
	const u32 fill = pressed ? accent : CLR_WHITE;
	const u32 clear = with_alpha(fill, 0x00);
	float inner_px = cx + inner_r;
	float inner_py = cy;
	float outer_px = cx + outer_r;
	float outer_py = cy;

	for (int i = 1; i <= segments; i++) {
		const float angle =
		    (float)i * 2.0f * (float)M_PI / (float)segments;
		const float inner_nx = cx + inner_r * cosf(angle);
		const float inner_ny = cy + inner_r * sinf(angle);
		const float outer_nx = cx + outer_r * cosf(angle);
		const float outer_ny = cy + outer_r * sinf(angle);

		C2D_DrawTriangle(cx, cy, fill, inner_px, inner_py, fill, inner_nx,
		                 inner_ny, fill, 0.0f);
		C2D_DrawTriangle(inner_px, inner_py, fill, outer_px, outer_py,
		                 clear, outer_nx, outer_ny, clear, 0.0f);
		C2D_DrawTriangle(inner_px, inner_py, fill, outer_nx, outer_ny,
		                 clear, inner_nx, inner_ny, fill, 0.0f);

		inner_px = inner_nx;
		inner_py = inner_ny;
		outer_px = outer_nx;
		outer_py = outer_ny;
	}
}

static void draw_playpause(float cx, float cy, bool playing, bool pressed,
                           u32 accent)
{
	/* The disc is already the brightest thing on screen, so it presses by
	 * tinting rather than by gaining a halo it would hide anyway. */
	draw_play_disc(cx, cy, pressed, accent);

	if (playing) {
		C2D_DrawRectSolid(cx - 6.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f,
		                  CLR_DISC_FG);
		C2D_DrawRectSolid(cx + 2.0f, cy - 7.5f, 0.0f, 4.0f, 15.0f,
		                  CLR_DISC_FG);
	} else {
		/* Nudged right so the triangle looks centred in the disc. */
		tri_right(cx - 4.0f, cy - 8.0f, 14.0f, 16.0f, CLR_DISC_FG);
	}
}

/* Speaker glyph for the mute toggle: a box plus a two-triangle cone, the
 * same proven shape as the volume overlay. Audible gets two chevrons opening
 * to the right, muted gets an X. */
static void draw_speaker(float cx, float cy, bool muted, u32 clr)
{
	C2D_DrawRectSolid(cx - 11.0f, cy - 4.0f, 0.0f, 5.0f, 8.0f, clr);
	C2D_DrawTriangle(cx - 6.0f, cy - 4.0f, clr, cx, cy - 9.0f, clr,
	                 cx, cy + 9.0f, clr, 0.0f);
	C2D_DrawTriangle(cx - 6.0f, cy - 4.0f, clr, cx, cy + 9.0f, clr,
	                 cx - 6.0f, cy + 4.0f, clr, 0.0f);
	if (muted) {
		const float down[4] = {cx + 4.0f, cy - 5.0f, cx + 11.0f, cy + 5.0f};
		const float up[4]   = {cx + 11.0f, cy - 5.0f, cx + 4.0f, cy + 5.0f};
		ui_polyline(down, 2, 2.0f, clr);
		ui_polyline(up, 2, 2.0f, clr);
	} else {
		const float near[6] = {cx + 3.0f, cy - 4.0f, cx + 6.5f, cy,
		                       cx + 3.0f, cy + 4.0f};
		const float far[6]  = {cx + 7.0f, cy - 7.0f, cx + 11.5f, cy,
		                       cx + 7.0f, cy + 7.0f};
		ui_polyline(near, 3, 1.8f, clr);
		ui_polyline(far, 3, 1.8f, clr);
	}
}

static void fmt_time(long ms, char *out, size_t out_sz)
{
	if (ms < 0)
		ms = 0;
	snprintf(out, out_sz, "%ld:%02ld", ms / 60000, (ms / 1000) % 60);
}

static void draw_header(const screen_player_args *a)
{
	const bool mute_pressed = a->pressed_id == BTN_P_MUTE;
	const bool lyr_pressed = a->pressed_id == BTN_P_LYRICS;
	const bool lib_pressed = a->pressed_id == BTN_P_LIBRARY;

	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, HEADER_H, a->panel);

	draw_speaker(20.0f, HEADER_H / 2, a->muted,
	             press_clr(CLR_IDLE, mute_pressed, a->accent));
	tb_add_hit(a->tb, 20.0f, HEADER_H / 2, 44.0f, BTN_P_MUTE);

	ui_text(a->buf, a->track, 44.0f, ui_baseline(7.0f, TY_ROW_NAME),
	        TY_ROW_NAME, 186.0f - 44.0f, CLR_WHITE);

	/* Two wide hit zones on the right: lyrics and the library. The whole
	 * stretch answers, the same way the reference gives its header labels a
	 * wide area rather than just the glyph. */
	{
		float w = ui_text_width(a->buf, tr(S_LYRICS), TY_MICRO) + 4.0f;
		ui_text_tracked(a->buf, tr(S_LYRICS), 246.0f - w,
		                ui_baseline(9.0f, TY_MICRO), TY_MICRO, 1.0f,
		                press_clr(CLR_LABEL, lyr_pressed, a->accent));
		tb_add(a->tb, 186.0f, 0.0f, 66.0f, HEADER_H, BTN_P_LYRICS);
	}
	{
		float w = ui_text_width(a->buf, tr(S_LIBRARY), TY_MICRO);
		ui_text_tracked(a->buf, tr(S_LIBRARY), 296.0f - w,
		                ui_baseline(9.0f, TY_MICRO), TY_MICRO, 1.0f,
		                press_clr(CLR_LABEL, lib_pressed, a->accent));
		tri_right(302.0f, HEADER_H / 2 - 5.0f, 6.0f, 10.0f,
		          press_clr(CLR_LABEL, lib_pressed, a->accent));
		tb_add(a->tb, 252.0f, 0.0f, 68.0f, HEADER_H, BTN_P_LIBRARY);
	}
}

static void draw_centre(const screen_player_args *a)
{
	if (a->loading) {
		/* A placeholder tile with the animated badge stands in for the
		 * cover art the backend cannot fetch yet. */
		const float tile = 50.0f;
		const float x = 160.0f - tile / 2;
		const float y = 62.0f;
		C2D_DrawRectSolid(x, y, 0.0f, tile, tile, CLR_TILE);
		ui_now_playing_badge(x, y, tile, true, a->animation_ms);
		return;
	}

	/* No art yet, so the artist carries the centre the way the reference's
	 * art-hidden layout does: large role, then the album below it. */
	{
		float y = ui_baseline(70.0f, TY_ARTIST_L);
		float w = ui_text_width(a->buf, a->artist, TY_ARTIST_L);
		if (w > 300.0f)
			w = 300.0f;
		ui_text(a->buf, a->artist, 160.0f - w / 2, y, TY_ARTIST_L,
		        300.0f, CLR_WHITE);

		y = ui_baseline(y + 6.0f, TY_ALBUM);
		w = ui_text_width(a->buf, a->album, TY_ALBUM);
		if (w > 300.0f)
			w = 300.0f;
		ui_text(a->buf, a->album, 160.0f - w / 2, y, TY_ALBUM, 300.0f,
		        CLR_LABEL);
	}
}

static void draw_transport(const screen_player_args *a)
{
	const float play_x = 160.0f;
	const float prev_x = play_x - (PLAY_R + CTRL_GAP + CTRL_SMALL / 2);
	const float next_x = play_x + (PLAY_R + CTRL_GAP + CTRL_SMALL / 2);

	const bool prev_pressed = a->pressed_id == BTN_P_PREV;
	const bool next_pressed = a->pressed_id == BTN_P_NEXT;

	/* Halos first, so every glyph draws on top of its own. */
	press_halo(prev_x, ROW_Y, prev_pressed, a->accent);
	press_halo(next_x, ROW_Y, next_pressed, a->accent);

	draw_prev(prev_x, ROW_Y,
	          press_clr(a->can_prev ? CLR_WHITE : CLR_IDLE, prev_pressed,
	                    a->accent));
	draw_next(next_x, ROW_Y,
	          press_clr(a->can_next ? CLR_WHITE : CLR_IDLE, next_pressed,
	                    a->accent));
	draw_playpause(play_x, ROW_Y, a->playing, a->pressed_id == BTN_P_PLAY,
	               a->accent);

	/* Centre-outward: the largest, most-tapped control wins contested
	 * pixels. */
	tb_add_hit(a->tb, play_x, ROW_Y, 44.0f, BTN_P_PLAY);
	tb_add_hit(a->tb, prev_x, ROW_Y, 44.0f, BTN_P_PREV);
	tb_add_hit(a->tb, next_x, ROW_Y, 44.0f, BTN_P_NEXT);
}

static void draw_scrubber(const screen_player_args *a)
{
	char elapsed[12], total[12];
	fmt_time(a->progress_ms, elapsed, sizeof(elapsed));
	fmt_time(a->duration_ms, total, sizeof(total));

	const float base = ui_baseline(SCRUB_TIMES_Y - 12.0f, TY_ROW_SUB);
	ui_text(a->buf, elapsed, 16.0f, base, TY_ROW_SUB, 40.0f, CLR_TIME);
	{
		float w = ui_text_width(a->buf, total, TY_ROW_SUB);
		ui_text(a->buf, total, 304.0f - w, base, TY_ROW_SUB, 40.0f,
		        CLR_TIME);
	}

	C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W,
	                  UI_PROGRESS_BAR_H, CLR_TRACK);

	float frac = 0.0f;
	if (a->duration_ms > 0) {
		frac = (float)a->progress_ms / (float)a->duration_ms;
		if (frac > 1.0f)
			frac = 1.0f;
		if (frac < 0.0f)
			frac = 0.0f;
	}
	C2D_DrawRectSolid(SCRUB_BAR_X, SCRUB_BAR_Y, 0.0f, SCRUB_BAR_W * frac,
	                  UI_PROGRESS_BAR_H, CLR_WHITE);
	ui_disc(SCRUB_BAR_X + SCRUB_BAR_W * frac, SCRUB_BAR_Y + 2.0f, 5.0f,
	        a->pressed_id == BTN_P_SCRUB ? a->accent : CLR_WHITE);

	/* Generous vertical padding: the bar itself is 4px tall, untouchable. */
	tb_add(a->tb, SCRUB_BAR_X - 8.0f, SCRUB_BAR_Y - 10.0f,
	       SCRUB_BAR_W + 16.0f, 24.0f, BTN_P_SCRUB);
}

void screen_player_draw(const screen_player_args *a)
{
	draw_header(a);
	draw_centre(a);
	draw_transport(a);
	draw_scrubber(a);
}
