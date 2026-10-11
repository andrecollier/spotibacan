/* Checks the C Shannon port against vectors from the implementation librespot
 * interoperates with. Builds and runs on the host, not the console: Shannon is
 * plain C with no 3DS dependency, and a cipher bug found here is far cheaper
 * than one found as "the access point hung up".
 *
 *     python3 client3ds/tests/run.py
 */
#include <stdio.h>
#include <string.h>

#include "shannon.h"
#include "shannon_vectors.h"

static int failures;

static void check(const char *what, const uint8_t *got, const uint8_t *want,
                  size_t n)
{
	if (memcmp(got, want, n) == 0) {
		printf("  ok   %s\n", what);
		return;
	}
	failures++;
	printf("  FAIL %s\n       got  ", what);
	for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
	printf("\n       want ");
	for (size_t i = 0; i < n; i++) printf("%02x", want[i]);
	printf("\n");
}

int main(void)
{
	shannon c;
	uint8_t buf[32], mac[4];

	/* One whole block. */
	shannon_init(&c, VEC_KEY_A, sizeof(VEC_KEY_A));
	shannon_nonce_u32(&c, 0);
	for (int i = 0; i < 32; i++) buf[i] = (uint8_t)i;
	shannon_encrypt(&c, buf, 32);
	check("32-byte block", buf, VEC_A_CIPHER, 32);
	shannon_finish(&c, mac, 4);
	check("32-byte block MAC", mac, VEC_A_MAC, 4);

	/* Header then body: the access point's own shape, where a part-buffered
	 * word has to carry from one call into the next. */
	shannon_init(&c, VEC_KEY_B, sizeof(VEC_KEY_B));
	shannon_nonce_u32(&c, 7);
	uint8_t hdr[3] = { 0xab, 0x01, 0x02 };
	shannon_encrypt(&c, hdr, sizeof(hdr));
	check("3-byte header", hdr, VEC_B_HEADER, 3);
	uint8_t body[5];
	memcpy(body, "hello", 5);
	shannon_encrypt(&c, body, sizeof(body));
	check("5-byte body after header", body, VEC_B_BODY, 5);
	shannon_finish(&c, mac, 4);
	check("header+body MAC", mac, VEC_B_MAC, 4);

	/* Three packets on one cipher, nonce advancing as the codec does. */
	const uint8_t *pkts[] = { VEC_C_PKT0, VEC_C_PKT1, VEC_C_PKT2 };
	const uint8_t *macs[] = { VEC_C_MAC0, VEC_C_MAC1, VEC_C_MAC2 };
	shannon_init(&c, VEC_KEY_A, sizeof(VEC_KEY_A));
	for (int n = 0; n < 3; n++) {
		uint8_t p[11];
		memset(p, n, sizeof(p));
		shannon_nonce_u32(&c, (uint32_t)n);
		shannon_encrypt(&c, p, sizeof(p));
		char label[48];
		snprintf(label, sizeof(label), "packet %d of 3", n);
		check(label, p, pkts[n], sizeof(p));
		shannon_finish(&c, mac, 4);
		snprintf(label, sizeof(label), "packet %d MAC", n);
		check(label, mac, macs[n], 4);
	}

	/* Decrypting must undo encrypting, and agree on the MAC. */
	shannon_init(&c, VEC_KEY_B, sizeof(VEC_KEY_B));
	shannon_nonce_u32(&c, 42);
	uint8_t plain[23], round[23];
	for (int i = 0; i < 23; i++) plain[i] = (uint8_t)(i * 11);
	memcpy(round, plain, sizeof(round));
	shannon_encrypt(&c, round, sizeof(round));
	uint8_t mac_enc[4];
	shannon_finish(&c, mac_enc, 4);
	shannon_nonce_u32(&c, 42);
	shannon_decrypt(&c, round, sizeof(round));
	uint8_t mac_dec[4];
	shannon_finish(&c, mac_dec, 4);
	check("decrypt undoes encrypt", round, plain, sizeof(plain));
	check("both directions agree on the MAC", mac_dec, mac_enc, 4);

	printf(failures ? "\n%d failure(s)\n" : "\nall vectors match\n", failures);
	return failures ? 1 : 0;
}
