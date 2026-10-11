/* The Shannon stream cipher, as Spotify's access point uses it.
 *
 * Ported to C from the `shannon` Rust crate (MIT, Copyright (c) 2016 Paul
 * Liétar), which is the implementation librespot interoperates with. Full
 * licence text in licenses/rust-shannon-MIT.txt.
 *
 * Shannon is both the cipher and the MAC: every packet is encrypted and
 * authenticated by the same keyed state, and the state carries across calls
 * within one packet. The sequence per packet is nonce -> encrypt/decrypt ->
 * finish, in that order, and nothing else may touch the context in between.
 */
#ifndef SHANNON_H
#define SHANNON_H

#include <stddef.h>
#include <stdint.h>

#define SHANNON_N 16

typedef struct {
	uint32_t R[SHANNON_N];       /* the stream register */
	uint32_t CRC[SHANNON_N];     /* 32 parallel CRC-16s, folded into the MAC */
	uint32_t initR[SHANNON_N];   /* saved post-key state, restored by nonce */
	uint32_t konst;
	uint32_t sbuf;
	uint32_t mbuf;
	size_t   nbuf;               /* bits still buffered, not bytes */
} shannon;

void shannon_init(shannon *c, const uint8_t *key, size_t keylen);
void shannon_nonce(shannon *c, const uint8_t *nonce, size_t len);
void shannon_nonce_u32(shannon *c, uint32_t n);     /* big-endian, as the AP wants */
void shannon_encrypt(shannon *c, uint8_t *buf, size_t len);
void shannon_decrypt(shannon *c, uint8_t *buf, size_t len);
void shannon_finish(shannon *c, uint8_t *mac, size_t len);

#endif
