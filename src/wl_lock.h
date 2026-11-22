/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_WL_LOCK_H
#define TAM_HIDE_WL_LOCK_H

#include "auth.h"

#include <signal.h>
#include <stdbool.h>

struct wl_lock_config {
	auth_verify_fn verify_fn;
	void *verify_ctx;
	/* Optional fd to receive "LOCKED\n" once ext_session_lock_v1.locked fires (-1 if unused). */
	int ready_fd;
	/* Optional signal stop flag; if set, exits WITHOUT unlocking (fail-closed). */
	const volatile sig_atomic_t *stop_flag;
};

/*
 * Connect to the Wayland compositor (WAYLAND_SOCKET or WAYLAND_DISPLAY),
 * acquire ext-session-lock-v1 across all current and hotplugged outputs,
 * and run the authentication loop until unlocked or terminated.
 *
 * Returns:
 *   0  unlocked cleanly after successful authentication
 *   1  failed to connect, lock refused/finished, or terminated without unlock
 */
int wl_lock_run(const struct wl_lock_config *cfg);

#endif
