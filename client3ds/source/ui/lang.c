#include "lang.h"

static lang_id s_lang = LANG_ES;

/* Keep the accents out of string literals the font must rasterize: the 3DS
 * system font covers them, but plain ASCII has one less way to surprise. */
static const char *const s_strings[LANG_COUNT][S_COUNT] = {
	[LANG_ES] = {
		[S_NOTHING_PLAYING]   = "Nada sonando",
		[S_STATE_LOADING]     = "CARGANDO",
		[S_STATE_PAUSED]      = "PAUSA",
		[S_STATE_MUTED]       = "SIN SONIDO",
		[S_STATE_PLAYING]     = "SONANDO",
		[S_LIKED]             = "Me gusta",
		[S_LOADING]           = "Cargando",
		[S_LOADING_DOTS]      = "Cargando...",
		[S_EMPTY_LIST]        = "Nada por aqui",
		[S_QUIT_TITLE]        = "Cerrar SpotiBacan?",
		[S_CANCEL]            = "Cancelar",
		[S_QUIT]              = "Salir",
		[S_PLAYLIST_FALLBACK] = "Lista",
		[S_RADIO]             = "Radio",
		[S_LISTS]             = "Listas",
		[S_SEARCH_VIEW]       = "Busqueda",
		[S_SEARCH_HINT]       = "Buscar en Spotify",
		[S_SIGNIN_FAIL]       = "No se pudo entrar",
		[S_PAIR_TITLE]        = "Vincula tu cuenta",
		[S_PAIR_LINE1]        = "En el navegador, entra a",
		[S_PAIR_LINE2]        = "y escribe el codigo que aparece abajo.",
		[S_CODE]              = "CODIGO",
		[S_B_CANCEL]          = "B  Cancelar",
		[S_START_QUIT]        = "START  Salir",
		[S_STARTING]          = "Iniciando...",
		[S_PLAY_FAIL]         = "No se pudo reproducir",
		[S_LYR_SEARCHING]     = "Buscando la letra...",
		[S_LYR_NONE]          = "Sin letra para este tema",
		[S_LYR_FAIL]          = "No se pudo buscar",
		[S_BACK]              = "VOLVER",
		[S_LYRICS]            = "LETRA",
		[S_LIBRARY]           = "LISTA",
		[S_SETTINGS]          = "Ajustes",
		[S_LANGUAGE]          = "Idioma",
		[S_ACCENT]            = "Color de acento",
		[S_UNLINK]            = "Desvincular cuenta",
		[S_UNLINKED_NOTE]     = "Borra la cuenta; se vincula de nuevo al abrir.",
		[S_BACK_MENU]         = "Volver",
		[S_ACCENT_GREEN]      = "Verde",
		[S_ACCENT_PURPLE]     = "Morado",
		[S_ACCENT_BLUE]       = "Azul",
		[S_ACCENT_ORANGE]     = "Naranja",
		[S_RECENTLY]          = "RECIEN SONADAS",
		[S_RECENT_TITLE]      = "Recientes",
		[S_CLEAR]             = "BORRAR",
	},
	[LANG_EN] = {
		[S_NOTHING_PLAYING]   = "Nothing playing",
		[S_STATE_LOADING]     = "LOADING",
		[S_STATE_PAUSED]      = "PAUSED",
		[S_STATE_MUTED]       = "MUTED",
		[S_STATE_PLAYING]     = "PLAYING",
		[S_LIKED]             = "Liked Songs",
		[S_LOADING]           = "Loading",
		[S_LOADING_DOTS]      = "Loading...",
		[S_EMPTY_LIST]        = "Nothing here",
		[S_QUIT_TITLE]        = "Quit SpotiBacan?",
		[S_CANCEL]            = "Cancel",
		[S_QUIT]              = "Quit",
		[S_PLAYLIST_FALLBACK] = "Playlist",
		[S_RADIO]             = "Radio",
		[S_LISTS]             = "Playlists",
		[S_SEARCH_VIEW]       = "Search",
		[S_SEARCH_HINT]       = "Search Spotify",
		[S_SIGNIN_FAIL]       = "Could not sign in",
		[S_PAIR_TITLE]        = "Link your account",
		[S_PAIR_LINE1]        = "In your browser, go to",
		[S_PAIR_LINE2]        = "and enter the code shown below.",
		[S_CODE]              = "CODE",
		[S_B_CANCEL]          = "B  Cancel",
		[S_START_QUIT]        = "START  Quit",
		[S_STARTING]          = "Starting...",
		[S_PLAY_FAIL]         = "Could not play",
		[S_LYR_SEARCHING]     = "Searching for lyrics...",
		[S_LYR_NONE]          = "No lyrics for this track",
		[S_LYR_FAIL]          = "Search failed",
		[S_BACK]              = "BACK",
		[S_LYRICS]            = "LYRICS",
		[S_LIBRARY]           = "LIBRARY",
		[S_SETTINGS]          = "Settings",
		[S_LANGUAGE]          = "Language",
		[S_ACCENT]            = "Accent color",
		[S_UNLINK]            = "Unlink account",
		[S_UNLINKED_NOTE]     = "Wipes the account; you link again on next launch.",
		[S_BACK_MENU]         = "Back",
		[S_ACCENT_GREEN]      = "Green",
		[S_ACCENT_PURPLE]     = "Purple",
		[S_ACCENT_BLUE]       = "Blue",
		[S_ACCENT_ORANGE]     = "Orange",
		[S_RECENTLY]          = "RECENTLY PLAYED",
		[S_RECENT_TITLE]      = "Recent",
		[S_CLEAR]             = "CLEAR",
	},
};

void lang_set(lang_id id)
{
	if (id >= 0 && id < LANG_COUNT)
		s_lang = id;
}

lang_id lang_get(void)
{
	return s_lang;
}

const char *tr(str_id id)
{
	if (id < 0 || id >= S_COUNT)
		return "";
	const char *s = s_strings[s_lang][id];
	return s ? s : "";
}
