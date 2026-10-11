/* See shannon.h for provenance: this is a C port of the MIT-licensed `shannon`
 * Rust crate by Paul Liétar, kept deliberately faithful. Where something below
 * looks odd, it is reproduced on purpose -- the server implements the same
 * quirks, so "fixing" one would break interoperability. */
#include "shannon.h"

#include <stdbool.h>
#include <string.h>

#define FOLD      SHANNON_N
#define INITKONST 0x6996c53aU
#define KEYP      13

static inline uint32_t rotl(uint32_t w, unsigned x)
{
	return (w << x) | (w >> (32 - x));
}

static inline uint32_t sbox1(uint32_t w)
{
	w ^= rotl(w, 5) | rotl(w, 7);
	w ^= rotl(w, 19) | rotl(w, 22);
	return w;
}

static inline uint32_t sbox2(uint32_t w)
{
	w ^= rotl(w, 7) | rotl(w, 22);
	w ^= rotl(w, 5) | rotl(w, 19);
	return w;
}

static inline uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Cycle the register and produce one output word in sbuf. */
static void cycle(shannon *c)
{
	uint32_t t = c->R[12] ^ c->R[13] ^ c->konst;
	t = sbox1(t) ^ rotl(c->R[0], 1);

	for (int i = 1; i < SHANNON_N; i++)
		c->R[i - 1] = c->R[i];
	c->R[SHANNON_N - 1] = t;

	t = sbox2(c->R[2] ^ c->R[15]);
	c->R[0] ^= t;
	c->sbuf = t ^ c->R[8] ^ c->R[12];
}

static void diffuse(shannon *c)
{
	for (int i = 0; i < FOLD; i++)
		cycle(c);
}

/* Accumulate a CRC of input words, later folded into the MAC. */
static void crcfunc(shannon *c, uint32_t i)
{
	uint32_t t = c->CRC[0] ^ c->CRC[2] ^ c->CRC[15] ^ i;
	for (int j = 1; j < SHANNON_N; j++)
		c->CRC[j - 1] = c->CRC[j];
	c->CRC[SHANNON_N - 1] = t;
}

static void macfunc(shannon *c, uint32_t i)
{
	crcfunc(c, i);
	c->R[KEYP] ^= i;
}

/* Folds key or nonce material in, a word at a time, zero-padding a short tail.
 * Initialises the CRC register as a side effect. */
static void loadkey(shannon *c, const uint8_t *key, size_t len)
{
	size_t i = 0;
	for (; i + 4 <= len; i += 4) {
		c->R[KEYP] ^= rd32le(key + i);
		cycle(c);
	}
	if (i < len) {
		uint8_t xtra[4] = {0};
		memcpy(xtra, key + i, len - i);
		c->R[KEYP] ^= rd32le(xtra);
		cycle(c);
	}

	/* Fold in the length too. */
	c->R[KEYP] ^= (uint32_t)len;
	cycle(c);

	memcpy(c->CRC, c->R, sizeof(c->CRC));
	diffuse(c);
	/* XOR the copy back, which is what makes key loading irreversible. */
	for (int j = 0; j < SHANNON_N; j++)
		c->R[j] ^= c->CRC[j];
}

void shannon_init(shannon *c, const uint8_t *key, size_t keylen)
{
	memset(c, 0, sizeof(*c));
	c->konst = INITKONST;

	/* Register seeded with Fibonacci numbers, counter zeroed. */
	c->R[0] = 1;
	c->R[1] = 1;
	for (int i = 2; i < SHANNON_N; i++)
		c->R[i] = c->R[i - 1] + c->R[i - 2];

	loadkey(c, key, keylen);
	c->konst = c->R[0];
	memcpy(c->initR, c->R, sizeof(c->initR));
}

void shannon_nonce(shannon *c, const uint8_t *nonce, size_t len)
{
	memcpy(c->R, c->initR, sizeof(c->R));
	c->konst = INITKONST;
	loadkey(c, nonce, len);
	c->konst = c->R[0];
	c->nbuf = 0;
}

void shannon_nonce_u32(shannon *c, uint32_t n)
{
	uint8_t be[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16),
	                  (uint8_t)(n >> 8), (uint8_t)n };
	shannon_nonce(c, be, sizeof(be));
}

/* Whether the MAC sees plaintext before or after the XOR is the only
 * difference between encrypting and decrypting, so both share this. */
static void process(shannon *c, uint8_t *buf, size_t len, bool decrypting)
{
	size_t at = 0;

	/* Finish a word left part-buffered by the previous call. The AP always
	 * leaves exactly one byte owing here, from the 3-byte packet header. */
	while (c->nbuf != 0 && at < len) {
		if (decrypting) {
			buf[at] ^= (uint8_t)((c->sbuf >> (32 - c->nbuf)) & 0xFF);
			c->mbuf ^= (uint32_t)buf[at] << (32 - c->nbuf);
		} else {
			c->mbuf ^= (uint32_t)buf[at] << (32 - c->nbuf);
			buf[at] ^= (uint8_t)((c->sbuf >> (32 - c->nbuf)) & 0xFF);
		}
		at++;
		c->nbuf -= 8;
		/* The LFSR was already cycled when the word was opened. */
		macfunc(c, c->mbuf);
	}
	if (c->nbuf != 0)
		return;                /* still not a whole word */

	/* Whole words. */
	size_t whole = (len - at) & ~(size_t)3;
	for (size_t end = at + whole; at < end; at += 4) {
		cycle(c);
		uint32_t t = rd32le(buf + at);
		if (decrypting) {
			t ^= c->sbuf;
			macfunc(c, t);
		} else {
			macfunc(c, t);
			t ^= c->sbuf;
		}
		wr32le(buf + at, t);
	}

	/* A trailing partial word opens a new buffered word. */
	if (at < len) {
		cycle(c);
		c->mbuf = 0;
		c->nbuf = 32;
		for (; at < len; at++) {
			if (decrypting) {
				buf[at] ^= (uint8_t)((c->sbuf >> (32 - c->nbuf)) & 0xFF);
				c->mbuf ^= (uint32_t)buf[at] << (32 - c->nbuf);
			} else {
				c->mbuf ^= (uint32_t)buf[at] << (32 - c->nbuf);
				buf[at] ^= (uint8_t)((c->sbuf >> (32 - c->nbuf)) & 0xFF);
			}
			c->nbuf -= 8;
		}
	}
}

void shannon_encrypt(shannon *c, uint8_t *buf, size_t len)
{
	process(c, buf, len, false);
}

void shannon_decrypt(shannon *c, uint8_t *buf, size_t len)
{
	process(c, buf, len, true);
}

void shannon_finish(shannon *c, uint8_t *mac, size_t len)
{
	if (c->nbuf != 0)
		macfunc(c, c->mbuf);

	/* Perturb the MAC to mark the end of input. Only the stream register is
	 * touched, not the CRC: that is an action no plaintext can reproduce,
	 * which is what defeats extension attacks. */
	cycle(c);
	c->R[KEYP] ^= INITKONST ^ ((uint32_t)c->nbuf << 3);
	c->nbuf = 0;

	for (int i = 0; i < SHANNON_N; i++)
		c->R[i] ^= c->CRC[i];
	diffuse(c);

	for (size_t i = 0; i < len; i += 4) {
		cycle(c);
		if (i + 4 <= len) {
			wr32le(mac + i, c->sbuf);
		} else {
			for (size_t j = 0; i + j < len; j++)
				mac[i + j] = (uint8_t)((c->sbuf >> (8 * j)) & 0xFF);
		}
	}
}
