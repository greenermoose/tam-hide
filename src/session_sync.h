/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_SESSION_SYNC_H
#define TAM_HIDE_SESSION_SYNC_H

#include "controller.h"

#include <stdbool.h>

struct session_watcher {
	void *monitor;
	int fd;
	bool active;
};

/*
 * Initialize live seat0 session watcher (systemd-logind sd-login monitor when
 * compiled with TAM_HIDE_HAVE_SYSTEMD=1). Returns 0 on success, -ENOTSUP when
 * built without libsystemd, or negative errno on failure.
 */
int session_watcher_init(struct session_watcher *w);

/* File descriptor suitable for poll(POLLIN), or -1 if inactive. */
int session_watcher_fd(const struct session_watcher *w);

/* Flush pending monitor events and reconcile seat0 sessions into controller. */
int session_watcher_sync(struct session_watcher *w, struct seat_controller *c);

/* Release monitor resources. */
void session_watcher_destroy(struct session_watcher *w);

#endif
