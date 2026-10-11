/* Just enough JSON for flat objects of strings and numbers.
 *
 * Every response this client parses at the OAuth stage is one flat object, so a
 * real parser would be dead weight. This looks up a key at the top level and
 * reads its value; it does not walk nested objects, and says so rather than
 * guessing when asked to.
 */
#ifndef JSON3DS_H
#define JSON3DS_H

#include <stdbool.h>
#include <stddef.h>

/* Copies the string value of `key` into `out`, resolving \" \\ \/ \n \r \t and
 * \uXXXX for the Latin-1 range. Returns false when the key is absent or its
 * value is not a string. */
bool json_str(const char *json, const char *key, char *out, size_t outlen);

/* Reads an integer value. Returns false when absent or not a number. */
bool json_int(const char *json, const char *key, long *out);

#endif
