#include "cfg3ds.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define CFG_NAME "spotify.cfg"

static char path[320];

void cfg_set_path(const char *argv0)
{
	if (argv0 && argv0[0]) {
		snprintf(path, sizeof(path), "%s", argv0);
		char *slash = strrchr(path, '/');
		if (slash) {
			slash[1] = '\0';
			/* Confirm the directory is really there before using it: under an
			 * emulator argv[0] is a host path the guest cannot reach. Asking
			 * opendir rather than opening the file for append, which would
			 * leave an empty spotify.cfg behind on every first run. */
			DIR *dir = opendir(path);
			if (dir) {
				closedir(dir);
				strncat(path, CFG_NAME, sizeof(path) - strlen(path) - 1);
				return;
			}
		}
	}
	snprintf(path, sizeof(path), "sdmc:/" CFG_NAME);
}

const char *cfg_path(void)
{
	if (!path[0])
		cfg_set_path(NULL);
	return path;
}

static bool read_key(const char *key, char *out, size_t outlen)
{
	FILE *f = fopen(cfg_path(), "r");
	if (!f)
		return false;
	size_t klen = strlen(key);
	char line[1024];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, klen) != 0 || line[klen] != '=')
			continue;
		char *v = line + klen + 1;
		char *nl = strpbrk(v, "\r\n");
		if (nl)
			*nl = '\0';
		if (v[0] && strlen(v) < outlen) {
			snprintf(out, outlen, "%s", v);
			found = true;
		}
		break;
	}
	fclose(f);
	return found;
}

bool cfg_load_refresh_token(char *out, size_t outlen)
{
	return read_key("refresh_token", out, outlen);
}

bool cfg_load_profile_art(char *out, size_t outlen)
{
	return read_key("profile_art", out, outlen);
}

static bool write_cfg(const char *token, const char *scope, const char *art)
{
	char tmp[sizeof(path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", cfg_path());

	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "# 3ds-spotify. This file holds a Spotify refresh token:\n"
	           "# treat it like a password and delete it to unpair.\n");
	fprintf(f, "refresh_token=%s\n", token);
	if (scope && scope[0])
		fprintf(f, "scope=%s\n", scope);
	if (art && art[0])
		fprintf(f, "profile_art=%s\n", art);

	bool ok = fflush(f) == 0 && ferror(f) == 0;
	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		remove(tmp);
		return false;
	}

	/* FAT has no atomic replace, so the old file goes first; the window
	 * between the two is the price, and it is far smaller than a partial
	 * write over the only copy of the token. */
	remove(cfg_path());
	if (rename(tmp, cfg_path()) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

/* Written to a temporary file and renamed into place.
 *
 * This matters more than it looks: Spotify rotates the refresh token on every
 * exchange, so the moment a refresh succeeds the stored one is dead. A write
 * that is interrupted -- a closed lid, a yanked card, a full filesystem --
 * would then leave no usable token at all and force the user to pair again.
 * The rename is the step that cannot half-happen. */
bool cfg_save_refresh_token(const char *token, const char *scope)
{
	if (!token || !token[0])
		return false;

	/* The rewrite must carry the other keys over, or every token refresh
	 * would silently drop the cached avatar. */
	char art[96] = "";
	read_key("profile_art", art, sizeof(art));
	return write_cfg(token, scope, art);
}

bool cfg_save_profile_art(const char *url)
{
	if (!url || !url[0])
		return false;

	char token[512], scope[256] = "";
	if (!read_key("refresh_token", token, sizeof(token)))
		return false;
	read_key("scope", scope, sizeof(scope));
	return write_cfg(token, scope, url);
}

bool cfg_clear(void)
{
	return remove(cfg_path()) == 0;
}
