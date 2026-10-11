/* Just enough protobuf to speak to the access point.
 *
 * The messages are few, small and fixed, and their field numbers are written
 * out literally in Spotify's .proto files, so a code generator and a runtime
 * would both be dead weight here. Writers take caller-supplied storage and
 * never allocate; nested messages are built into their own buffer and then
 * embedded, which is also how the wire format works.
 */
#ifndef PROTO3DS_H
#define PROTO3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint8_t *p;
	size_t   len;
	size_t   cap;
	bool     overflow;      /* sticky: one failed append poisons the message */
} pb;

void pb_init(pb *b, uint8_t *storage, size_t cap);
bool pb_varint(pb *b, uint32_t field, uint64_t v);
bool pb_bytes(pb *b, uint32_t field, const void *data, size_t n);
bool pb_str(pb *b, uint32_t field, const char *s);
bool pb_sub(pb *b, uint32_t field, const pb *sub);
bool pb_ok(const pb *b);

/* A length-delimited value inside a message: either a nested message to read
 * further, or bytes to copy out. */
typedef struct {
	const uint8_t *p;
	size_t         len;
} pbv;

bool pb_get_bytes(const uint8_t *buf, size_t len, uint32_t field, pbv *out);
bool pb_get_varint(const uint8_t *buf, size_t len, uint32_t field, uint64_t *out);

/* A `sint32`/`sint64` field. These are zigzag-encoded, so reading one as a
 * plain varint silently returns roughly double the real value for positives --
 * which looks like a plausible number and not like a bug. Spotify's
 * metadata.proto declares duration, popularity, number and disc_number this
 * way. */
bool pb_get_sint(const uint8_t *buf, size_t len, uint32_t field, int64_t *out);
bool pb_has(const uint8_t *buf, size_t len, uint32_t field);

/* Chases a path of nested length-delimited fields, e.g. challenge ->
 * login_crypto_challenge -> diffie_hellman. */
bool pb_dig(const uint8_t *buf, size_t len, const uint32_t *fields, size_t depth,
            pbv *out);

#endif
