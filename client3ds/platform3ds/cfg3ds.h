/* The refresh token on the SD card, and nothing else.
 *
 * An SD card has no permissions to hide behind, so what protects this is
 * keeping it small and keeping it off the screen and out of the log: the file
 * holds the one long-lived secret, and the access token is never written down.
 */
#ifndef CFG3DS_H
#define CFG3DS_H

#include <stdbool.h>
#include <stddef.h>

/* Resolves the config path next to the running .3dsx, from argv[0], falling
 * back to the card root when argv[0] is not a path the console can open. */
void cfg_set_path(const char *argv0);
const char *cfg_path(void);

bool cfg_load_refresh_token(char *out, size_t outlen);
bool cfg_save_refresh_token(const char *token, const char *scope);
bool cfg_clear(void);

#endif
