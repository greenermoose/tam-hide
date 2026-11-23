/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_IPC_LINE 384
#define MAX_IPC_RESP 4096
#define PICK_TTL_MS 30000
#define MAX_SERVER_LOCKOUT_SEC 10

static int64_t ctrl_monotonic_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

const char *seat_state_name(enum seat_state st) {
	switch (st) {
	case SEAT_STATE_LOCKED:
		return "LOCKED";
	case SEAT_STATE_ACTIVE_USER:
		return "ACTIVE_USER";
	default:
		return "UNKNOWN";
	}
}

const char *session_kind_name(enum session_kind kind) {
	return (kind == SESSION_KIND_GRAPHICAL) ? "graphical" : "console";
}

static void wipe_pending_pick(struct seat_controller *c) {
	explicit_bzero(&c->pending_pick, sizeof(c->pending_pick));
}

static int bind_unix_socket(const char *path, mode_t mode, gid_t gid) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	if (!path || strlen(path) >= sizeof(addr.sun_path))
		return -EINVAL;

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	(void)unlink(path);

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	mode_t old_umask = umask(0177);
	int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	umask(old_umask);
	if (rc < 0) {
		int err = -errno;
		close(fd);
		return err;
	}

	if (gid != (gid_t)-1) {
		if (chown(path, (uid_t)-1, gid) < 0) {
			int err = -errno;
			close(fd);
			(void)unlink(path);
			return err;
		}
	}

	if (chmod(path, mode) < 0) {
		int err = -errno;
		close(fd);
		(void)unlink(path);
		return err;
	}

	if (listen(fd, 8) < 0) {
		int err = -errno;
		close(fd);
		(void)unlink(path);
		return err;
	}

	return fd;
}

static struct seat_session *find_session_by_vt(struct seat_controller *c,
											   int vt_nr) {
	for (size_t i = 0; i < TAM_HIDE_MAX_SESSIONS; i++) {
		if (c->sessions[i].active && c->sessions[i].vt_nr == vt_nr)
			return &c->sessions[i];
	}
	return NULL;
}

static bool active_graphical_unconfirmed(struct seat_controller *c) {
	if (c->state != SEAT_STATE_ACTIVE_USER || c->active_vt == c->lock_vt)
		return false;
	struct seat_session *s = find_session_by_vt(c, c->active_vt);
	if (!s)
		return false;
	return s->kind == SESSION_KIND_GRAPHICAL && !s->lock_confirmed;
}

static bool auth_peer_allowed(struct seat_controller *c, uid_t peer_uid) {
	if (peer_uid == c->allowed_uid)
		return true;
	struct seat_session *s = find_session_by_vt(c, c->active_vt);
	if (s && s->kind == SESSION_KIND_GRAPHICAL && s->lock_confirmed &&
		s->uid == peer_uid)
		return true;
	return false;
}

static int parse_vt_arg(const char *s, int *out_vt) {
	if (!s || *s == '\0')
		return -EINVAL;
	char *end = NULL;
	errno = 0;
	long val = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0')
		return -EINVAL;
	if (val < TAM_VT_MIN || val > TAM_VT_MAX)
		return -ERANGE;
	*out_vt = (int)val;
	return 0;
}

static int generate_pick_token(char out[TAM_HIDE_PICK_TOKEN_HEX_LEN + 1]) {
	static const char hex[] = "0123456789abcdef";
	uint8_t raw[16];
	size_t off = 0;
	while (off < sizeof(raw)) {
		ssize_t n = getrandom(raw + off, sizeof(raw) - off, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			explicit_bzero(raw, sizeof(raw));
			return -errno;
		}
		off += (size_t)n;
	}
	for (size_t i = 0; i < sizeof(raw); i++) {
		out[i * 2] = hex[(raw[i] >> 4) & 0x0f];
		out[i * 2 + 1] = hex[raw[i] & 0x0f];
	}
	out[TAM_HIDE_PICK_TOKEN_HEX_LEN] = '\0';
	explicit_bzero(raw, sizeof(raw));
	return 0;
}

