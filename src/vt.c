#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int open_tty_dev(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    return fd;
}

static int linux_get_active(void *vctx, int *out_vt) {
    struct linux_vt_ctx *ctx = vctx;
    int fd = open_tty_dev(ctx->tty_dev);
    if (fd < 0) {
        return fd;
    }
    struct vt_stat st;
    memset(&st, 0, sizeof(st));
    int rc = 0;
    if (ioctl(fd, VT_GETSTATE, &st) < 0) {
        rc = -errno;
    } else {
        *out_vt = (int)st.v_active;
    }
    close(fd);
    return rc;
}

static int linux_lock_switch(void *vctx) {
    struct linux_vt_ctx *ctx = vctx;
    int fd = open_tty_dev(ctx->tty_dev);
    if (fd < 0) {
        return fd;
    }
    int rc = 0;
    if (ioctl(fd, VT_LOCKSWITCH, 0) < 0) {
        rc = -errno;
    }
    close(fd);
    return rc;
}

static int linux_unlock_switch(void *vctx) {
    struct linux_vt_ctx *ctx = vctx;
    int fd = open_tty_dev(ctx->tty_dev);
    if (fd < 0) {
        return fd;
    }
    int rc = 0;
    if (ioctl(fd, VT_UNLOCKSWITCH, 0) < 0) {
        rc = -errno;
    }
    close(fd);
    return rc;
}

static int linux_activate_and_wait(void *vctx, int vt_nr) {
    struct linux_vt_ctx *ctx = vctx;
    int fd = open_tty_dev(ctx->tty_dev);
    if (fd < 0) {
        return fd;
    }
    int rc = 0;
    if (ioctl(fd, VT_ACTIVATE, vt_nr) < 0) {
        rc = -errno;
        close(fd);
        return rc;
    }
    while (ioctl(fd, VT_WAITACTIVE, vt_nr) < 0) {
        if (errno == EINTR) {
            continue;
        }
        rc = -errno;
        break;
    }
    close(fd);
    return rc;
}

#ifndef TAM_HIDE_VERSION
#define TAM_HIDE_VERSION "0.2.2"
#endif

static int linux_render_banner(void *vctx, int lock_vt, const char *status) {
    (void)vctx;
    char path[32];
    int n = snprintf(path, sizeof(path), "/dev/tty%d", lock_vt);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -EINVAL;
    }
    int fd = open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    char msg[256];
    int m = snprintf(msg, sizeof(msg),
                     "\033c\n"
                     "  tam-hide %s\n"
                     "  Seat locked on tty%d (%s)\n",
                     TAM_HIDE_VERSION, lock_vt, status ? status : "locked");
    int rc = 0;
    if (m > 0) {
        size_t len = (size_t)m;
        if (len >= sizeof(msg)) {
            len = sizeof(msg) - 1;
        }
        ssize_t w = write(fd, msg, len);
        if (w < 0) {
            rc = -errno;
        }
    }
    close(fd);
    return rc;
}

void vt_linux_init(struct vt_ops *ops, struct linux_vt_ctx *ctx, const char *tty_dev) {
    ctx->tty_dev = tty_dev ? tty_dev : "/dev/tty0";
    ops->ctx = ctx;
    ops->get_active = linux_get_active;
    ops->lock_switch = linux_lock_switch;
    ops->unlock_switch = linux_unlock_switch;
    ops->activate_and_wait = linux_activate_and_wait;
    ops->render_banner = linux_render_banner;
}

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int vt_atomic_handoff(const struct vt_ops *ops, int target_vt, double *elapsed_ms) {
    if (!ops || !ops->unlock_switch || !ops->activate_and_wait || !ops->lock_switch) {
        return -EINVAL;
    }
    if (target_vt < TAM_VT_MIN || target_vt > TAM_VT_MAX) {
        return -EINVAL;
    }

    double t0 = monotonic_ms();
    int rc = ops->unlock_switch(ops->ctx);
    if (rc < 0) {
        /* Still ensure VT_LOCKSWITCH is engaged before returning. */
        (void)ops->lock_switch(ops->ctx);
        if (elapsed_ms) {
            *elapsed_ms = monotonic_ms() - t0;
        }
        return rc;
    }

    int act_rc = ops->activate_and_wait(ops->ctx, target_vt);
    int lock_rc = ops->lock_switch(ops->ctx);

    if (elapsed_ms) {
        *elapsed_ms = monotonic_ms() - t0;
    }
    if (act_rc < 0) {
        return act_rc;
    }
    if (lock_rc < 0) {
        return lock_rc;
    }
    return 0;
}
