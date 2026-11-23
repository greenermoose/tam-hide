/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller.h"

#include <errno.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef TAM_HIDE_VERSION
#define TAM_HIDE_VERSION "0.2.0"
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s [--lock-vt N] [--socket PATH] [--auth-socket PATH]\n"
			"          [--auth-group GROUP] [--auth-mode OCTAL] [--store PATH]\n"
			"          [--tty-dev PATH] [--release-on-exit]\n"
			"tam-hided %s — whole-seat VT controller (isolated VM testing until Phase 2b).\n",
			prog, TAM_HIDE_VERSION);
}

int main(int argc, char **argv) {
	int lock_vt = 6;
	const char *socket_path = "/run/tam-hide/control.sock";
	const char *auth_socket_path = NULL;
	const char *auth_group = NULL;
	mode_t auth_mode = 0;
	bool auth_mode_set = false;
	const char *store_path = TAM_HIDE_DEFAULT_STORE_PATH;
	const char *tty_dev = "/dev/tty0";
	bool release_on_exit = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--lock-vt") == 0 && i + 1 < argc) {
			lock_vt = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
			socket_path = argv[++i];
		} else if (strcmp(argv[i], "--auth-socket") == 0 && i + 1 < argc) {
			auth_socket_path = argv[++i];
		} else if (strcmp(argv[i], "--auth-group") == 0 && i + 1 < argc) {
			auth_group = argv[++i];
		} else if (strcmp(argv[i], "--auth-mode") == 0 && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			unsigned long m = strtoul(argv[++i], &end, 8);
			if (errno != 0 || !end || *end != '\0' || m > 0777) {
				fprintf(stderr, "tam-hided: invalid --auth-mode\n");
				return 2;
			}
			auth_mode = (mode_t)m;
			auth_mode_set = true;
		} else if (strcmp(argv[i], "--store") == 0 && i + 1 < argc) {
			store_path = argv[++i];
		} else if (strcmp(argv[i], "--tty-dev") == 0 && i + 1 < argc) {
			tty_dev = argv[++i];
		} else if (strcmp(argv[i], "--release-on-exit") == 0) {
			release_on_exit = true;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	struct linux_vt_ctx vctx;
	struct vt_ops vops;
	vt_linux_init(&vops, &vctx, tty_dev);

	struct seat_controller ctrl;
	int rc = controller_init(&ctrl, &vops, lock_vt, socket_path, 0);
	if (rc < 0) {
		fprintf(stderr, "tam-hided: init failed: %s\n", strerror(-rc));
		return 1;
	}

	int lrc = controller_load_store(&ctrl, store_path);
	if (lrc < 0 && lrc != -ENOENT) {
		fprintf(stderr, "tam-hided: failed to load passcode store %s: %s\n",
				store_path, strerror(-lrc));
		controller_cleanup(&ctrl, release_on_exit);
		return 1;
	}

	if (auth_socket_path) {
		gid_t gid = (gid_t)-1;
		if (auth_group) {
			struct group *gr = getgrnam(auth_group);
			if (!gr) {
				fprintf(stderr, "tam-hided: unknown group %s\n", auth_group);
				controller_cleanup(&ctrl, release_on_exit);
				return 1;
			}
			gid = gr->gr_gid;
		}
		if (!auth_mode_set)
			auth_mode = auth_group ? 0660 : 0666;

		rc = controller_enable_auth_socket(&ctrl, auth_socket_path, auth_mode,
										   gid);
		if (rc < 0) {
			fprintf(stderr, "tam-hided: auth socket failed: %s\n",
					strerror(-rc));
			controller_cleanup(&ctrl, release_on_exit);
			return 1;
		}
	}

	fprintf(stderr, "tam-hided %s: ready on lock_vt=%d socket=%s%s%s\n",
			TAM_HIDE_VERSION, lock_vt, socket_path,
			auth_socket_path ? " auth_socket=" : "",
			auth_socket_path ? auth_socket_path : "");
	fflush(stderr);

	while (!g_stop) {
		struct pollfd pfds[2];
		nfds_t nfds = 1;
		pfds[0].fd = ctrl.listen_fd;
		pfds[0].events = POLLIN;
		pfds[0].revents = 0;

		if (ctrl.auth_fd >= 0) {
			pfds[1].fd = ctrl.auth_fd;
			pfds[1].events = POLLIN;
			pfds[1].revents = 0;
			nfds = 2;
		}

		int prc = poll(pfds, nfds, 500);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "tam-hided: poll failed: %s\n", strerror(errno));
			break;
		}
		if (prc > 0) {
			if (pfds[0].revents & POLLIN)
				(void)controller_handle_one(&ctrl);
			if (nfds == 2 && (pfds[1].revents & POLLIN))
				(void)controller_handle_auth_one(&ctrl);
		}
	}

	controller_cleanup(&ctrl, release_on_exit);
	return 0;
}
