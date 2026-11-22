/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test-only Wayland lock runner linking the real wl_lock/render/lock_state/auth
 * modules with a mock auth_verify_fn (never linked into production build/tam-hide).
 */
#include "auth.h"
#include "wl_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static enum auth_outcome mock_verify(const char *password, size_t len,
									 void *ctx) {
	const char *expected = ctx;
	if (expected && len == strlen(expected) &&
		memcmp(password, expected, len) == 0)
		return AUTH_OK;
	return AUTH_DENIED;
}

int main(int argc, char **argv) {
	const char *expected = "testpass";
	const char *ready_path = NULL;
	const char *pid_path = NULL;
	const char *event_path = NULL;
	uint32_t blank_timeout_ms = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
			expected = argv[++i];
		} else if (strcmp(argv[i], "--ready-file") == 0 && i + 1 < argc) {
			ready_path = argv[++i];
		} else if (strcmp(argv[i], "--pid-file") == 0 && i + 1 < argc) {
			pid_path = argv[++i];
		} else if (strcmp(argv[i], "--event-file") == 0 && i + 1 < argc) {
			event_path = argv[++i];
		} else if (strcmp(argv[i], "--blank-after-ms") == 0 && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			unsigned long long v = strtoull(argv[++i], &end, 10);
			if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
				fprintf(stderr, "invalid --blank-after-ms\n");
				return 2;
			}
			blank_timeout_ms = (uint32_t)v;
		} else {
			fprintf(stderr,
					"Usage: %s [--expect PASS] [--ready-file PATH] "
					"[--pid-file PATH] [--event-file PATH] "
					"[--blank-after-ms MS]\n",
					argv[0]);
			return 2;
		}
	}

	if (pid_path) {
		FILE *pf = fopen(pid_path, "w");
		if (!pf) {
			perror("fopen pid-file");
			return 1;
		}
		fprintf(pf, "%ld\n", (long)getpid());
		fclose(pf);
	}

	int ready_fd = -1;
	if (ready_path) {
		ready_fd = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
						0600);
		if (ready_fd < 0) {
			perror("open ready-file");
			return 1;
		}
	}

	int event_fd = -1;
	if (event_path) {
		event_fd = open(event_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
						0600);
		if (event_fd < 0) {
			perror("open event-file");
			if (ready_fd >= 0)
				close(ready_fd);
			return 1;
		}
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct wl_lock_config cfg = {
		.verify_fn = mock_verify,
		.verify_ctx = (void *)expected,
		.ready_fd = ready_fd,
		.stop_flag = &g_stop,
		.blank_timeout_ms = blank_timeout_ms,
		.event_fd = event_fd,
	};
	int rc = wl_lock_run(&cfg);
	if (event_fd >= 0)
		close(event_fd);
	if (ready_fd >= 0)
		close(ready_fd);
	return rc;
}
