#include "track.h"
#include "http3ds.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"

/* Every Spotify audio file uses this IV with the per-file AES-128 key, in
 * counter mode, with the counter running from the start of the file. */
static const uint8_t AUDIO_AESIV[16] = {
	0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
	0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93,
};

void track_free(track_audio *t)
{
	if (!t)
		return;
	free(t->data);
	t->data = NULL;
	t->len = 0;
}

static bool decrypt(uint8_t *buf, size_t len, const uint8_t key[16])
{
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	bool ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0;
	if (ok) {
		uint8_t nonce[16];
		uint8_t stream_block[16];
		size_t nc_off = 0;
		memcpy(nonce, AUDIO_AESIV, sizeof(nonce));
		/* The whole file is one counter run starting at zero, so this single
		 * call is equivalent to decrypting it chunk by chunk in order. */
		ok = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce, stream_block,
		                           buf, buf) == 0;
	}
	mbedtls_aes_free(&aes);
	return ok;
}

int track_fetch(const char *sp_host, const char *token, ap_session *ap,
                const char *track_uri, size_t limit, track_audio *out,
                char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));
	TickCounter t;

	osTickCounterStart(&t);
	if (sp_track_metadata(sp_host, token, track_uri, &out->meta,
	                      err, errlen) != 0)
		return -1;
	if (!out->meta.has_gid) {
		snprintf(err, errlen, "metadata carried no track id");
		return -1;
	}
	const sp_file *file = sp_pick_file(&out->meta);
	if (!file) {
		snprintf(err, errlen, "no Vorbis file this console can play");
		return -1;
	}
	out->format = file->format;

	sp_storage storage;
	if (sp_storage_resolve(sp_host, token, file->id, &storage,
	                       err, errlen) < 0)
		return -1;
	osTickCounterUpdate(&t);
	out->resolve_ms = osTickCounterRead(&t);

	/* The key must be asked for before the bytes are worth fetching: without
	 * it the download is just noise. */
	osTickCounterStart(&t);
	uint8_t key[16];
	int refused = 0;
	int rc = ap_audio_key(ap, file->id, out->meta.gid, key, &refused,
	                      err, errlen);
	osTickCounterUpdate(&t);
	out->key_ms = osTickCounterRead(&t);
	if (rc != 0)
		return rc;

	osTickCounterStart(&t);
	http_reply r;
	int got = -1;
	for (int i = 0; i < storage.count; i++) {
		got = http_get_url(storage.urls[i], limit, &r);
		if (got == 0 && (r.status == 200 || r.status == 206))
			break;
		/* A signed URL can have expired or its edge can be down; the other
		 * two serve the same bytes. */
		if (got == 0)
			http_reply_free(&r);
		got = -1;
	}
	osTickCounterUpdate(&t);
	out->download_ms = osTickCounterRead(&t);
	if (got != 0) {
		snprintf(err, errlen, "no CDN url served the file");
		return -1;
	}
	if (r.len <= SPOTIFY_OGG_HEADER) {
		snprintf(err, errlen, "file is only %u bytes", (unsigned)r.len);
		http_reply_free(&r);
		return -1;
	}

	osTickCounterStart(&t);
	if (!decrypt((uint8_t *)r.body, r.len, key)) {
		snprintf(err, errlen, "AES-CTR failed");
		http_reply_free(&r);
		return -1;
	}
	osTickCounterUpdate(&t);
	out->decrypt_ms = osTickCounterRead(&t);

	/* Decryption is in place, so the reply's buffer becomes the track's. */
	out->data = (uint8_t *)r.body;
	out->len = r.len;

	if (memcmp(out->data + SPOTIFY_OGG_HEADER, "OggS", 4) != 0) {
		snprintf(err, errlen,
		         "no Ogg stream after the %d byte header -- wrong key?",
		         SPOTIFY_OGG_HEADER);
		track_free(out);
		return -1;
	}
	return 0;
}
