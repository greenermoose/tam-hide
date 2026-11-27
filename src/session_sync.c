/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TAM_HIDE_HAVE_SYSTEMD
#include <pwd.h>
#include <systemd/sd-login.h>

int session_watcher_init(struct session_watcher *w) {
	if (!w)
		return -EINVAL;
	memset(w, 0, sizeof(*w));
	w->fd = -1;

	sd_login_monitor *mon = NULL;
	int rc = sd_login_monitor_new("session", &mon);
	if (rc < 0)
		return rc;

	int fd = sd_login_monitor_get_fd(mon);
	if (fd < 0) {
		sd_login_monitor_unref(mon);
		return fd;
	}

	w->monitor = mon;
	w->fd = fd;
	w->active = true;
	return 0;
}

int session_watcher_fd(const struct session_watcher *w) {
	if (!w || !w->active)
		return -1;
	return w->fd;
}

static bool resolve_username(uid_t uid, char out[TAM_HIDE_USER_MAX]) {
	struct passwd pw;
	struct passwd *res = NULL;
	char buf[1024];
	if (getpwuid_r(uid, &pw, buf, sizeof(buf), &res) == 0 && res &&
		res->pw_name && passcode_username_valid(res->pw_name)) {
		snprintf(out, TAM_HIDE_USER_MAX, "%s", res->pw_name);
		return true;
	}
	snprintf(out, TAM_HIDE_USER_MAX, "uid%u", (unsigned)uid);
	return passcode_username_valid(out);
}

int session_watcher_sync(struct session_watcher *w, struct seat_controller *c) {
	if (!w || !w->active || !c)
		return -EINVAL;

	if (w->monitor)
		(void)sd_login_monitor_flush((sd_login_monitor *)w->monitor);

	char **sessions = NULL;
	int n = sd_seat_get_sessions("seat0", &sessions, NULL, NULL);
	if (n < 0)
		return n;

	struct session_snapshot_entry entries[TAM_HIDE_MAX_SESSIONS];
	size_t count = 0;

	for (int i = 0; i < n && sessions && sessions[i]; i++) {
		const char *sid = sessions[i];

		char *state = NULL;
		if (sd_session_get_state(sid, &state) >= 0 && state) {
			bool closing = (strcmp(state, "closing") == 0);
			free(state);
			if (closing)
				continue;
		}

		unsigned int vtnr = 0;
		if (sd_session_get_vt(sid, &vtnr) < 0 ||
			vtnr < (unsigned int)TAM_VT_MIN ||
			vtnr > (unsigned int)TAM_VT_MAX ||
			(int)vtnr == c->lock_vt) {
			continue;
		}

		uid_t uid = 0;
		if (sd_session_get_uid(sid, &uid) < 0)
			continue;

		char *type = NULL;
		if (sd_session_get_type(sid, &type) < 0 || !type)
			continue;

		enum session_kind kind;
		if (strcmp(type, "wayland") == 0 || strcmp(type, "x11") == 0) {
			kind = SESSION_KIND_GRAPHICAL;
		} else if (strcmp(type, "tty") == 0) {
			kind = SESSION_KIND_CONSOLE;
		} else {
			free(type);
			continue;
		}
		free(type);

		char username[TAM_HIDE_USER_MAX];
		if (!resolve_username(uid, username))
			continue;

		/* Deduplicate by VT, preferring graphical if both exist on a VT. */
		int dup_idx = -1;
		for (size_t k = 0; k < count; k++) {
			if (entries[k].vt_nr == (int)vtnr) {
				dup_idx = (int)k;
				break;
			}
		}
		if (dup_idx >= 0) {
			if (entries[dup_idx].kind == SESSION_KIND_CONSOLE &&
				kind == SESSION_KIND_GRAPHICAL) {
				entries[dup_idx].uid = uid;
				entries[dup_idx].kind = kind;
				snprintf(entries[dup_idx].username,
						 sizeof(entries[dup_idx].username), "%s", username);
			}
			continue;
		}

		if (count < TAM_HIDE_MAX_SESSIONS) {
			entries[count].vt_nr = (int)vtnr;
			entries[count].uid = uid;
			entries[count].kind = kind;
			snprintf(entries[count].username, sizeof(entries[count].username),
					 "%s", username);
			count++;
		}
	}

	if (sessions) {
		for (int i = 0; i < n; i++)
			free(sessions[i]);
		free(sessions);
	}

	return controller_reconcile_sessions(c, entries, count);
}

void session_watcher_destroy(struct session_watcher *w) {
	if (!w)
		return;
	if (w->monitor) {
		sd_login_monitor_unref((sd_login_monitor *)w->monitor);
		w->monitor = NULL;
	}
	w->fd = -1;
	w->active = false;
}

#else

int session_watcher_init(struct session_watcher *w) {
	if (!w)
		return -EINVAL;
	memset(w, 0, sizeof(*w));
	w->fd = -1;
	return -ENOTSUP;
}

int session_watcher_fd(const struct session_watcher *w) {
	(void)w;
	return -1;
}

int session_watcher_sync(struct session_watcher *w, struct seat_controller *c) {
	(void)w;
	(void)c;
	return -ENOTSUP;
}

void session_watcher_destroy(struct session_watcher *w) {
	if (!w)
		return;
	w->fd = -1;
	w->active = false;
}

#endif
