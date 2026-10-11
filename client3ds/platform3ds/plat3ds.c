/* The two platform hooks mbedTLS cannot provide for itself on the 3DS.
 *
 * Kept out of the pinned mbedTLS tree so that tree stays untouched, and out of
 * libmbedtls.a so it can link against libctru without the archive doing so.
 */
#include <3ds.h>
#include <string.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/entropy.h"

/* osGetTime() is milliseconds since 1900 off the console's RTC. mbedTLS only
 * ever takes differences of this, so the epoch does not matter. */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)osGetTime();
}

/* ps:ps owns the console's hardware CSPRNG. Without it we would be reduced to
 * the MAC address and the clock, which is not seed material for a TLS client.
 * psInit() is the app's job; a failure here fails the handshake rather than
 * quietly producing predictable randomness. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen)
{
	(void)data;
	*olen = 0;
	if (R_FAILED(PS_GenerateRandomBytes(output, len)))
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	*olen = len;
	return 0;
}
