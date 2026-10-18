/* Spotify on a Google Cast speaker, with the console as the remote.
 *
 * The speaker is woken the way the phone app does it: Cast v2 to port 8009
 * starts Spotify's receiver (CC32E753), getInfo says which client the
 * speaker wants a token for, device-auth turns our token into one, and
 * addUser signs the speaker in. From then on it is an ordinary Connect
 * device, driven over spclient's connect-state like any other: the Cast
 * link is closed again, the speaker streams on its own.
 *
 * Spotify names the device md5(friendly name), the same id the phone app
 * gets, so the speaker does not show up twice in anyone's device list.
 * All of it blocks for seconds; call it from a worker, never the UI.
 */
#ifndef CAST3DS_H
#define CAST3DS_H

#include <stddef.h>

#define CAST_DEV_LEN 33   /* 32 hex digits and the NUL */

/* Spotify's device id for a speaker called `name`. */
void cast_device_id(const char *name, char out[CAST_DEV_LEN]);

/* Starts Spotify on the speaker at `ip` and signs it in. `bearer` is tried
 * first and `fallback` (may be NULL) if device-auth refuses it. On success
 * `dev` holds the Connect device id and `volume` the speaker's level
 * (0..1, -1 when it did not say). Returns 0, or -1 with `err` set. */
int cast_wake(const char *ip, const char *name, const char *sp_host,
              const char *bearer, const char *fallback,
              char dev[CAST_DEV_LEN], float *volume, char *err, size_t errlen);

/* One connect-state player command: `command_json` is the object that goes
 * under "command", e.g. {"endpoint":"pause"}. Returns the HTTP status, or
 * -1 when no reply came. */
int cast_command(const char *sp_host, const char *bearer, const char *dev,
                 const char *command_json, char *err, size_t errlen);

/* Plays `context` (a playlist, album, collection or track URI) on the
 * speaker, from `track` when non-empty, `pos_ms` into it. */
int cast_play(const char *sp_host, const char *bearer, const char *dev,
              const char *context, const char *track, long pos_ms,
              char *err, size_t errlen);

/* Sets the speaker's volume, 0..1. Returns the HTTP status or -1. */
int cast_set_volume(const char *sp_host, const char *bearer, const char *dev,
                    float level, char *err, size_t errlen);

#endif
