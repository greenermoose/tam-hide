/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "wl_lock.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#ifndef TAM_HIDE_VERSION
#define TAM_HIDE_VERSION "0.1.0"
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s [-v | --version] [-h | --help]\n"
			"\n"
			"tam-hide %s — Wayland ext-session-lock-v1 locker for tam-pane.\n"
			"Launch via tam-pane's tp.lock{\"tam-hide\"} (WAYLAND_SOCKET).\n"
			"\n"
			"Phase 1 scope: locks the active Wayland session using PAM for the\n"
			"current user. Whole-seat VT protection (tam-hided) is Phase 2.\n",
			prog, TAM_HIDE_VERSION);
}

int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("tam-hide %s\n", TAM_HIDE_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
		usage(argv[0]);
		return 2;
	}

	/* Block ptrace and core dumps to protect password memory. */
	(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct wl_lock_config cfg = {
		.verify_fn = auth_pam_verify,
		.verify_ctx = NULL,
		.ready_fd = -1,
		.stop_flag = &g_stop,
	};
	return wl_lock_run(&cfg);
}
