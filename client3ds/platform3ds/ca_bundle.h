#ifndef CA_BUNDLE_H
#define CA_BUNDLE_H
#include <stddef.h>

/* NUL-terminated concatenation of PEM root certificates; the length includes
 * the terminator, which mbedtls_x509_crt_parse() requires. */
extern const char spotify_ca_bundle[];
extern const size_t spotify_ca_bundle_len;

#endif
