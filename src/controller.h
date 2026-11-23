/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_CONTROLLER_H
#define TAM_HIDE_CONTROLLER_H

#include "passcode_store.h"
#include "vt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TAM_HIDE_MAX_SESSIONS 16
#define TAM_HIDE_MAX_PICK_CANDIDATES 16
#define TAM_HIDE_PICK_TOKEN_HEX_LEN 32

enum seat_state {
	SEAT_STATE_LOCKED = 0,
	SEAT_STATE_ACTIVE_USER = 1,
};

enum session_kind {
	SESSION_KIND_CONSOLE = 0,
	SESSION_KIND_GRAPHICAL = 1,
};

struct seat_session {
	bool active;
	int vt_nr;
	uid_t uid;
	char username[TAM_HIDE_USER_MAX];
	enum session_kind kind;
	bool lock_confirmed;
};

struct seat_pick_candidate {
	int index;
	uid_t uid;
	int vt_nr;
	char username[TAM_HIDE_USER_MAX];
	enum session_kind kind;
};

struct pending_pick {
	bool active;
	uid_t owner_uid;
	int64_t expires_ms;
	char token[TAM_HIDE_PICK_TOKEN_HEX_LEN + 1];
	struct seat_pick_candidate items[TAM_HIDE_MAX_PICK_CANDIDATES];
	size_t count;
};

struct seat_controller {
	struct vt_ops vt;
	int lock_vt;
	int active_vt;
	enum seat_state state;
	bool vt_locked;
	uid_t allowed_uid;
	int listen_fd;
	char socket_path[108];

	/* Optional privilege-separated auth socket for tam-hide locker instances. */
	int auth_fd;
	char auth_socket_path[108];

	/* Registered sessions & compositor lock-confirmation tracking. */
	struct seat_session sessions[TAM_HIDE_MAX_SESSIONS];

	/* Privileged Argon2id passcode store and server-side backoff state. */
	struct passcode_store store;
	unsigned int auth_fail_count;
	int64_t auth_lockout_until_ms;
	struct pending_pick pending_pick;
};

/*
 * Initialize controller, immediately engage VT_LOCKSWITCH, atomically
 * switch to lock_vt (recovering from any prior crash/user VT), render the
 * non-disclosing banner, and bind the root-only Unix domain control socket.
 */
int controller_init(struct seat_controller *c,
					const struct vt_ops *vt,
					int lock_vt,
					const char *socket_path,
					uid_t allowed_uid);

/*
 * Bind the optional privilege-separated auth socket (CONFIRM_LOCK, AUTH,
 * SELECT, CANCEL only, gated by SO_PEERCRED). Pass gid == (gid_t)-1 to keep
 * default group.
 */
int controller_enable_auth_socket(struct seat_controller *c,
								  const char *auth_socket_path,
								  mode_t mode,
								  gid_t gid);

/* Load Argon2id passcode store from path into controller. */
int controller_load_store(struct seat_controller *c, const char *store_path);

/* Handle one pending connection on listen_fd (root-only control socket). */
int controller_handle_one(struct seat_controller *c);

/* Handle one pending connection on auth_fd (privilege-separated auth socket). */
int controller_handle_auth_one(struct seat_controller *c);

/* Process a single admin command line directly (control.sock semantics). */
int controller_dispatch(struct seat_controller *c,
						const char *line,
						char *resp,
						size_t resp_size);

/* Process a single auth-socket command line for peer_uid. */
int controller_dispatch_auth(struct seat_controller *c,
							 uid_t peer_uid,
							 const char *line,
							 char *resp,
							 size_t resp_size);

/* Close sockets, wipe secrets, and optionally release VT_LOCKSWITCH. */
void controller_cleanup(struct seat_controller *c, bool release_vt_lock);

const char *seat_state_name(enum seat_state st);
const char *session_kind_name(enum session_kind kind);

#endif
