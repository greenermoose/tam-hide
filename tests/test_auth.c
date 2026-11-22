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

int main(void) {
	assert(run_sync(mock_check, "hunter2", "hunter2") == AUTH_OK);
	assert(run_sync(mock_check, "hunter2", "wrong") == AUTH_DENIED);
	/* Crashing worker must always fail closed (AUTH_ERROR). */
	assert(run_sync(mock_crash, NULL, "anything") == AUTH_ERROR);

	printf("test_auth: all tests passed\n");
	return 0;
}
