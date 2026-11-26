/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "passcode_store.h"

#include <tam.h>

#include <argon2.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

void passcode_store_init(struct passcode_store *store) {
	if (!store)
		return;
	memset(store, 0, sizeof(*store));
}

void passcode_store_destroy(struct passcode_store *store) {
	if (!store)
		return;
	explicit_bzero(store, sizeof(*store));
}

struct passcode_params passcode_params_default(void) {
	struct passcode_params p = {
		.t_cost = TAM_HIDE_ARGON2_DEFAULT_T,
		.m_cost = TAM_HIDE_ARGON2_DEFAULT_M,
		.parallelism = TAM_HIDE_ARGON2_DEFAULT_P,
	};
	return p;
}

bool passcode_params_valid_prod(const struct passcode_params *params) {
	if (!params)
		return false;
	if (params->t_cost < TAM_HIDE_ARGON2_MIN_T)
		return false;
	if (params->m_cost < TAM_HIDE_ARGON2_MIN_M)
		return false;
	if (params->parallelism != TAM_HIDE_ARGON2_MIN_P)
		return false;
	return true;
}

bool passcode_username_valid(const char *username) {
	if (!username || username[0] == '\0')
		return false;
	size_t len = strnlen(username, TAM_HIDE_USER_MAX);
	if (len == 0 || len >= TAM_HIDE_USER_MAX)
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)username[i];
		if (!(isalnum(c) || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

static int fill_random(uint8_t *buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = getrandom(buf + off, len - off, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static bool valid_argon2id_phc(const char *hash) {
	if (!hash)
		return false;
	if (strncmp(hash, "$argon2id$", 10) != 0)
		return false;
	size_t len = strnlen(hash, TAM_HIDE_HASH_MAX);
	if (len < 24 || len >= TAM_HIDE_HASH_MAX)
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)hash[i];
		if (c <= 0x20 || c >= 0x7f || c == ':')
			return false;
	}
	return true;
}

int passcode_store_load(struct passcode_store *store, const char *path) {
	if (!store || !path || path[0] == '\0')
		return -EINVAL;

	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	struct stat st;
	if (fstat(fd, &st) < 0) {
		int err = -errno;
		close(fd);
		return err;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		return -EINVAL;
	}
	/* Reject group/world readable or writable passcode stores. */
	if ((st.st_mode & 0077) != 0) {
		close(fd);
		return -EACCES;
	}

	FILE *fp = fdopen(fd, "r");
	if (!fp) {
		int err = -errno;
		close(fd);
		return err;
	}

	struct passcode_store tmp;
	passcode_store_init(&tmp);

	char line[512];
	int rc = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
			rc = -E2BIG;
			break;
		}
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#')
			continue;

		char *c1 = strchr(line, ':');
		if (!c1) {
			rc = -EINVAL;
			break;
		}
		*c1 = '\0';
		char *c2 = strchr(c1 + 1, ':');
		if (!c2) {
			rc = -EINVAL;
			break;
		}
		*c2 = '\0';

		const char *uid_s = line;
		const char *user_s = c1 + 1;
		const char *hash_s = c2 + 1;

		char *end = NULL;
		errno = 0;
		unsigned long uid_v = strtoul(uid_s, &end, 10);
		if (errno != 0 || end == uid_s || *end != '\0' || uid_v > UINT_MAX) {
			rc = -EINVAL;
			break;
		}
		if (!passcode_username_valid(user_s) || !valid_argon2id_phc(hash_s)) {
			rc = -EINVAL;
			break;
		}
		if (tmp.count >= TAM_HIDE_MAX_ACCOUNTS) {
			rc = -ENOSPC;
			break;
		}

		struct passcode_entry *e = &tmp.entries[tmp.count++];
		e->uid = (uid_t)uid_v;
		snprintf(e->username, sizeof(e->username), "%s", user_s);
		snprintf(e->encoded_hash, sizeof(e->encoded_hash), "%s", hash_s);
	}

	explicit_bzero(line, sizeof(line));
	if (ferror(fp))
		rc = -EIO;
	fclose(fp);

	if (rc == 0) {
		passcode_store_destroy(store);
		*store = tmp;
	}
	passcode_store_destroy(&tmp);
	return rc;
}

