#pragma once

#include <citro2d.h>
#include <stdbool.h>

#include "lyrics3ds.h"
#include "touch.h"

/* Bottom screen, lyrics view: synced lyrics auto-centre the current line;
 * plain ones drag by hand. A tap on the header (or B) goes back. */

#define LYR_HEADER_H 30.0f
#define LYR_LINE_H   26.0f
#define LYR_CONTENT_H (240.0f - LYR_HEADER_H)

enum {
	LYR_BTN_BACK = 20,   /* clear of the player's 0..4 and the list's 10..11 */
};

typedef enum {
	LYR_ST_LOADING = 0,
	LYR_ST_OK,
	LYR_ST_NONE,
	LYR_ST_ERR,
} screen_lyrics_state;

typedef struct {
	C2D_TextBuf buf;
	touch_builder *tb;

	const char *track;      /* header */
	screen_lyrics_state state;
	const char *error;      /* for LYR_ST_ERR */

	const lyrics_doc *doc;
	int current;            /* lyrics_index_at(), or -1 */
	float scroll;           /* manual scroll for plain docs, in px */
	long position_ms;

	int pressed_id;
	unsigned animation_ms;

	u32 accent, text, dim, panel, meter;
} screen_lyrics_args;

void screen_lyrics_draw(const screen_lyrics_args *a);

/* How far a plain document of `count` lines can scroll. */
static inline float screen_lyrics_max_scroll(int count)
{
	float m = count * LYR_LINE_H - LYR_CONTENT_H;
	return m > 0.0f ? m : 0.0f;
}
