#include "controller.h"

#include <assert.h>
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
    if (m->fail_activate_errno != 0) {
        return -m->fail_activate_errno;
    }
    /* Kernel only permits switching while unlocked. */
    if (m->locked) {
        return -EPERM;
    }
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
    ssize_t n = read(fd, out, out_sz - 1);
    assert(n > 0);
    out[n] = '\0';
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
        char resp[256];
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

    for (int i = 0; i < 5; i++) {
        assert(controller_handle_one(&c) == 0);
    }
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

int main(void) {
    test_startup_recovery_and_ipc();
    test_fail_closed_on_activate_error();
    printf("test_controller: all tests passed\n");
    return 0;
}
