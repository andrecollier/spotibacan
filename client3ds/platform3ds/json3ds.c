#include "json3ds.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Finds `"key"` followed by a colon, at any depth. Every object parsed here is
 * flat, so depth tracking would be unused weight -- but a key that could also
 * appear nested would make this ambiguous, which is why the header says so. */
static const char *find_value(const char *json, const char *key)
{
	size_t klen = strlen(key);
	for (const char *p = json; (p = strchr(p, '"')); p++) {
		if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"')
			continue;
		const char *q = p + 2 + klen;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		if (*q != ':')
			continue;
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		return q;
	}
	return NULL;
}

bool json_str(const char *json, const char *key, char *out, size_t outlen)
{
	if (!json || !out || outlen == 0)
		return false;
	out[0] = '\0';

	const char *v = find_value(json, key);
	if (!v || *v != '"')
		return false;
	v++;

	size_t n = 0;
	for (; *v && *v != '"'; v++) {
		if (n + 1 >= outlen)
			return false;              /* truncation is a failure, not a value */
		if (*v != '\\') {
			out[n++] = *v;
			continue;
		}
		switch (*++v) {
		case 'n': out[n++] = '\n'; break;
		case 'r': out[n++] = '\r'; break;
		case 't': out[n++] = '\t'; break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case '"': case '\\': case '/': out[n++] = *v; break;
		case 'u': {
			/* Only the Latin-1 range is representable in the ASCII-plus font
			 * this client draws with; anything above it becomes '?'. */
			char hex[5] = {0};
			for (int i = 0; i < 4 && v[1]; i++)
				hex[i] = *++v;
			long cp = strtol(hex, NULL, 16);
			out[n++] = (cp > 0 && cp < 0x100) ? (char)cp : '?';
			break;
		}
		case '\0': return false;
		default:   out[n++] = *v; break;
		}
	}
	if (*v != '"')
		return false;                  /* unterminated string */
	out[n] = '\0';
	return true;
}

bool json_int(const char *json, const char *key, long *out)
{
	const char *v = find_value(json, key);
	if (!v || (!isdigit((unsigned char)*v) && *v != '-'))
		return false;
	char *end = NULL;
	long n = strtol(v, &end, 10);
	if (end == v)
		return false;
	if (out)
		*out = n;
	return true;
}
