#include "screen_list.h"

#include <math.h>

#include "thumbs.h"
#include "ui.h"

#define THUMB_SZ 34.0f
#define THUMB_X  8.0f
#define TEXT_X   52.0f

#define CLR_WHITE C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_TILE  C2D_Color32(0x19, 0x19, 0x19, 0xFF)
#define CLR_BARS  C2D_Color32(0x8F, 0x8F, 0x8F, 0xFF)

static void draw_search_icon(float cx, float cy, u32 clr, u32 bg)
{
	/* Lucide-style magnifier: a thin open ring and a short round-capped
	 * handle read sharper at this size than the old filled donut. */
	ui_disc(cx - 1.0f, cy - 1.5f, 5.0f, clr);
	ui_disc(cx - 1.0f, cy - 1.5f, 3.4f, bg);
	const float handle[4] = {cx + 2.8f, cy + 2.3f, cx + 6.6f, cy + 6.1f};
	ui_polyline(handle, 2, 2.2f, clr);
	ui_disc(cx + 6.6f, cy + 6.1f, 1.1f, clr);
}

static void draw_close_icon(float cx, float cy, u32 clr)
{
	const float a[4] = {cx - 5.0f, cy - 5.0f, cx + 5.0f, cy + 5.0f};
	const float b[4] = {cx + 5.0f, cy - 5.0f, cx - 5.0f, cy + 5.0f};
	ui_polyline(a, 2, 2.0f, clr);
	ui_polyline(b, 2, 2.0f, clr);
}

/* The stand-in for a row with no cover (and for covers still in flight):
 * three bars of descending length, the same idea as the hero placeholder. */
static void draw_placeholder(float x, float y, const screen_list_args *a)
{
	C2D_DrawRectSolid(x, y, 0.0f, THUMB_SZ, THUMB_SZ, a->panel);
	C2D_DrawRectSolid(x + 9.0f, y + 9.0f, 0.1f, 16.0f, 3.0f, a->accent);
	C2D_DrawRectSolid(x + 9.0f, y + 16.0f, 0.1f, 16.0f, 3.0f, a->meter);
	C2D_DrawRectSolid(x + 9.0f, y + 23.0f, 0.1f, 10.0f, 3.0f, a->meter);
}

static void draw_row(const screen_list_args *a, const sl_row *r, int abs_idx,
                     float y)
{
	if (abs_idx == a->sel)
		C2D_DrawRectSolid(0.0f, y + 1.0f, 0.05f, 3.0f, SL_ROW_H - 4.0f,
		                  a->accent);

	if (r->playing) {
		/* The visualizer replaces the cover entirely: drawing it over the
		 * art (or the placeholder) reads as two images fighting for the
		 * same tile. */
		C2D_DrawRectSolid(THUMB_X, y + 4.0f, 0.0f, THUMB_SZ, THUMB_SZ,
		                  a->panel);
		ui_now_playing_badge(THUMB_X, y + 4.0f, THUMB_SZ, true,
		                     a->animation_ms);
	} else {
		const C2D_Image *img = r->art_url[0]
		                       ? a->thumb(r->art_url, a->thumb_ctx) : NULL;
		if (img) {
			C2D_DrawImageAt(*img, THUMB_X, y + 4.0f, 0.1f, NULL,
			                THUMB_SZ / (float)THUMB_PX,
			                THUMB_SZ / (float)THUMB_PX);
		} else {
			draw_placeholder(THUMB_X, y + 4.0f, a);
		}
	}

	bool sel = abs_idx == a->sel;
	ui_text(a->buf, r->name, TEXT_X, ui_baseline(y + 7.0f, TY_ROW_NAME),
	        TY_ROW_NAME, 320.0f - TEXT_X - 28.0f,
	        sel ? a->accent : a->text);
	if (r->sub[0])
		ui_text(a->buf, r->sub, TEXT_X,
		        ui_baseline(y + 23.0f, TY_ROW_SUB), TY_ROW_SUB,
		        320.0f - TEXT_X - 28.0f, a->dim);

	if (r->is_playlist)
		C2D_DrawTriangle(302.0f, y + SL_ROW_H / 2 - 5.0f, a->dim,
		                 302.0f, y + SL_ROW_H / 2 + 5.0f, a->dim,
		                 308.0f, y + SL_ROW_H / 2, a->dim, 0.0f);

	/* Partially visible rows count too: cutting their hit area short would
	 * make the list feel broken mid-scroll. But clip it to what the header
	 * band leaves visible: the band paints over whatever crosses it, and a
	 * touch up there must reach the header buttons, not a covered row. */
	if (y + SL_ROW_H > SL_HEADER_H && y < 240.0f) {
		const float hy = y < SL_HEADER_H ? SL_HEADER_H : y;
		tb_add(a->tb, 0.0f, hy, 320.0f, SL_ROW_H - (hy - y),
		       SL_ROW0 + abs_idx);
	}
}

