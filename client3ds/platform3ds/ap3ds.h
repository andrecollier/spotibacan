/* A session with Spotify's access point.
 *
 * This is not HTTP and not TLS: raw TCP carrying a Diffie-Hellman handshake,
 * then every packet encrypted and authenticated with Shannon. The AP is what
 * issues audio keys, so nothing downstream of here works without it.
 *
 * Protocol transcribed from librespot (MIT): core/src/connection/, 
 * core/src/diffie_hellman.rs, protocol/proto/{keyexchange,authentication}.proto.
 */
#ifndef AP3DS_H
#define AP3DS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Packet types this client uses. */
#define AP_PKT_LOGIN        0xAB
#define AP_PKT_APWELCOME    0xAC
#define AP_PKT_AUTHFAILURE  0xAD
#define AP_PKT_PING         0x04
#define AP_PKT_PONG         0x49
#define AP_PKT_PONGACK      0x4A
#define AP_PKT_COUNTRYCODE  0x1B

typedef struct ap_session ap_session;

typedef struct {
	char   ap[160];              /* the host:port actually used */
	char   username[128];        /* canonical_username from APWelcome */
	char   country[8];           /* from the CountryCode packet, when it arrives */
	int    account_type;
	int    login_error;          /* APLoginFailed error_code, 0 when none */
	double connect_ms;
	double handshake_ms;
	double login_ms;
	size_t reusable_len;         /* reusable credentials, not stored by us */
} ap_info;

/* Asks apresolve which access point to dial. Returns 0, or -1 with err set. */
int ap_resolve(char *host, size_t hostlen, uint16_t *port, char *err, size_t errlen);

/* Dials the AP, completes the handshake and logs in with an OAuth access
 * token. `info` may be NULL. Returns NULL with `err` set on failure; when the
 * AP itself refused the login, info->login_error carries its reason. */
ap_session *ap_login(const char *host, uint16_t port, const char *access_token,
                     const char *device_id, ap_info *info,
                     char *err, size_t errlen);

int  ap_send(ap_session *s, uint8_t cmd, const void *payload, size_t len);

/* Is there a packet waiting? 1 yes, 0 not yet, -1 the connection is broken.
 *
 * An idle session is normal -- the AP only speaks when it has something to say,
 * and may say nothing for a minute between pings -- so silence must not be
 * read as a dead connection. Callers wait here and only then call ap_recv,
 * which blocks until the packet it was promised is complete. */
int  ap_wait(ap_session *s, int timeout_ms);

/* Returns the payload length, or -1. `cmd` is always set on success. */
int  ap_recv(ap_session *s, uint8_t *cmd, uint8_t *buf, size_t cap);
void ap_close(ap_session *s);

/* Answers a Ping with a Pong, which is what keeps the session alive. */
int  ap_handle_ping(ap_session *s, const uint8_t *payload, size_t len);

/* A stable per-console id, created once and kept beside the config. */
void ap_device_id(char *out, size_t outlen);

const char *ap_login_error_text(int code);

#endif
