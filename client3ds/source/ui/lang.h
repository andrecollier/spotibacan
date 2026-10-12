/* UI strings, Spanish and English. One flat enum, one table; tr() reads the
 * current language. The choice persists as lang= in spotify.cfg. */
#pragma once

typedef enum {
	LANG_ES = 0,
	LANG_EN,
	LANG_COUNT
} lang_id;

typedef enum {
	S_NOTHING_PLAYING,
	S_STATE_LOADING,
	S_STATE_PAUSED,
	S_STATE_MUTED,
	S_STATE_PLAYING,
	S_LIKED,
	S_LOADING,
	S_LOADING_DOTS,
	S_EMPTY_LIST,
	S_QUIT_TITLE,
	S_CANCEL,
	S_QUIT,
	S_PLAYLIST_FALLBACK,
	S_RADIO,
	S_LISTS,
	S_SEARCH_VIEW,
	S_SEARCH_HINT,
	S_SIGNIN_FAIL,
	S_PAIR_TITLE,
	S_PAIR_LINE1,
	S_PAIR_LINE2,
	S_CODE,
	S_B_CANCEL,
	S_START_QUIT,
	S_STARTING,
	S_PLAY_FAIL,
	S_LYR_SEARCHING,
	S_LYR_NONE,
	S_LYR_FAIL,
	S_BACK,
	S_LYRICS,
	S_LIBRARY,
	S_SETTINGS,
	S_LANGUAGE,
	S_ACCENT,
	S_UNLINK,
	S_UNLINKED_NOTE,
	S_BACK_MENU,
	S_ACCENT_GREEN,
	S_ACCENT_PURPLE,
	S_ACCENT_BLUE,
	S_ACCENT_ORANGE,
	S_RECENTLY,
	S_RECENT_TITLE,
	S_CLEAR,
	S_COUNT
} str_id;

void    lang_set(lang_id id);
lang_id lang_get(void);
const char *tr(str_id id);
