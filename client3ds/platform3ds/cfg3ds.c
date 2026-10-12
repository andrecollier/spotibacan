#include "cfg3ds.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CFG_NAME "spotify.cfg"
/* A CIA has no usable argv[0], so without this everything (the token,
 * native.log, liked.txt, downloads) landed loose on the SD root. */
#define CIA_DIR  "sdmc:/3ds/spotibacan/"
#define CIA_PATH CIA_DIR CFG_NAME
#define CFG_MAX_KEYS 8
#define CFG_VAL_MAX 512

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
	/* CIA install: keep everything under its own folder. The files a root
	 * install of the early builds may have left behind move with it, so the
	 * pairing survives the tidy-up. */
	mkdir("sdmc:/3ds/spotibacan", 0777);
	static const char *legacy[] = {
		"spotify.cfg", "liked.txt", "likes_pending.txt", NULL
	};
	for (int i = 0; legacy[i]; i++) {
		char from[64], to[96];
		snprintf(from, sizeof(from), "sdmc:/%s", legacy[i]);
		snprintf(to, sizeof(to), "%s%s", CIA_DIR, legacy[i]);
		FILE *old = fopen(from, "r");
		if (old) {
			fclose(old);
			FILE *have = fopen(to, "r");
			if (have)
				fclose(have);   /* already migrated: leave both alone */
			else
				rename(from, to);
		}
	}
	snprintf(path, sizeof(path), "%s", CIA_PATH);
}

const char *cfg_path(void)
{
	if (!path[0])
		cfg_set_path(NULL);
	return path;
}

/* The directory the config lives in, for files that belong beside it
 * (native.log, a user-supplied avatar). */
void cfg_dir(char *out, size_t outlen)
{
	snprintf(out, outlen, "%s", cfg_path());
	char *slash = strrchr(out, '/');
	if (slash)
		slash[1] = '\0';
	else
		out[0] = '\0';
}

bool cfg_get(const char *key, char *out, size_t outlen)
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

/* Rewrites the whole file with one key set or replaced.
 *
 * This matters more than it looks: Spotify rotates the refresh token on every
 * exchange, so the moment a refresh succeeds the stored one is dead. A write
 * that is interrupted -- a closed lid, a yanked card, a full filesystem --
 * would then leave no usable token at all and force the user to pair again.
 * The temp-file-plus-rename is the step that cannot half-happen. */
bool cfg_set(const char *key, const char *value)
{
	if (!key || !value || !value[0])
		return false;

	/* Collect the existing pairs, replacing the one being set. */
	char keys[CFG_MAX_KEYS][32], vals[CFG_MAX_KEYS][CFG_VAL_MAX];
	int n = 0;
	FILE *f = fopen(cfg_path(), "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f) && n < CFG_MAX_KEYS) {
			char *eq = strchr(line, '=');
			if (!eq || line[0] == '#')
				continue;
			*eq = '\0';
			char *v = eq + 1;
			char *nl = strpbrk(v, "\r\n");
			if (nl)
				*nl = '\0';
			if (strcmp(line, key) == 0)
				continue;   /* rewritten below */
			if (strlen(line) < sizeof(keys[0]) &&
			    strlen(v) < sizeof(vals[0]) && v[0]) {
				snprintf(keys[n], sizeof(keys[0]), "%s", line);
				snprintf(vals[n], sizeof(vals[0]), "%s", v);
				n++;
			}
		}
		fclose(f);
	}

	char tmp[sizeof(path) + 8];
	snprintf(tmp, sizeof(tmp), "%s.new", cfg_path());

	f = fopen(tmp, "w");
	if (!f)
		return false;
	fprintf(f, "# 3ds-spotify. This file holds a Spotify refresh token:\n"
	           "# treat it like a password and delete it to unpair.\n");
	fprintf(f, "%s=%s\n", key, value);
	for (int i = 0; i < n; i++)
		fprintf(f, "%s=%s\n", keys[i], vals[i]);

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

bool cfg_load_refresh_token(char *out, size_t outlen)
{
	return cfg_get("refresh_token", out, outlen);
}

bool cfg_save_refresh_token(const char *token, const char *scope)
{
	if (!token || !token[0])
		return false;
	if (!cfg_set("refresh_token", token))
		return false;
	if (scope && scope[0])
		cfg_set("scope", scope);
	return true;
}

bool cfg_load_profile_art(char *out, size_t outlen)
{
	return cfg_get("profile_art", out, outlen);
}

bool cfg_save_profile_art(const char *url)
{
	return url && url[0] && cfg_set("profile_art", url);
}

bool cfg_clear(void)
{
	return remove(cfg_path()) == 0;
}