static bool consttime_streq(const char *a, const char *b, size_t len) {
	unsigned char diff = 0;
	for (size_t i = 0; i < len; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int controller_init(struct seat_controller *c,
					const struct vt_ops *vt,
					int lock_vt,
					const char *socket_path,
					uid_t allowed_uid) {
	if (!c || !vt || !socket_path)
		return -EINVAL;
	if (lock_vt < TAM_VT_MIN || lock_vt > TAM_VT_MAX)
		return -EINVAL;

	memset(c, 0, sizeof(*c));
	c->vt = *vt;
	c->lock_vt = lock_vt;
	c->active_vt = -1;
	c->state = SEAT_STATE_LOCKED;
	c->vt_locked = false;
	c->allowed_uid = allowed_uid;
	c->listen_fd = -1;
	c->auth_fd = -1;
	passcode_store_init(&c->store);
	strncpy(c->socket_path, socket_path, sizeof(c->socket_path) - 1);

	/* Fail-closed first: engage VT_LOCKSWITCH before anything else. */
	int rc = c->vt.lock_switch(c->vt.ctx);
	if (rc < 0)
		return rc;
	c->vt_locked = true;

	/* Check current active VT; atomically move to lock_vt if needed. */
	int cur_vt = -1;
	rc = c->vt.get_active(c->vt.ctx, &cur_vt);
	if (rc < 0 || cur_vt != lock_vt) {
		rc = vt_atomic_handoff(&c->vt, lock_vt, NULL);
		if (rc < 0)
			return rc;
	}
	c->active_vt = lock_vt;
	c->state = SEAT_STATE_LOCKED;

	if (c->vt.render_banner)
		(void)c->vt.render_banner(c->vt.ctx, lock_vt, "controller ready");

	int fd = bind_unix_socket(socket_path, 0600, (gid_t)-1);
	if (fd < 0)
		return fd;
	c->listen_fd = fd;
	return 0;
}

int controller_enable_auth_socket(struct seat_controller *c,
								  const char *auth_socket_path,
								  mode_t mode,
								  gid_t gid) {
	if (!c || !auth_socket_path || auth_socket_path[0] == '\0')
		return -EINVAL;
	if (c->auth_fd >= 0) {
		close(c->auth_fd);
		c->auth_fd = -1;
	}
	if (c->auth_socket_path[0] != '\0') {
		(void)unlink(c->auth_socket_path);
		c->auth_socket_path[0] = '\0';
	}

	int fd = bind_unix_socket(auth_socket_path, mode, gid);
	if (fd < 0)
		return fd;
	c->auth_fd = fd;
	strncpy(c->auth_socket_path, auth_socket_path,
			sizeof(c->auth_socket_path) - 1);
	return 0;
}

int controller_load_store(struct seat_controller *c, const char *store_path) {
	if (!c || !store_path)
		return -EINVAL;
	return passcode_store_load(&c->store, store_path);
}

static int perform_handoff_to_vt(struct seat_controller *c, int target_vt,
								 double *elapsed_ms) {
	if (target_vt == c->active_vt && c->state == SEAT_STATE_ACTIVE_USER) {
		struct seat_session *s = find_session_by_vt(c, target_vt);
		if (s && s->kind == SESSION_KIND_GRAPHICAL)
			s->lock_confirmed = false;
		if (elapsed_ms)
			*elapsed_ms = 0.0;
		return 0;
	}
	if (active_graphical_unconfirmed(c))
		return -EAGAIN;

	int rc = vt_atomic_handoff(&c->vt, target_vt, elapsed_ms);
	if (rc < 0)
		return rc;

	c->active_vt = target_vt;
	c->state = SEAT_STATE_ACTIVE_USER;
	struct seat_session *dst = find_session_by_vt(c, target_vt);
	if (dst && dst->kind == SESSION_KIND_GRAPHICAL)
		dst->lock_confirmed = false;
	return 0;
}

static int handle_register_session(struct seat_controller *c, const char *args,
								   char *resp, size_t resp_size) {
	char vt_s[16], uid_s[32], user_s[TAM_HIDE_USER_MAX], kind_s[16], extra[4];
	int n = sscanf(args, "%15s %31s %63s %15s %3s", vt_s, uid_s, user_s, kind_s,
				   extra);
	if (n != 4) {
		snprintf(resp, resp_size, "ERR invalid_args\n");
		return -EINVAL;
	}

	int vt_nr = 0;
	int rc = parse_vt_arg(vt_s, &vt_nr);
	if (rc < 0 || vt_nr == c->lock_vt) {
		snprintf(resp, resp_size, "ERR invalid_vt\n");
		return -EINVAL;
	}

	char *end = NULL;
	errno = 0;
	unsigned long uid_v = strtoul(uid_s, &end, 10);
	if (errno != 0 || end == uid_s || *end != '\0' || uid_v > UINT_MAX) {
		snprintf(resp, resp_size, "ERR invalid_uid\n");
		return -EINVAL;
	}

	if (!passcode_username_valid(user_s)) {
		snprintf(resp, resp_size, "ERR invalid_user\n");
		return -EINVAL;
	}

	enum session_kind kind;
	if (strcmp(kind_s, "graphical") == 0) {
		kind = SESSION_KIND_GRAPHICAL;
	} else if (strcmp(kind_s, "console") == 0) {
		kind = SESSION_KIND_CONSOLE;
	} else {
		snprintf(resp, resp_size, "ERR invalid_kind\n");
		return -EINVAL;
	}

	struct seat_session *slot = find_session_by_vt(c, vt_nr);
	if (!slot) {
		for (size_t i = 0; i < TAM_HIDE_MAX_SESSIONS; i++) {
			if (!c->sessions[i].active) {
				slot = &c->sessions[i];
				break;
			}
		}
	}
	if (!slot) {
		snprintf(resp, resp_size, "ERR sessions_full\n");
		return -ENOSPC;
	}

	slot->active = true;
	slot->vt_nr = vt_nr;
	slot->uid = (uid_t)uid_v;
	snprintf(slot->username, sizeof(slot->username), "%s", user_s);
	slot->kind = kind;
	slot->lock_confirmed = (kind == SESSION_KIND_CONSOLE);

	snprintf(resp, resp_size,
			 "OK registered vt=%d uid=%u user=%s kind=%s\n",
			 slot->vt_nr, (unsigned)slot->uid, slot->username,
			 session_kind_name(slot->kind));
	return 0;
}

static int handle_unregister_session(struct seat_controller *c,
									 const char *args, char *resp,
									 size_t resp_size) {
	int vt_nr = 0;
	int rc = parse_vt_arg(args, &vt_nr);
	if (rc < 0) {
		snprintf(resp, resp_size, "ERR invalid_vt\n");
		return rc;
	}
	struct seat_session *s = find_session_by_vt(c, vt_nr);
	if (!s) {
		snprintf(resp, resp_size, "ERR unknown_session\n");
		return -ENOENT;
	}
	memset(s, 0, sizeof(*s));
	snprintf(resp, resp_size, "OK unregistered vt=%d\n", vt_nr);
	return 0;
}

static int handle_confirm_lock(struct seat_controller *c, uid_t peer_uid,
							   bool is_admin, const char *args, char *resp,
							   size_t resp_size) {
	int vt_nr = 0;
	int rc = parse_vt_arg(args, &vt_nr);
	if (rc < 0) {
		snprintf(resp, resp_size, "ERR invalid_vt\n");
		return rc;
	}
	struct seat_session *s = find_session_by_vt(c, vt_nr);
	if (!s) {
		snprintf(resp, resp_size, "ERR unknown_session\n");
		return -ENOENT;
	}
	if (!is_admin && peer_uid != c->allowed_uid && s->uid != peer_uid) {
		snprintf(resp, resp_size, "ERR permission_denied\n");
		return -EPERM;
	}
	s->lock_confirmed = true;
	snprintf(resp, resp_size, "OK lock_confirmed vt=%d\n", vt_nr);
	return 0;
}

static int handle_sessions_list(struct seat_controller *c, char *resp,
								size_t resp_size) {
	size_t count = 0;
	for (size_t i = 0; i < TAM_HIDE_MAX_SESSIONS; i++) {
		if (c->sessions[i].active)
			count++;
	}
	size_t off = 0;
	int n = snprintf(resp, resp_size, "OK count=%zu\n", count);
	if (n < 0 || (size_t)n >= resp_size)
		return -ENOSPC;
	off = (size_t)n;

	for (size_t i = 0; i < TAM_HIDE_MAX_SESSIONS; i++) {
		if (!c->sessions[i].active)
			continue;
		const struct seat_session *s = &c->sessions[i];
		n = snprintf(resp + off, resp_size - off,
					 "SESSION vt=%d uid=%u user=%s kind=%s locked=%d\n",
					 s->vt_nr, (unsigned)s->uid, s->username,
					 session_kind_name(s->kind), s->lock_confirmed ? 1 : 0);
		if (n < 0 || off + (size_t)n >= resp_size)
			return -ENOSPC;
		off += (size_t)n;
	}
	n = snprintf(resp + off, resp_size - off, "END\n");
	if (n < 0 || off + (size_t)n >= resp_size)
		return -ENOSPC;
	return 0;
}

static int handle_auth_attempt(struct seat_controller *c, uid_t peer_uid,
							   const char *pass, size_t pass_len, char *resp,
							   size_t resp_size) {
	if (!auth_peer_allowed(c, peer_uid)) {
		snprintf(resp, resp_size, "ERR permission_denied\n");
		return -EPERM;
	}

	int64_t now_ms = ctrl_monotonic_ms();
	if (c->auth_lockout_until_ms > now_ms) {
		int rem = (int)((c->auth_lockout_until_ms - now_ms + 999) / 1000);
		snprintf(resp, resp_size, "ERR lockout sec=%d\n", rem);
		return -EAGAIN;
	}

	if (pass_len == 0) {
		snprintf(resp, resp_size, "ERR denied\n");
		return -EACCES;
	}

	struct passcode_match matches[TAM_HIDE_MAX_ACCOUNTS];
	size_t nmatch = passcode_store_match(&c->store, pass, pass_len, matches,
										 TAM_HIDE_MAX_ACCOUNTS);
	if (nmatch == 0) {
		wipe_pending_pick(c);
		c->auth_fail_count++;
		if (c->auth_fail_count >= 3) {
			unsigned int shift = c->auth_fail_count - 3;
			int64_t delay_sec = (shift >= 4) ? MAX_SERVER_LOCKOUT_SEC
											 : ((int64_t)1 << shift);
			if (delay_sec > MAX_SERVER_LOCKOUT_SEC)
				delay_sec = MAX_SERVER_LOCKOUT_SEC;
			c->auth_lockout_until_ms = now_ms + delay_sec * 1000;
			snprintf(resp, resp_size, "ERR lockout sec=%d\n", (int)delay_sec);
			return -EAGAIN;
		}
		snprintf(resp, resp_size, "ERR denied\n");
		return -EACCES;
	}

	/* Build candidate list across matched accounts and their sessions. */
	struct seat_pick_candidate cands[TAM_HIDE_MAX_PICK_CANDIDATES];
	size_t ncand = 0;
	for (size_t m = 0; m < nmatch && ncand < TAM_HIDE_MAX_PICK_CANDIDATES; m++) {
		bool had_session = false;
		for (size_t s = 0;
			 s < TAM_HIDE_MAX_SESSIONS && ncand < TAM_HIDE_MAX_PICK_CANDIDATES;
			 s++) {
			if (c->sessions[s].active && c->sessions[s].uid == matches[m].uid) {
				struct seat_pick_candidate *dst = &cands[ncand];
				dst->index = (int)ncand;
				dst->uid = c->sessions[s].uid;
				dst->vt_nr = c->sessions[s].vt_nr;
				snprintf(dst->username, sizeof(dst->username), "%s",
						 matches[m].username);
				dst->kind = c->sessions[s].kind;
				ncand++;
				had_session = true;
			}
		}
		if (!had_session && ncand < TAM_HIDE_MAX_PICK_CANDIDATES) {
			struct seat_pick_candidate *dst = &cands[ncand];
			dst->index = (int)ncand;
			dst->uid = matches[m].uid;
			dst->vt_nr = 0;
			snprintf(dst->username, sizeof(dst->username), "%s",
					 matches[m].username);
			dst->kind = SESSION_KIND_CONSOLE;
			ncand++;
		}
	}

	c->auth_fail_count = 0;
	c->auth_lockout_until_ms = 0;

	if (ncand == 1) {
		wipe_pending_pick(c);
		if (cands[0].vt_nr >= TAM_VT_MIN) {
			double ms = 0.0;
			int rc = perform_handoff_to_vt(c, cands[0].vt_nr, &ms);
			if (rc == -EAGAIN) {
				snprintf(resp, resp_size, "ERR lock_unconfirmed\n");
				return rc;
			}
			if (rc < 0) {
				snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
				return rc;
			}
		}
		snprintf(resp, resp_size, "OK user=%s uid=%u vt=%d\n",
				 cands[0].username, (unsigned)cands[0].uid, cands[0].vt_nr);
		return 0;
	}

	/* Multiple accounts or sessions matched: issue a single-use PICK token. */
	wipe_pending_pick(c);
	if (generate_pick_token(c->pending_pick.token) < 0) {
		snprintf(resp, resp_size, "ERR internal\n");
		return -EIO;
	}
	c->pending_pick.active = true;
	c->pending_pick.owner_uid = peer_uid;
	c->pending_pick.expires_ms = now_ms + PICK_TTL_MS;
	c->pending_pick.count = ncand;
	memcpy(c->pending_pick.items, cands, ncand * sizeof(*cands));

	size_t off = 0;
	int w = snprintf(resp, resp_size, "PICK token=%s count=%zu\n",
					 c->pending_pick.token, ncand);
	if (w < 0 || (size_t)w >= resp_size) {
		wipe_pending_pick(c);
		snprintf(resp, resp_size, "ERR resp_overflow\n");
		return -ENOSPC;
	}
	off = (size_t)w;
	for (size_t i = 0; i < ncand; i++) {
		w = snprintf(resp + off, resp_size - off,
					 "ITEM %d user=%s uid=%u vt=%d kind=%s\n",
					 cands[i].index, cands[i].username,
					 (unsigned)cands[i].uid, cands[i].vt_nr,
					 session_kind_name(cands[i].kind));
		if (w < 0 || off + (size_t)w >= resp_size) {
			wipe_pending_pick(c);
			snprintf(resp, resp_size, "ERR resp_overflow\n");
			return -ENOSPC;
		}
		off += (size_t)w;
	}
	w = snprintf(resp + off, resp_size - off, "END\n");
	if (w < 0 || off + (size_t)w >= resp_size) {
		wipe_pending_pick(c);
		snprintf(resp, resp_size, "ERR resp_overflow\n");
		return -ENOSPC;
	}
	return 0;
}

static int handle_select(struct seat_controller *c, uid_t peer_uid,
						 const char *args, char *resp, size_t resp_size) {
	if (!auth_peer_allowed(c, peer_uid)) {
		snprintf(resp, resp_size, "ERR permission_denied\n");
		return -EPERM;
	}

	char tok[64], idx_s[16], extra[4];
	if (sscanf(args, "%63s %15s %3s", tok, idx_s, extra) != 2) {
		snprintf(resp, resp_size, "ERR invalid_args\n");
		return -EINVAL;
	}

	int64_t now_ms = ctrl_monotonic_ms();
	if (!c->pending_pick.active || now_ms > c->pending_pick.expires_ms) {
		wipe_pending_pick(c);
		snprintf(resp, resp_size, "ERR pick_expired\n");
		return -ENOENT;
	}
	if (c->pending_pick.owner_uid != peer_uid) {
		snprintf(resp, resp_size, "ERR permission_denied\n");
		return -EPERM;
	}
	if (strlen(tok) != TAM_HIDE_PICK_TOKEN_HEX_LEN ||
		!consttime_streq(tok, c->pending_pick.token,
						 TAM_HIDE_PICK_TOKEN_HEX_LEN)) {
		snprintf(resp, resp_size, "ERR invalid_token\n");
		return -EACCES;
	}

	char *end = NULL;
	errno = 0;
	long idx = strtol(idx_s, &end, 10);
	if (errno != 0 || end == idx_s || *end != '\0' || idx < 0 ||
		(size_t)idx >= c->pending_pick.count) {
		snprintf(resp, resp_size, "ERR invalid_index\n");
		return -EINVAL;
	}

	struct seat_pick_candidate chosen = c->pending_pick.items[idx];
	/* Single-use token: consume and wipe immediately before handoff. */
	wipe_pending_pick(c);

	if (chosen.vt_nr >= TAM_VT_MIN) {
		double ms = 0.0;
		int rc = perform_handoff_to_vt(c, chosen.vt_nr, &ms);
		if (rc == -EAGAIN) {
			snprintf(resp, resp_size, "ERR lock_unconfirmed\n");
			return rc;
		}
		if (rc < 0) {
			snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
			return rc;
		}
	}

	snprintf(resp, resp_size, "OK user=%s uid=%u vt=%d\n",
			 chosen.username, (unsigned)chosen.uid, chosen.vt_nr);
	return 0;
}

int controller_dispatch_auth(struct seat_controller *c,
							 uid_t peer_uid,
							 const char *line,
							 char *resp,
							 size_t resp_size) {
	if (!c || !line || !resp || resp_size == 0)
		return -EINVAL;

	char buf[MAX_IPC_LINE];
	size_t len = strnlen(line, sizeof(buf));
	if (len >= sizeof(buf)) {
		snprintf(resp, resp_size, "ERR line_too_long\n");
		return -E2BIG;
	}
	memcpy(buf, line, len);
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	int rc = -EINVAL;
	if (strncmp(buf, "CONFIRM_LOCK ", 13) == 0) {
		while (len > 13 && buf[len - 1] == ' ')
			buf[--len] = '\0';
		rc = handle_confirm_lock(c, peer_uid, false, buf + 13, resp, resp_size);
	} else if (strncmp(buf, "AUTH ", 5) == 0) {
		rc = handle_auth_attempt(c, peer_uid, buf + 5, len - 5, resp,
								 resp_size);
	} else if (strncmp(buf, "SELECT ", 7) == 0) {
		while (len > 7 && buf[len - 1] == ' ')
			buf[--len] = '\0';
		rc = handle_select(c, peer_uid, buf + 7, resp, resp_size);
	} else if (strcmp(buf, "CANCEL") == 0) {
		if (!auth_peer_allowed(c, peer_uid)) {
			snprintf(resp, resp_size, "ERR permission_denied\n");
			rc = -EPERM;
		} else {
			if (c->pending_pick.active &&
				(c->pending_pick.owner_uid == peer_uid ||
				 peer_uid == c->allowed_uid))
				wipe_pending_pick(c);
			snprintf(resp, resp_size, "OK cancelled\n");
			rc = 0;
		}
	} else {
		snprintf(resp, resp_size, "ERR unknown_command\n");
		rc = -EINVAL;
	}

	explicit_bzero(buf, sizeof(buf));
	return rc;
}

int controller_dispatch(struct seat_controller *c,
						const char *line,
						char *resp,
						size_t resp_size) {
	if (!c || !line || !resp || resp_size == 0)
		return -EINVAL;

	char buf[MAX_IPC_LINE];
	size_t len = strnlen(line, sizeof(buf));
	if (len >= sizeof(buf)) {
		snprintf(resp, resp_size, "ERR line_too_long\n");
		return -E2BIG;
	}
	memcpy(buf, line, len);
	buf[len] = '\0';

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
					   buf[len - 1] == ' '))
		buf[--len] = '\0';

	int rc = 0;
	if (strcmp(buf, "PING") == 0) {
		snprintf(resp, resp_size, "OK pong\n");
	} else if (strcmp(buf, "STATUS") == 0) {
		int live_vt = c->active_vt;
		if (c->vt.get_active(c->vt.ctx, &live_vt) == 0)
			c->active_vt = live_vt;
		snprintf(resp, resp_size,
				 "OK state=%s lock_vt=%d active_vt=%d vt_locked=%d\n",
				 seat_state_name(c->state), c->lock_vt, c->active_vt,
				 c->vt_locked ? 1 : 0);
	} else if (strcmp(buf, "LOCK") == 0) {
		if (active_graphical_unconfirmed(c)) {
			snprintf(resp, resp_size, "ERR lock_unconfirmed\n");
			rc = -EAGAIN;
		} else {
			double ms = 0.0;
			rc = vt_atomic_handoff(&c->vt, c->lock_vt, &ms);
			if (rc < 0) {
				snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
			} else {
				c->active_vt = c->lock_vt;
				c->state = SEAT_STATE_LOCKED;
				if (c->vt.render_banner)
					(void)c->vt.render_banner(c->vt.ctx, c->lock_vt, "locked");
				snprintf(resp, resp_size,
						 "OK state=%s active_vt=%d vt_locked=1 handoff_ms=%.2f\n",
						 seat_state_name(c->state), c->active_vt, ms);
			}
		}
	} else if (strncmp(buf, "HANDOFF ", 8) == 0) {
		int target_vt = 0;
		rc = parse_vt_arg(buf + 8, &target_vt);
		if (rc < 0) {
			snprintf(resp, resp_size, "ERR invalid_vt\n");
		} else if (target_vt == c->lock_vt) {
			snprintf(resp, resp_size, "ERR target_is_lock_vt\n");
			rc = -EINVAL;
		} else {
			double ms = 0.0;
			rc = perform_handoff_to_vt(c, target_vt, &ms);
			if (rc == -EAGAIN) {
				snprintf(resp, resp_size, "ERR lock_unconfirmed\n");
			} else if (rc < 0) {
				snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
			} else {
				snprintf(resp, resp_size,
						 "OK state=%s active_vt=%d vt_locked=1 handoff_ms=%.2f\n",
						 seat_state_name(c->state), c->active_vt, ms);
			}
		}
	} else if (strncmp(buf, "REGISTER_SESSION ", 17) == 0) {
		rc = handle_register_session(c, buf + 17, resp, resp_size);
	} else if (strncmp(buf, "UNREGISTER_SESSION ", 19) == 0) {
		rc = handle_unregister_session(c, buf + 19, resp, resp_size);
	} else if (strncmp(buf, "CONFIRM_LOCK ", 13) == 0) {
		rc = handle_confirm_lock(c, c->allowed_uid, true, buf + 13, resp,
								 resp_size);
	} else if (strcmp(buf, "SESSIONS") == 0) {
		rc = handle_sessions_list(c, resp, resp_size);
	} else {
		snprintf(resp, resp_size, "ERR unknown_command\n");
		rc = -EINVAL;
	}

	explicit_bzero(buf, sizeof(buf));
	return rc;
}

