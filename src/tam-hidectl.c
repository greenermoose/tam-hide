/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "passcode_store.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

static void usage(const char *prog) {
	fprintf(stderr,
			"Usage: %s [--socket PATH] [--store PATH] <command>\n"
			"\n"
			"Controller IPC commands:\n"
			"  ping\n"
			"  status\n"
			"  lock\n"
			"  handoff <VT>\n"
			"  register-session <VT> <UID> <USER> <graphical|console>\n"
			"  unregister-session <VT>\n"
			"  confirm-lock <VT>\n"
			"  sessions\n"
			"\n"
			"Privileged passcode store commands (passcode read from stdin/TTY, never argv):\n"
			"  passcode-set [--argon2-m KiB] [--argon2-t ITERS] <USER> <UID>\n"
			"  passcode-del <USER>\n"
			"  passcode-list\n",
			prog);
}

static int read_passcode_stdin(char *buf, size_t buf_size, size_t *out_len) {
	bool is_tty = isatty(STDIN_FILENO) != 0;
	struct termios oldt, newt;
	bool echo_disabled = false;

	if (is_tty) {
		if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
			newt = oldt;
			newt.c_lflag &= ~(tcflag_t)ECHO;
			if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0)
				echo_disabled = true;
		}
		fprintf(stderr, "Passcode: ");
		fflush(stderr);
	}

	size_t pos = 0;
	int rc = 0;
	while (pos + 1 < buf_size) {
		char ch = 0;
		ssize_t n = read(STDIN_FILENO, &ch, 1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		if (n == 0 || ch == '\n')
			break;
		if (ch == '\r')
			continue;
		buf[pos++] = ch;
	}
	buf[pos] = '\0';

	if (echo_disabled) {
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
		fprintf(stderr, "\n");
	}
	if (rc < 0) {
		explicit_bzero(buf, buf_size);
		return rc;
	}
	if (pos == 0) {
		explicit_bzero(buf, buf_size);
		return -EINVAL;
	}
	*out_len = pos;
	return 0;
}

static int cmd_passcode_set(const char *store_path, int argc, char **argv) {
	struct passcode_params params = passcode_params_default();
	int i = 0;
	while (i < argc) {
		if (strcmp(argv[i], "--argon2-m") == 0 && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			unsigned long v = strtoul(argv[++i], &end, 10);
			if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
				fprintf(stderr, "tam-hidectl: invalid --argon2-m\n");
				return 2;
			}
			params.m_cost = (uint32_t)v;
			i++;
		} else if (strcmp(argv[i], "--argon2-t") == 0 && i + 1 < argc) {
			char *end = NULL;
			errno = 0;
			unsigned long v = strtoul(argv[++i], &end, 10);
			if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX) {
				fprintf(stderr, "tam-hidectl: invalid --argon2-t\n");
				return 2;
			}
			params.t_cost = (uint32_t)v;
			i++;
		} else {
			break;
		}
	}

	if (argc - i != 2) {
		fprintf(stderr, "tam-hidectl: passcode-set requires <USER> <UID>\n");
		return 2;
	}
	if (!passcode_params_valid_prod(&params)) {
		fprintf(stderr,
				"tam-hidectl: Argon2id params below safety floor "
				"(require m >= %u KiB, t >= %u, p = %u)\n",
				TAM_HIDE_ARGON2_MIN_M, TAM_HIDE_ARGON2_MIN_T,
				TAM_HIDE_ARGON2_MIN_P);
		return 2;
	}

	const char *user = argv[i];
	char *end = NULL;
	errno = 0;
	unsigned long uid_v = strtoul(argv[i + 1], &end, 10);
	if (errno != 0 || !end || *end != '\0' || uid_v > UINT_MAX ||
		!passcode_username_valid(user)) {
		fprintf(stderr, "tam-hidectl: invalid user or uid\n");
		return 2;
	}

	char pass[257];
	size_t plen = 0;
	int rc = read_passcode_stdin(pass, sizeof(pass), &plen);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: failed to read passcode: %s\n",
				strerror(-rc));
		return 1;
	}

	struct passcode_store store;
	passcode_store_init(&store);
	rc = passcode_store_load(&store, store_path);
	if (rc < 0 && rc != -ENOENT) {
		explicit_bzero(pass, sizeof(pass));
		fprintf(stderr, "tam-hidectl: load %s failed: %s\n", store_path,
				strerror(-rc));
		passcode_store_destroy(&store);
		return 1;
	}

	rc = passcode_store_set(&store, (uid_t)uid_v, user, pass, plen, &params);
	explicit_bzero(pass, sizeof(pass));
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: hashing failed: %s\n", strerror(-rc));
		passcode_store_destroy(&store);
		return 1;
	}

	rc = passcode_store_save(&store, store_path);
	passcode_store_destroy(&store);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: save %s failed: %s\n", store_path,
				strerror(-rc));
		return 1;
	}
	printf("OK passcode updated for %s (uid=%lu, m=%u, t=%u, p=%u)\n",
		   user, uid_v, params.m_cost, params.t_cost, params.parallelism);
	return 0;
}