int passcode_store_save(const struct passcode_store *store, const char *path) {
	if (!store || !path || path[0] == '\0')
		return -EINVAL;

	/* The whole file is built in one buffer, written with
	 * tam_fs_write_atomic (temporary file, fsync, rename, directory fsync:
	 * the old save skipped the directory fsync, so a crash could lose a
	 * new passcode), and wiped. The path is replaced, never followed, as
	 * the loader opens it with O_NOFOLLOW. */
	const char *hdr = "# tam-hide passcode store v1 (uid:username:argon2id)\n";
	size_t cap = strlen(hdr) + 1 + store->count * 512;
	char *buf = malloc(cap);
	if (!buf)
		return -ENOMEM;
	size_t len = (size_t)snprintf(buf, cap, "%s", hdr);
	int rc = 0;
	for (size_t i = 0; i < store->count; i++) {
		const struct passcode_entry *e = &store->entries[i];
		int m = snprintf(buf + len, cap - len, "%u:%s:%s\n", (unsigned)e->uid,
						 e->username, e->encoded_hash);
		if (m < 0 || (size_t)m >= 512 || (size_t)m >= cap - len) {
			rc = -E2BIG;
			break;
		}
		len += (size_t)m;
	}
	if (rc == 0 && tam_fs_write_atomic(path, buf, len, TAM_FS_REPLACE_LINK, 0600) < 0)
		rc = -errno;
	explicit_bzero(buf, cap);
	free(buf);
	return rc;
}

int passcode_store_set(struct passcode_store *store, uid_t uid,
					   const char *username, const char *passcode,
					   size_t pass_len, const struct passcode_params *params) {
	if (!store || !passcode_username_valid(username) || !passcode ||
		pass_len == 0)
		return -EINVAL;

	struct passcode_params p =
		params ? *params : passcode_params_default();
	if (p.t_cost == 0 || p.m_cost < 8 || p.parallelism == 0)
		return -EINVAL;

	uint8_t salt[TAM_HIDE_ARGON2_SALT_LEN];
	int rc = fill_random(salt, sizeof(salt));
	if (rc < 0) {
		explicit_bzero(salt, sizeof(salt));
		return rc;
	}

	char encoded[TAM_HIDE_HASH_MAX];
	memset(encoded, 0, sizeof(encoded));
	int arc = argon2id_hash_encoded(p.t_cost, p.m_cost, p.parallelism, passcode,
									pass_len, salt, sizeof(salt),
									TAM_HIDE_ARGON2_HASH_LEN, encoded,
									sizeof(encoded));
	explicit_bzero(salt, sizeof(salt));
	if (arc != ARGON2_OK) {
		explicit_bzero(encoded, sizeof(encoded));
		return -EINVAL;
	}

	for (size_t i = 0; i < store->count; i++) {
		if (strcmp(store->entries[i].username, username) == 0) {
			store->entries[i].uid = uid;
			snprintf(store->entries[i].encoded_hash,
					 sizeof(store->entries[i].encoded_hash), "%s", encoded);
			explicit_bzero(encoded, sizeof(encoded));
			return 0;
		}
	}

	if (store->count >= TAM_HIDE_MAX_ACCOUNTS) {
		explicit_bzero(encoded, sizeof(encoded));
		return -ENOSPC;
	}

	struct passcode_entry *e = &store->entries[store->count++];
	e->uid = uid;
	snprintf(e->username, sizeof(e->username), "%s", username);
	snprintf(e->encoded_hash, sizeof(e->encoded_hash), "%s", encoded);
	explicit_bzero(encoded, sizeof(encoded));
	return 0;
}

int passcode_store_remove(struct passcode_store *store, const char *username) {
	if (!store || !username || username[0] == '\0')
		return -EINVAL;
	for (size_t i = 0; i < store->count; i++) {
		if (strcmp(store->entries[i].username, username) == 0) {
			explicit_bzero(&store->entries[i], sizeof(store->entries[i]));
			for (size_t j = i + 1; j < store->count; j++)
				store->entries[j - 1] = store->entries[j];
			store->count--;
			explicit_bzero(&store->entries[store->count],
						   sizeof(store->entries[store->count]));
			return 0;
		}
	}
	return -ENOENT;
}

size_t passcode_store_match(const struct passcode_store *store,
							const char *passcode, size_t pass_len,
							struct passcode_match *out, size_t max_out) {
	if (!store || !passcode || pass_len == 0)
		return 0;

	size_t found = 0;
	for (size_t i = 0; i < store->count; i++) {
		const struct passcode_entry *e = &store->entries[i];
		int rc = argon2id_verify(e->encoded_hash, passcode, pass_len);
		if (rc == ARGON2_OK) {
			if (out && found < max_out) {
				out[found].uid = e->uid;
				snprintf(out[found].username, sizeof(out[found].username), "%s",
						 e->username);
			}
			found++;
		}
	}
	return found;
}