void screen_list_draw(const screen_list_args *a)
{
	/* Rows first, the band after: a row scrolled halfway under the header
	 * has no scissor to clip it, so the band paints over the overlap. */
	for (int i = 0; i < a->n; i++) {
		float y = SL_HEADER_H + i * SL_ROW_H - a->frac;
		draw_row(a, &a->rows[i], a->first + i, y);
	}

	if (a->total == 0 && a->empty) {
		ui_text(a->buf, a->empty, TEXT_X,
		        ui_baseline(SL_HEADER_H + 14.0f, TY_ROW_NAME), TY_ROW_NAME,
		        320.0f - TEXT_X - 16.0f, a->dim);
		if (a->loading) {
			/* Three dots lighting up in turn, so a slow fetch reads as
			 * "working" instead of a frozen or broken screen. */
			int lit = (int)((a->animation_ms / 350) % 3);
			for (int i = 0; i < 3; i++)
				ui_disc(TEXT_X + 10.0f + i * 14.0f, SL_HEADER_H + 44.0f,
				        3.0f, i == lit ? a->accent : a->meter);
		}
	}

	/* A hairline scrollbar, only when there is more than fits. */
	float max_scroll = screen_list_max_scroll(a->total);
	if (max_scroll > 0.0f) {
		float track = SL_CONTENT_H;
		float frac = track / (a->total * SL_ROW_H) * track;
		float pos = (a->first * SL_ROW_H + a->frac) / max_scroll *
		            (track - frac);
		C2D_DrawRectSolid(317.0f, SL_HEADER_H, 0.0f, 2.0f, track, a->meter);
		C2D_DrawRectSolid(317.0f, SL_HEADER_H + pos, 0.1f, 2.0f, frac,
		                  a->accent);
	}

	/* The band and everything on it must sit above the row content that
	 * scrolls underneath: row covers draw at z=0.1 and would otherwise
	 * punch through the band. ui_depth lifts the ui_* primitives with it. */
	ui_depth(0.5f);
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, SL_HEADER_H, a->panel);

	float title_x = 12.0f;
	if (a->can_back) {
		/* Left chevron, the mirror of a playlist row's right one. */
		u32 clr = a->pressed_id == SL_BTN_BACK ? a->accent : a->dim;
		float cy = SL_HEADER_H / 2;
		C2D_DrawTriangle(19.0f, cy - 6.0f, clr, 19.0f, cy + 6.0f, clr,
		                 12.0f, cy, clr, 0.5f);
		tb_add_hit(a->tb, 15.0f, cy, 38.0f, SL_BTN_BACK);
		title_x = 30.0f;
	}
	ui_text(a->buf, a->title, title_x, ui_baseline(7.0f, TY_ROW_NAME),
	        TY_ROW_NAME, 246.0f - title_x, a->text);
	if (a->count) {
		float w = ui_text_width(a->buf, a->count, TY_ROW_SUB);
		ui_text(a->buf, a->count, 246.0f - w,
		        ui_baseline(9.0f, TY_ROW_SUB), TY_ROW_SUB, 60.0f, a->dim);
	}

	draw_search_icon(266.0f, SL_HEADER_H / 2,
	                 a->pressed_id == SL_BTN_SEARCH ? a->accent : a->dim,
	                 a->panel);
	draw_close_icon(302.0f, SL_HEADER_H / 2,
	                a->pressed_id == SL_BTN_CLOSE ? a->accent : a->dim);
	tb_add_hit(a->tb, 266.0f, SL_HEADER_H / 2, 40.0f, SL_BTN_SEARCH);
	tb_add_hit(a->tb, 302.0f, SL_HEADER_H / 2, 40.0f, SL_BTN_CLOSE);
	ui_depth(0.0f);
}
