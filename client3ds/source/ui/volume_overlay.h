#pragma once

#include <citro2d.h>
#include <stdbool.h>

typedef struct {
	C2D_TextBuf buf;
	int         volume_percent;
	u8          alpha;      /* 0 draws nothing */
} volume_overlay_args;

/* Drawn above the completed bottom-screen view. The overlay has no touch
 * area: it only reports. */
void volume_overlay_draw(const volume_overlay_args *a);
