#include "spclient.h"
#include "http3ds.h"
#include "proto3ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPCLIENT_FALLBACK "spclient.wg.spotify.com"

/* extended_metadata.proto: BatchedEntityRequest { header 1, entity_request 2 },
 * EntityRequest { entity_uri 1, query 2 }, ExtensionQuery { extension_kind 1 }.
 * Putting the entity request in field 1 yields HTTP 200 and an empty body,
 * which is a confusing way to be told the shape is wrong. */
#define EXT_KIND_TRACK_V4 10

static void copy_str(char *dst, size_t cap, const pbv *v)
{
	size_t n = v->len < cap - 1 ? v->len : cap - 1;
	memcpy(dst, v->p, n);
	dst[n] = '\0';
}

const char *sp_format_name(int format)
{
	switch (format) {
	case SP_FMT_OGG_96:  return "Vorbis 96";
	case SP_FMT_OGG_160: return "Vorbis 160";
	case SP_FMT_OGG_320: return "Vorbis 320";
	case 3: case 4: case 5: case 6: case 7: return "MP3";
	case 8: case 9: return "AAC";
	case 16: case 22: return "FLAC";
	default: return "other";
	}
}

const sp_file *sp_pick_file(const sp_track *t)
{
	/* 160 first, then 96, then 320: the order of what this console should
	 * actually stream and decode, not of audio quality. */
	static const int WANT[] = { SP_FMT_OGG_160, SP_FMT_OGG_96, SP_FMT_OGG_320 };
	for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++)
		for (int i = 0; i < t->file_count; i++)
			if (t->files[i].format == WANT[w])
				return &t->files[i];
	return NULL;
}

int sp_resolve(char *host, size_t hostlen, char *err, size_t errlen)
{
	http_reply r;
	if (http_get("apresolve.spotify.com", "/?type=spclient", NULL, &r) == 0 &&
	    r.status == 200) {
		const char *key = strstr(r.body, "\"spclient\"");
		const char *open = key ? strchr(key, '[') : NULL;
		const char *q1 = open ? strchr(open, '"') : NULL;
		const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
		if (q2) {
			char entry[160];
			size_t n = (size_t)(q2 - q1 - 1);
			if (n < sizeof(entry)) {
				memcpy(entry, q1 + 1, n);
				entry[n] = '\0';
				/* The entry carries a port; 443 is implied and the port is
				 * dropped, since everything here goes over TLS. */
				char *colon = strrchr(entry, ':');
				if (colon)
					*colon = '\0';
				snprintf(host, hostlen, "%s", entry);
				http_reply_free(&r);
				return 0;
			}
		}
	}
	http_reply_free(&r);
	snprintf(host, hostlen, "%s", SPCLIENT_FALLBACK);
	snprintf(err, errlen, "apresolve gave no spclient; using %s", SPCLIENT_FALLBACK);
	return 0;
}

