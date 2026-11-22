#include "controller.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--lock-vt N] [--socket PATH] [--tty-dev PATH] [--release-on-exit]\n"
            "Prototype 0.0.0 seat controller (isolated VM testing only; not a security control).\n",
            prog);
}

int main(int argc, char **argv) {
    int lock_vt = 6;
    const char *socket_path = "/run/tam-hide/control.sock";
    const char *tty_dev = "/dev/tty0";
    bool release_on_exit = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lock-vt") == 0 && i + 1 < argc) {
            lock_vt = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--tty-dev") == 0 && i + 1 < argc) {
            tty_dev = argv[++i];
        } else if (strcmp(argv[i], "--release-on-exit") == 0) {
            release_on_exit = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct linux_vt_ctx vctx;
    struct vt_ops vops;
    vt_linux_init(&vops, &vctx, tty_dev);

    struct seat_controller ctrl;
    int rc = controller_init(&ctrl, &vops, lock_vt, socket_path, 0);
    if (rc < 0) {
        fprintf(stderr, "tam-hided: init failed: %s\n", strerror(-rc));
        return 1;
    }

    fprintf(stderr, "tam-hided: prototype 0.0.0 ready on lock_vt=%d socket=%s\n",
            lock_vt, socket_path);
    fflush(stderr);

    while (!g_stop) {
        struct pollfd pfd = { .fd = ctrl.listen_fd, .events = POLLIN };
        int prc = poll(&pfd, 1, 500);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "tam-hided: poll failed: %s\n", strerror(errno));
            break;
        }
        if (prc > 0 && (pfd.revents & POLLIN)) {
            (void)controller_handle_one(&ctrl);
        }
    }

    controller_cleanup(&ctrl, release_on_exit);
    return 0;
}
