/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth_controller.h"
#include "controller.h"

#include <assert.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct mock_vt {
	int active_vt;
	bool locked;
	int lock_calls;
	int unlock_calls;
	int activate_calls;
	int banner_calls;
	int fail_activate_errno;
};

static int m_get_active(void *v, int *out) {
	struct mock_vt *m = v;
	*out = m->active_vt;
	return 0;
}

static int m_lock(void *v) {
	struct mock_vt *m = v;
	m->locked = true;
	m->lock_calls++;
	return 0;
}

static int m_unlock(void *v) {
	struct mock_vt *m = v;
	m->locked = false;
	m->unlock_calls++;
	return 0;
}

static int m_activate(void *v, int vt_nr) {
	struct mock_vt *m = v;
	m->activate_calls++;
	if (m->fail_activate_errno != 0)
		return -m->fail_activate_errno;
	/* Kernel only permits switching while unlocked. */
	if (m->locked)
		return -EPERM;
	m->active_vt = vt_nr;
	return 0;
}

static int m_banner(void *v, int lock_vt, const char *status) {
	(void)lock_vt;
	(void)status;
	struct mock_vt *m = v;
	m->banner_calls++;
	return 0;
}

static struct vt_ops make_mock_ops(struct mock_vt *m) {
	struct vt_ops ops = {
		.ctx = m,
		.get_active = m_get_active,
		.lock_switch = m_lock,
		.unlock_switch = m_unlock,
		.activate_and_wait = m_activate,
		.render_banner = m_banner,
	};
	return ops;
}

static void send_ipc(const char *sock, const char *req, char *out, size_t out_sz) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
	assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(write(fd, req, strlen(req)) == (ssize_t)strlen(req));
	size_t total = 0;
	while (total + 1 < out_sz) {
		ssize_t n = read(fd, out + total, out_sz - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	assert(total > 0);
	out[total] = '\0';
	close(fd);
}

static void test_startup_recovery_and_ipc(void) {
	struct mock_vt m = { .active_vt = 3, .locked = false };
	struct vt_ops ops = make_mock_ops(&m);
	char sock[64];
	snprintf(sock, sizeof(sock), "/tmp/tam-hide-test-%d.sock", (int)getpid());

	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, sock, getuid()) == 0);
	/* Must engage lock, move from tty3 to lock_vt=6, and stay locked. */
	assert(m.locked == true);
	assert(m.active_vt == 6);
	assert(c.state == SEAT_STATE_LOCKED);
	assert(m.banner_calls == 1);

	/* Fork a client to test real Unix socket + SO_PEERCRED. */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		char resp[512];
		send_ipc(sock, "STATUS\n", resp, sizeof(resp));
		assert(strstr(resp, "OK state=LOCKED lock_vt=6 active_vt=6 vt_locked=1") != NULL);

		send_ipc(sock, "HANDOFF 3\n", resp, sizeof(resp));
		assert(strstr(resp, "OK state=ACTIVE_USER active_vt=3 vt_locked=1") != NULL);

		send_ipc(sock, "HANDOFF 6\n", resp, sizeof(resp));
		assert(strstr(resp, "ERR target_is_lock_vt") != NULL);

		send_ipc(sock, "HANDOFF 99\n", resp, sizeof(resp));
		assert(strstr(resp, "ERR invalid_vt") != NULL);

		send_ipc(sock, "LOCK\n", resp, sizeof(resp));
		assert(strstr(resp, "OK state=LOCKED active_vt=6 vt_locked=1") != NULL);
		_exit(0);
	}

	for (int i = 0; i < 5; i++)
		assert(controller_handle_one(&c) == 0);
	int status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	assert(m.locked == true);
	assert(m.active_vt == 6);
	assert(c.state == SEAT_STATE_LOCKED);

	/* Cleanup without release_vt_lock keeps VT_LOCKSWITCH engaged (fail-closed). */
	controller_cleanup(&c, false);
	assert(m.locked == true);
}

