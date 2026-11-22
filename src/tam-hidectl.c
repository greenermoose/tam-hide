#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [--socket PATH] <ping | status | lock | handoff VT>\n",
            prog);
}

int main(int argc, char **argv) {
    const char *socket_path = "/run/tam-hide/control.sock";
    int argi = 1;

    if (argi + 1 < argc && strcmp(argv[argi], "--socket") == 0) {
        socket_path = argv[argi + 1];
        argi += 2;
    }

    if (argi >= argc) {
        usage(argv[0]);
        return 2;
    }

    char cmd[128];
    if (strcmp(argv[argi], "ping") == 0 && argi + 1 == argc) {
        snprintf(cmd, sizeof(cmd), "PING\n");
    } else if (strcmp(argv[argi], "status") == 0 && argi + 1 == argc) {
        snprintf(cmd, sizeof(cmd), "STATUS\n");
    } else if (strcmp(argv[argi], "lock") == 0 && argi + 1 == argc) {
        snprintf(cmd, sizeof(cmd), "LOCK\n");
    } else if (strcmp(argv[argi], "handoff") == 0 && argi + 2 == argc) {
        snprintf(cmd, sizeof(cmd), "HANDOFF %s\n", argv[argi + 1]);
    } else {
        usage(argv[0]);
        return 2;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "tam-hidectl: socket path too long\n");
        close(fd);
        return 2;
    }
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "tam-hidectl: connect %s: %s\n", socket_path, strerror(errno));
        close(fd);
        return 1;
    }

    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len) {
        perror("write");
        close(fd);
        return 1;
    }

    char resp[256];
    ssize_t n = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (n <= 0) {
        fprintf(stderr, "tam-hidectl: empty response\n");
        return 1;
    }
    resp[n] = '\0';
    fputs(resp, stdout);
    return (strncmp(resp, "OK", 2) == 0) ? 0 : 1;
}
