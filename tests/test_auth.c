/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"

#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static enum auth_outcome mock_check(const char *password, size_t len,
									void *ctx) {
	const char *expected = ctx;
	if (expected && len == strlen(expected) &&
		memcmp(password, expected, len) == 0)
		return AUTH_OK;
	return AUTH_DENIED;
}

static enum auth_outcome mock_crash(const char *password, size_t len,
									void *ctx) {
	(void)password;
	(void)len;
	(void)ctx;
	raise(SIGKILL);
	return AUTH_OK;
}

static void mock_pick(const char *password, size_t len, void *ctx,
					  struct auth_result *out) {
	(void)ctx;
	if (len == 6 && memcmp(password, "shared", 6) == 0) {
		out->outcome = AUTH_PICK;
		snprintf(out->pick_token, sizeof(out->pick_token),
				 "0123456789abcdef0123456789abcdef");
		out->pick_count = 2;
		out->pick_items[0].index = 0;
		out->pick_items[0].uid = 1000;
		out->pick_items[0].vt_nr = 2;
		snprintf(out->pick_items[0].username,
				 sizeof(out->pick_items[0].username), "alice");
		out->pick_items[1].index = 1;
		out->pick_items[1].uid = 1001;
		out->pick_items[1].vt_nr = 3;
		snprintf(out->pick_items[1].username,
				 sizeof(out->pick_items[1].username), "bob");
		return;
	}
	out->outcome = AUTH_LOCKOUT;
	out->lockout_sec = 4;
}

static enum auth_outcome run_sync(auth_verify_fn fn, void *ctx,
								  const char *pw) {
	struct auth_job job;
	auth_job_init(&job);
	char buf[64];
	size_t len = strlen(pw);
	memcpy(buf, pw, len + 1);

	assert(auth_start(&job, fn, ctx, buf, len) == 0);
	/* Parent buffer must be wiped immediately after auth_start. */
	for (size_t i = 0; i < len; i++)
		assert(buf[i] == 0);

	struct pollfd pfd = { .fd = job.result_fd, .events = POLLIN };
	assert(poll(&pfd, 1, 5000) == 1);
	return auth_finish(&job);
}

static void test_extended_pick_and_lockout(void) {
	struct auth_job job;
	auth_job_init(&job);
	char buf[64] = "shared";
	assert(auth_start_ex(&job, mock_pick, NULL, buf, 6) == 0);
	for (size_t i = 0; i < 6; i++)
		assert(buf[i] == 0);

	struct pollfd pfd = { .fd = job.result_fd, .events = POLLIN };
	assert(poll(&pfd, 1, 5000) == 1);

	struct auth_result res;
	auth_finish_ex(&job, &res);
	assert(res.outcome == AUTH_PICK);
	assert(res.pick_count == 2);
	assert(strcmp(res.pick_token, "0123456789abcdef0123456789abcdef") == 0);
	assert(strcmp(res.pick_items[0].username, "alice") == 0);
	assert(strcmp(res.pick_items[1].username, "bob") == 0);

	char buf2[64] = "nope";
	assert(auth_start_ex(&job, mock_pick, NULL, buf2, 4) == 0);
	pfd.fd = job.result_fd;
	assert(poll(&pfd, 1, 5000) == 1);
	auth_finish_ex(&job, &res);
	assert(res.outcome == AUTH_LOCKOUT);
	assert(res.lockout_sec == 4);
}

int main(void) {
	assert(run_sync(mock_check, "hunter2", "hunter2") == AUTH_OK);
	assert(run_sync(mock_check, "hunter2", "wrong") == AUTH_DENIED);
	/* Crashing worker must always fail closed (AUTH_ERROR). */
	assert(run_sync(mock_crash, NULL, "anything") == AUTH_ERROR);
	test_extended_pick_and_lockout();

	printf("test_auth: all tests passed\n");
	return 0;
}