static void test_fail_closed_on_activate_error(void) {
	struct mock_vt m = { .active_vt = 6, .locked = false };
	struct vt_ops ops = make_mock_ops(&m);
	char sock[64];
	snprintf(sock, sizeof(sock), "/tmp/tam-hide-test-err-%d.sock", (int)getpid());

	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, sock, getuid()) == 0);
	assert(m.locked == true);

	m.fail_activate_errno = EIO;
	char resp[256];
	int rc = controller_dispatch(&c, "HANDOFF 3\n", resp, sizeof(resp));
	assert(rc == -EIO);
	assert(strstr(resp, "ERR handoff_failed") != NULL);
	/* Critical invariant: VT_LOCKSWITCH MUST be re-engaged on error! */
	assert(m.locked == true);
	assert(m.active_vt == 6);
	assert(c.state == SEAT_STATE_LOCKED);

	controller_cleanup(&c, true);
	assert(m.locked == false);
}

static void test_lock_confirmation_gate(void) {
	struct mock_vt m = { .active_vt = 6, .locked = false };
	struct vt_ops ops = make_mock_ops(&m);
	char sock[64];
	snprintf(sock, sizeof(sock), "/tmp/tam-hide-test-gate-%d.sock", (int)getpid());

	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, sock, 0) == 0);

	char resp[1024];
	assert(controller_dispatch(&c, "REGISTER_SESSION 2 1000 alice graphical\n",
							   resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK registered vt=2") != NULL);
	assert(controller_dispatch(&c, "REGISTER_SESSION 3 1001 bob console\n",
							   resp, sizeof(resp)) == 0);

	/* Handoff to graphical tty2 resets lock_confirmed = false. */
	assert(controller_dispatch(&c, "HANDOFF 2\n", resp, sizeof(resp)) == 0);
	assert(m.active_vt == 2);

	/* Leaving unconfirmed graphical session MUST fail closed! */
	assert(controller_dispatch(&c, "LOCK\n", resp, sizeof(resp)) == -EAGAIN);
	assert(strstr(resp, "ERR lock_unconfirmed") != NULL);
	assert(m.active_vt == 2);

	assert(controller_dispatch(&c, "HANDOFF 3\n", resp, sizeof(resp)) == -EAGAIN);
	assert(strstr(resp, "ERR lock_unconfirmed") != NULL);
	assert(m.active_vt == 2);

	/* Different UID cannot confirm lock for tty2. */
	assert(controller_dispatch_auth(&c, 1001, "CONFIRM_LOCK 2\n",
									resp, sizeof(resp)) == -EPERM);

	/* Owner UID confirms lock after ext_session_lock_v1.locked. */
	assert(controller_dispatch_auth(&c, 1000, "CONFIRM_LOCK 2\n",
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK lock_confirmed vt=2") != NULL);

	/* Now LOCK succeeds atomically. */
	assert(controller_dispatch(&c, "LOCK\n", resp, sizeof(resp)) == 0);
	assert(m.active_vt == 6);
	assert(c.state == SEAT_STATE_LOCKED);

	/* SESSIONS lists both registered sessions. */
	assert(controller_dispatch(&c, "SESSIONS\n", resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK count=2") != NULL);
	assert(strstr(resp, "SESSION vt=2 uid=1000 user=alice kind=graphical") != NULL);

	controller_cleanup(&c, true);
}

static void test_auth_socket_and_multi_account_picker(void) {
	struct mock_vt m = { .active_vt = 6, .locked = false };
	struct vt_ops ops = make_mock_ops(&m);
	char ctrl_sock[64], auth_sock[64];
	snprintf(ctrl_sock, sizeof(ctrl_sock), "/tmp/tam-hide-ctrl-%d.sock", (int)getpid());
	snprintf(auth_sock, sizeof(auth_sock), "/tmp/tam-hide-auth-%d.sock", (int)getpid());

	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, ctrl_sock, getuid()) == 0);
	assert(controller_enable_auth_socket(&c, auth_sock, 0600, (gid_t)-1) == 0);

	struct passcode_params fast = { .t_cost = 1, .m_cost = 1024, .parallelism = 1 };
	assert(passcode_store_set(&c.store, 1000, "alice", "shared", 6, &fast) == 0);
	assert(passcode_store_set(&c.store, 1001, "bob", "shared", 6, &fast) == 0);
	assert(passcode_store_set(&c.store, 1002, "carol", "carolpass", 9, &fast) == 0);

	char resp[2048];
	assert(controller_dispatch(&c, "REGISTER_SESSION 2 1000 alice graphical\n",
							   resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "REGISTER_SESSION 3 1001 bob console\n",
							   resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "REGISTER_SESSION 4 1002 carol console\n",
							   resp, sizeof(resp)) == 0);

	/* auth.sock must reject admin commands. */
	assert(controller_dispatch_auth(&c, getuid(), "HANDOFF 2\n",
									resp, sizeof(resp)) == -EINVAL);
	assert(strstr(resp, "ERR unknown_command") != NULL);

	/* Unrelated UID is denied AUTH while seat is on lock_vt. */
	uid_t stranger = getuid() + 9999;
	assert(controller_dispatch_auth(&c, stranger, "AUTH carolpass\n",
									resp, sizeof(resp)) == -EPERM);

	/* Single-match AUTH immediately hands off to carol's tty4. */
	assert(controller_dispatch_auth(&c, getuid(), "AUTH carolpass\n",
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK user=carol uid=1002 vt=4") != NULL);
	assert(m.active_vt == 4);
	assert(c.state == SEAT_STATE_ACTIVE_USER);

	/* Duplicate-passcode AUTH returns PICK with single-use 32-hex token. */
	assert(controller_dispatch_auth(&c, getuid(), "AUTH shared\n",
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "PICK token=") != NULL);
	assert(strstr(resp, "count=2") != NULL);
	assert(strstr(resp, "ITEM 0 user=alice uid=1000 vt=2 kind=graphical") != NULL);
	assert(strstr(resp, "ITEM 1 user=bob uid=1001 vt=3 kind=console") != NULL);

	char token[40] = {0};
	assert(sscanf(resp, "PICK token=%39s", token) == 1);
	assert(strlen(token) == TAM_HIDE_PICK_TOKEN_HEX_LEN);

	/* Wrong token is rejected. */
	assert(controller_dispatch_auth(&c, getuid(),
									"SELECT 00000000000000000000000000000000 1\n",
									resp, sizeof(resp)) == -EACCES);

	/* Valid SELECT consumes token and hands off to bob's tty3. */
	char sel_cmd[128];
	snprintf(sel_cmd, sizeof(sel_cmd), "SELECT %s 1\n", token);
	assert(controller_dispatch_auth(&c, getuid(), sel_cmd,
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK user=bob uid=1001 vt=3") != NULL);
	assert(m.active_vt == 3);

	/* Replaying the single-use token must fail. */
	assert(controller_dispatch_auth(&c, getuid(), sel_cmd,
									resp, sizeof(resp)) == -ENOENT);

	/* Real auth.sock IPC via auth_controller client: CONFIRM_LOCK, cross-VT HANDOFF,
	 * return to still-locked graphical VT, PICK, SELECT, and lockout. */
	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		struct auth_controller_ctx actx = {
			.socket_path = auth_sock,
			.session_vt = 2,
		};
		assert(auth_controller_confirm_lock(auth_sock, 2) == 0);

		/* Direct AUTH for another VT (carol on tty4) must return AUTH_HANDOFF, not AUTH_OK. */
		struct auth_result res;
		auth_controller_verify("carolpass", 9, &actx, &res);
		assert(res.outcome == AUTH_HANDOFF);

		/* Picker SELECT for another VT (bob on tty3) must also return AUTH_HANDOFF. */
		auth_controller_verify("shared", 6, &actx, &res);
		assert(res.outcome == AUTH_PICK);
		assert(res.pick_count == 2);
		assert(strcmp(res.pick_items[0].username, "alice") == 0);
		assert(strcmp(res.pick_items[1].username, "bob") == 0);
		assert(auth_controller_select(res.pick_token, 1, &actx) == AUTH_HANDOFF);

		/* Picker SELECT for our own VT (alice on tty2) returns AUTH_OK. */
		auth_controller_verify("shared", 6, &actx, &res);
		assert(res.outcome == AUTH_PICK);
		assert(auth_controller_select(res.pick_token, 0, &actx) == AUTH_OK);

		auth_controller_verify("wrong1", 6, &actx, &res);
		assert(res.outcome == AUTH_DENIED);
		auth_controller_verify("wrong2", 6, &actx, &res);
		assert(res.outcome == AUTH_DENIED);
		auth_controller_verify("wrong3", 6, &actx, &res);
		assert(res.outcome == AUTH_LOCKOUT);
		assert(res.lockout_sec == 1);
		auth_controller_cancel(&actx);
		_exit(0);
	}
	for (int i = 0; i < 10; i++)
		assert(controller_handle_auth_one(&c) == 0);
	int status = 0;
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(m.active_vt == 2);

	/* Clear the 1s server lockout from the wrong1/wrong2/wrong3 checks above. */
	c.auth_fail_count = 0;
	c.auth_lockout_until_ms = 0;

	/* Verify lock_confirmed persisted across cross-VT return to tty2, and
	 * resets to false only on same-VT unlock. */
	assert(controller_dispatch_auth(&c, 1000, "CONFIRM_LOCK 2\n",
									resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "HANDOFF 3\n", resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "HANDOFF 2\n", resp, sizeof(resp)) == 0);
	/* tty2 is still locked, so alice (uid=1000) is still allowed to AUTH and unlock tty2. */
	assert(controller_dispatch_auth(&c, 1000, "AUTH shared\n",
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "PICK token=") != NULL);
	char tok2[40] = {0};
	assert(sscanf(resp, "PICK token=%39s", tok2) == 1);
	snprintf(sel_cmd, sizeof(sel_cmd), "SELECT %s 0\n", tok2);
	assert(controller_dispatch_auth(&c, 1000, sel_cmd,
									resp, sizeof(resp)) == 0);
	assert(strstr(resp, "OK user=alice uid=1000 vt=2") != NULL);
	/* Now tty2 is unlocked: leaving tty2 must fail closed until re-confirmed. */
	assert(controller_dispatch(&c, "LOCK\n", resp, sizeof(resp)) == -EAGAIN);

	controller_cleanup(&c, true);
}

/* A client that trickles bytes cannot hold the daemon: the whole request
 * must arrive within 2 s (it used to be 2 s per byte). */
static void test_slow_client_is_dropped(void) {
	struct mock_vt m = { .active_vt = 3, .locked = false };
	struct vt_ops ops = make_mock_ops(&m);
	char sock[64];
	snprintf(sock, sizeof(sock), "/tmp/tam-hide-slow-%d.sock", (int)getpid());
	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, sock, getuid()) == 0);

	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
		if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
			_exit(1);
		/* One byte every 300 ms, never a newline. */
		for (int i = 0; i < 20; i++) {
			if (write(fd, "S", 1) != 1)
				break;
			struct timespec ts = { 0, 300 * 1000000L };
			nanosleep(&ts, NULL);
		}
		_exit(0);
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	(void)controller_handle_one(&c);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double took = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	assert(took < 3.0);
	kill(pid, SIGKILL);
	(void)waitpid(pid, NULL, 0);
	controller_cleanup(&c, false);
}

int main(void) {
	test_slow_client_is_dropped();
	test_startup_recovery_and_ipc();
	test_fail_closed_on_activate_error();
	test_lock_confirmation_gate();
	test_auth_socket_and_multi_account_picker();
	printf("test_controller: all tests passed\n");
	return 0;
}
