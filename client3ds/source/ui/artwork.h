#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Downloads one cover and decodes it to RGB565 at px*px: 128 for the hero
 * art on the top screen, 48 for list thumbnails. Runs on the art worker
 * thread -- a fetch is a TLS round trip plus a software JPEG decode, and
 * neither belongs on the render loop. */
bool artwork_fetch_decode(const char *url, int px, uint8_t *out_rgb565,
                          char *err, size_t errlen);

/* Shuffles a linear RGB565 buffer into the PICA200's texture layout: 8x8
 * tiles whose pixels follow a Morton (Z-order) curve. C3D_TexLoadImage copies
 * bytes verbatim and expects them already tiled -- feeding it a linear image
 * renders as scrambled vertical strips. dst must be dim*dim*2 bytes. */
void artwork_tile_rgb565(const uint8_t *src, uint8_t *dst, int dim);