static int cmd_passcode_del(const char *store_path, const char *user) {
	struct passcode_store store;
	passcode_store_init(&store);
	int rc = passcode_store_load(&store, store_path);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: load %s failed: %s\n", store_path,
				strerror(-rc));
		return 1;
	}
	rc = passcode_store_remove(&store, user);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: remove %s failed: %s\n", user,
				strerror(-rc));
		passcode_store_destroy(&store);
		return 1;
	}
	rc = passcode_store_save(&store, store_path);
	passcode_store_destroy(&store);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: save %s failed: %s\n", store_path,
				strerror(-rc));
		return 1;
	}
	printf("OK passcode removed for %s\n", user);
	return 0;
}

static int cmd_passcode_list(const char *store_path) {
	struct passcode_store store;
	passcode_store_init(&store);
	int rc = passcode_store_load(&store, store_path);
	if (rc < 0) {
		fprintf(stderr, "tam-hidectl: load %s failed: %s\n", store_path,
				strerror(-rc));
		return 1;
	}
	for (size_t i = 0; i < store.count; i++) {
		printf("%u %s\n", (unsigned)store.entries[i].uid,
			   store.entries[i].username);
	}
	passcode_store_destroy(&store);
	return 0;
}

int main(int argc, char **argv) {
	const char *socket_path = "/run/tam-hide/control.sock";
	const char *store_path = TAM_HIDE_DEFAULT_STORE_PATH;
	int argi = 1;

	while (argi < argc) {
		if (strcmp(argv[argi], "--socket") == 0 && argi + 1 < argc) {
			socket_path = argv[argi + 1];
			argi += 2;
		} else if (strcmp(argv[argi], "--store") == 0 && argi + 1 < argc) {
			store_path = argv[argi + 1];
			argi += 2;
		} else if (strcmp(argv[argi], "-h") == 0 ||
				   strcmp(argv[argi], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else {
			break;
		}
	}

	if (argi >= argc) {
		usage(argv[0]);
		return 2;
	}

	if (strcmp(argv[argi], "passcode-set") == 0)
		return cmd_passcode_set(store_path, argc - argi - 1, argv + argi + 1);
	if (strcmp(argv[argi], "passcode-del") == 0 && argi + 2 == argc)
		return cmd_passcode_del(store_path, argv[argi + 1]);
	if (strcmp(argv[argi], "passcode-list") == 0 && argi + 1 == argc)
		return cmd_passcode_list(store_path);

	char cmd[256];
	if (strcmp(argv[argi], "ping") == 0 && argi + 1 == argc) {
		snprintf(cmd, sizeof(cmd), "PING\n");
	} else if (strcmp(argv[argi], "status") == 0 && argi + 1 == argc) {
		snprintf(cmd, sizeof(cmd), "STATUS\n");
	} else if (strcmp(argv[argi], "lock") == 0 && argi + 1 == argc) {
		snprintf(cmd, sizeof(cmd), "LOCK\n");
	} else if (strcmp(argv[argi], "handoff") == 0 && argi + 2 == argc) {
		snprintf(cmd, sizeof(cmd), "HANDOFF %s\n", argv[argi + 1]);
	} else if (strcmp(argv[argi], "register-session") == 0 && argi + 5 == argc) {
		snprintf(cmd, sizeof(cmd), "REGISTER_SESSION %s %s %s %s\n",
				 argv[argi + 1], argv[argi + 2], argv[argi + 3],
				 argv[argi + 4]);
	} else if (strcmp(argv[argi], "unregister-session") == 0 &&
			   argi + 2 == argc) {
		snprintf(cmd, sizeof(cmd), "UNREGISTER_SESSION %s\n", argv[argi + 1]);
	} else if (strcmp(argv[argi], "confirm-lock") == 0 && argi + 2 == argc) {
		snprintf(cmd, sizeof(cmd), "CONFIRM_LOCK %s\n", argv[argi + 1]);
	} else if (strcmp(argv[argi], "sessions") == 0 && argi + 1 == argc) {
		snprintf(cmd, sizeof(cmd), "SESSIONS\n");
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
		fprintf(stderr, "tam-hidectl: connect %s: %s\n", socket_path,
				strerror(errno));
		close(fd);
		return 1;
	}

	size_t len = strlen(cmd);
	if (write(fd, cmd, len) != (ssize_t)len) {
		perror("write");
		close(fd);
		return 1;
	}

	char resp[4096];
	size_t total = 0;
	while (total + 1 < sizeof(resp)) {
		ssize_t n = read(fd, resp + total, sizeof(resp) - 1 - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	close(fd);
	if (total == 0) {
		fprintf(stderr, "tam-hidectl: empty response\n");
		return 1;
	}
	resp[total] = '\0';
	fputs(resp, stdout);
	return (strncmp(resp, "OK", 2) == 0) ? 0 : 1;
}
