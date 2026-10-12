#include "thumbs.h"

#include "artwork.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

/* The state machine is single-producer (the render thread queues and
 * uploads) single-consumer (the art worker fetches): a slot in flight is
 * never evicted, and every hand-off goes through a barrier before the state
 * byte changes. */
enum { S_EMPTY, S_WANT, S_LOADING, S_PENDING, S_READY, S_FAILED };

typedef struct {
	char url[80];
	volatile u8 state;
	u32 last_used;      /* frame tick, for LRU eviction */
	u8  pixels[THUMB_PX * THUMB_PX * 2];
	C3D_Tex tex;
	Tex3DS_SubTexture sub;
	C2D_Image img;
} slot;

static slot slots[THUMB_SLOTS];
static u32 tick;

void thumbs_init(void)
{
	for (int i = 0; i < THUMB_SLOTS; i++) {
		slot *s = &slots[i];
		C3D_TexInit(&s->tex, THUMB_PX, THUMB_PX, GPU_RGB565);
		C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_LINEAR);
		s->sub = (Tex3DS_SubTexture){THUMB_PX, THUMB_PX,
		                             0.0f, 1.0f, 1.0f, 0.0f};
		s->img = (C2D_Image){&s->tex, &s->sub};
	}
}

void thumbs_free_all(void)
{
	for (int i = 0; i < THUMB_SLOTS; i++)
		C3D_TexDelete(&slots[i].tex);
}

const C2D_Image *thumbs_get(const char *url)
{
	tick++;
	if (!url || !*url)
		return NULL;

	int empty = -1, failed = -1, lru = -1;
	for (int i = 0; i < THUMB_SLOTS; i++) {
		slot *s = &slots[i];
		if (s->state != S_EMPTY && strcmp(s->url, url) == 0) {
			s->last_used = tick;
			/* A failed fetch gets another chance when it scrolls back
			 * into view instead of sitting as a blank square. */
			if (s->state == S_FAILED) {
				__sync_synchronize();
				s->state = S_WANT;
			}
			return s->state == S_READY ? &s->img : NULL;
		}
		if (s->state == S_EMPTY)
			empty = i;
		else if (s->state == S_FAILED &&
		         (failed < 0 || s->last_used < slots[failed].last_used))
			failed = i;
		else if (s->state == S_READY &&
		         (lru < 0 || s->last_used < slots[lru].last_used))
			lru = i;
	}

	/* A new url takes an empty slot first, then a dead one, and only then
	 * evicts the coldest ready slot: on a fast fling the same covers ping
	 * back into view, and refetching what was already decoded is what made
	 * the list flicker. */
	int use = empty >= 0 ? empty : (failed >= 0 ? failed : lru);
	if (use < 0)
		return NULL;
	slot *s = &slots[use];
	snprintf(s->url, sizeof(s->url), "%s", url);
	s->last_used = tick;
	__sync_synchronize();
	s->state = S_WANT;
	return NULL;
}

void thumbs_pump(void)
{
	/* One scratch buffer is enough: the pump runs on the render thread and
	 * uploads one slot at a time. */
	static u8 tiled[THUMB_PX * THUMB_PX * 2];
	for (int i = 0; i < THUMB_SLOTS; i++) {
		slot *s = &slots[i];
		if (s->state != S_PENDING)
			continue;
		artwork_tile_rgb565(s->pixels, tiled, THUMB_PX);
		C3D_TexLoadImage(&s->tex, tiled, GPU_TEXFACE_2D, 0);
		C3D_TexFlush(&s->tex);
		__sync_synchronize();
		s->state = S_READY;
	}
}

bool thumbs_work_pending(void)
{
	for (int i = 0; i < THUMB_SLOTS; i++)
		if (slots[i].state == S_WANT)
			return true;
	return false;
}

void thumbs_work_one(void)
{
	for (int i = 0; i < THUMB_SLOTS; i++) {
		slot *s = &slots[i];
		if (s->state != S_WANT)
			continue;
		s->state = S_LOADING;
		char err[128] = "";
		bool ok = artwork_fetch_decode(s->url, THUMB_PX, s->pixels,
		                               err, sizeof(err));
		__sync_synchronize();
		s->state = ok ? S_PENDING : S_FAILED;
		return;
	}
}
