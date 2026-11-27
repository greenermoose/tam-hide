/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller.h"
#include "session_sync.h"
#include "vt_prompt.h"

#include <tam.h>

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
#define TAM_HIDE_VERSION "0.3.0"
#endif

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;

static void on_signal(int sig) {
	if (sig == SIGHUP)
		g_reload = 1;
	else
		g_stop = 1;
}

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s [--lock-vt N] [--socket PATH] [--auth-socket PATH]\n"
			"          [--auth-group GROUP] [--auth-mode OCTAL] [--store PATH]\n"
			"          [--tty-dev PATH] [--no-logind] [--no-vt-prompt]\n"
			"          [--release-on-exit]\n"
			"tam-hided %s — whole-seat VT controller.\n",
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
	bool use_logind = true;
	bool use_vt_prompt = true;
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
		} else if (strcmp(argv[i], "--no-logind") == 0) {
			use_logind = false;
		} else if (strcmp(argv[i], "--no-vt-prompt") == 0) {
			use_vt_prompt = false;
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
	sigaction(SIGHUP, &sa, NULL);
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

	struct session_watcher watcher;
	memset(&watcher, 0, sizeof(watcher));
	watcher.fd = -1;
	if (use_logind) {
		int wrc = session_watcher_init(&watcher);
		if (wrc == 0) {
			(void)session_watcher_sync(&watcher, &ctrl);
		} else if (wrc != -ENOTSUP) {
			fprintf(stderr, "tam-hided: logind watcher unavailable: %s\n",
					strerror(-wrc));
		}
	}

	struct vt_prompt prompt;
	memset(&prompt, 0, sizeof(prompt));
	prompt.fd = -1;
	if (use_vt_prompt) {
		int prc = vt_prompt_init_vt(&prompt, lock_vt);
		if (prc < 0) {
			fprintf(stderr, "tam-hided: tty%d prompt unavailable: %s\n",
					lock_vt, strerror(-prc));
		}
	}

	fprintf(stderr, "tam-hided %s: ready on lock_vt=%d socket=%s%s%s%s%s\n",
			TAM_HIDE_VERSION, lock_vt, socket_path,
			auth_socket_path ? " auth_socket=" : "",
			auth_socket_path ? auth_socket_path : "",
			watcher.active ? " logind=on" : "",
			prompt.active ? " vt_prompt=on" : "");
	fflush(stderr);

	while (!g_stop) {
		if (g_reload) {
			g_reload = 0;
			int rrc = controller_reload_store(&ctrl);
			if (rrc < 0) {
				fprintf(stderr, "tam-hided: store reload failed: %s\n",
						strerror(-rrc));
			} else {
				fprintf(stderr, "tam-hided: store reloaded (%zu accounts)\n",
						ctrl.store.count);
			}
			fflush(stderr);
		}

		struct pollfd pfds[4];
		nfds_t nfds = 0;
		int idx_ctrl = -1, idx_auth = -1, idx_watch = -1, idx_prompt = -1;

		idx_ctrl = (int)nfds;
		pfds[nfds].fd = ctrl.listen_fd;
		pfds[nfds].events = POLLIN;
		pfds[nfds].revents = 0;
		nfds++;

		if (ctrl.auth_fd >= 0) {
			idx_auth = (int)nfds;
			pfds[nfds].fd = ctrl.auth_fd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		int wfd = session_watcher_fd(&watcher);
		if (wfd >= 0) {
			idx_watch = (int)nfds;
			pfds[nfds].fd = wfd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		int pfd = vt_prompt_fd(&prompt);
		if (pfd >= 0 && ctrl.state == SEAT_STATE_LOCKED &&
			ctrl.active_vt == ctrl.lock_vt) {
			idx_prompt = (int)nfds;
			pfds[nfds].fd = pfd;
			pfds[nfds].events = POLLIN;
			pfds[nfds].revents = 0;
			nfds++;
		}

		enum seat_state prev_state = ctrl.state;
		int prc = poll(pfds, nfds, 500);
		if (prc < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "tam-hided: poll failed: %s\n", strerror(errno));
			break;
		}

		if (prc > 0) {
			if (idx_ctrl >= 0 && (pfds[idx_ctrl].revents & POLLIN))
				(void)controller_handle_one(&ctrl);
			if (idx_auth >= 0 && (pfds[idx_auth].revents & POLLIN))
				(void)controller_handle_auth_one(&ctrl);
			if (idx_watch >= 0 && (pfds[idx_watch].revents & POLLIN))
				(void)session_watcher_sync(&watcher, &ctrl);
			if (idx_prompt >= 0 && (pfds[idx_prompt].revents & POLLIN) &&
				ctrl.state == SEAT_STATE_LOCKED)
				(void)vt_prompt_handle_input(&prompt, &ctrl);
		}

		if (prompt.active) {
			if (prev_state != SEAT_STATE_LOCKED &&
				ctrl.state == SEAT_STATE_LOCKED) {
				vt_prompt_reset(&prompt, NULL);
			} else if (ctrl.state == SEAT_STATE_LOCKED) {
				vt_prompt_tick(&prompt, tam_monotonic_ms());
			}
		}
	}

	vt_prompt_destroy(&prompt);
	session_watcher_destroy(&watcher);
	controller_cleanup(&ctrl, release_on_exit);
	return 0;
}