int sp_track_metadata(const char *host, const char *token, const char *track_uri,
                      sp_track *out, char *err, size_t errlen)
{
	memset(out, 0, sizeof(*out));

	uint8_t q_store[16], er_store[256], req_store[320];
	pb q, er, req;
	pb_init(&q, q_store, sizeof(q_store));
	pb_varint(&q, 1, EXT_KIND_TRACK_V4);
	pb_init(&er, er_store, sizeof(er_store));
	pb_str(&er, 1, track_uri);
	pb_sub(&er, 2, &q);
	pb_init(&req, req_store, sizeof(req_store));
	pb_sub(&req, 2, &er);
	if (!pb_ok(&req)) {
		snprintf(err, errlen, "metadata request did not fit");
		return -1;
	}

	http_reply r;
	/* The Bearer matters here as much as on the GETs: without it the service
	 * answers 200 with an empty body rather than 401, which reads like a
	 * malformed request. */
	if (http_request(host, "/extended-metadata/v0/extended-metadata", "POST",
	                 token, "application/x-protobuf", "application/x-protobuf",
	                 req.p, req.len, &r) != 0) {
		snprintf(err, errlen, "metadata: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "metadata returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	pbv arr, ed, any, track;
	/* BatchedExtensionResponse.extended_metadata -> EntityExtensionDataArray
	 * .extension_data -> EntityExtensionData.extension_data (an Any) -> value */
	bool found = pb_get_bytes(body, r.len, 2, &arr) &&
	             pb_get_bytes(arr.p, arr.len, 3, &ed) &&
	             pb_get_bytes(ed.p, ed.len, 3, &any) &&
	             pb_get_bytes(any.p, any.len, 2, &track);
	if (!found) {
		snprintf(err, errlen, "no track in the metadata response (%u bytes)",
		         (unsigned)r.len);
		http_reply_free(&r);
		return -1;
	}

	pbv v;
	if (pb_get_bytes(track.p, track.len, 2, &v))      /* Track.name */
		copy_str(out->name, sizeof(out->name), &v);
	int64_t dur = 0;
	/* Track.duration is a sint32: zigzag, not a plain varint. Read the wrong
	 * way it comes back doubled, which reads as a long-but-believable track
	 * rather than as an error. */
	if (pb_get_sint(track.p, track.len, 7, &dur))
		out->duration_ms = (int)dur;
	if (pb_get_bytes(track.p, track.len, 3, &v)) {    /* Track.album */
		pbv an;
		if (pb_get_bytes(v.p, v.len, 2, &an))         /* Album.name */
			copy_str(out->album, sizeof(out->album), &an);
	}
	if (pb_get_bytes(track.p, track.len, 4, &v)) {    /* first Artist */
		pbv an;
		/* Artist.gid is field 1 and Artist.name is field 2; reading the first
		 * field here would hand back raw bytes that are not text. */
		if (pb_get_bytes(v.p, v.len, 2, &an))
			copy_str(out->artist, sizeof(out->artist), &an);
	}

	/* Track.file is repeated, and pb_get_bytes only yields the first, so the
	 * files are walked by hand. */
	size_t at = 0;
	while (at < track.len && out->file_count < SP_MAX_FILES) {
		pbv rest = { track.p + at, track.len - at };
		pbv file;
		if (!pb_get_bytes(rest.p, rest.len, 12, &file))
			break;
		pbv fid;
		uint64_t fmt = 0;
		if (pb_get_bytes(file.p, file.len, 1, &fid) && fid.len == 20) {
			memcpy(out->files[out->file_count].id, fid.p, 20);
			pb_get_varint(file.p, file.len, 2, &fmt);
			out->files[out->file_count].format = (int)fmt;
			out->file_count++;
		}
		at = (size_t)(file.p + file.len - track.p);
	}

	http_reply_free(&r);
	return 0;
}

int sp_rootlist(const char *host, const char *token, const char *username,
                char uris[][96], int max, int *count, char *err, size_t errlen)
{
	char path[256];
	snprintf(path, sizeof(path), "/playlist/v2/user/%s/rootlist", username);

	http_reply r;
	if (http_get_proto(host, path, token, &r) != 0) {
		snprintf(err, errlen, "rootlist: %s", r.err);
		return -1;
	}
	if (r.status != 200) {
		snprintf(err, errlen, "rootlist returned HTTP %d", r.status);
		http_reply_free(&r);
		return -1;
	}

	const uint8_t *body = (const uint8_t *)r.body;
	uint64_t length = 0;
	pb_get_varint(body, r.len, 2, &length);   /* SelectedListContent.length */
	*count = (int)length;

	int n = 0;
	pbv contents;
	if (pb_get_bytes(body, r.len, 5, &contents)) {   /* ListItems */
		size_t at = 0;
		while (at < contents.len && n < max) {
			pbv rest = { contents.p + at, contents.len - at };
			pbv item;
			if (!pb_get_bytes(rest.p, rest.len, 3, &item))
				break;
			pbv uri;
			if (pb_get_bytes(item.p, item.len, 1, &uri)) {
				size_t len = uri.len < 95 ? uri.len : 95;
				memcpy(uris[n], uri.p, len);
				uris[n][len] = '\0';
				n++;
			}
			at = (size_t)(item.p + item.len - contents.p);
		}
	}
	http_reply_free(&r);
	return n;
}
