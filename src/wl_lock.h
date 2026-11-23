/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_WL_LOCK_H
#define TAM_HIDE_WL_LOCK_H

#include "auth.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum auth_outcome (*wl_lock_select_fn)(const char *token, int index,
											   void *ctx);
typedef void (*wl_lock_cancel_fn)(void *ctx);
typedef void (*wl_lock_on_locked_fn)(void *ctx);

struct wl_lock_config {
	auth_verify_fn verify_fn;
	/* Optional extended verifier supporting AUTH_PICK and AUTH_LOCKOUT. */
	auth_verify_ex_fn verify_ex_fn;
	void *verify_ctx;
	/* Optional picker selection/cancel callbacks (used when AUTH_PICK fires). */
	wl_lock_select_fn select_fn;
	wl_lock_cancel_fn cancel_fn;
	/* Optional callback invoked when ext_session_lock_v1.locked fires. */
	wl_lock_on_locked_fn on_locked_fn;
	/* Optional fd to receive "LOCKED\n" once ext_session_lock_v1.locked fires (-1 if unused). */
	int ready_fd;
	/* Optional signal stop flag; if set, exits WITHOUT unlocking (fail-closed). */
	const volatile sig_atomic_t *stop_flag;
	/* Idle timeout in ms before blanking outputs while locked (0 disables). */
	uint32_t blank_timeout_ms;
	/* Optional fd to receive power/idle/picker state lines for headless tests (-1 if unused). */
	int event_fd;
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
