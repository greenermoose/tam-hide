/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "auth_controller.h"
#include "power_policy.h"
#include "vt.h"
#include "wl_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TAM_HIDE_VERSION
#define TAM_HIDE_VERSION "0.3.0"
#endif

#define TAM_HIDE_DEFAULT_AUTH_SOCKET "/run/tam-hide/auth.sock"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s [--blank-after SECONDS] [--ready-fd FD]\n"
			"          [--auth-socket PATH] [--session-vt N] [--pam-only]\n"
			"          [-v | --version] [-h | --help]\n"
			"\n"
			"tam-hide %s — Wayland ext-session-lock-v1 locker for tam-pane.\n"
			"Launch via tam-pane's tp.lock{\"tam-hide\"} (WAYLAND_SOCKET).\n"
			"\n"
			"Options:\n"
			"  --blank-after SECONDS  Blank displays after SECONDS of idle while\n"
			"                         locked (default: %u, 0 disables blanking).\n"
			"  --ready-fd FD          Write LOCKED\\n to open file descriptor FD once\n"
			"                         ext_session_lock_v1.locked is confirmed.\n"
			"  --auth-socket PATH     Authenticate via tam-hided's auth socket\n"
			"                         (Argon2id multi-account/session picker).\n"
			"                         When omitted, auto-connects to\n"
			"                         %s if available.\n"
			"  --session-vt N         Report CONFIRM_LOCK N to --auth-socket once\n"
			"                         ext_session_lock_v1.locked is confirmed\n"
			"                         (defaults to $XDG_VTNR when auto-connected).\n"
			"  --pam-only             Always authenticate via local PAM even if\n"
			"                         tam-hided's auth socket is available.\n"
			"  -v, --version          Print version and exit.\n"
			"  -h, --help             Show this help and exit.\n",
			prog, TAM_HIDE_VERSION, TAM_HIDE_DEFAULT_BLANK_SEC,
			TAM_HIDE_DEFAULT_AUTH_SOCKET);
}

static bool auth_socket_available(const char *path) {
	struct stat st;
	if (!path || stat(path, &st) < 0)
		return false;
	if (!S_ISSOCK(st.st_mode))
		return false;
	return access(path, R_OK | W_OK) == 0;
}

static bool parse_int_range(const char *s, int min_v, int max_v, int *out) {
	if (!s || s[0] == '\0')
		return false;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < min_v || v > max_v)
		return false;
	*out = (int)v;
	return true;
}

int main(int argc, char **argv) {
	uint32_t blank_timeout_ms = TAM_HIDE_DEFAULT_BLANK_MS;
	int ready_fd = -1;
	const char *auth_socket = NULL;
	int session_vt = 0;
	bool pam_only = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("tam-hide %s\n", TAM_HIDE_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--blank-after") == 0 && i + 1 < argc) {
			if (!power_policy_parse_seconds(argv[++i], &blank_timeout_ms)) {
				fprintf(stderr, "tam-hide: invalid --blank-after value\n");
				return 2;
			}
			continue;
		}
		if (strcmp(argv[i], "--ready-fd") == 0 && i + 1 < argc) {
			if (!parse_int_range(argv[++i], 0, INT_MAX, &ready_fd) ||
				fcntl(ready_fd, F_GETFD) < 0) {
				fprintf(stderr, "tam-hide: invalid --ready-fd\n");
				return 2;
			}
			continue;
		}
		if (strcmp(argv[i], "--auth-socket") == 0 && i + 1 < argc) {
			auth_socket = argv[++i];
			continue;
		}
		if (strcmp(argv[i], "--session-vt") == 0 && i + 1 < argc) {
			if (!parse_int_range(argv[++i], TAM_VT_MIN, TAM_VT_MAX,
								 &session_vt)) {
				fprintf(stderr, "tam-hide: invalid --session-vt\n");
				return 2;
			}
			continue;
		}
		if (strcmp(argv[i], "--pam-only") == 0) {
			pam_only = true;
			continue;
		}
		usage(argv[0]);
		return 2;
	}

	if (!auth_socket && !pam_only &&
		auth_socket_available(TAM_HIDE_DEFAULT_AUTH_SOCKET)) {
		auth_socket = TAM_HIDE_DEFAULT_AUTH_SOCKET;
		if (session_vt == 0) {
			const char *vtnr = getenv("XDG_VTNR");
			if (vtnr)
				(void)parse_int_range(vtnr, TAM_VT_MIN, TAM_VT_MAX,
									  &session_vt);
		}
	}

	/* Block ptrace and core dumps to protect password memory. */
	(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct auth_controller_ctx ctrl_ctx = {
		.socket_path = auth_socket,
		.session_vt = session_vt,
	};

	struct wl_lock_config cfg = {
		.verify_fn = auth_socket ? NULL : auth_pam_verify,
		.verify_ex_fn = auth_socket ? auth_controller_verify : NULL,
		.verify_ctx = auth_socket ? &ctrl_ctx : NULL,
		.select_fn = auth_socket ? auth_controller_select : NULL,
		.cancel_fn = auth_socket ? auth_controller_cancel : NULL,
		.on_locked_fn =
			(auth_socket && session_vt > 0) ? auth_controller_on_locked : NULL,
		.ready_fd = ready_fd,
		.stop_flag = &g_stop,
		.blank_timeout_ms = blank_timeout_ms,
		.event_fd = -1,
	};
	return wl_lock_run(&cfg);
}
