/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_AUTH_CONTROLLER_H
#define TAM_HIDE_AUTH_CONTROLLER_H

#include "auth.h"

#include <stddef.h>

struct auth_controller_ctx {
	const char *socket_path;
	int session_vt;
};

/* Report compositor lock acquisition (ext_session_lock_v1.locked) to tam-hided. */
int auth_controller_confirm_lock(const char *socket_path, int vt_nr);

/* Verify a passcode against tam-hided's auth socket (AUTH <passcode>). */
void auth_controller_verify(const char *password, size_t len, void *ctx,
							struct auth_result *out);

/* Complete a multi-account/session PICK selection (SELECT <token> <index>). */
enum auth_outcome auth_controller_select(const char *token, int index,
										 void *ctx);

/* Cancel an in-flight PICK selection on tam-hided (CANCEL). */
void auth_controller_cancel(void *ctx);

/* wl_lock on_locked callback: sends CONFIRM_LOCK when session_vt > 0. */
void auth_controller_on_locked(void *ctx);

#endif
