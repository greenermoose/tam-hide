/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_PASSCODE_STORE_H
#define TAM_HIDE_PASSCODE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TAM_HIDE_MAX_ACCOUNTS 64
#define TAM_HIDE_USER_MAX 64
#define TAM_HIDE_HASH_MAX 256
#define TAM_HIDE_DEFAULT_STORE_PATH "/var/lib/tam-hide/passcodes.db"

/* Production Argon2id defaults: 64 MiB, 2 iterations, 1 lane. */
#define TAM_HIDE_ARGON2_DEFAULT_T 2u
#define TAM_HIDE_ARGON2_DEFAULT_M 65536u
#define TAM_HIDE_ARGON2_DEFAULT_P 1u

/* Hard production floors for --argon2-m / --argon2-t tuning. */
#define TAM_HIDE_ARGON2_MIN_T 2u
#define TAM_HIDE_ARGON2_MIN_M 19456u
#define TAM_HIDE_ARGON2_MIN_P 1u

#define TAM_HIDE_ARGON2_SALT_LEN 16u
#define TAM_HIDE_ARGON2_HASH_LEN 32u

struct passcode_params {
	uint32_t t_cost;
	uint32_t m_cost;
	uint32_t parallelism;
};

struct passcode_entry {
	uid_t uid;
	char username[TAM_HIDE_USER_MAX];
	char encoded_hash[TAM_HIDE_HASH_MAX];
};

struct passcode_store {
	struct passcode_entry entries[TAM_HIDE_MAX_ACCOUNTS];
	size_t count;
};

struct passcode_match {
	uid_t uid;
	char username[TAM_HIDE_USER_MAX];
};

/* Initialize an empty passcode store. */
void passcode_store_init(struct passcode_store *store);

/* Wipe all hashes and metadata from memory via explicit_bzero(). */
void passcode_store_destroy(struct passcode_store *store);

/* Return default production Argon2id parameters (m=65536, t=2, p=1). */
struct passcode_params passcode_params_default(void);

/* True if params meet production security floors (m >= 19456, t >= 2, p == 1). */
bool passcode_params_valid_prod(const struct passcode_params *params);

/* True if username is safe for store/IPC serialization. */
bool passcode_username_valid(const char *username);

/*
 * Load passcodes from a privileged file (must be regular file with mode & 0077 == 0).
 * Only accepts $argon2id$ PHC-encoded hashes.
 * Returns 0 on success, negative errno on error (-ENOENT if file missing).
 */
int passcode_store_load(struct passcode_store *store, const char *path);

/*
 * Atomically write store to path with mode 0600 (temp file + fsync + rename).
 * Returns 0 on success, negative errno on error.
 */
int passcode_store_save(const struct passcode_store *store, const char *path);

/*
 * Hash passcode with Argon2id using a fresh 16-byte CSPRNG salt and insert or
 * replace the entry for username. Uses default params when params == NULL.
 * Returns 0 on success, negative errno on error.
 */
int passcode_store_set(struct passcode_store *store, uid_t uid,
					   const char *username, const char *passcode,
					   size_t pass_len, const struct passcode_params *params);

/* Remove entry for username. Returns 0 on success, -ENOENT if not found. */
int passcode_store_remove(struct passcode_store *store, const char *username);

/*
 * Verify passcode against EVERY entry in the store (constant sweep, no early
 * exit) so all accounts sharing a passcode are returned without leaking match
 * position via timing.
 * Returns total number of matching accounts (up to max_out written into out).
 */
size_t passcode_store_match(const struct passcode_store *store,
							const char *passcode, size_t pass_len,
							struct passcode_match *out, size_t max_out);

#endif
