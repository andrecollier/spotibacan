#include "screen_lyrics.h"

#include "lang.h"
#include "ui.h"

/* Synced docs put the current line at this slot; plain docs scroll
 * manually from the top. */
#define CURRENT_SLOT 3

static void draw_centred_note(const screen_lyrics_args *a, const char *msg)
{
	float w = ui_text_width(a->buf, msg, TY_ARTIST);
	if (w > 300.0f)
		w = 300.0f;
	ui_text(a->buf, msg, 160.0f - w / 2,
	        ui_baseline(120.0f, TY_ARTIST), TY_ARTIST, 300.0f, a->dim);
}

void screen_lyrics_draw(const screen_lyrics_args *a)
{
	if (a->state == LYR_ST_LOADING) {
		draw_centred_note(a, tr(S_LYR_SEARCHING));
	} else if (a->state == LYR_ST_NONE) {
		draw_centred_note(a, tr(S_LYR_NONE));
	} else if (a->state == LYR_ST_ERR) {
		draw_centred_note(a, a->error && a->error[0]
		                       ? a->error : tr(S_LYR_FAIL));
	} else {
		const lyrics_doc *doc = a->doc;
		float top;   /* document px at the content area's top */
		if (doc->synced) {
			int cur = a->current >= 0 ? a->current : 0;
			top = cur * LYR_LINE_H - CURRENT_SLOT * LYR_LINE_H;
			float max = screen_lyrics_max_scroll(doc->count) +
			            (LYR_CONTENT_H - LYR_LINE_H);
			if (top < -CURRENT_SLOT * LYR_LINE_H)
				top = -CURRENT_SLOT * LYR_LINE_H;
			if (top > max)
				top = max;
		} else {
			top = a->scroll;
		}

		int first = (int)(top / LYR_LINE_H);
		if (first < 0)
			first = 0;
		for (int i = first; i < doc->count; i++) {
			float y = LYR_HEADER_H + i * LYR_LINE_H - top;
			if (y > 240.0f)
				break;
			if (y + LYR_LINE_H < LYR_HEADER_H)
				continue;
			u32 clr = a->dim;
			if (doc->synced && i == a->current)
				clr = a->accent;
			else if (doc->synced && i < a->current)
				clr = a->text;
			ui_text(a->buf, doc->lines[i].text[0]
			                ? doc->lines[i].text : "-",
			        16.0f, ui_baseline(y + 5.0f, TY_ARTIST), TY_ARTIST,
			        288.0f, clr);
		}

		/* A hairline scrollbar for plain documents. */
		if (!doc->synced) {
			float max = screen_lyrics_max_scroll(doc->count);
			if (max > 0.0f) {
				float track = LYR_CONTENT_H;
				float frac = track / (doc->count * LYR_LINE_H) * track;
				float pos = a->scroll / max * (track - frac);
				C2D_DrawRectSolid(317.0f, LYR_HEADER_H, 0.0f, 2.0f,
				                  track, a->meter);
				C2D_DrawRectSolid(317.0f, LYR_HEADER_H + pos, 0.1f, 2.0f,
				                  frac, a->accent);
			}
		}
	}

	/* The band paints last: lines scrolled under it have no scissor. */
	C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, LYR_HEADER_H, a->panel);
	u32 back = a->pressed_id == LYR_BTN_BACK ? a->accent : a->dim;
	C2D_DrawTriangle(22.0f, LYR_HEADER_H / 2 - 5.0f, back,
	                 22.0f, LYR_HEADER_H / 2 + 5.0f, back,
	                 16.0f, LYR_HEADER_H / 2, back, 0.0f);
	{
		float w = ui_text_width(a->buf, tr(S_BACK), TY_MICRO);
		ui_text_tracked(a->buf, tr(S_BACK), 28.0f,
		                ui_baseline(9.0f, TY_MICRO), TY_MICRO, 1.0f, back);
		tb_add(a->tb, 0.0f, 0.0f, 28.0f + w + 16.0f, LYR_HEADER_H,
		       LYR_BTN_BACK);
	}
	ui_text(a->buf, a->track, 120.0f, ui_baseline(7.0f, TY_ROW_NAME),
	        TY_ROW_NAME, 184.0f, a->text);
}
