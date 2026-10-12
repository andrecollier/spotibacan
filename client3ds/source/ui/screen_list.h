#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "touch.h"

/* Bottom screen, library view: a pixel-scrolled list of rows with cover
 * thumbnails. A simplified port of the reference's screen_list - one
 * section, no armed state: a tap plays or opens the row directly, a drag
 * scrolls, and a flick keeps coasting. */

#define SL_HEADER_H  30.0f
#define SL_ROW_H     42.0f
#define SL_CONTENT_H (240.0f - SL_HEADER_H)

/* Fling tuning: a resistive 240px screen wants short travels, so the cap is
 * low and the friction high. */
#define SL_FLING_MAX      28.0f
#define SL_FLING_FRICTION 0.92f
#define SL_FLING_STOP     0.5f

/* Touch ids. Row ids encode the absolute row index; the buttons keep clear
 * of the player screen's 0..4. */
enum {
	SL_BTN_SEARCH = 10,
	SL_BTN_CLOSE,
	SL_BTN_BACK,
	SL_ROW0 = 1000,   /* + absolute row index */
};

typedef struct {
	const char *name;
	const char *sub;
	const char *art_url;    /* empty -> placeholder tile */
	bool is_playlist;       /* chevron on the right */
	bool playing;           /* now-playing badge over the thumb */
} sl_row;

typedef struct {
	C2D_TextBuf buf;
	touch_builder *tb;

	const char *title;      /* header band */
	const char *count;      /* right-aligned position, may be NULL */
	const char *empty;      /* shown when total is 0, may be NULL */
	bool can_back;          /* sub-view: header gains a back chevron */
	bool loading;           /* empty is a loading state: animate it */

	const sl_row *rows;     /* the visible window... */
	int n;                  /* ...this many rows... */
	int first;              /* ...starting at this absolute index */
	float frac;             /* pixel offset of the partial first row */
	int total;              /* rows in the whole list, for the scrollbar */

	int sel;                /* absolute d-pad selection, or -1 */
	int pressed_id;
	unsigned animation_ms;

	/* NULL until the thumbnail arrives; asked every frame. */
	const C2D_Image *(*thumb)(const char *url, void *ctx);
	void *thumb_ctx;

	u32 accent, text, dim, panel, meter;
} screen_list_args;

void screen_list_draw(const screen_list_args *a);

/* How far a list of `total` rows can scroll. */
static inline float screen_list_max_scroll(int total)
{
	float m = total * SL_ROW_H - SL_CONTENT_H;
	return m > 0.0f ? m : 0.0f;
}
