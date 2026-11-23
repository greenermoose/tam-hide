/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "passcode_store.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void test_prod_params_and_validation(void) {
	struct passcode_params def = passcode_params_default();
	assert(def.m_cost == 65536u);
	assert(def.t_cost == 2u);
	assert(def.parallelism == 1u);
	assert(passcode_params_valid_prod(&def));

	struct passcode_params low_m = { .t_cost = 2, .m_cost = 1024, .parallelism = 1 };
	assert(!passcode_params_valid_prod(&low_m));

	struct passcode_params low_t = { .t_cost = 1, .m_cost = 65536, .parallelism = 1 };
	assert(!passcode_params_valid_prod(&low_t));

	struct passcode_params bad_p = { .t_cost = 2, .m_cost = 65536, .parallelism = 2 };
	assert(!passcode_params_valid_prod(&bad_p));

	assert(passcode_username_valid("fred"));
	assert(passcode_username_valid("alice_1-2.3"));
	assert(!passcode_username_valid(""));
	assert(!passcode_username_valid("bad:user"));
	assert(!passcode_username_valid("bad user"));
}

static void test_hash_duplicate_save_and_permissions(void) {
	/* Fast low-cost params for unit test speed; real Argon2id algorithm. */
	struct passcode_params fast = { .t_cost = 1, .m_cost = 1024, .parallelism = 1 };

	struct passcode_store st;
	passcode_store_init(&st);

	assert(passcode_store_set(&st, 1000, "alice", "shared-secret", 13, &fast) == 0);
	assert(passcode_store_set(&st, 1001, "bob", "shared-secret", 13, &fast) == 0);
	assert(passcode_store_set(&st, 1002, "carol", "carol-only", 10, &fast) == 0);
	assert(st.count == 3);

	/* Duplicate passcode must return ALL matching accounts. */
	struct passcode_match matches[8];
	size_t n = passcode_store_match(&st, "shared-secret", 13, matches, 8);
	assert(n == 2);
	assert(matches[0].uid == 1000 && strcmp(matches[0].username, "alice") == 0);
	assert(matches[1].uid == 1001 && strcmp(matches[1].username, "bob") == 0);

	/* Single match. */
	n = passcode_store_match(&st, "carol-only", 10, matches, 8);
	assert(n == 1);
	assert(matches[0].uid == 1002 && strcmp(matches[0].username, "carol") == 0);

	/* Wrong passcode returns 0 matches. */
	n = passcode_store_match(&st, "wrong", 5, matches, 8);
	assert(n == 0);

	/* Update existing user replaces hash in place. */
	assert(passcode_store_set(&st, 1001, "bob", "bob-new", 7, &fast) == 0);
	assert(st.count == 3);
	n = passcode_store_match(&st, "shared-secret", 13, matches, 8);
	assert(n == 1 && strcmp(matches[0].username, "alice") == 0);

	/* Save atomically with 0600 permissions and reload. */
	char path[128];
	snprintf(path, sizeof(path), "/tmp/tam-hide-store-test-%d.db", (int)getpid());
	assert(passcode_store_save(&st, path) == 0);

	struct stat sb;
	assert(stat(path, &sb) == 0);
	assert((sb.st_mode & 0777) == 0600);

	struct passcode_store loaded;
	passcode_store_init(&loaded);
	assert(passcode_store_load(&loaded, path) == 0);
	assert(loaded.count == 3);
	assert(passcode_store_match(&loaded, "bob-new", 7, matches, 8) == 1);

	/* Remove user. */
	assert(passcode_store_remove(&loaded, "carol") == 0);
	assert(loaded.count == 2);
	assert(passcode_store_remove(&loaded, "nonexistent") == -ENOENT);

	/* World/group-readable store must be refused with -EACCES. */
	assert(chmod(path, 0644) == 0);
	struct passcode_store bad;
	passcode_store_init(&bad);
	assert(passcode_store_load(&bad, path) == -EACCES);

	/* Non-argon2id hash lines must be refused with -EINVAL. */
	assert(chmod(path, 0600) == 0);
	FILE *fp = fopen(path, "w");
	assert(fp != NULL);
	fprintf(fp, "1000:alice:$argon2i$v=19$m=1024,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n");
	fclose(fp);
	assert(passcode_store_load(&bad, path) == -EINVAL);

	(void)unlink(path);
	passcode_store_destroy(&st);
	passcode_store_destroy(&loaded);
	passcode_store_destroy(&bad);
}

int main(void) {
	test_prod_params_and_validation();
	test_hash_duplicate_save_and_permissions();
	printf("test_passcode_store: all tests passed\n");
	return 0;
}
