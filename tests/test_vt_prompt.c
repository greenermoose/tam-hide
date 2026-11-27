/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "controller.h"
#include "vt_prompt.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct mock_vt {
	int active_vt;
	bool locked;
};

static int m_get_active(void *v, int *out) {
	struct mock_vt *m = v;
	*out = m->active_vt;
	return 0;
}
static int m_lock(void *v) {
	struct mock_vt *m = v;
	m->locked = true;
	return 0;
}
static int m_unlock(void *v) {
	struct mock_vt *m = v;
	m->locked = false;
	return 0;
}
static int m_activate(void *v, int vt_nr) {
	struct mock_vt *m = v;
	if (m->locked)
		return -EPERM;
	m->active_vt = vt_nr;
	return 0;
}

static size_t drain_pty(int master_fd, char *buf, size_t sz) {
	size_t total = 0;
	while (total + 1 < sz) {
		ssize_t n = read(master_fd, buf + total, sz - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
	}
	buf[total] = '\0';
	return total;
}

int main(void) {
	int master = -1, slave = -1;
	assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
	int mflags = fcntl(master, F_GETFL);
	assert(mflags >= 0);
	assert(fcntl(master, F_SETFL, mflags | O_NONBLOCK) == 0);

	struct mock_vt m = { .active_vt = 6, .locked = false };
	struct vt_ops ops = {
		.ctx = &m,
		.get_active = m_get_active,
		.lock_switch = m_lock,
		.unlock_switch = m_unlock,
		.activate_and_wait = m_activate,
		.render_banner = NULL,
	};

	char sock[64];
	snprintf(sock, sizeof(sock), "/tmp/tam-hide-vtprompt-%d.sock", (int)getpid());
	struct seat_controller c;
	assert(controller_init(&c, &ops, 6, sock, getuid()) == 0);

	struct passcode_params fast = { .t_cost = 1, .m_cost = 1024, .parallelism = 1 };
	assert(passcode_store_set(&c.store, 1000, "alice", "shared", 6, &fast) == 0);
	assert(passcode_store_set(&c.store, 1001, "bob", "shared", 6, &fast) == 0);
	assert(passcode_store_set(&c.store, 1002, "carol", "carolpass", 9, &fast) == 0);

	char resp[256];
	assert(controller_dispatch(&c, "REGISTER_SESSION 2 1000 alice graphical\n",
							   resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "REGISTER_SESSION 3 1001 bob console\n",
							   resp, sizeof(resp)) == 0);
	assert(controller_dispatch(&c, "REGISTER_SESSION 4 1002 carol console\n",
							   resp, sizeof(resp)) == 0);

	struct vt_prompt p;
	assert(vt_prompt_init_fd(&p, slave, true) == 0);

	/* Initial render must be non-disclosing. */
	char out[4096];
	assert(drain_pty(master, out, sizeof(out)) > 0);
	assert(strstr(out, "tam-hide") != NULL);
	assert(strstr(out, "Passcode:") != NULL);
	assert(strstr(out, "alice") == NULL);
	assert(strstr(out, "bob") == NULL);
	assert(strstr(out, "carol") == NULL);

	/* Wrong passcode must be masked and trigger Authentication failed. */
	assert(write(master, "wrong\r", 6) == 6);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(drain_pty(master, out, sizeof(out)) > 0);
	assert(strstr(out, "wrong") == NULL);
	assert(strstr(out, "Authentication failed") != NULL);
	assert(c.state == SEAT_STATE_LOCKED);

	/* Two more failures trigger server-side lockout. */
	assert(write(master, "w2\r", 3) == 3);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	(void)drain_pty(master, out, sizeof(out));
	assert(write(master, "w3\r", 3) == 3);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(drain_pty(master, out, sizeof(out)) > 0);
	assert(strstr(out, "Locked out (1s)") != NULL);

	/* Reset lockout for remaining functional checks. */
	c.auth_fail_count = 0;
	c.auth_lockout_until_ms = 0;

	/* Unique passcode immediately hands off to carol's tty4 without echoing passcode. */
	assert(write(master, "carolpass\r", 10) == 10);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(m.active_vt == 4);
	assert(c.state == SEAT_STATE_ACTIVE_USER);
	(void)drain_pty(master, out, sizeof(out));
	assert(strstr(out, "carolpass") == NULL);

	/* Return to lock_vt and test shared-passcode picker + Down arrow + Enter. */
	assert(controller_dispatch(&c, "LOCK\n", resp, sizeof(resp)) == 0);
	vt_prompt_reset(&p, NULL);
	(void)drain_pty(master, out, sizeof(out));

	assert(write(master, "shared\r", 7) == 7);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.mode == VT_PROMPT_PICKER);
	assert(p.pick_count == 2);
	assert(drain_pty(master, out, sizeof(out)) > 0);
	assert(strstr(out, "1. alice (tty2, graphical)") != NULL);
	assert(strstr(out, "2. bob (tty3, console)") != NULL);

	/* Down arrow selects bob (index 1), Enter hands off to tty3. */
	assert(write(master, "\033[B\r", 4) == 4);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(m.active_vt == 3);
	assert(c.state == SEAT_STATE_ACTIVE_USER);
	assert(p.mode == VT_PROMPT_IDLE);

	/* Return to lock_vt, enter picker, then cancel with Esc. */
	assert(controller_dispatch(&c, "LOCK\n", resp, sizeof(resp)) == 0);
	vt_prompt_reset(&p, NULL);
	(void)drain_pty(master, out, sizeof(out));

	assert(write(master, "shared\r", 7) == 7);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.mode == VT_PROMPT_PICKER);
	(void)drain_pty(master, out, sizeof(out));

	assert(write(master, "\033", 1) == 1);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.mode == VT_PROMPT_IDLE);
	assert(c.state == SEAT_STATE_LOCKED);
	assert(c.pending_pick.active == false);

	/* Test Backspace, Ctrl+U, and idle buffer wipe. */
	assert(write(master, "abc\x7f", 4) == 4);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.pass_len == 2);
	assert(write(master, "\x15", 1) == 1);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.pass_len == 0);

	assert(write(master, "xy", 2) == 2);
	assert(vt_prompt_handle_input(&p, &c) == 0);
	assert(p.pass_len == 2);
	vt_prompt_tick(&p, p.last_input_ms + TAM_HIDE_VT_PROMPT_IDLE_CLEAR_MS + 1);
	assert(p.pass_len == 0);

	vt_prompt_destroy(&p);
	close(master);
	controller_cleanup(&c, true);
	printf("test_vt_prompt: all tests passed\n");
	return 0;
}
