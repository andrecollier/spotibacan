// Renderer for Rockbox "While Playing Screen" themes.
//
// A .wps is a tag language describing where a theme paints its bitmaps and
// text. The measured mix across sixteen real themes is roughly 2400 bitmap and
// viewport tags against 100 text ones, so this is first of all a bitmap
// compositor, not a text engine.
//
// Themes target 320x240; the 3DS top screen is 400x240, so a theme renders at
// native size with a margin either side. Nothing is scaled.
#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#define WPS_W 320
#define WPS_H 240

// What the player knows right now. The renderer only reads this.
typedef struct {
	const char *title, *artist, *album;
	float position, duration;
	bool playing, has_art;
	C2D_Image art;
} WpsState;

// Load a theme's .wps and the bitmaps it preloads. dir is the folder holding
// the images, normally sdmc:/3ds/audio3ds/wps/<theme>.
bool wps_load(const char *wps_path, const char *dir);
void wps_unload(void);
bool wps_ready(void);

// Draw the loaded theme, offset so a 320-wide theme sits centred on a 400-wide
// screen. Call between C2D_SceneBegin and C3D_FrameEnd.
void wps_render(const WpsState *state, float origin_x, float origin_y);

// Diagnostics for the on-screen status line.
const char *wps_status(void);
