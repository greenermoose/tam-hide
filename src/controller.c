#include "controller.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_IPC_LINE 256

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

static int bind_control_socket(const char *path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (!path || strlen(path) >= sizeof(addr.sun_path)) {
        return -EINVAL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }

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

    if (chmod(path, 0600) < 0) {
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

int controller_init(struct seat_controller *c,
                    const struct vt_ops *vt,
                    int lock_vt,
                    const char *socket_path,
                    uid_t allowed_uid) {
    if (!c || !vt || !socket_path) {
        return -EINVAL;
    }
    if (lock_vt < TAM_VT_MIN || lock_vt > TAM_VT_MAX) {
        return -EINVAL;
    }

    memset(c, 0, sizeof(*c));
    c->vt = *vt;
    c->lock_vt = lock_vt;
    c->active_vt = -1;
    c->state = SEAT_STATE_LOCKED;
    c->vt_locked = false;
    c->allowed_uid = allowed_uid;
    c->listen_fd = -1;
    strncpy(c->socket_path, socket_path, sizeof(c->socket_path) - 1);

    /* Fail-closed first: engage VT_LOCKSWITCH before anything else. */
    int rc = c->vt.lock_switch(c->vt.ctx);
    if (rc < 0) {
        return rc;
    }
    c->vt_locked = true;

    /* Check current active VT; atomically move to lock_vt if needed. */
    int cur_vt = -1;
    rc = c->vt.get_active(c->vt.ctx, &cur_vt);
    if (rc < 0 || cur_vt != lock_vt) {
        rc = vt_atomic_handoff(&c->vt, lock_vt, NULL);
        if (rc < 0) {
            return rc;
        }
    }
    c->active_vt = lock_vt;
    c->state = SEAT_STATE_LOCKED;

    if (c->vt.render_banner) {
        (void)c->vt.render_banner(c->vt.ctx, lock_vt, "controller ready");
    }

    int fd = bind_control_socket(socket_path);
    if (fd < 0) {
        return fd;
    }
    c->listen_fd = fd;
    return 0;
}

static int parse_vt_arg(const char *s, int *out_vt) {
    if (!s || *s == '\0') {
        return -EINVAL;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -EINVAL;
    }
    if (val < TAM_VT_MIN || val > TAM_VT_MAX) {
        return -ERANGE;
    }
    *out_vt = (int)val;
    return 0;
}

int controller_dispatch(struct seat_controller *c,
                        const char *line,
                        char *resp,
                        size_t resp_size) {
    if (!c || !line || !resp || resp_size == 0) {
        return -EINVAL;
    }

    char buf[MAX_IPC_LINE];
    size_t len = strnlen(line, sizeof(buf));
    if (len >= sizeof(buf)) {
        snprintf(resp, resp_size, "ERR line_too_long\n");
        return -E2BIG;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';

    /* Strip trailing CR/LF/spaces */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }

    if (strcmp(buf, "PING") == 0) {
        snprintf(resp, resp_size, "OK pong\n");
        return 0;
    }

    if (strcmp(buf, "STATUS") == 0) {
        int live_vt = c->active_vt;
        if (c->vt.get_active(c->vt.ctx, &live_vt) == 0) {
            c->active_vt = live_vt;
        }
        snprintf(resp, resp_size,
                 "OK state=%s lock_vt=%d active_vt=%d vt_locked=%d\n",
                 seat_state_name(c->state), c->lock_vt, c->active_vt,
                 c->vt_locked ? 1 : 0);
        return 0;
    }

    if (strcmp(buf, "LOCK") == 0) {
        double ms = 0.0;
        int rc = vt_atomic_handoff(&c->vt, c->lock_vt, &ms);
        if (rc < 0) {
            snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
            return rc;
        }
        c->active_vt = c->lock_vt;
        c->state = SEAT_STATE_LOCKED;
        if (c->vt.render_banner) {
            (void)c->vt.render_banner(c->vt.ctx, c->lock_vt, "locked");
        }
        snprintf(resp, resp_size,
                 "OK state=%s active_vt=%d vt_locked=1 handoff_ms=%.2f\n",
                 seat_state_name(c->state), c->active_vt, ms);
        return 0;
    }

    if (strncmp(buf, "HANDOFF ", 8) == 0) {
        int target_vt = 0;
        int rc = parse_vt_arg(buf + 8, &target_vt);
        if (rc < 0) {
            snprintf(resp, resp_size, "ERR invalid_vt\n");
            return rc;
        }
        if (target_vt == c->lock_vt) {
            snprintf(resp, resp_size, "ERR target_is_lock_vt\n");
            return -EINVAL;
        }
        double ms = 0.0;
        rc = vt_atomic_handoff(&c->vt, target_vt, &ms);
        if (rc < 0) {
            snprintf(resp, resp_size, "ERR handoff_failed errno=%d\n", -rc);
            return rc;
        }
        c->active_vt = target_vt;
        c->state = SEAT_STATE_ACTIVE_USER;
        snprintf(resp, resp_size,
                 "OK state=%s active_vt=%d vt_locked=1 handoff_ms=%.2f\n",
                 seat_state_name(c->state), c->active_vt, ms);
        return 0;
    }

    snprintf(resp, resp_size, "ERR unknown_command\n");
    return -EINVAL;
}

static ssize_t read_bounded_line(int fd, char *buf, size_t max_len) {
    size_t pos = 0;
    while (pos + 1 < max_len) {
        char ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (n == 0) {
            break;
        }
        buf[pos++] = ch;
        if (ch == '\n') {
            break;
        }
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

int controller_handle_one(struct seat_controller *c) {
    if (!c || c->listen_fd < 0) {
        return -EINVAL;
    }

    int client_fd = accept4(c->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0) {
        return -errno;
    }

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

    if (cred.uid != c->allowed_uid) {
        const char *err = "ERR permission_denied\n";
        (void)write(client_fd, err, strlen(err));
        close(client_fd);
        return -EPERM;
    }

    char req[MAX_IPC_LINE];
    ssize_t n = read_bounded_line(client_fd, req, sizeof(req));
    if (n <= 0) {
        close(client_fd);
        return (n < 0) ? (int)n : 0;
    }

    char resp[256];
    (void)controller_dispatch(c, req, resp, sizeof(resp));
    (void)write(client_fd, resp, strlen(resp));
    close(client_fd);
    return 0;
}

void controller_cleanup(struct seat_controller *c, bool release_vt_lock) {
    if (!c) {
        return;
    }
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        c->listen_fd = -1;
    }
    if (c->socket_path[0] != '\0') {
        (void)unlink(c->socket_path);
    }
    if (release_vt_lock && c->vt.unlock_switch) {
        (void)c->vt.unlock_switch(c->vt.ctx);
        c->vt_locked = false;
    }
}
