#pragma once

#include <citro2d.h>
#include <stdbool.h>

/* Render-thread cache of list-row thumbnails.
 *
 * Textures live on the GPU and can only be touched by the thread owning the
 * graphics context, so the art worker hands over decoded pixels and this
 * turns them into images on the render thread. Lookups are by URL, which is
 * what the list rows carry. Small on purpose: the list shows about six rows
 * at a time, and at 64px a slot is 8 KB.
 *
 * 64px and not less because the PICA200 only takes power-of-two textures: a
 * 48px slot texture never actually allocated, and the "loaded" thumbnail
 * drew as an empty square. */
#define THUMB_PX    64
#define THUMB_SLOTS 16

/* Allocates the slot textures. Call once on the render thread, after C2D. */
void thumbs_init(void);
void thumbs_free_all(void);

/* Look up `url`.
 *
 * Returns NULL when not loaded, and queues a fetch for it - so the caller
 * can simply ask every frame and draw a placeholder until an image appears.
 * A NULL or empty url queues nothing, which is what a row with no artwork
 * gets. */
const C2D_Image *thumbs_get(const char *url);

/* Uploads completed thumbnails to the GPU. Once per frame, before any image
 * draws. */
void thumbs_pump(void);

/* Art-worker side: whether any slot is waiting, and fetching + decoding one
 * of them. Never called from the render thread. */
bool thumbs_work_pending(void);
void thumbs_work_one(void);