static ssize_t read_bounded_line(int fd, char *buf, size_t max_len) {
	size_t pos = 0;
	while (pos + 1 < max_len) {
		char ch = 0;
		ssize_t n = read(fd, &ch, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			break;
		buf[pos++] = ch;
		if (ch == '\n')
			break;
	}
	buf[pos] = '\0';
	return (ssize_t)pos;
}

static int handle_socket_one(struct seat_controller *c, int listen_fd,
							 bool is_auth) {
	if (!c || listen_fd < 0)
		return -EINVAL;

	int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (client_fd < 0)
		return -errno;

	struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
	(void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct ucred cred;
	socklen_t cred_len = sizeof(cred);
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0) {
		const char *err = "ERR peercred_failed\n";
		(void)write(client_fd, err, strlen(err));
		close(client_fd);
		return -EACCES;
	}

	if (!is_auth && cred.uid != c->allowed_uid) {
		const char *err = "ERR permission_denied\n";
		(void)write(client_fd, err, strlen(err));
		close(client_fd);
		return -EPERM;
	}

	char req[MAX_IPC_LINE];
	ssize_t n = read_bounded_line(client_fd, req, sizeof(req));
	if (n <= 0) {
		explicit_bzero(req, sizeof(req));
		close(client_fd);
		return (n < 0) ? (int)n : 0;
	}

	char resp[MAX_IPC_RESP];
	if (is_auth)
		(void)controller_dispatch_auth(c, cred.uid, req, resp, sizeof(resp));
	else
		(void)controller_dispatch(c, req, resp, sizeof(resp));

	explicit_bzero(req, sizeof(req));
	(void)write(client_fd, resp, strlen(resp));
	explicit_bzero(resp, sizeof(resp));
	close(client_fd);
	return 0;
}

int controller_handle_one(struct seat_controller *c) {
	return handle_socket_one(c, c ? c->listen_fd : -1, false);
}

int controller_handle_auth_one(struct seat_controller *c) {
	return handle_socket_one(c, c ? c->auth_fd : -1, true);
}

void controller_cleanup(struct seat_controller *c, bool release_vt_lock) {
	if (!c)
		return;
	wipe_pending_pick(c);
	passcode_store_destroy(&c->store);
	if (c->listen_fd >= 0) {
		close(c->listen_fd);
		c->listen_fd = -1;
	}
	if (c->socket_path[0] != '\0')
		(void)unlink(c->socket_path);
	if (c->auth_fd >= 0) {
		close(c->auth_fd);
		c->auth_fd = -1;
	}
	if (c->auth_socket_path[0] != '\0')
		(void)unlink(c->auth_socket_path);
	if (release_vt_lock && c->vt.unlock_switch) {
		(void)c->vt.unlock_switch(c->vt.ctx);
		c->vt_locked = false;
	}
}
